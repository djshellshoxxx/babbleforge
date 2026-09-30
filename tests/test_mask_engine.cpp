// Integrated MaskEngine acceptance tests (docs/ENGINE.md §12, short offline forms).
// Long forms (5 min / 1 h) are tagged [.long] and run only with BF_LONG_TESTS=1 (ctest -L long).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/analysis/OfflineAnalysis.h"
#include "core/analysis/SpectrumAnalyzer.h"
#include "core/dsp/TruePeakDetector.h"
#include "core/engine/MaskEngine.h"
#include "core/spectrum/FirDesigner.h"
#include "mask_engine_fixtures.h"

using namespace bf;
using bftest::rmsDb;

namespace {

constexpr std::size_t kFs = 48000;

double pearson(const float* a, const float* b, std::size_t n) {
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    for (std::size_t i = 0; i < n; ++i) {
        sa += a[i];
        sb += b[i];
        saa += static_cast<double>(a[i]) * a[i];
        sbb += static_cast<double>(b[i]) * b[i];
        sab += static_cast<double>(a[i]) * b[i];
    }
    const double dn = static_cast<double>(n);
    return (sab - sa * sb / dn) / std::sqrt((saa - sa * sa / dn) * (sbb - sb * sb / dn));
}

// Max |shape deviation| over 100 Hz..10 kHz (or 125..8k) of measured band powers vs reference dB.
double maxShapeDev(const ThirdOctArray& powerLin, const ThirdOctArray& refDb, bool range125to8k) {
    OperatingBands d{};
    shapeDeviation(operatingSlice(powerToDb(powerLin)), operatingSlice(refDb), d, nullptr);
    double m = 0.0;
    const std::size_t a = range125to8k ? 1 : 0, b = range125to8k ? kNumOperatingBands - 1 : kNumOperatingBands;
    for (std::size_t i = a; i < b; ++i) m = std::max(m, std::fabs(d[i]));
    return m;
}

SpectrumAnalysis spectrumOf(const std::vector<std::vector<float>>& x, std::size_t a, std::size_t b) {
    std::vector<const float*> p;
    for (const auto& ch : x) p.push_back(ch.data() + a);
    return analyzeSpectrum(p.data(), p.size(), b - a, 48000.0);
}

double truePeakDb(const std::vector<std::vector<float>>& x) {
    TruePeakDetector d;
    d.prepare(48000.0, static_cast<int>(x.size()));
    float m = 0.0f;
    std::vector<float> pk(4096);
    for (std::size_t c = 0; c < x.size(); ++c)
        for (std::size_t off = 0; off < x[c].size(); off += 4096) {
            const int n = static_cast<int>(std::min<std::size_t>(4096, x[c].size() - off));
            d.process(static_cast<int>(c), x[c].data() + off, pk.data(), n);
            for (int i = 0; i < n; ++i) m = std::max(m, pk[static_cast<std::size_t>(i)]);
        }
    return 20.0 * std::log10(std::max(m, 1e-12f));
}

bool longTestsEnabled() { return std::getenv("BF_LONG_TESTS") != nullptr; }

}  // namespace

// ------------------------------------------------------------------ stationary (no corpus)

TEST_CASE("A-LV-1 / A-SP-1 / A-SPA-1: stationary level + spectrum and channel independence", "[maskengine][stationary]") {
    // Speech Noise strategy, ring4, Strength +3 dB, 60 s, no corpus at all.
    auto doc = bftest::scenarioDoc("office", "speech_noise", "ring4", 60.0, 4242, {{"strengthDb", 3.0}});
    const RenderResult r = bftest::render(doc, {});
    REQUIRE(r.numChannels == 4);
    const std::size_t n = r.audio[0].size();
    REQUIRE(n == 60 * kFs);
    const double target = -26.0 + 3.0;
    double worst = 0.0;
    for (const auto& ch : r.audio) worst = std::max(worst, std::fabs(rmsDb(ch, 0, n) - target));
    std::printf("[A-LV-1] stationary 60 s RMS per channel: max |error| %.3f dB (all %.3f dBFS)\n", worst,
                rmsDb(r.audio, 0, n));
    CHECK(worst <= 0.1);

    REQUIRE(r.referenceDb);
    const SpectrumAnalysis sa = spectrumOf(r.audio, 0, n);
    const double dev = maxShapeDev(sa.overallPowerLin, *r.referenceDb, false);
    std::printf("[A-SP-1] stationary 1/3-oct 100 Hz-10 kHz max |shape deviation| %.3f dB\n", dev);
    CHECK(dev <= 1.0);
    // A-SP-3 (stationary part): octave bands 125..8k within +-1 dB, LF limit 80 Hz.
    CHECK(r.stats.stationaryOctaveMaxDevDb <= 1.0);

    // |rho| of two independent LTASS-shaped noises over 10 s is itself a random variable with a
    // standard deviation of ~0.01 (most power lies below 600 Hz, i.e. ~1e4 independent samples),
    // so "|rho| < 0.02 in every 10 s window" is a ~2-sigma statistic that truly independent
    // channels fail in ~5 % of windows. Checked here: whole-run |rho| < 0.02 (60 s), median of
    // the 10 s windows < 0.02 and max < 0.04 (4 sigma); all values are reported.
    std::vector<double> rhos;
    double rhoRun = 0.0;
    const std::size_t w = 10 * kFs;
    for (std::size_t a = 0; a < 4; ++a)
        for (std::size_t b = a + 1; b < 4; ++b) {
            rhoRun = std::max(rhoRun, std::fabs(pearson(r.audio[a].data(), r.audio[b].data(), n)));
            for (std::size_t off = 0; off + w <= n; off += w)
                rhos.push_back(std::fabs(pearson(r.audio[a].data() + off, r.audio[b].data() + off, w)));
        }
    std::sort(rhos.begin(), rhos.end());
    const double med = rhos[rhos.size() / 2], p95 = rhos[rhos.size() * 95 / 100], rmax = rhos.back();
    std::printf("[A-SPA-1] stationary |rho|, 6 pairs x 6 windows of 10 s: median %.4f p95 %.4f max %.4f; 60 s max %.4f\n",
                med, p95, rmax, rhoRun);
    CHECK(rhoRun < 0.02);
    CHECK(med < 0.02);
    CHECK(rmax < 0.04);
    CHECK_FALSE(r.stats.babbleActive);
    CHECK(r.stats.clipEvents == 0);
}

// ------------------------------------------------------------------ babble level

TEST_CASE("A-LV-2 / A-TM-1 / A-SP-2 (short): babble RMS + mean active talkers + babble spectrum", "[maskengine][babble]") {
    auto& src = bftest::shapedCorpus();
    // Office / Balanced with the mix locked to b = 1 (babble only), stereo, 60 s.
    auto doc = bftest::scenarioDoc("office", "balanced", "stereo", 60.0, 7, {{"mix", {{"babbleFraction", 1.0}}}});
    const RenderResult r = bftest::render(doc, bftest::handle(src));
    const std::size_t n = r.audio[0].size();
    const double l60 = rmsDb(r.audio, 0, n);
    std::size_t inside = 0, windows = 0;
    const std::size_t w = 3 * kFs;
    for (std::size_t off = 0; off + w <= n; off += w, ++windows)
        if (std::fabs(rmsDb(r.audio, off, off + w) + 26.0) <= 3.0) ++inside;
    const double frac = static_cast<double>(inside) / static_cast<double>(windows);
    std::printf("[A-LV-2] babble 60 s RMS %.3f dBFS (target -26), %.1f %% of 3 s windows within +-3 dB, trim %.2f dB\n",
                l60, 100.0 * frac, r.stats.babbleTrimDb);
    CHECK(std::fabs(l60 + 26.0) <= 0.5);
    CHECK(frac >= 0.95);

    const MaskRenderPlan* p = nullptr;
    (void)p;
    const double m = r.planHistory[0]["meanActive"].get<double>();
    std::printf("[A-TM-1] mean k_a %.3f (configured %.3f, %+.1f %%), range [%d, %d]\n", r.stats.meanActive, m,
                100.0 * (r.stats.meanActive / m - 1.0), r.stats.minActive, r.stats.maxActive);
    CHECK(std::fabs(r.stats.meanActive / m - 1.0) <= 0.05);
    CHECK(r.stats.minActive >= 4);
    CHECK(r.stats.maxActive <= 9);
    CHECK(r.stats.underflows == 0);
    CHECK_FALSE(r.stats.eqClamped);
    // Babble spectrum (60 s long-term estimate of T1 vs the target incl. LF/HF limits).
    std::printf("[A-SP-2 short] babble 1/3-oct 125-8k max dev %.2f dB, octave max dev %.2f dB\n",
                r.stats.babbleThirdOctMaxDevDb, r.stats.babbleOctaveMaxDevDb);
    CHECK(r.stats.haveBabbleSpectrum);
    CHECK(r.stats.babbleThirdOctMaxDevDb <= 2.0);
    CHECK(r.stats.babbleOctaveMaxDevDb <= 1.0);
}

TEST_CASE("A-LV-3: hybrid mix constancy and measured babble fraction", "[maskengine][hybrid]") {
    auto& src = bftest::shapedCorpus();
    std::vector<double> levels;
    for (double b : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        auto doc = bftest::scenarioDoc("office", "hybrid", "mono", 60.0, 11, {{"mix", {{"babbleFraction", b}}}});
        const RenderResult r = bftest::render(doc, bftest::handle(src));
        const double l = rmsDb(r.audio, 0, r.audio[0].size());
        levels.push_back(l);
        std::printf("[A-LV-3] b = %.2f: 60 s RMS %.3f dBFS, measured babble fraction %.3f\n", b, l,
                    r.stats.measuredBabbleFraction);
        CHECK(std::fabs(r.stats.measuredBabbleFraction - b) <= 0.03);
    }
    const auto [lo, hi] = std::minmax_element(levels.begin(), levels.end());
    std::printf("[A-LV-3] spread of the five 60 s RMS values %.3f dB\n", *hi - *lo);
    CHECK(*hi - *lo <= 1.0);  // all within +-0.5 dB of each other
}

TEST_CASE("A-LV-4: strategy switches keep the level (10 s before/after + 50 ms crossfade windows)", "[maskengine][switch]") {
    auto& src = bftest::shapedCorpus();
    auto doc = bftest::scenarioDoc("office", "balanced", "stereo", 80.0, 21);
    doc["events"] = {{{"atS", 20.0}, {"set", {{"strategy", "speech_noise"}}}},
                     {{"atS", 40.0}, {"set", {{"strategy", "dense"}}}},
                     {{"atS", 60.0}, {"set", {{"strategy", "natural"}}}}};
    const RenderResult r = bftest::render(doc, bftest::handle(src));
    REQUIRE(r.planHistory.size() == 4);
    const std::size_t w50 = kFs / 20;
    for (int k = 1; k <= 3; ++k) {
        const std::size_t s = static_cast<std::size_t>(20 * k) * kFs;
        const double before = rmsDb(r.audio, s - 10 * kFs, s), after = rmsDb(r.audio, s + 2 * kFs, s + 12 * kFs);
        // Steady-state 50 ms windows around the switch vs the windows inside the 2 s crossfade.
        std::vector<double> steady, xf;
        for (std::size_t off = s - 10 * kFs; off + w50 <= s; off += w50) steady.push_back(rmsDb(r.audio, off, off + w50));
        for (std::size_t off = s + 2 * kFs; off + w50 <= s + 12 * kFs; off += w50)
            steady.push_back(rmsDb(r.audio, off, off + w50));
        for (std::size_t off = s; off + w50 <= s + 2 * kFs; off += w50) xf.push_back(rmsDb(r.audio, off, off + w50));
        std::sort(steady.begin(), steady.end());
        const double p5 = steady[steady.size() / 20], p95 = steady[steady.size() * 19 / 20];
        const auto [xlo, xhi] = std::minmax_element(xf.begin(), xf.end());
        std::printf("[A-LV-4] %s -> %s: 10 s RMS %.2f -> %.2f dB (diff %+.2f); crossfade 50 ms windows [%.2f, %.2f] "
                    "vs steady p5/p95 [%.2f, %.2f]\n",
                    r.planHistory[k - 1]["strategy"].get<std::string>().c_str(),
                    r.planHistory[k]["strategy"].get<std::string>().c_str(), before, after, after - before, *xlo,
                    *xhi, p5, p95);
        CHECK(std::fabs(after - before) <= 1.0);
        CHECK(*xhi <= p95 + 2.0);
        CHECK(*xlo >= p5 - 3.0);
    }
}

TEST_CASE("A-LV-5: Character sweep 0 -> 1 over 60 s keeps every 10 s window within +-1 dB", "[maskengine][character]") {
    auto& src = bftest::shapedCorpus();
    auto doc = bftest::scenarioDoc("office", "balanced", "stereo", 60.0, 31, {{"character", 0.0}});
    nlohmann::json ev = nlohmann::json::array();
    for (int k = 1; k <= 12; ++k) ev.push_back({{"atS", 5.0 * k}, {"set", {{"macros.character", k / 12.0}}}});
    doc["events"] = ev;
    const RenderResult r = bftest::render(doc, bftest::handle(src));
    double worst = 0.0;
    for (std::size_t k = 0; k < 6; ++k) {
        const double l = rmsDb(r.audio, k * 10 * kFs, (k + 1) * 10 * kFs);
        worst = std::max(worst, std::fabs(l + 26.0));
        std::printf("[A-LV-5] window %zu: %.3f dBFS\n", k, l);
    }
    CHECK(worst <= 1.0);
}

// ------------------------------------------------------------------ limiter

TEST_CASE("A-LV-6 (short): limiter transparent for factory strategies at Strength +6 dB", "[maskengine][limiter]") {
    auto& src = bftest::shapedCorpus();
    for (const char* strat : {"balanced", "dense", "natural", "speech_noise", "multi_voice"}) {
        auto doc = bftest::scenarioDoc("office", strat, "stereo", 20.0, 41, {{"strengthDb", 6.0}});
        const RenderResult r = bftest::render(doc, bftest::handle(src));
        std::printf("[A-LV-6] %-12s +6 dB: GR>0 %.4f %% of samples, GR max %.2f dB, TP %.2f dBTP, clips %llu\n", strat,
                    100.0 * r.stats.limiterActiveFraction, r.stats.limiterGrMaxDb, r.stats.truePeakMaxDb,
                    static_cast<unsigned long long>(r.stats.clipEvents));
        CHECK(r.stats.limiterEnabled);
        CHECK(r.stats.limiterActiveFraction <= 0.001);
        CHECK(r.stats.clipEvents == 0);
    }
}

TEST_CASE("A-LV-7: output true peak never exceeds the ceiling + 0.2 dB (Strength +12 dB)", "[maskengine][limiter]") {
    auto& src = bftest::shapedCorpus();
    for (const char* strat : {"speech_noise", "dense", "multi_voice"}) {
        auto doc = bftest::scenarioDoc("office", strat, "stereo", 10.0, 51, {{"strengthDb", 12.0}});
        const RenderResult r = bftest::render(doc, bftest::handle(src));
        const double tp = truePeakDb(r.audio);
        std::printf("[A-LV-7] %-12s +12 dB: true peak %.3f dBTP (ceiling -1), GR max %.2f dB, clips %llu\n", strat, tp,
                    r.stats.limiterGrMaxDb, static_cast<unsigned long long>(r.stats.clipEvents));
        CHECK(tp <= -1.0 + 0.2);
        CHECK(r.stats.limiterGrMaxDb > 0.0);
    }
}

// ------------------------------------------------------------------ spatial

TEST_CASE("A-SPA-3 (short): per-channel babble energy balance on a ring of 4", "[maskengine][spatial]") {
    auto& src = bftest::shapedCorpus();
    auto doc = bftest::scenarioDoc("open_office", "dense", "ring4", 60.0, 61, {{"mix", {{"babbleFraction", 1.0}}}});
    const RenderResult r = bftest::render(doc, bftest::handle(src));
    std::vector<double> l;
    for (const auto& ch : r.audio) l.push_back(rmsDb(ch, 0, ch.size()));
    const auto [lo, hi] = std::minmax_element(l.begin(), l.end());
    std::printf("[A-SPA-3] ring4 babble 60 s per-channel RMS [%.2f %.2f %.2f %.2f] dBFS, spread %.2f dB\n", l[0], l[1],
                l[2], l[3], *hi - *lo);
    CHECK(*hi - *lo <= 2.0);  // every channel within +-1 dB of the others' centre
    for (double v : l) CHECK(std::fabs(v - 0.5 * (*hi + *lo)) <= 1.0);
}

// ------------------------------------------------------------------ laboratory

TEST_CASE("A-TM-3: laboratory continuousN 1/2/4/8/16 - equal RMS + equal LTASS + decreasing envelope", "[maskengine][lab]") {
    auto& src = bftest::shapedCorpus();
    std::vector<double> rms, env;
    std::vector<ThirdOctArray> spec;
    std::vector<std::vector<std::int64_t>> sets;
    for (int N : {1, 2, 4, 8, 16}) {
        auto doc = bftest::scenarioDoc("office", "research", "mono", 20.0, 1001);
        doc["laboratory"] = {{"mode", "continuousN"}, {"talkers", N}, {"maxGapMs", 100},
                             {"rmsNormalization", "two-pass"}, {"spectrumMatch", "none"}};
        const RenderResult r = bftest::render(doc, bftest::handle(src));
        CHECK_FALSE(r.stats.limiterEnabled);
        const std::size_t n = r.audio[0].size();
        rms.push_back(rmsDb(r.audio, 0, n));
        const nlohmann::json a = analyzeAudio(std::vector<const float*>{r.audio[0].data()}.data(), 1, n, 48000.0);
        env.push_back(a["temporal"]["envelope"]["L10minusL90Db"].get<double>());
        spec.push_back(spectrumOf(r.audio, 0, n).overallPowerLin);
        sets.push_back(r.laboratory["speakers"].get<std::vector<std::int64_t>>());
        std::printf("[A-TM-3] N=%2d: RMS %.4f dBFS (two-pass gain %+.2f dB), L10-L90 %.2f dB, speakers %zu\n", N,
                    rms.back(), r.laboratory["twoPass"]["staticGainDb"].get<double>(), env.back(), sets.back().size());
        CHECK(sets.back().size() == static_cast<std::size_t>(N));
    }
    for (double v : rms) CHECK(std::fabs(v - rms[0]) <= 0.2);
    for (double v : rms) CHECK(std::fabs(v + 26.0) <= 0.01);  // two-pass: L_ref within +-0.01 dB
    for (std::size_t k = 1; k < env.size(); ++k) CHECK(env[k] < env[k - 1]);
    // Nested speaker sets: the set for N is a prefix of the set for the next larger N.
    for (std::size_t k = 1; k < sets.size(); ++k)
        for (std::size_t i = 0; i < sets[k - 1].size(); ++i) CHECK(sets[k][i] == sets[k - 1][i]);
    // Equal LTASS: each fixture vs the 16-talker fixture, shape-normalised, 125 Hz..8 kHz.
    for (std::size_t k = 0; k + 1 < spec.size(); ++k) {
        const double d = maxShapeDev(spec[k], powerToDb(spec.back()), true);
        std::printf("[A-TM-3] LTASS N-fixture %zu vs N=16: max |1/3-oct deviation| %.2f dB\n", k, d);
        CHECK(d <= 1.0);
    }
}

TEST_CASE("Laboratory: Strict spectrum pre-roll converges and freezes the babble correction", "[maskengine][lab]") {
    auto& src = bftest::shapedCorpus();
    auto doc = bftest::scenarioDoc("office", "research", "mono", 10.0, 1003);
    doc["laboratory"] = {{"mode", "ltassMatchedBabble"}, {"talkers", 4}, {"rmsNormalization", "two-pass"}};
    const RenderResult r = bftest::render(doc, bftest::handle(src));
    REQUIRE(r.laboratory.contains("strictSpectrum"));
    const auto c = r.laboratory["strictSpectrum"]["correctionDb"].get<std::vector<double>>();
    double cmax = 0.0;
    for (double v : c) cmax = std::max(cmax, std::fabs(v));
    std::printf("[lab strict] pre-roll babble 1/3-oct max dev %.2f dB, |C| max %.2f dB; render 1/3-oct dev %.2f dB\n",
                r.laboratory["strictSpectrum"]["prerollBabbleThirdOctMaxDevDb"].get<double>(), cmax,
                r.stats.babbleThirdOctMaxDevDb);
    CHECK(cmax <= 4.0);  // within the healthy range
    CHECK(r.stats.babbleThirdOctMaxDevDb <= 2.0);
    // Frozen: the render used exactly the pre-roll correction.
    for (std::size_t i = 0; i < c.size(); ++i) CHECK(r.stats.correctionDb[i] == c[i]);
}

TEST_CASE("Laboratory: SSN and pink sub-modes + Strict fallback aborts on source failure", "[maskengine][lab]") {
    SECTION("ssn / pink need no corpus, limiter off, exact level") {
        for (const char* mode : {"ssn", "pink"}) {
            auto doc = bftest::scenarioDoc("office", "research", "stereo", 10.0, 5);
            doc["laboratory"] = {{"mode", mode}};
            const RenderResult r = bftest::render(doc, {});
            CHECK(std::fabs(rmsDb(r.audio, 0, r.audio[0].size()) + 26.0) <= 0.01);
            CHECK_FALSE(r.stats.limiterEnabled);
            if (std::string(mode) == "pink") CHECK(r.targetId == "pink");
        }
    }
    SECTION("hybrid sub-mode: explicit b, stochastic talkers, two-pass level") {
        auto doc = bftest::scenarioDoc("office", "research", "mono", 20.0, 6);
        doc["laboratory"] = {{"mode", "hybrid"}, {"babbleFraction", 0.25}};
        const RenderResult r = bftest::render(doc, bftest::handle(bftest::shapedCorpus()));
        std::printf("[lab hybrid] b = 0.25: measured babble fraction %.3f, RMS %.4f dBFS\n",
                    r.stats.measuredBabbleFraction, rmsDb(r.audio, 0, r.audio[0].size()));
        CHECK(std::fabs(r.stats.measuredBabbleFraction - 0.25) <= 0.05);
        CHECK(std::fabs(rmsDb(r.audio, 0, r.audio[0].size()) + 26.0) <= 0.01);
        CHECK(r.planHistory[0]["strategyClass"] == "LaboratoryMask");
    }
    SECTION("continuousN with a failing recording aborts with exit code 3") {
        bftest::ShapedWhiteSource src(8, 30.0, 1);
        for (RecordingId rec = 0; rec < 8; ++rec) src.inner().setFailing(rec, true);
        auto doc = bftest::scenarioDoc("office", "research", "mono", 5.0, 9);
        doc["laboratory"] = {{"mode", "continuousN"}, {"talkers", 2}};
        const RenderResult r = renderScenario(bftest::dataSet(), bftest::scenario(doc), bftest::handle(src), {});
        CHECK_FALSE(r.ok);
        CHECK(r.exitCode == kRenderSourceFailure);
    }
    SECTION("continuousN with more talkers than speakers aborts (Strict)") {
        bftest::ShapedWhiteSource src(3, 30.0, 1);
        auto doc = bftest::scenarioDoc("office", "research", "mono", 5.0, 9);
        doc["laboratory"] = {{"mode", "continuousN"}, {"talkers", 4}};
        const RenderResult r = renderScenario(bftest::dataSet(), bftest::scenario(doc), bftest::handle(src), {});
        CHECK_FALSE(r.ok);
        CHECK(r.exitCode == kRenderSourceFailure);
    }
}

// ------------------------------------------------------------------ determinism

TEST_CASE("A-DT-1: bit-identical output across block sizes 64/256/480/1024 and repeated runs", "[maskengine][determinism]") {
    auto& src = bftest::shapedCorpus();
    auto doc = bftest::scenarioDoc("office", "balanced", "stereo", 8.0, 77);
    doc["events"] = {{{"atS", 4.321}, {"set", {{"macros.character", 0.9}}}},
                     {{"atS", 6.5}, {"set", {{"strategy", "hybrid"}, {"macros.strengthDb", -3.0}}}}};
    std::string ref, refEvents, refT1;
    int run = 0;
    for (int block : {480, 64, 256, 1024, 480}) {
        RenderOptions opt;
        opt.blockSize = block;
        opt.taps = {0};
        const RenderResult r = bftest::render(doc, bftest::handle(src), opt);
        const std::string h = audioSha256(r.audio), ht = audioSha256(r.taps.at(0));
        const std::string ev = r.events.dump();
        if (run++ == 0) {
            ref = h;
            refEvents = ev;
            refT1 = ht;
            std::printf("[A-DT-1] reference SHA-256 %s\n", h.c_str());
        }
        INFO("block " << block);
        CHECK(h == ref);
        CHECK(ht == refT1);
        CHECK(ev == refEvents);
    }
    // A different seed gives different audio.
    doc["seed"] = 78;
    CHECK(audioSha256(bftest::render(doc, bftest::handle(src)).audio) != ref);
}

TEST_CASE("MaskEngine: plan changes at stamped samples + statistics and taps", "[maskengine]") {
    auto& src = bftest::shapedCorpus();
    const DataSet& ds = bftest::dataSet();
    const CorpusSummary cs = CorpusSummary::from(*src.snapshot());
    auto doc = bftest::scenarioDoc("office", "balanced", "stereo", 1.0, 3);
    const ScenarioPlan a = buildScenarioPlan(ds, doc["preset"], std::nullopt, cs, 3);
    REQUIRE(a.ok);
    MaskEngineConfig cfg;
    cfg.seed = 3;
    cfg.corpus = src.snapshot();
    cfg.audio = &src;
    MaskEngine eng(cfg);
    REQUIRE(eng.prepare(48000.0, a.layout, 512));
    REQUIRE(eng.setPlan(a.plan, 0));
    MaskRenderPlan b = a.plan;
    b.level.strengthDb = -10.0;
    REQUIRE(eng.setPlan(b, 96000));
    std::vector<std::vector<float>> out(2, std::vector<float>(192000));
    std::vector<float*> p = {out[0].data(), out[1].data()};
    std::vector<float*> q(2);
    for (std::size_t off = 0; off < 192000; off += 1000) {
        q[0] = p[0] + off;
        q[1] = p[1] + off;
        eng.process(q.data(), 1000);
    }
    const MaskStatistics s = eng.statistics();
    CHECK(s.samples == 192000);
    CHECK(s.planChanges == 1);
    CHECK(eng.planHistoryJson()[1]["sample"].get<std::int64_t>() == 96000);
    // Strength -10 dB (tau 200 ms): the last 0.5 s is ~10 dB below the level before the change.
    const double before = rmsDb(out, 60000, 96000), after = rmsDb(out, 168000, 192000);
    CHECK(after - before < -7.0);
    CHECK(s.babbleActive);
    CHECK(s.meanActive > 3.0);
    const nlohmann::json j = toJson(s);
    CHECK(j.contains("level"));
    CHECK(j["mix"].contains("measuredBabbleFraction"));
    CHECK(eng.latencySamples() > 0);
}

// ------------------------------------------------------------------ long forms ([.long])

TEST_CASE("A-SP-2 (long): babble spectrum after 5 min convergence", "[.long][maskengine]") {
    if (!longTestsEnabled()) SKIP("set BF_LONG_TESTS=1");
    auto& src = bftest::shapedCorpus();
    auto doc = bftest::scenarioDoc("office", "balanced", "mono", 420.0, 90, {{"mix", {{"babbleFraction", 1.0}}}});
    RenderOptions opt;
    opt.taps = {0};
    const RenderResult r = bftest::render(doc, bftest::handle(src), opt);
    const auto& t1 = r.taps.at(0);
    const SpectrumAnalysis sa = spectrumOf(t1, 300 * kFs, 420 * kFs);
    const double d13 = maxShapeDev(sa.overallPowerLin, *r.referenceDb, true);
    std::printf("[A-SP-2] babble after 5 min: 1/3-oct 125-8k max dev %.2f dB, engine octave dev %.2f dB\n", d13,
                r.stats.babbleOctaveMaxDevDb);
    CHECK(d13 <= 2.0);
    CHECK(r.stats.babbleOctaveMaxDevDb <= 1.0);
}

TEST_CASE("A-TM-1 / A-LV-6 (long): 1 h mean active talkers and limiter transparency", "[.long][maskengine]") {
    if (!longTestsEnabled()) SKIP("set BF_LONG_TESTS=1");
    auto& src = bftest::shapedCorpus();
    auto doc = bftest::scenarioDoc("office", "balanced", "stereo", 3600.0, 91, {{"strengthDb", 6.0}});
    const RenderResult r = bftest::render(doc, bftest::handle(src));
    const double m = r.planHistory[0]["meanActive"].get<double>();
    std::printf("[A-TM-1 1h] mean k_a %.3f vs %.3f, range [%d, %d]; [A-LV-6 1h] GR>0 %.4f %%\n", r.stats.meanActive, m,
                r.stats.minActive, r.stats.maxActive, 100.0 * r.stats.limiterActiveFraction);
    CHECK(std::fabs(r.stats.meanActive / m - 1.0) <= 0.05);
    CHECK(r.stats.minActive >= 4);
    CHECK(r.stats.maxActive <= 9);
    CHECK(r.stats.limiterActiveFraction <= 0.001);
}
