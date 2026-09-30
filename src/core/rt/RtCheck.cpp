#include "core/rt/RtCheck.h"

#include <cstdlib>

namespace bf::rt {

namespace {
thread_local bool tlRealtime = false;
std::atomic<std::uint64_t> gAllocs{0}, gDeallocs{0}, gLocks{0};
std::atomic<bool> gAbort{false};
std::atomic<bool> gHooks{false};

void violation(std::atomic<std::uint64_t>& counter) noexcept {
    counter.fetch_add(1, std::memory_order_relaxed);
    if (gAbort.load(std::memory_order_relaxed)) std::abort();
}
}  // namespace

void setThreadRealtime(bool realtime) noexcept { tlRealtime = realtime; }
bool isRealtimeThread() noexcept { return tlRealtime; }

std::uint64_t rtAllocationCount() noexcept { return gAllocs.load(std::memory_order_relaxed); }
std::uint64_t rtDeallocationCount() noexcept { return gDeallocs.load(std::memory_order_relaxed); }
std::uint64_t rtLockCount() noexcept { return gLocks.load(std::memory_order_relaxed); }

void resetRtViolationCounts() noexcept {
    gAllocs.store(0, std::memory_order_relaxed);
    gDeallocs.store(0, std::memory_order_relaxed);
    gLocks.store(0, std::memory_order_relaxed);
}

void setAbortOnRtViolation(bool abortOnViolation) noexcept { gAbort.store(abortOnViolation, std::memory_order_relaxed); }
bool rtCheckHooksActive() noexcept { return gHooks.load(std::memory_order_relaxed); }

namespace detail {
void noteAllocation() noexcept { violation(gAllocs); }
void noteDeallocation() noexcept { violation(gDeallocs); }
void noteLock() noexcept { violation(gLocks); }
void markHooksLinked() noexcept { gHooks.store(true, std::memory_order_relaxed); }
}  // namespace detail

}  // namespace bf::rt
