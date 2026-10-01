#include "core/dsp/Fft.h"

#include <stdexcept>

#include "core/math/DetMath.h"
#include "core/math/Restrict.h"

namespace bf {

template <typename T>
bool RealFft<T>::isValidSize(std::size_t n) noexcept {
    return n >= 4 && n <= (std::size_t{1} << 20) && (n & (n - 1)) == 0;
}

template <typename T>
RealFft<T>::RealFft(std::size_t n) : n_(n), m_(n / 2) {
    if (!isValidSize(n)) throw std::invalid_argument("RealFft: size must be a power of two in [4, 2^20]");
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    tw_.resize(2 * m_);
    for (std::size_t k = 0; k < m_; ++k) {
        double s = 0.0, c = 1.0;
        detsincos(-kTwoPi * static_cast<double>(k) / static_cast<double>(n_), s, c);
        tw_[2 * k] = static_cast<T>(c);
        tw_[2 * k + 1] = static_cast<T>(s);
    }
    unsigned bits = 0;
    while ((std::size_t{1} << bits) < m_) ++bits;
    rev_.resize(m_);
    for (std::size_t i = 0; i < m_; ++i) {
        std::uint32_t r = 0;
        for (unsigned b = 0; b < bits; ++b)
            if (i & (std::size_t{1} << b)) r |= std::uint32_t{1} << (bits - 1 - b);
        rev_[i] = r;
    }
    // Stage with half-length h (h = 1, 2, 4, ... m/2) uses twiddles tw_[2 j (n / 2h)], j < h,
    // stored contiguously at offset h - 1. The inverse sign is applied here (sgn * wi with
    // sgn = -1 is an exact negation), not per butterfly.
    const std::size_t nt = m_ > 1 ? m_ - 1 : 0;
    stw_.assign(3 * nt, T(0));
    for (std::size_t h = 1; h < m_; h <<= 1) {
        const std::size_t step = n_ / (2 * h);
        for (std::size_t j = 0; j < h; ++j) {
            stw_[h - 1 + j] = tw_[2 * j * step];
            stw_[nt + h - 1 + j] = T(1) * tw_[2 * j * step + 1];
            stw_[2 * nt + h - 1 + j] = T(-1) * tw_[2 * j * step + 1];
        }
    }
    wre_.assign(m_, T(0));
    wim_.assign(m_, T(0));
}

namespace {
// One radix-2 butterfly (a, b) <- (a + w b, a - w b), operations in the reference order.
template <typename T>
inline void butterfly(T& ar, T& ai, T& br, T& bi, T wr, T wi) noexcept {
    const T xr = br * wr - bi * wi;
    const T xi = br * wi + bi * wr;
    const T a0 = ar, a1 = ai;
    br = a0 - xr;
    bi = a1 - xi;
    ar = a0 + xr;
    ai = a1 + xi;
}

// A run of `half` butterflies with contiguous twiddles. Restrict-qualified parameters let the
// compiler vectorise without runtime overlap checks (the a and b halves never overlap).
template <typename T>
inline void butterflyRun(T* BF_RESTRICT ar, T* BF_RESTRICT ai, T* BF_RESTRICT br, T* BF_RESTRICT bi,
                         const T* BF_RESTRICT wr, const T* BF_RESTRICT wi, std::size_t half) noexcept {
    for (std::size_t j = 0; j < half; ++j) {
        const T xr = br[j] * wr[j] - bi[j] * wi[j];
        const T xi = br[j] * wi[j] + bi[j] * wr[j];
        const T a0 = ar[j], a1 = ai[j];
        br[j] = a0 - xr;
        bi[j] = a1 - xi;
        ar[j] = a0 + xr;
        ai[j] = a1 + xi;
    }
}
}  // namespace

// In-place iterative radix-2 DIT on the planar work buffer (input in bit-reversed order).
// The first two stages (half = 1, 2) are fused per group of 4 points (each group is
// independent), later stages run vectorisable butterfly runs. Per element the operations
// and their order are those of the textbook loop, so the output is bit-identical to it.
template <typename T>
void RealFft<T>::complexTransform(bool inverseDir) noexcept {
    T* re = wre_.data();
    T* im = wim_.data();
    const std::size_t nt = m_ - 1;
    const T* twr = stw_.data();
    const T* twi = stw_.data() + (inverseDir ? 2 * nt : nt);
    std::size_t half = 1;
    if (m_ == 2) {
        butterfly(re[0], im[0], re[1], im[1], twr[0], twi[0]);
        return;
    }
    {
        const T w1r = twr[0], w1i = twi[0];            // stage half = 1
        const T w20r = twr[1], w20i = twi[1];          // stage half = 2, j = 0
        const T w21r = twr[2], w21i = twi[2];          //                 j = 1
        for (std::size_t i = 0; i < m_; i += 4) {
            butterfly(re[i], im[i], re[i + 1], im[i + 1], w1r, w1i);
            butterfly(re[i + 2], im[i + 2], re[i + 3], im[i + 3], w1r, w1i);
            butterfly(re[i], im[i], re[i + 2], im[i + 2], w20r, w20i);
            butterfly(re[i + 1], im[i + 1], re[i + 3], im[i + 3], w21r, w21i);
        }
        half = 4;
    }
    for (; half < m_; half <<= 1) {
        const std::size_t len = half << 1;
        const T* wr = twr + (half - 1);
        const T* wi = twi + (half - 1);
        for (std::size_t i = 0; i < m_; i += len) butterflyRun(re + i, im + i, re + i + half, im + i + half, wr, wi, half);
    }
}

template <typename T>
template <class Store>
void RealFft<T>::forwardImpl(const T* in, Store store) noexcept {
    T* re = wre_.data();
    T* im = wim_.data();
    for (std::size_t k = 0; k < m_; ++k) {
        const std::size_t r = rev_[k];
        re[r] = in[2 * k];
        im[r] = in[2 * k + 1];
    }
    complexTransform(false);
    const T half = T(0.5);
    // k = 0 and k = m
    const T z0r = re[0], z0i = im[0];
    for (std::size_t k = 1; k < m_; ++k) {
        const T zr = re[k], zi = im[k];
        const T cr = re[m_ - k], ci = -im[m_ - k];  // conj(Z[m-k])
        const T fer = half * (zr + cr), fei = half * (zi + ci);
        // Fo = -i (Z - Zc) / 2
        const T dr = zr - cr, di = zi - ci;
        const T for_ = half * di, foi = -half * dr;
        const T wr = tw_[2 * k], wi = tw_[2 * k + 1];
        store(k, fer + (for_ * wr - foi * wi), fei + (for_ * wi + foi * wr));
    }
    store(0, z0r + z0i, T(0));
    store(m_, z0r - z0i, T(0));
}

template <typename T>
template <class Load>
void RealFft<T>::inverseImpl(Load load, T* out) noexcept {
    T* re = wre_.data();
    T* im = wim_.data();
    const T half = T(0.5);
    {
        T x0 = 0, xm = 0, unused = 0;
        load(0, x0, unused);
        load(m_, xm, unused);
        const std::size_t r = rev_[0];
        re[r] = half * (x0 + xm);
        im[r] = half * (x0 - xm);
    }
    for (std::size_t k = 1; k < m_; ++k) {
        T xr = 0, xi = 0, cr = 0, ci = 0;
        load(k, xr, xi);
        load(m_ - k, cr, ci);
        ci = -ci;
        const T fer = half * (xr + cr), fei = half * (xi + ci);
        const T dr = half * (xr - cr), di = half * (xi - ci);
        // Fo = d * conj(W^k)
        const T wr = tw_[2 * k], wi = -tw_[2 * k + 1];
        const T for_ = dr * wr - di * wi;
        const T foi = dr * wi + di * wr;
        // Z = Fe + i Fo
        const std::size_t r = rev_[k];
        re[r] = fer - foi;
        im[r] = fei + for_;
    }
    complexTransform(true);
    const T scale = T(1) / static_cast<T>(m_);
    for (std::size_t k = 0; k < m_; ++k) {
        out[2 * k] = re[k] * scale;
        out[2 * k + 1] = im[k] * scale;
    }
}

template <typename T>
void RealFft<T>::forward(const T* in, std::complex<T>* outC) noexcept {
    T* out = reinterpret_cast<T*>(outC);
    forwardImpl(in, [out](std::size_t k, T r, T i) {
        out[2 * k] = r;
        out[2 * k + 1] = i;
    });
}

template <typename T>
void RealFft<T>::forwardSplit(const T* in, T* re, T* im) noexcept {
    forwardImpl(in, [re, im](std::size_t k, T r, T i) {
        re[k] = r;
        im[k] = i;
    });
}

template <typename T>
void RealFft<T>::inverse(const std::complex<T>* inC, T* out) noexcept {
    const T* in = reinterpret_cast<const T*>(inC);
    inverseImpl([in](std::size_t k, T& r, T& i) {
        r = in[2 * k];
        i = in[2 * k + 1];
    }, out);
}

template <typename T>
void RealFft<T>::inverseSplit(const T* re, const T* im, T* out) noexcept {
    inverseImpl([re, im](std::size_t k, T& r, T& i) {
        r = re[k];
        i = im[k];
    }, out);
}

template class RealFft<float>;
template class RealFft<double>;

}  // namespace bf
