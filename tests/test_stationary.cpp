#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

#include "core/config/DataSet.h"
#include "core/dsp/Fft.h"
#include "core/engine/StationaryMaskEngine.h"
#include "core/spectrum/FirDesigner.h"
#include "core/spectrum/SpectrumTarget.h"

using namespace bf;

namespace {
constexpr double kFs = 48000.0;
constexpr std::uint64_t kSeed = 0xB0BB1EF0F6E5ULL;

const FirDesignResult& ltassDesign() {
    static const FirDesignResult r = [] {
        auto ds = loadDataSet(BF_DATA_DIR);
        REQUIRE(ds.ok);
        auto t = buildSpectrumTarget(ds.data.targets.at("ltass_universal_byrne1994"));
        REQUIRE(t.target);
        FirDesignParams p;
        p.fs = kFs;
        return designMinPhaseFir(t.target->effectiveThirdOctDb(), p);
    }();
    return r;
}

std::vector<std::vector<float>> render(std::size_t channels, std::size_t frames, std::size_t block,
                                       std::uint64_t seed = kSeed) {
    StationaryMaskEngine eng;
    eng.prepare(kFs, channels, seed, 8192);
    REQUIRE(eng.setFilter(ltassDesign().tapsFloat()));
    std::vector<std::vector<float>> out(channels, std::vector<float>(frames));
    std::vector<float*> ptrs(channels);
    for (std::size_t off = 0; off < frames; off += block) {
        const std::size_t n = std::min(block, frames - off);
        for (std::size_t c = 0; c < channels; ++c) ptrs[c] = out[c].data() + off;
        eng.process(ptrs.data(), n);
    }
    return out;
}

// Welch PSD (Hann 8192, 50 % overlap) aggregated into IEC 1/3-octave bands (fractional bins).
ThirdOctArray welchThirdOct(const float* x, std::size_t n) {
    const std::size_t N = 8192;
    FftD fft(N);
    std::vector<double> win(N), buf(N), psd(N / 2 + 1, 0.0);
    for (std::size_t i = 0; i < N; ++i) win[i] = 0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * static_cast<double>(i) / static_cast<double>(N));
    std::vector<std::complex<double>> X(N / 2 + 1);
    for (std::size_t s = 0; s + N <= n; s += N / 2) {
        for (std::size_t i = 0; i < N; ++i) buf[i] = win[i] * x[s + i];
        fft.forward(buf.data(), X.data());
        for (std::size_t k = 0; k <= N / 2; ++k) psd[k] += std::norm(X[k]);
    }
    const double df = kFs / N;
    ThirdOctArray r{};
    for (std::size_t b = 0; b < kNumThirdOctBands; ++b) {
        const double lo = thirdOctLowerEdgeHz(b), hi = thirdOctUpperEdgeHz(b);
        double s = 0.0;
        for (auto k = static_cast<std::size_t>(lo / df); k <= static_cast<std::size_t>(hi / df) + 1 && k <= N / 2; ++k) {
            const double a = std::max(lo, (static_cast<double>(k) - 0.5) * df), e = std::min(hi, (static_cast<double>(k) + 0.5) * df);
            if (e > a) s += psd[k] * (e - a) / df;
        }
        r[b] = 10.0 * std::log10(s);
    }
    return r;
}
}  // namespace

TEST_CASE("Stationary masker: level, spectrum, independence and Gaussianity (60 s)", "[stationary]") {
    const auto& d = ltassDesign();
    REQUIRE_FALSE(d.degraded);
    const std::size_t frames = static_cast<std::size_t>(60 * kFs);
    const auto out = render(2, frames, 512);
    const std::size_t skip = 8192;  // latency + filter fill
    const std::size_t n = frames - skip;
    const float* y0 = out[0].data() + skip;
    const float* y1 = out[1].data() + skip;

    // RMS within +-0.1 dB of -26 dBFS (ENGINE.md §3.3).
    double p0 = 0.0, p1 = 0.0, m4 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        p0 += static_cast<double>(y0[i]) * y0[i];
        p1 += static_cast<double>(y1[i]) * y1[i];
    }
    const double rms0 = 10.0 * std::log10(p0 / n), rms1 = 10.0 * std::log10(p1 / n);
    INFO("RMS ch0 " << rms0 << " dBFS, ch1 " << rms1 << " dBFS");
    CHECK(std::fabs(rms0 + 26.0) < 0.1);
    CHECK(std::fabs(rms1 + 26.0) < 0.1);

    // Kurtosis (Gaussian = 3).
    const double var = p0 / n;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(y0[i]) * y0[i];
        m4 += v * v;
    }
    const double kurt = (m4 / n) / (var * var);
    INFO("kurtosis " << kurt);
    CHECK(std::fabs(kurt - 3.0) < 0.1);

    // Channel independence in 10 s windows (A-SPA-1).
    const std::size_t w = static_cast<std::size_t>(10 * kFs);
    for (std::size_t s = 0; s + w <= n; s += w) {
        double sxy = 0.0, sxx = 0.0, syy = 0.0;
        for (std::size_t i = s; i < s + w; ++i) {
            sxy += static_cast<double>(y0[i]) * y1[i];
            sxx += static_cast<double>(y0[i]) * y0[i];
            syy += static_cast<double>(y1[i]) * y1[i];
        }
        const double rho = sxy / std::sqrt(sxx * syy);
        INFO("window " << s / w << " rho " << rho);
        CHECK(std::fabs(rho) < 0.02);
    }

    // 1/3-octave spectrum within +-1 dB of the target shape, 100 Hz..10 kHz. The reference is
    // the target including the specified 80 Hz / 12.5 kHz roll-offs (FirDesigner §4.3 step 2).
    // A 60 s Welch average gives ~700 segments; even the 100 Hz band (~4 bins) has a
    // standard error of ~0.1 dB, so single-band comparison is not flaky (and the seed is fixed).
    const auto meas = welchThirdOct(y0, n);
    const double dev = maxShapeDeviationDb(meas, d.referenceDb);
    INFO("max 1/3-oct deviation " << dev << " dB");
    CHECK(dev < 1.0);
}

TEST_CASE("Stationary masker is bit-reproducible and block-size independent", "[stationary]") {
    const std::size_t frames = static_cast<std::size_t>(2 * kFs);
    const auto ref = render(2, frames, 512);
    const auto again = render(2, frames, 512);
    for (std::size_t c = 0; c < 2; ++c)
        CHECK(std::memcmp(ref[c].data(), again[c].data(), frames * sizeof(float)) == 0);
    for (std::size_t block : {1u, 37u, 256u, 480u, 4096u}) {
        const auto o = render(2, frames, block);
        INFO("block " << block);
        for (std::size_t c = 0; c < 2; ++c)
            CHECK(std::memcmp(ref[c].data(), o[c].data(), frames * sizeof(float)) == 0);
    }
    const auto other = render(2, frames, 512, kSeed + 1);
    CHECK(std::memcmp(ref[0].data(), other[0].data(), frames * sizeof(float)) != 0);
    CHECK(std::memcmp(ref[0].data(), ref[1].data(), frames * sizeof(float)) != 0);
}
