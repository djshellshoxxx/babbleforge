// Global operator new / delete replacements for RT checking (REALTIME_ARCHITECTURE.md §1.3).
// Linked only into RT-check binaries (object library bf_rtcheck_hooks). Allocation or
// deallocation on a thread flagged RT (bf::rt::setThreadRealtime) is counted, or aborts when
// bf::rt::setAbortOnRtViolation(true).
#include <cstdlib>
#include <new>
#if defined(_MSC_VER)
#include <malloc.h>
#endif

#include "core/rt/RtCheck.h"

namespace {

struct HooksLinked {
    HooksLinked() { bf::rt::detail::markHooksLinked(); }
};
const HooksLinked gHooksLinked;

inline void onAlloc() noexcept {
    if (bf::rt::isRealtimeThread()) bf::rt::detail::noteAllocation();
}
inline void onFree(void* p) noexcept {
    if (p && bf::rt::isRealtimeThread()) bf::rt::detail::noteDeallocation();
}

void* allocPlain(std::size_t n) noexcept {
    onAlloc();
    return std::malloc(n ? n : 1);
}

void* allocAligned(std::size_t n, std::size_t al) noexcept {
    onAlloc();
    if (al < sizeof(void*)) al = sizeof(void*);
    if (n == 0) n = 1;
#if defined(_MSC_VER)
    return _aligned_malloc(n, al);
#else
    void* p = nullptr;
    return posix_memalign(&p, al, n) == 0 ? p : nullptr;
#endif
}

void freeAligned(void* p) noexcept {
    onFree(p);
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

}  // namespace

void* operator new(std::size_t n) {
    if (void* p = allocPlain(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (void* p = allocPlain(n)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return allocPlain(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return allocPlain(n); }
void* operator new(std::size_t n, std::align_val_t al) {
    if (void* p = allocAligned(n, static_cast<std::size_t>(al))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t al) {
    if (void* p = allocAligned(n, static_cast<std::size_t>(al))) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    return allocAligned(n, static_cast<std::size_t>(al));
}
void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    return allocAligned(n, static_cast<std::size_t>(al));
}

void operator delete(void* p) noexcept {
    onFree(p);
    std::free(p);
}
void operator delete[](void* p) noexcept {
    onFree(p);
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    onFree(p);
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    onFree(p);
    std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    onFree(p);
    std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    onFree(p);
    std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept { freeAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { freeAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { freeAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { freeAligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned(p); }
