#pragma once
// Flush-to-zero / denormals-are-zero for the callback thread (REALTIME_ARCHITECTURE.md §1.1
// item 12, §3 step 1). x86/x64: MXCSR FTZ (bit 15) and DAZ (bit 6) via _mm_setcsr. Other
// architectures: no-op.
#include <cstdint>

namespace bf::rt {

// Sets FTZ/DAZ on the calling thread; returns the previous control word (0 where unsupported).
std::uint32_t disableDenormals() noexcept;
void restoreFloatingPointState(std::uint32_t previous) noexcept;
bool denormalsDisabled() noexcept;  // FTZ and DAZ both set on this thread (false where unsupported)

class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept : prev_(disableDenormals()) {}
    ~ScopedNoDenormals() { restoreFloatingPointState(prev_); }
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

private:
    std::uint32_t prev_;
};

}  // namespace bf::rt
