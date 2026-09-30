#pragma once
// Deterministic radix-2 real FFT (docs/SPECTRUM_ENGINE.md §4.2-4.4,
// docs/REALTIME_ARCHITECTURE.md §1.2 "fixed-size FFTs with plans built at prepare").
//
// - Power-of-two sizes 4 ... 2^20 (the engine uses up to 65536).
// - The plan (twiddles from detsincos, bit-reversal table, work buffer) is built in the
//   constructor. forward()/inverse() never allocate and are noexcept (RT-safe).
// - No platform FFT library is used; results depend only on IEEE arithmetic, so they are
//   reproducible for a given build (D2) and within rounding across platforms (D3).
// - Instances are NOT thread-safe (they own a work buffer); use one per thread.
//
// Conventions:
//   forward: X[k] = sum_n x[n] e^{-2 pi i k n / N}, k = 0..N/2 (unnormalised, N/2+1 bins)
//   inverse: x[n] = (1/N) sum_k X[k] e^{+2 pi i k n / N} (Hermitian symmetry implied);
//            the imaginary parts of the DC and Nyquist bins are ignored.
//   inverse(forward(x)) == x (up to rounding).
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bf {

template <typename T>
class RealFft {
public:
    // Throws std::invalid_argument if n is not a power of two in [4, 2^20].
    explicit RealFft(std::size_t n);

    std::size_t size() const noexcept { return n_; }
    std::size_t numBins() const noexcept { return n_ / 2 + 1; }

    // in: n real samples; out: n/2+1 complex bins. in and out must not alias.
    void forward(const T* in, std::complex<T>* out) noexcept;
    // in: n/2+1 complex bins; out: n real samples. in and out must not alias.
    void inverse(const std::complex<T>* in, T* out) noexcept;

    static bool isValidSize(std::size_t n) noexcept;

private:
    void complexTransform(bool inverseDir) noexcept;

    std::size_t n_;   // real size
    std::size_t m_;   // complex size n/2
    std::vector<T> tw_;              // interleaved cos,sin of -2 pi k / n, k < m
    std::vector<std::uint32_t> rev_; // bit reversal of log2(m) bits
    std::vector<T> work_;            // interleaved complex, m entries
};

using FftF = RealFft<float>;
using FftD = RealFft<double>;

extern template class RealFft<float>;
extern template class RealFft<double>;

}  // namespace bf
