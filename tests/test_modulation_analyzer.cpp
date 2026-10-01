#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/analysis/ModulationAnalyzer.h"
#include "core/random/Random.h"

using namespace bf;

namespace {
constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// 10 ms frame powers (480 samples) of white noise whose power envelope is 1 + depth*cos(2 pi f t).
std::vector<double> noiseFramePowers(double seconds, double modHz, double depth, std::uint64_t seed) {
    RngStream rng(seed);
    const std::size_t frames = static_cast<std::size_t>(seconds * 100.0);
    std::vector<double> p(frames);
    std::size_t n = 0;
    for (std::size_t f = 0; f < frames; ++f) {
        double s = 0.0;
        for (int i = 0; i < 480; ++i, ++n) {
            const double env = 1.0 + depth * std::cos(2.0 * kPi * modHz * static_cast<double>(n) / kFs);
            const double x = std::sqrt(3.0) * static_cast<double>(rng.uniformPM1f()) * std::sqrt(env);
            s += x * x;
        }
        p[f] = s / 480.0;
    }
    return p;
}

double dbToPower(double db) { return std::pow(10.0, db / 10.0); }
}  // namespace

TEST_CASE("Modulation spectrum: 4 Hz 100 % power AM gives m ~ 1 in the 4 Hz band only", "[modulation]") {
    ModulationAnalyzer an;
    for (double p : noiseFramePowers(60.0, 4.0, 1.0, 1)) an.pushFrame(p);
    const auto s = an.snapshot();
    REQUIRE(s.haveModSpectrum);
    REQUIRE(s.modBlocks >= 4);
    const auto& fc = modulationBandCentresHz();
    std::size_t k4 = 0;
    for (std::size_t b = 0; b < kNumModBands; ++b) if (fc[b] == 4.0) k4 = b;
    REQUIRE(std::fabs(s.modBroadband[k4] - 1.0) < 0.1);
    for (std::size_t b = 0; b < kNumModBands; ++b)
        if (b != k4) REQUIRE(s.modBroadband[b] < 0.1);
}

TEST_CASE("Modulation spectrum: modulation index scales and other rates land in their band", "[modulation]") {
    ModulationAnalyzer an;
    for (double p : noiseFramePowers(60.0, 2.5, 0.5, 2)) an.pushFrame(p);
    const auto s = an.snapshot();
    const auto& fc = modulationBandCentresHz();
    for (std::size_t b = 0; b < kNumModBands; ++b) {
        if (fc[b] == 2.5) REQUIRE(std::fabs(s.modBroadband[b] - 0.5) < 0.06);
        else REQUIRE(s.modBroadband[b] < 0.1);
    }
}

TEST_CASE("Modulation spectrum: stationary noise sits at the statistical floor", "[modulation]") {
    ModulationAnalyzer an;
    for (double p : noiseFramePowers(60.0, 4.0, 0.0, 3)) an.pushFrame(p);
    const auto s = an.snapshot();
    REQUIRE(s.haveModSpectrum);
    for (double m : s.modBroadband) REQUIRE(m < 0.05);
    REQUIRE(s.modulationDepth10s < 0.1);
    REQUIRE(s.density == TemporalDensity::High);
    REQUIRE(s.gaps.count == 0);
}

TEST_CASE("Modulation spectrum: octave carriers are tracked separately", "[modulation]") {
    ModulationAnalyzer an(true);
    const auto mod = noiseFramePowers(60.0, 4.0, 1.0, 5);
    const auto flatp = noiseFramePowers(60.0, 4.0, 0.0, 6);
    for (std::size_t i = 0; i < mod.size(); ++i) {
        std::array<double, kNumModCarriers> oct{};
        oct.fill(flatp[i]);
        oct[3] = mod[i];
        an.pushFrame(mod[i], -1, -1, {}, oct);
    }
    const auto s = an.snapshot();
    REQUIRE(std::fabs(s.modOctave[3][9] - 1.0) < 0.1);
    REQUIRE(s.modOctave[1][9] < 0.05);
}

TEST_CASE("Gap detector: exact durations on synthetic on/off envelopes", "[modulation][gaps]") {
    ModulationAnalyzer an;
    const double on = 1.0, off = dbToPower(-50.0);
    struct Seg { bool isOn; int frames; };
    const Seg segs[] = {{true, 200}, {false, 30}, {true, 100}, {false, 7}, {true, 40},
                        {false, 120}, {true, 300}};
    for (const auto& sg : segs)
        for (int i = 0; i < sg.frames; ++i) an.pushFrame(sg.isOn ? on : off);
    const auto& g = an.allGaps();
    REQUIRE(g.size() == 3);
    REQUIRE(g[0].seconds == 0.30);
    REQUIRE(g[1].seconds == 0.07);
    REQUIRE(g[2].seconds == 1.20);
    const auto s = an.snapshot();
    REQUIRE(s.gaps.count == 3);
    REQUIRE(s.gaps.maxSec == 1.20);
    REQUIRE(std::fabs(s.gaps.medianSec - 0.30) < 1e-12);
    REQUIRE(std::fabs(s.gaps.p95Sec - (0.3 * 0.1 + 1.2 * 0.9)) < 1e-9);
    REQUIRE(std::fabs(s.gaps.ratePerSec - 3.0 / 7.97) < 1e-9);  // 797 frames < 60 s
}

TEST_CASE("Gap detector: threshold is 12 dB below the 10 s Leq; only last 60 s counted", "[modulation][gaps]") {
    ModulationAnalyzer an;
    // Frames at -6 dB are not gaps relative to Leq (0 dB); frames at -13 dB are.
    for (int i = 0; i < 500; ++i) an.pushFrame(1.0);
    for (int i = 0; i < 5; ++i) an.pushFrame(dbToPower(-6.0));
    for (int i = 0; i < 500; ++i) an.pushFrame(1.0);
    REQUIRE(an.allGaps().empty());
    for (int i = 0; i < 4; ++i) an.pushFrame(dbToPower(-13.0));
    for (int i = 0; i < 500; ++i) an.pushFrame(1.0);
    REQUIRE(an.allGaps().size() == 1);
    REQUIRE(an.allGaps()[0].seconds == 0.04);
    // 61 s later that gap no longer counts
    for (int i = 0; i < 6100; ++i) an.pushFrame(1.0);
    REQUIRE(an.snapshot().gaps.count == 0);
}

TEST_CASE("L10-L90 crest on known distributions and density class", "[modulation]") {
    SECTION("two-level: 85 % at 0 dB, 15 % at -20 dB -> 20 dB (Low)") {
        ModulationAnalyzer an;
        for (int i = 0; i < 6000; ++i) an.pushFrame((i * 7) % 20 < 3 ? dbToPower(-20.0) : 1.0);
        const auto s = an.snapshot();
        REQUIRE(std::fabs(s.crest60sDb - 20.0) < 1e-9);
        REQUIRE(s.density == TemporalDensity::Low);
    }
    SECTION("uniform in dB over 0..60 -> L10-L90 = 48 dB") {
        ModulationAnalyzer an;
        for (int i = 0; i < 6000; ++i) an.pushFrame(dbToPower(60.0 * ((i * 2711) % 6000) / 5999.0));
        REQUIRE(std::fabs(an.snapshot().crest60sDb - 48.0) < 0.05);
    }
    SECTION("uniform over 0..9 dB -> 7.2 dB (Medium), 0..6 dB -> 4.8 dB (High)") {
        ModulationAnalyzer a, b;
        for (int i = 0; i < 6000; ++i) {
            a.pushFrame(dbToPower(9.0 * ((i * 2711) % 6000) / 5999.0));
            b.pushFrame(dbToPower(6.0 * ((i * 2711) % 6000) / 5999.0));
        }
        REQUIRE(std::fabs(a.snapshot().crest60sDb - 7.2) < 0.02);
        REQUIRE(a.snapshot().density == TemporalDensity::Medium);
        REQUIRE(b.snapshot().density == TemporalDensity::High);
    }
    REQUIRE(ModulationAnalyzer::classify(12.5) == TemporalDensity::Low);
    REQUIRE(ModulationAnalyzer::classify(12.0) == TemporalDensity::Medium);
    REQUIRE(ModulationAnalyzer::classify(6.9) == TemporalDensity::High);
}

TEST_CASE("Occupancy, mean k_a/k_s and solo exposure", "[modulation][solo]") {
    ModulationAnalyzer an;
    for (int i = 0; i < 100; ++i) an.pushFrame(1.0, 2, 1);                    // k_s == 1 -> solo
    const double dom[] = {10.0, 1.0, 1.0};
    for (int i = 0; i < 100; ++i) an.pushFrame(1.0, 3, 3, dom);               // 7 dB dominance -> solo
    const double eq[] = {1.0, 1.0, 1.0};
    for (int i = 0; i < 100; ++i) an.pushFrame(1.0, 3, 3, eq);                // not solo
    for (int i = 0; i < 100; ++i) an.pushFrame(1.0, 0, 0);                    // silence
    const auto s = an.snapshot();
    REQUIRE(s.haveK);
    REQUIRE(std::fabs(s.occupancy60s - 0.75) < 1e-12);
    REQUIRE(std::fabs(s.meanKs60s - (100 + 300 + 300) / 400.0) < 1e-12);
    REQUIRE(std::fabs(s.meanKa60s - (200 + 300 + 300) / 400.0) < 1e-12);
    REQUIRE(std::fabs(s.soloExposurePct - 50.0) < 1e-12);

    // without per-talker levels only the k_s == 1 part is available
    ModulationAnalyzer b;
    for (int i = 0; i < 100; ++i) b.pushFrame(1.0, 2, 1);
    for (int i = 0; i < 100; ++i) b.pushFrame(1.0, 3, 3);
    REQUIRE(std::fabs(b.snapshot().soloExposurePct - 50.0) < 1e-12);
}

TEST_CASE("Leq10s and modulation depth", "[modulation]") {
    ModulationAnalyzer an;
    for (int i = 0; i < 1000; ++i) an.pushFrame(i % 2 ? 4.0 : 0.0 + 1e-30);
    const auto s = an.snapshot();
    REQUIRE(std::fabs(s.leq10sDb - 10.0 * std::log10(2.0)) < 1e-9);
    // 50 Hz on/off is removed by the 16 Hz low-pass
    REQUIRE(s.modulationDepth10s < 0.05);
    ModulationAnalyzer c;
    for (int i = 0; i < 1000; ++i) c.pushFrame(1.0 + std::cos(2.0 * kPi * 2.0 * i / 100.0));
    REQUIRE(std::fabs(c.snapshot().modulationDepth10s - 1.0 / std::sqrt(2.0)) < 0.05);
}

TEST_CASE("analyzeModulation offline API", "[modulation]") {
    const std::size_t n = static_cast<std::size_t>(30 * kFs);
    std::vector<float> x(n);
    RngStream rng(9);
    for (std::size_t i = 0; i < n; ++i) x[i] = 0.1f * rng.uniformPM1f();
    const float* ch[] = {x.data(), x.data()};
    const auto s = analyzeModulation(ch, 2, n, kFs);
    REQUIRE(s.frames == 3000);
    REQUIRE(s.haveModSpectrum);
    REQUIRE(s.density == TemporalDensity::High);
}
