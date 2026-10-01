#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <vector>

#include "core/dsp/Fft.h"
#include "core/random/Random.h"

using namespace bf;

namespace {
template <typename T>
void checkAgainstDft(std::size_t n, double tol) {
    RealFft<T> fft(n);
    RngStream rng(1234 + n);
    std::vector<T> x(n);
    for (auto& v : x) v = static_cast<T>(rng.uniform01() * 2.0 - 1.0);
    std::vector<std::complex<T>> X(n / 2 + 1);
    fft.forward(x.data(), X.data());
    double maxErr = 0.0, maxMag = 0.0;
    const double pi2 = 6.283185307179586;
    for (std::size_t k = 0; k <= n / 2; ++k) {
        std::complex<double> ref(0.0, 0.0);
        for (std::size_t t = 0; t < n; ++t) {
            const double a = -pi2 * static_cast<double>((k * t) % n) / static_cast<double>(n);
            ref += static_cast<double>(x[t]) * std::complex<double>(std::cos(a), std::sin(a));
        }
        const std::complex<double> got(static_cast<double>(X[k].real()), static_cast<double>(X[k].imag()));
        maxErr = std::max(maxErr, std::abs(got - ref));
        maxMag = std::max(maxMag, std::abs(ref));
    }
    INFO("n=" << n << " maxErr=" << maxErr << " maxMag=" << maxMag);
    CHECK(maxErr / maxMag < tol);

    std::vector<T> y(n);
    fft.inverse(X.data(), y.data());
    double rt = 0.0;
    for (std::size_t t = 0; t < n; ++t) rt = std::max(rt, std::fabs(static_cast<double>(y[t] - x[t])));
    INFO("round trip err=" << rt);
    CHECK(rt < tol * 10);
}
}  // namespace

TEST_CASE("RealFft<double> matches naive DFT and round-trips", "[fft]") {
    for (std::size_t n : {4u, 8u, 16u, 64u, 512u, 2048u}) checkAgainstDft<double>(n, 1e-12);
}

TEST_CASE("RealFft<float> matches naive DFT and round-trips", "[fft]") {
    for (std::size_t n : {4u, 16u, 512u, 1024u}) checkAgainstDft<float>(n, 2e-6);
}

TEST_CASE("RealFft large sizes round-trip and Parseval", "[fft]") {
    const std::size_t n = 65536;
    FftD fft(n);
    RngStream rng(99);
    std::vector<double> x(n), y(n);
    for (auto& v : x) v = rng.uniform01() - 0.5;
    std::vector<std::complex<double>> X(n / 2 + 1);
    fft.forward(x.data(), X.data());
    double et = 0.0, ef = 0.0;
    for (double v : x) et += v * v;
    for (std::size_t k = 0; k <= n / 2; ++k) ef += ((k == 0 || k == n / 2) ? 1.0 : 2.0) * std::norm(X[k]);
    CHECK(std::fabs(ef / static_cast<double>(n) - et) / et < 1e-12);
    fft.inverse(X.data(), y.data());
    double err = 0.0;
    for (std::size_t i = 0; i < n; ++i) err = std::max(err, std::fabs(x[i] - y[i]));
    CHECK(err < 1e-13);
}

TEST_CASE("RealFft rejects invalid sizes", "[fft]") {
    CHECK_FALSE(FftD::isValidSize(0));
    CHECK_FALSE(FftD::isValidSize(3));
    CHECK_FALSE(FftD::isValidSize(1000));
    CHECK(FftD::isValidSize(65536));
    CHECK_THROWS(FftD(100));
}

TEST_CASE("RealFft planar forwardSplit/inverseSplit are bit-identical to forward/inverse", "[fft]") {
    for (std::size_t n : {4u, 8u, 512u, 1024u}) {
        FftF fft(n);
        RngStream rng(99 + n);
        std::vector<float> x(n), y1(n), y2(n), re(n / 2 + 1), im(n / 2 + 1);
        for (auto& v : x) v = static_cast<float>(rng.uniform01() * 2.0 - 1.0);
        std::vector<std::complex<float>> X(n / 2 + 1);
        fft.forward(x.data(), X.data());
        fft.forwardSplit(x.data(), re.data(), im.data());
        bool same = true;
        for (std::size_t k = 0; k <= n / 2; ++k) same = same && X[k].real() == re[k] && X[k].imag() == im[k];
        CHECK(same);
        fft.inverse(X.data(), y1.data());
        fft.inverseSplit(re.data(), im.data(), y2.data());
        CHECK(y1 == y2);
    }
}
