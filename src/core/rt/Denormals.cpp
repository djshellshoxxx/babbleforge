#include "core/rt/Denormals.h"

#if defined(__SSE__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#define BF_HAVE_MXCSR 1
#include <xmmintrin.h>
#else
#define BF_HAVE_MXCSR 0
#endif

namespace bf::rt {

namespace {
constexpr std::uint32_t kFtz = 0x8000u, kDaz = 0x0040u;
}

std::uint32_t disableDenormals() noexcept {
#if BF_HAVE_MXCSR
    const unsigned int prev = _mm_getcsr();
    _mm_setcsr(prev | kFtz | kDaz);
    return static_cast<std::uint32_t>(prev);
#else
    return 0;
#endif
}

void restoreFloatingPointState(std::uint32_t previous) noexcept {
#if BF_HAVE_MXCSR
    _mm_setcsr(static_cast<unsigned int>(previous));
#else
    (void)previous;
#endif
}

bool denormalsDisabled() noexcept {
#if BF_HAVE_MXCSR
    const unsigned int csr = _mm_getcsr();
    return (csr & kFtz) != 0 && (csr & kDaz) != 0;
#else
    return false;
#endif
}

}  // namespace bf::rt
