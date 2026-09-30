#include "core/dsp/Fft.h"

#include <stdexcept>

#include "core/math/DetMath.h"

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
    work_.assign(2 * m_, T(0));
}

// In-place iterative radix-2 DIT on work_ (input already in bit-reversed order).
template <typename T>
void RealFft<T>::complexTransform(bool inverseDir) noexcept {
    T* w = work_.data();
    const T sgn = inverseDir ? T(-1) : T(1);
    for (std::size_t len = 2; len <= m_; len <<= 1) {
        const std::size_t half = len >> 1;
        const std::size_t step = n_ / len;  // W_m^{j m/len} = W_n^{j n/len}
        for (std::size_t i = 0; i < m_; i += len) {
            for (std::size_t j = 0; j < half; ++j) {
                const T wr = tw_[2 * j * step];
                const T wi = sgn * tw_[2 * j * step + 1];
                T* a = w + 2 * (i + j);
                T* b = w + 2 * (i + j + half);
                const T br = b[0] * wr - b[1] * wi;
                const T bi = b[0] * wi + b[1] * wr;
                b[0] = a[0] - br;
                b[1] = a[1] - bi;
                a[0] = a[0] + br;
                a[1] = a[1] + bi;
            }
        }
    }
}

template <typename T>
void RealFft<T>::forward(const T* in, std::complex<T>* outC) noexcept {
    T* w = work_.data();
    for (std::size_t k = 0; k < m_; ++k) {
        const std::size_t r = rev_[k];
        w[2 * r] = in[2 * k];
        w[2 * r + 1] = in[2 * k + 1];
    }
    complexTransform(false);
    T* out = reinterpret_cast<T*>(outC);
    const T half = T(0.5);
    // k = 0 and k = m
    const T z0r = w[0], z0i = w[1];
    for (std::size_t k = 1; k < m_; ++k) {
        const T zr = w[2 * k], zi = w[2 * k + 1];
        const T cr = w[2 * (m_ - k)], ci = -w[2 * (m_ - k) + 1];  // conj(Z[m-k])
        const T fer = half * (zr + cr), fei = half * (zi + ci);
        // Fo = -i (Z - Zc) / 2
        const T dr = zr - cr, di = zi - ci;
        const T for_ = half * di, foi = -half * dr;
        const T wr = tw_[2 * k], wi = tw_[2 * k + 1];
        out[2 * k] = fer + (for_ * wr - foi * wi);
        out[2 * k + 1] = fei + (for_ * wi + foi * wr);
    }
    out[0] = z0r + z0i;
    out[1] = T(0);
    out[2 * m_] = z0r - z0i;
    out[2 * m_ + 1] = T(0);
}

template <typename T>
void RealFft<T>::inverse(const std::complex<T>* inC, T* out) noexcept {
    const T* in = reinterpret_cast<const T*>(inC);
    T* w = work_.data();
    const T half = T(0.5);
    {
        const T x0 = in[0], xm = in[2 * m_];
        const std::size_t r = rev_[0];
        w[2 * r] = half * (x0 + xm);
        w[2 * r + 1] = half * (x0 - xm);
    }
    for (std::size_t k = 1; k < m_; ++k) {
        const T xr = in[2 * k], xi = in[2 * k + 1];
        const T cr = in[2 * (m_ - k)], ci = -in[2 * (m_ - k) + 1];
        const T fer = half * (xr + cr), fei = half * (xi + ci);
        const T dr = half * (xr - cr), di = half * (xi - ci);
        // Fo = d * conj(W^k)
        const T wr = tw_[2 * k], wi = -tw_[2 * k + 1];
        const T for_ = dr * wr - di * wi;
        const T foi = dr * wi + di * wr;
        // Z = Fe + i Fo
        const std::size_t r = rev_[k];
        w[2 * r] = fer - foi;
        w[2 * r + 1] = fei + for_;
    }
    complexTransform(true);
    const T scale = T(1) / static_cast<T>(m_);
    for (std::size_t k = 0; k < m_; ++k) {
        out[2 * k] = w[2 * k] * scale;
        out[2 * k + 1] = w[2 * k + 1] * scale;
    }
}

template class RealFft<float>;
template class RealFft<double>;

}  // namespace bf
