#pragma once
// Bounded single-producer / single-consumer FIFO (docs/REALTIME_ARCHITECTURE.md §5.1).
//
// Capacity is a power of two; head/tail are monotonically increasing std::atomic<size_t>
// counters (all Capacity slots are usable), published with release stores and observed with
// acquire loads. Messages are trivially copyable. push()/pop() never block or allocate and are
// RT-safe on either side. Exactly one producer thread and one consumer thread at a time.
// Objects with large capacities should be heap-allocated at prepare time.
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // padded because of alignas (intended: no false sharing)
#endif

namespace bf::rt {

template <class T, std::size_t Capacity>
class SpscFifo {
    static_assert(std::is_trivially_copyable_v<T>, "SpscFifo messages must be trivially copyable");
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

public:
    static constexpr std::size_t kCapacity = Capacity;

    // Producer. False (message dropped) when full.
    bool push(const T& v) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= Capacity) return false;
        buf_[h & (Capacity - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    // Consumer. False when empty.
    bool pop(T& v) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        v = buf_[t & (Capacity - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    // Consumer: next message without removing it.
    bool peek(T& v) const noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        v = buf_[t & (Capacity - 1)];
        return true;
    }
    // Approximate (exact on the producer or consumer thread for its own side).
    std::size_t size() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return h >= t ? h - t : 0;
    }
    bool empty() const noexcept { return size() == 0; }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::array<T, Capacity> buf_{};
};

}  // namespace bf::rt

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
