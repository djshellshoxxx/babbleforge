#pragma once
// Seqlock snapshots (docs/REALTIME_ARCHITECTURE.md §5.5): one writer (typically RT) publishes a
// trivially copyable POD; readers retry while the sequence is odd or changed. The writer never
// blocks. The payload is stored in relaxed atomic words so concurrent access is race-free.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace bf::rt {

template <class T>
class SeqLock {
    static_assert(std::is_trivially_copyable_v<T>, "SeqLock payload must be trivially copyable");
    static constexpr std::size_t kWords = (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
    SeqLock() { store(T{}); }

    // Single writer.
    void store(const T& v) noexcept {
        std::array<std::uint64_t, kWords> w{};
        std::memcpy(w.data(), static_cast<const void*>(&v), sizeof(T));
        const std::uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < kWords; ++i) data_[i].store(w[i], std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }

    // Any reader. False if a write was in progress (caller may retry).
    bool tryLoad(T& out) const noexcept {
        const std::uint64_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1u) return false;
        std::array<std::uint64_t, kWords> w{};
        for (std::size_t i = 0; i < kWords; ++i) w[i] = data_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s1) return false;
        std::memcpy(static_cast<void*>(&out), w.data(), sizeof(T));
        return true;
    }
    // Retries until a consistent snapshot is read (non-RT readers).
    T load() const noexcept {
        T v{};
        while (!tryLoad(v)) {
        }
        return v;
    }
    std::uint64_t sequence() const noexcept { return seq_.load(std::memory_order_acquire); }

private:
    std::atomic<std::uint64_t> seq_{0};
    std::array<std::atomic<std::uint64_t>, kWords> data_{};
};

}  // namespace bf::rt
