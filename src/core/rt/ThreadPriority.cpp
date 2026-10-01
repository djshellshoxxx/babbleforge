#include "core/rt/ThreadPriority.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace bf::rt {

bool setCurrentThreadPriority(ThreadPriority p) noexcept {
#if defined(_WIN32)
    const int w = p == ThreadPriority::Low           ? THREAD_PRIORITY_LOWEST
                  : p == ThreadPriority::BelowNormal ? THREAD_PRIORITY_BELOW_NORMAL
                                                     : THREAD_PRIORITY_NORMAL;
    return SetThreadPriority(GetCurrentThread(), w) != 0;
#elif defined(__APPLE__)
    const qos_class_t q = p == ThreadPriority::Low           ? QOS_CLASS_BACKGROUND
                          : p == ThreadPriority::BelowNormal ? QOS_CLASS_UTILITY
                                                             : QOS_CLASS_DEFAULT;
    return pthread_set_qos_class_self_np(q, 0) == 0;
#elif defined(__linux__)
    // Linux nice values are per thread (PRIO_PROCESS with a thread id). CFS weight: nice 5 is
    // ~1/3, nice 10 ~1/9 of a nice-0 thread; raising back towards 0 may need CAP_SYS_NICE.
    const int nice = p == ThreadPriority::Low ? 10 : p == ThreadPriority::BelowNormal ? 5 : 0;
    const auto tid = static_cast<id_t>(syscall(SYS_gettid));
    return setpriority(PRIO_PROCESS, tid, nice) == 0;
#else
    (void)p;
    return false;
#endif
}

}  // namespace bf::rt
