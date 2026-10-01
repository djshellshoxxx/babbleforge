#include "core/random/Random.h"

namespace bf {

namespace {
inline void mul128(std::uint64_t a, std::uint64_t b, std::uint64_t& hi, std::uint64_t& lo) noexcept {
    const std::uint64_t a0 = a & 0xFFFFFFFFULL, a1 = a >> 32;
    const std::uint64_t b0 = b & 0xFFFFFFFFULL, b1 = b >> 32;
    const std::uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const std::uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
    lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
    hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}
}  // namespace

std::uint64_t RngStream::uniformInt(std::uint64_t n) noexcept {
    if (n == 0) return 0;
    std::uint64_t hi, lo;
    mul128(gen_.next(), n, hi, lo);
    if (lo < n) {
        const std::uint64_t t = (0 - n) % n;
        while (lo < t) mul128(gen_.next(), n, hi, lo);
    }
    return hi;
}

}  // namespace bf
