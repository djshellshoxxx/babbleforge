#pragma once
// RCU pointer exchange for large immutable objects (docs/REALTIME_ARCHITECTURE.md §5.2).
//
//   Control:  slot.publish(std::make_unique<T>(...));   // non-RT allocation
//   RT:       T* cur = slot.acquire();                  // at sub-block start: picks up the
//                                                       // pending object, retires the old one
//   Control:  slot.collectGarbage();                    // deletes retired / superseded objects
//
// The RT side never deletes and never reference-counts: a retired pointer is handed back to
// Control through an SPSC retire queue. If that queue is full (Control not collecting), the RT
// side keeps the current object and picks the pending one up later. An object superseded
// before RT picked it up is reclaimed by Control directly.
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "core/rt/SpscFifo.h"

namespace bf::rt {

template <class T, std::size_t RetireCapacity = 64>
class RcuSlot {
    static_assert(std::atomic<T*>::is_always_lock_free);

public:
    RcuSlot() = default;
    RcuSlot(const RcuSlot&) = delete;
    RcuSlot& operator=(const RcuSlot&) = delete;
    // Only when no RT thread uses the slot any more.
    ~RcuSlot() {
        delete pending_.exchange(nullptr, std::memory_order_acq_rel);
        delete current_;
        collectGarbage();
    }

    // Control thread. Takes ownership.
    void publish(std::unique_ptr<T> obj) {
        T* old = pending_.exchange(obj.release(), std::memory_order_acq_rel);
        if (old) garbage_.emplace_back(old);  // superseded before RT picked it up
    }

    // RT thread: returns the current object after picking up a pending one (may be null).
    T* acquire() noexcept {
        if (pending_.load(std::memory_order_relaxed) != nullptr &&
            retire_.size() < RetireCapacity) {
            if (T* n = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
                if (current_) retire_.push(current_);
                current_ = n;
                ++swaps_;
            }
        }
        return current_;
    }
    // RT thread: the current object without picking anything up.
    T* current() const noexcept { return current_; }
    std::uint64_t swaps() const noexcept { return swaps_; }

    // Control thread: deletes retired and superseded objects; returns how many were freed.
    std::size_t collectGarbage() {
        std::size_t n = garbage_.size();
        garbage_.clear();
        T* p = nullptr;
        while (retire_.pop(p)) {
            delete p;
            ++n;
        }
        return n;
    }
    bool hasPending() const noexcept { return pending_.load(std::memory_order_acquire) != nullptr; }

private:
    std::atomic<T*> pending_{nullptr};
    T* current_ = nullptr;                         // RT-owned
    std::uint64_t swaps_ = 0;                      // RT-owned
    SpscFifo<T*, RetireCapacity> retire_;          // RT -> Control
    std::vector<std::unique_ptr<T>> garbage_;      // Control-owned
};

}  // namespace bf::rt
