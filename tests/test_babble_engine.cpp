#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "core/corpus/SyntheticCorpus.h"
#include "core/engine/BabbleEngine.h"
#include "core/talker/SourcePreparer.h"

using namespace bf;

namespace {
constexpr std::int64_t kFs = 48000;

SyntheticCorpus& speechCorpus() {
    static SyntheticCorpus c = [] {
        SyntheticCorpusParams p;
        p.numSpeakers = 16;
        p.recordingsPerSpeaker = 2;
        p.recordingSeconds = 60.0;
        return SyntheticCorpus(p);
    }();
    return c;
}

BabbleEngineConfig defaultConfig(std::uint64_t seed) {
    BabbleEngineConfig cfg;
    cfg.plan.pool = 12;
    cfg.plan.mean = 6.5;
    cfg.plan.minActive = 4;
    cfg.plan.maxActive = 9;
    cfg.plan.seed = seed;
    cfg.plan.applyCvr(1.0);
    cfg.diversity = DiversityMode::Balanced;
    return cfg;
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

double powerDb(const std::vector<std::vector<float>>& out, std::size_t a, std::size_t b) {
    double e = 0.0;
    for (const auto& ch : out)
        for (std::size_t i = a; i < b; ++i) e += static_cast<double>(ch[i]) * ch[i];
    return 10.0 * std::log10(e / static_cast<double>(b - a));
}
}  // namespace

TEST_CASE("SourcePreparer: pause shortening with centred splices and remapped speech mask", "[engine][preparer]") {
    SyntheticCorpusParams cp;
    cp.numSpeakers = 2;
    cp.recordingsPerSpeaker = 1;
    cp.recordingSeconds = 40.0;
    SyntheticCorpus corpus(cp);
    const auto snap = corpus.snapshot();
    const SegmentRec& seg = snap->segment(1);
    const std::int64_t maxGap = 100 * 48;
    const auto L = buildProcessedLayout(*snap, seg.recording, seg.anchor, maxGap, 30 * kFs);
    REQUIRE(L.length > 0);
    REQUIRE(!L.speech.empty());
    CHECK(L.speech.front().start == 0);
    for (const auto& p : L.pauses) CHECK(p.end - p.start <= maxGap);
    // Speech regions keep their lengths and order.
    const auto regs = snap->regionsOf(seg.recording);
    std::size_t k = 0;
    while (regs[k].start != seg.anchor) ++k;
    for (std::size_t i = 0; i + 1 < L.speech.size(); ++i) CHECK(L.speech[i].length() == regs[k + i].length());
    // Processed audio equals the source inside speech (splices lie inside pauses).
    SourcePreparer prep(corpus);
    std::vector<float> proc(static_cast<std::size_t>(L.length));
    REQUIRE(prep.render(L, 0, proc.data(), proc.size()));
    for (std::size_t i = 0; i + 1 < L.speech.size(); ++i) {
        const auto& s = L.speech[i];
        std::vector<float> src(static_cast<std::size_t>(s.length()));
        REQUIRE(corpus.read(seg.recording, static_cast<std::uint64_t>(L.sourcePosAt(s.start)), src.data(), src.size()));
        for (std::size_t n = 0; n < src.size(); ++n) REQUIRE(proc[static_cast<std::size_t>(s.start) + n] == src[n]);
    }
    // Chunked rendering equals one-shot rendering.
    std::vector<float> chunked(proc.size());
    for (std::size_t off = 0; off < chunked.size(); off += 1000)
        REQUIRE(prep.render(L, static_cast<std::int64_t>(off), chunked.data() + off, std::min<std::size_t>(1000, chunked.size() - off)));
    CHECK(chunked == proc);
}

TEST_CASE("BabbleEngine: same seed -> identical events and bit-identical audio for any block size", "[engine][determinism]") {
    auto& corpus = speechCorpus();
    auto run = [&](std::uint64_t seed, const std::vector<std::size_t>& blocks, std::string* json) {
        auto cfg = defaultConfig(seed);
        cfg.numChannels = 2;
        cfg.trimEnabled = true;
        BabbleEngine eng(corpus.snapshot(), corpus, cfg);
        auto p2 = cfg.plan;
        p2.mean = 4.0;
        p2.minActive = 3;
        p2.maxActive = 6;
        eng.scheduleReplan(p2, 7 * kFs + 12345);  // scenario plan change (mid-block)
        auto out = renderEngine(eng, 2, 20 * kFs, blocks);
        if (json) *json = eng.planner().eventsJsonString();
        CHECK(eng.underflows() == 0);
        CHECK(eng.planner().epoch() == 1);
        return out;
    };
    std::string j256, j64, j480, j1024, jOther;
    const auto ref = run(77, {256}, &j256);
    CHECK(run(77, {64}, &j64) == ref);
    CHECK(run(77, {480}, &j480) == ref);
    CHECK(run(77, {1024}, &j1024) == ref);
    CHECK(run(77, {1, 4095, 17, 2048}, nullptr) == ref);
    CHECK(j64 == j256);
    CHECK(j480 == j256);
    CHECK(j1024 == j256);
    const auto other = run(78, {256}, &jOther);
    CHECK(jOther != j256);
    CHECK(other != ref);
}

TEST_CASE("BabbleEngine: 60 s babble RMS within +-0.5 dB of -26 dBFS (m = 6.5, slow trim on)", "[engine][energy]") {
    auto& corpus = speechCorpus();
    for (std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
        auto cfg = defaultConfig(seed);
        cfg.trimEnabled = true;
        BabbleEngine eng(corpus.snapshot(), corpus, cfg);
        const auto out = renderEngine(eng, 1, 60 * kFs, {512});
        const double db = powerDb(out, 0, out[0].size());
        std::printf("[engine] seed %llu: 60 s RMS %.3f dBFS (trim %.2f dB), g_bnorm %.4f, mean k_a %.3f, "
                    "mean k_s %.3f, k_a range [%u, %u]\n",
                    (unsigned long long)seed, db, eng.trimDb(), eng.countNorm(), eng.stats().meanActive(),
                    eng.stats().meanSpeaking(), eng.stats().minActive, eng.stats().maxActive);
        CHECK(std::fabs(db + 26.0) <= 0.5);
        CHECK(eng.underflows() == 0);
        CHECK(eng.droppedEvents() == 0);
        CHECK_FALSE(eng.sourceErrors());
        CHECK(eng.stats().minActive >= 4);
        CHECK(eng.stats().maxActive <= 9);
    }
}

namespace {
SyntheticCorpus& noiseCorpus() {
    static SyntheticCorpus c = [] {
        SyntheticCorpusParams p;
        p.numSpeakers = 72;
        p.recordingsPerSpeaker = 2;
        p.recordingSeconds = 60.0;
        p.signal = SyntheticCorpusParams::Signal::WhiteNoise;
        // Controlled stationary talkers: fixed 2 s bursts and 160 ms (phrase-boundary) pauses.
        p.speechMedianS = 2.0;
        p.speechSigmaLn = 0.0;
        p.pauseMedianS = 0.16;
        p.pauseSigmaLn = 0.0;
        return SyntheticCorpus(p);
    }();
    return c;
}

// Talker-count modes (TALKER_ENGINE.md §5): MultiVoice K for m <= 7, stochastic above.
TalkerPlanParams modeParams(int m, std::uint64_t seed) {
    TalkerPlanParams p;
    p.seed = seed;
    p.mean = m;
    if (m <= 7) {
        const int pool[8] = {0, 4, 6, 7, 8, 10, 10, 12};
        p.mode = PlanMode::FixedK;
        p.minActive = p.maxActive = static_cast<std::uint32_t>(m);
        p.pool = static_cast<std::uint32_t>(pool[m]);
        p.medianS = 8.0;
        p.sigmaLn = 0.4;
    } else {
        p.minActive = static_cast<std::uint32_t>(std::lround(0.75 * m));
        p.maxActive = static_cast<std::uint32_t>(std::lround(1.25 * m));
        p.pool = std::max<std::uint32_t>(static_cast<std::uint32_t>(2 * m + 2), p.maxActive + 4);
    }
    p.applyCvr(1.0);
    p.gainSigmaDb = 0.0;  // unit-power talkers: no per-segment level variation
    return p;
}
}  // namespace

TEST_CASE("Energy normalisation: unit-power talkers, planned bus power within +-0.2 dB for m = 1..32", "[engine][energy]") {
    auto& corpus = noiseCorpus();
    const auto snap = corpus.snapshot();
    for (int m : {1, 2, 4, 8, 16, 32}) {
        const auto p = modeParams(m, 100 + static_cast<std::uint64_t>(m));
        SelectorConfig sc;
        sc.poolSize = p.pool;
        sc.voiceSlots = p.slots();
        sc.meanActive = p.targetMean();
        sc.seed = p.seed;
        sc.diversity = DiversityMode::Balanced;
        sc.soloRiskWeight = p.cvrSoloRiskWeight;
        SegmentSelector sel(snap, sc);
        TalkerPlanner pl(snap, sel, p);
        const double F = pl.estimateBusPowerFactor(1800.0);  // feed-forward g_bnorm = 1/sqrt(F)
        pl.setLayoutRetention(TalkerPlanner::LayoutRetention::All);
        const std::int64_t T = 3600 * kFs;
        pl.planUntil(T);
        const double realized = TalkerPlanner::plannedBusPower(pl.events(), 60 * kFs, T);
        const double errDb = 10.0 * std::log10(realized / F);
        std::printf("[energy] m=%2d: E[k_speech]=%.3f, 1 h bus power error %.3f dB\n", m, F, errDb);
        CHECK(std::fabs(errDb) <= 0.2);
    }
}

TEST_CASE("Energy normalisation: rendered bus power equals planned power (unit-power talkers)", "[engine][energy]") {
    auto& corpus = noiseCorpus();
    for (int m : {1, 4, 8, 32}) {
        BabbleEngineConfig cfg;
        cfg.plan = modeParams(m, 200 + static_cast<std::uint64_t>(m));
        cfg.diversity = DiversityMode::Balanced;
        cfg.retainLayouts = true;
        cfg.probeSeconds = 300.0;
        BabbleEngine eng(corpus.snapshot(), corpus, cfg);
        const std::size_t N = static_cast<std::size_t>(15 * kFs);
        const auto out = renderEngine(eng, 1, N, {1024});
        const double actual = powerDb(out, 0, N);
        const double g = eng.countNorm();
        const double planned =
            -26.0 + 10.0 * std::log10(g * g * TalkerPlanner::plannedBusPower(eng.planner().events(), 0, static_cast<std::int64_t>(N)));
        std::printf("[energy] m=%2d: rendered %.3f dBFS, planned %.3f dBFS\n", m, actual, planned);
        CHECK(std::fabs(actual - planned) <= 0.05);
        CHECK(eng.underflows() == 0);
        CHECK(eng.droppedEvents() == 0);
    }
}
