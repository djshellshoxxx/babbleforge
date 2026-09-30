#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "core/analysis/SpectrumAnalyzer.h"
#include "core/config/DataSet.h"
#include "core/dsp/Fft.h"
#include "core/engine/StationaryMaskEngine.h"
#include "core/random/Distributions.h"
#include "core/spectrum/FirDesigner.h"
#include "core/spectrum/SpectrumTarget.h"

using namespace bf;

namespace {
constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// Pink noise with exact 1/f power density (deterministic magnitudes, random phases), 3 x 2^20.
std::vector<float> makePink(std::size_t frames, std::uint64_t seed) {
    const std::size_t N = 1u << 20;
    FftD fft(N);
    RngStream rng(seed);
    std::vector<float> out;
    std::vector<std::complex<double>> X(N / 2 + 1);
    std::vector<double> x(N);
    while (out.size() < frames) {
        X[0] = 0.0;
        for (std::size_t k = 1; k <= N / 2; ++k) {
            const double ph = 2.0 * kPi * rng.uniform01();
            X[k] = std::polar(1.0 / std::sqrt(static_cast<double>(k)), ph);
        }
        X[N / 2] = X[N / 2].real();
        fft.inverse(X.data(), x.data());
        double ss = 0.0;
        for (double v : x) ss += v * v;
        const double g = 0.1 / std::sqrt(ss / static_cast<double>(N));
        for (double v : x) out.push_back(static_cast<float>(v * g));
    }
    out.resize(frames);
    return out;
}

double maxAbsDevFrom(const ThirdOctArray& measuredDb, const ThirdOctArray& refDb) {
    OperatingBands d{};
    shapeDeviation(operatingSlice(measuredDb), operatingSlice(refDb), d);
    double m = 0.0;
    for (double v : d) m = std::max(m, std::fabs(v));
    return m;
}
}  // namespace

TEST_CASE("SpectrumAnalyzer: absolute calibration (sine, white noise, channel power sum)", "[spectrum][analyzer]") {
    const std::size_t n = static_cast<std::size_t>(20 * kFs);
    std::vector<float> a(n), b(n);
    RngStream rng(7);
    for (std::size_t i = 0; i < n; ++i) {
        a[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kFs));
        b[i] = static_cast<float>(std::sqrt(3.0) * rng.uniformPM1f() * 0.2);  // variance 0.04 / 3 * 3
    }
    SECTION("sine 0.5 at 1 kHz -> 0.125 mean-square in that band") {
        SpectrumAnalyzer an;
        an.prepare(kFs, 1);
        const float* ch[] = {a.data()};
        an.process(ch, n);
        const auto db = powerToDb(an.overallPower());
        REQUIRE(std::fabs(db[kBand1k] - 10.0 * std::log10(0.125)) < 0.05);
        // total power across bands
        double tot = 0;
        for (double p : an.overallPower()) tot += p;
        REQUIRE(std::fabs(10.0 * std::log10(tot / 0.125)) < 0.05);
    }
    SECTION("two channels are power-summed") {
        SpectrumAnalyzer an;
        an.prepare(kFs, 2);
        const float* ch[] = {a.data(), a.data()};
        an.process(ch, n);
        REQUIRE(std::fabs(powerToDb(an.overallPower()[kBand1k]) - 10.0 * std::log10(0.25)) < 0.05);
    }
    SECTION("white noise: band power proportional to bandwidth (fractional bin edges)") {
        SpectrumAnalyzer an;
        an.prepare(kFs, 1);
        const float* ch[] = {b.data()};
        an.process(ch, n);
        const auto p = an.overallPower();
        for (std::size_t band = 6; band <= 23; ++band) {
            const double bw = thirdOctUpperEdgeHz(band) - thirdOctLowerEdgeHz(band);
            const double expect = 0.04 * bw / (kFs / 2.0);
            REQUIRE(std::fabs(powerToDb(p[band]) - powerToDb(expect)) < 0.25);
        }
    }
}

TEST_CASE("SpectrumAnalyzer: FFT size scales with fs, blocks and long-term estimate", "[spectrum][analyzer]") {
    SpectrumAnalyzer an;
    an.prepare(48000.0, 1);
    REQUIRE(an.fftSize() == 8192);
    an.prepare(96000.0, 1);
    REQUIRE(an.fftSize() == 16384);
    an.prepare(44100.0, 1);
    REQUIRE(an.fftSize() == 8192);

    an.prepare(kFs, 1);
    const std::size_t n = static_cast<std::size_t>(23 * kFs);
    std::vector<float> x(n);
    RngStream rng(3);
    for (auto& v : x) v = 0.1f * rng.uniformPM1f();
    // feed in odd-sized chunks
    std::size_t pos = 0, chunk = 1;
    while (pos < n) {
        const std::size_t c = std::min(chunk, n - pos);
        const float* ch[] = {x.data() + pos};
        an.process(ch, c);
        pos += c;
        chunk = chunk * 3 + 1;
        if (chunk > 30000) chunk = 1;
    }
    auto blocks = an.takeBlocks();
    REQUIRE(blocks.size() == 4);  // 5,10,15,20 s
    REQUIRE(an.takeBlocks().empty());
    REQUIRE(an.hasLongTerm());
    REQUIRE(blocks[0].endSeconds > 4.9);
    REQUIRE(blocks[0].endSeconds < 5.2);
    // chunked processing equals one-shot processing
    SpectrumAnalyzer ref;
    ref.prepare(kFs, 1);
    const float* all[] = {x.data()};
    ref.process(all, n);
    for (std::size_t b = 0; b < kNumThirdOctBands; ++b)
        REQUIRE(std::fabs(an.overallPower()[b] - ref.overallPower()[b]) <= 1e-12 * ref.overallPower()[b] + 1e-30);
}

TEST_CASE("SpectrumAnalyzer: pink noise gives flat band levels within 0.3 dB (100 Hz-10 kHz)", "[spectrum][analyzer]") {
    const std::size_t n = static_cast<std::size_t>(60 * kFs);
    const auto pink = makePink(n, 11);
    const float* ch[] = {pink.data()};
    const auto r = analyzeSpectrum(ch, 1, n, kFs);
    ThirdOctArray flat{};
    flat.fill(0.0);
    REQUIRE(maxAbsDevFrom(r.overallDb, flat) < 0.3);
    // slope 0 dB/oct, long-term estimate available and consistent
    REQUIRE(std::fabs(leastSquaresSlopeDbPerOct(r.overallDb)) < 0.1);
    REQUIRE(r.hasLongTerm);
    REQUIRE(r.blocks.size() >= 11);
    REQUIRE(maxAbsDevFrom(r.longTermDb, flat) < 0.5);

    // Octave derivation: 3 equal bands -> +4.77 dB over each band
    for (std::size_t o = 1; o + 1 < kNumOctaveBands; ++o)
        REQUIRE(std::fabs(r.octaveDb[o] - r.overallDb[4 + 3 * o + 1] - 10.0 * std::log10(3.0)) < 0.3);

    // metrics against a flat target
    const auto m = computeShapeMetrics(r.overallPowerLin, flat);
    REQUIRE(m.rmsDev125to8k < 0.2);
    REQUIRE(m.maxAbsDev < 0.3);
    REQUIRE(m.lfFraction > 0.10);  // 4 equal bands of 26 (100..200) ~ 4/26 of the total
    REQUIRE(m.speechFraction > 0.5);
    REQUIRE(m.hfFraction > 0.15);
}

TEST_CASE("SpectrumAnalyzer: shape deviation removes level offsets and reports slope/metrics", "[spectrum][metrics]") {
    ThirdOctArray tgt{};
    for (std::size_t b = 0; b < kNumThirdOctBands; ++b)
        tgt[b] = -3.0 * std::log2(thirdOctCentreHz(b) / 1000.0);  // -3 dB/oct tilt
    SECTION("pure level offset gives d = 0") {
        ThirdOctArray p{};
        for (std::size_t b = 0; b < kNumThirdOctBands; ++b) p[b] = std::pow(10.0, (tgt[b] + 7.0) / 10.0);
        const auto m = computeShapeMetrics(p, tgt);
        REQUIRE(m.maxAbsDev < 1e-9);
        REQUIRE(std::fabs(m.mu - 7.0) < 0.5);  // power-weighted, so 7 exactly
        REQUIRE(std::fabs(m.slopeDbPerOct + 3.0) < 1e-9);
    }
    SECTION("a bump in one band is found") {
        ThirdOctArray p{};
        for (std::size_t b = 0; b < kNumThirdOctBands; ++b) p[b] = std::pow(10.0, tgt[b] / 10.0);
        p[kBand1k + 2] *= std::pow(10.0, 0.4);  // +4 dB at 1.6 kHz
        const auto m = computeShapeMetrics(p, tgt);
        REQUIRE(m.maxAbsBand == kBand1k + 2);
        REQUIRE(m.maxAbsDev > 3.0);
        REQUIRE(m.maxAbsDev < 4.0);
        REQUIRE(m.rmsDev125to8k > 0.5);
    }
}

TEST_CASE("SpectrumAnalyzer: stationary SSN rendered 60 s matches its analytic band levels", "[spectrum][analyzer][stationary]") {
    auto ds = loadDataSet(BF_DATA_DIR);
    REQUIRE(ds.ok);
    auto t = buildSpectrumTarget(ds.data.targets.at("ltass_universal"));
    REQUIRE(t.target);
    FirDesignParams p;
    p.fs = kFs;
    const auto design = designMinPhaseFir(t.target->effectiveThirdOctDb(), p);
    REQUIRE_FALSE(design.degraded);
    const auto analytic = firThirdOctResponseDb(design.h, kFs);

    StationaryMaskEngine eng;
    const std::size_t frames = static_cast<std::size_t>(60 * kFs);
    eng.prepare(kFs, 1, 0x5EEDULL, 8192);
    REQUIRE(eng.setFilter(design.tapsFloat()));
    std::vector<float> out(frames);
    for (std::size_t off = 0; off < frames; off += 512) {
        float* pp[] = {out.data() + off};
        eng.process(pp, std::min<std::size_t>(512, frames - off));
    }
    const float* ch[] = {out.data() + 8192};
    const auto r = analyzeSpectrum(ch, 1, frames - 8192, kFs, &analytic);
    REQUIRE(r.hasShape);
    REQUIRE(r.shape.maxAbsDev < 0.3);
    // and against the design target (roll-off applied) within the FIR design tolerance
    REQUIRE(maxAbsDevFrom(r.overallDb, design.referenceDb) < 0.8);
}
