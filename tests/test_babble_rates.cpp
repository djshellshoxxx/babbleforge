// Babble at 44.1 / 48 / 88.2 / 96 kHz (docs/TALKER_ENGINE.md §8.1, ENGINE.md §4, CORPUS.md §3.2):
// the corpus stays 48 kHz, events are resampled at preload time and the planner clock runs in
// engine samples.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/corpus/SyntheticCorpus.h"
#include "core/engine/BabbleEngine.h"
#include "core/talker/SegmentSelector.h"
#include "core/talker/SourcePreparer.h"
#include "core/talker/TalkerPlanner.h"
#include "mask_engine_fixtures.h"

using namespace bf;
using bftest::rmsDb;

namespace {

constexpr double kRates[] = {44100.0, 48000.0, 88200.0, 96000.0};

SyntheticCorpus& rateCorpus() {
    static SyntheticCorpus c = [] {
        SyntheticCorpusParams p;
        p.numSpeakers = 16;
        p.recordingsPerSpeaker = 2;
        p.recordingSeconds = 60.0;
        p.measureAsl = false;  // nominal levels: fast fixture
        return SyntheticCorpus(p);
    }();
    return c;
}

TalkerPlanParams planParams(std::uint64_t seed) {
    TalkerPlanParams p;
    p.pool = 12;
    p.mean = 6.5;
    p.minActive = 4;
    p.maxActive = 9;
    p.seed = seed;
    p.applyCvr(1.0);
    return p;
}

SelectorConfig selCfg(const TalkerPlanParams& p, double fs) {
    SelectorConfig c;
    c.poolSize = p.pool;
    c.voiceSlots = p.slots();
    c.meanActive = p.targetMean();
    c.seed = p.seed;
    c.segMinS = p.segMinS;
    c.soloRiskWeight = p.cvrSoloRiskWeight;
    c.clockRate = fs;
    return c;
}

std::vector<std::vector<float>> renderEngine(BabbleEngine& eng, std::size_t nCh, std::size_t frames,
                                             const std::vector<std::size_t>& blocks) {
    std::vector<std::vector<float>> out(nCh, std::vector<float>(frames));
    std::vector<float*> p(nCh);
    std::size_t off = 0, bi = 0;
    while (off < frames) {
        const std::size_t n = std::min(blocks[bi++ % blocks.size()], frames - off);
        for (std::size_t c = 0; c < nCh; ++c) p[c] = out[c].data() + off;
        eng.render(p.data(), static_cast<int>(nCh), static_cast<int>(n));
        off += n;
    }
    return out;
}

}  // namespace

TEST_CASE("Rate helpers: supported rates, deterministic conversions, layouts in engine samples", "[talker][rates]") {
    CHECK(isSupportedBabbleRate(44100.0));
    CHECK(isSupportedBabbleRate(96000.0));
    CHECK_FALSE(isSupportedBabbleRate(32000.0));
    CHECK_FALSE(isSupportedBabbleRate(192000.0));
    CHECK(msToEngine(40.0, 48000) == 1920);
    CHECK(msToEngine(40.0, 96000) == 3840);
    CHECK(msToEngine(40.0, 44100) == 1764);
    CHECK(msToEngine(40.0, 88200) == 3528);
    CHECK(secondsToEngine(0.5, 96000) == 2 * secondsToEngine(0.5, 48000));
    CHECK(corpusToEngine(48000, 44100) == 44100);
    CHECK(engineToCorpus(corpusToEngine(123457, 96000), 96000) == 123457);

    auto& corpus = rateCorpus();
    const auto snap = corpus.snapshot();
    const SegmentRec& seg = snap->segment(1);
    const auto L48 = buildProcessedLayout(*snap, seg.recording, seg.anchor, 100 * 48, 20 * 48000);
    SourcePreparer prep(corpus);
    for (double fs : kRates) {
        const auto R = static_cast<std::int64_t>(fs);
        const auto L = buildProcessedLayout(*snap, seg.recording, seg.anchor, 100 * 48, 20 * 48000, R);
        CHECK(L.length == corpusToEngine(L48.length, R));
        REQUIRE(L.speech.size() == L48.speech.size());
        CHECK(L.speech.front().start == 0);
        // Processed audio: whole-event resample, identical for any request pattern; level kept.
        std::vector<float> whole(static_cast<std::size_t>(L.length)), chunked(whole.size());
        REQUIRE(prep.render(L, 0, whole.data(), whole.size()));
        for (std::size_t off = 0; off < chunked.size(); off += 4099)
            REQUIRE(prep.render(L, static_cast<std::int64_t>(off), chunked.data() + off,
                                std::min<std::size_t>(4099, chunked.size() - off)));
        CHECK(chunked == whole);
        std::vector<float> ref(static_cast<std::size_t>(L48.length));
        REQUIRE(prep.render(L48, 0, ref.data(), ref.size()));
        const double dDb = rmsDb(whole, 0, whole.size()) - rmsDb(ref, 0, ref.size());
        INFO("fs " << fs << ": resampled event level " << dDb << " dB vs 48 kHz");
        CHECK(std::fabs(dDb) <= 0.1);
    }
}

TEST_CASE("Babble at 44.1/88.2/96 kHz: 60 s RMS, mean k_a and babble spectrum (short form)", "[maskengine][babble][rates]") {
    // 48 kHz: the identical scenario and checks are A-LV-2 / A-TM-1 / A-SP-2 in test_mask_engine.cpp.
    auto& src = bftest::shapedCorpus();
    for (double fs : {44100.0, 88200.0, 96000.0}) {
        auto doc = bftest::scenarioDoc("office", "balanced", "stereo", 60.0, 7, {{"mix", {{"babbleFraction", 1.0}}}});
        doc["sampleRate"] = fs;
        const RenderResult r = bftest::render(doc, bftest::handle(src));
        const std::size_t n = r.audio[0].size();
        REQUIRE(n == static_cast<std::size_t>(60.0 * fs));
        const double l60 = rmsDb(r.audio, 0, n);
        const double m = r.planHistory[0]["meanActive"].get<double>();
        std::printf("[rates] %.1f kHz: babble 60 s RMS %.3f dBFS, mean k_a %.3f (%+.1f %%), 1/3-oct max dev %.2f dB, "
                    "octave %.2f dB\n",
                    fs / 1000.0, l60, r.stats.meanActive, 100.0 * (r.stats.meanActive / m - 1.0),
                    r.stats.babbleThirdOctMaxDevDb, r.stats.babbleOctaveMaxDevDb);
        CHECK(r.stats.babbleActive);
        CHECK_FALSE(r.stats.babbleUnavailable);
        CHECK(std::fabs(l60 + 26.0) <= 0.5);
        CHECK(std::fabs(r.stats.meanActive / m - 1.0) <= 0.05);
        CHECK(r.stats.underflows == 0);
        CHECK(r.stats.haveBabbleSpectrum);
        CHECK(r.stats.babbleThirdOctMaxDevDb <= 2.0);
    }
}

TEST_CASE("BabbleEngine at every rate: identical events and bit-identical audio for any block size", "[engine][determinism][rates]") {
    auto& corpus = rateCorpus();
    for (double fs : kRates) {
        const auto R = static_cast<std::int64_t>(fs);
        auto run = [&](const std::vector<std::size_t>& blocks, std::string* json) {
            BabbleEngineConfig cfg;
            cfg.plan = planParams(77);
            cfg.diversity = DiversityMode::Balanced;
            cfg.numChannels = 2;
            cfg.trimEnabled = true;
            cfg.probeSeconds = 60.0;
            cfg.fs = fs;
            BabbleEngine eng(corpus.snapshot(), corpus, cfg);
            auto p2 = cfg.plan;
            p2.mean = 4.0;
            p2.minActive = 3;
            p2.maxActive = 6;
            eng.scheduleReplan(p2, 3 * R + 12345);  // plan change mid-block
            auto out = renderEngine(eng, 2, static_cast<std::size_t>(6 * R), blocks);
            *json = eng.planner().eventsJsonString();
            CHECK(eng.underflows() == 0);
            CHECK(eng.planner().epoch() == 1);
            return out;
        };
        INFO("fs " << fs);
        std::string ja, jb;
        const auto a = run({512}, &ja);
        const auto b = run({1, 4095, 97, 2048}, &jb);
        CHECK(ja == jb);
        CHECK(a == b);
        CHECK(rmsDb(a, 0, a[0].size()) > -40.0);
    }
}

TEST_CASE("Planner timeline in seconds: 96 kHz matches 48 kHz (and 88.2 matches 44.1)", "[planner][rates]") {
    const auto snap = rateCorpus().snapshot();
    auto plan = [&](double fs) {
        const auto p = planParams(5);
        auto sel = std::make_unique<SegmentSelector>(snap, selCfg(p, fs));
        auto pl = std::make_unique<TalkerPlanner>(snap, *sel, p, fs);
        pl->setLayoutRetention(TalkerPlanner::LayoutRetention::None);
        pl->planUntil(static_cast<std::int64_t>(300.0 * fs));
        std::vector<TalkerEvent> ev;
        for (const auto& pe : pl->events()) ev.push_back(pe.ev);
        return ev;
    };
    for (auto [lo, hi] : {std::pair{48000.0, 96000.0}, std::pair{44100.0, 88200.0}}) {
        const auto a = plan(lo), b = plan(hi);
        INFO(lo << " Hz vs " << hi << " Hz");
        REQUIRE(a.size() > 100);
        REQUIRE(a.size() == b.size());
        // Within one sample period of the higher rate (the conversions make it exact).
        auto close = [&](std::int64_t x, std::int64_t y) {
            return std::fabs(static_cast<double>(x) / lo - static_cast<double>(y) / hi) <= 1.0 / hi + 1e-12;
        };
        std::size_t bad = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const bool ok = a[i].slot == b[i].slot && a[i].speaker == b[i].speaker && a[i].segment == b[i].segment &&
                            a[i].flags == b[i].flags && close(a[i].startSample, b[i].startSample) &&
                            close(a[i].fadeOutStart, b[i].fadeOutStart) && close(a[i].endSample, b[i].endSample) &&
                            close(a[i].startSample + a[i].fadeInLen, b[i].startSample + b[i].fadeInLen);
            if (!ok) ++bad;
        }
        std::printf("[rates] planner %g vs %g Hz: %zu events, %zu mismatches\n", lo, hi, a.size(), bad);
        CHECK(bad == 0);
    }
}
