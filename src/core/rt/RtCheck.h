#pragma once
// RT-thread marker and allocation / lock instrumentation (docs/REALTIME_ARCHITECTURE.md §1.3).
//
// Every thread that runs RT code (the audio callback) marks itself with setThreadRealtime(true)
// (RealtimeEngine does this at callback entry). Two hooks report violations on such threads:
//
//  - Allocation hooks: global operator new / delete replacements in rt/hooks/RtCheckHooks.cpp.
//    They are linked only into binaries built for RT checking (the `bf_rtcheck_hooks` object
//    library: the dedicated RT-check test binary, and every executable when the CMake option
//    BF_RT_CHECK is ON). rtCheckHooksActive() tells whether they are present.
//  - Lock hooks: CheckedMutex, the mutex used by every non-RT shared structure of the real-time
//    host (control queue, logger queue, preload scheduler, service state). Locking it on an RT
//    thread is counted.
//
// Violations are counted (rtAllocationCount(), rtLockCount()); with setAbortOnRtViolation(true)
// the process aborts on the first one (fail loudly in CI builds).
#include <atomic>
#include <cstdint>
#include <mutex>

namespace bf::rt {

void setThreadRealtime(bool realtime) noexcept;
bool isRealtimeThread() noexcept;

std::uint64_t rtAllocationCount() noexcept;    // operator new on an RT thread
std::uint64_t rtDeallocationCount() noexcept;  // operator delete on an RT thread
std::uint64_t rtLockCount() noexcept;          // CheckedMutex::lock on an RT thread
void resetRtViolationCounts() noexcept;
void setAbortOnRtViolation(bool abortOnViolation) noexcept;
bool rtCheckHooksActive() noexcept;

namespace detail {
void noteAllocation() noexcept;    // called by the hooks (RT threads only)
void noteDeallocation() noexcept;
void noteLock() noexcept;
void markHooksLinked() noexcept;
}  // namespace detail

// RAII marker (restores the previous flag).
class ScopedRealtimeThread {
public:
    ScopedRealtimeThread() noexcept : prev_(isRealtimeThread()) { setThreadRealtime(true); }
    ~ScopedRealtimeThread() { setThreadRealtime(prev_); }
    ScopedRealtimeThread(const ScopedRealtimeThread&) = delete;
    ScopedRealtimeThread& operator=(const ScopedRealtimeThread&) = delete;

private:
    bool prev_;
};

// Lockable wrapper that reports locks taken on an RT thread (never used on the RT path).
class CheckedMutex {
public:
    void lock() {
        if (isRealtimeThread()) detail::noteLock();
        m_.lock();
    }
    bool try_lock() {
        if (isRealtimeThread()) detail::noteLock();
        return m_.try_lock();
    }
    void unlock() { m_.unlock(); }
    std::mutex& native() noexcept { return m_; }

private:
    std::mutex m_;
};

}  // namespace bf::rt
