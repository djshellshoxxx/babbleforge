#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "core/corpus/SyntheticCorpus.h"
#include "core/talker/SegmentSelector.h"
#include "core/talker/TalkerPlanner.h"

using namespace bf;

namespace {
constexpr std::int64_t kFs = 48000;

std::shared_ptr<const CorpusSnapshot> metaCorpus() {
    static const auto snap = [] {
        SyntheticCorpusParams p;
        p.numSpeakers = 24;
        p.recordingsPerSpeaker = 3;
        p.recordingSeconds = 120.0;
        p.measureAsl = false;
        return SyntheticCorpus(p).snapshot();
    }();
    return snap;
}

SelectorConfig selCfg(const TalkerPlanParams& p) {
    SelectorConfig c;
    c.poolSize = p.pool;
    c.voiceSlots = p.slots();
    c.meanActive = p.targetMean();
    c.seed = p.seed;
    c.segMinS = p.segMinS;
    c.soloRiskWeight = p.cvrSoloRiskWeight;
    c.rotationPeriodS = p.mode == PlanMode::ContinuousN ? 0.0 : 1200.0;
    return c;
}

TalkerPlanParams balanced(std::uint64_t seed = 42) {
    TalkerPlanParams p;
    p.pool = 16;
    p.mean = 6.5;
    p.minActive = 4;
    p.maxActive = 9;
    p.seed = seed;
    p.applyCvr(1.0);  // High (default)
    return p;
}

struct Occupancy {
    double mean = 0.0;
    int minK = 1 << 30, maxK = 0;
};

// k_a over [t0, t1) from the event list (events: [start, end)).
Occupancy occupancy(const std::vector<PlannedEvent>& ev, std::int64_t t0, std::int64_t t1) {
    std::map<std::int64_t, int> delta;
    int k = 0;
    for (const auto& pe : ev) {
        const auto s = pe.ev.startSample, e = pe.ev.endSample;
        if (e <= t0 || s >= t1) continue;
        if (s <= t0) ++k; else delta[s] += 1;
        if (e < t1) delta[e] -= 1;
    }
    Occupancy o;
    std::int64_t t = t0;
    double integ = 0.0;
    o.minK = o.maxK = k;
    for (const auto& [time, d] : delta) {
        integ += static_cast<double>(k) * static_cast<double>(time - t);
        t = time;
        k += d;
        o.minK = std::min(o.minK, k);
        o.maxK = std::max(o.maxK, k);
    }
    integ += static_cast<double>(k) * static_cast<double>(t1 - t);
    o.mean = integ / static_cast<double>(t1 - t0);
    return o;
}

// Per-slot occupancy count (slots with any event) at every event boundary.
std::pair<int, int> slotOccupancyRange(const std::vector<PlannedEvent>& ev, std::int64_t t0, std::int64_t t1) {
    std::set<std::int64_t> times{t0};
    for (const auto& pe : ev) {
        if (pe.ev.startSample >= t0 && pe.ev.startSample < t1) times.insert(pe.ev.startSample);
        if (pe.ev.endSample >= t0 && pe.ev.endSample < t1) times.insert(pe.ev.endSample);
    }
    int mn = 1 << 30, mx = 0;
    for (std::int64_t t : times) {
        std::set<std::uint32_t> slots;
        for (const auto& pe : ev)
            if (pe.ev.startSample <= t && t < pe.ev.endSample) slots.insert(pe.ev.slot);
        mn = std::min(mn, static_cast<int>(slots.size()));
        mx = std::max(mx, static_cast<int>(slots.size()));
    }
    return {mn, mx};
}
}  // namespace

TEST_CASE("Planner: 1 h timeline m=6.5 V=9 meets mean, bounds, guards and cooldown", "[planner]") {
    const auto snap = metaCorpus();
    auto p = balanced();
    SegmentSelector sel(snap, selCfg(p));
    TalkerPlanner pl(snap, sel, p);
    pl.setLayoutRetention(TalkerPlanner::LayoutRetention::None);
    const std::int64_t T = 3600 * kFs;
    pl.planUntil(T);
    const auto& ev = pl.events();

    const auto occ = occupancy(ev, 0, T);
    std::printf("[planner] 1h m=6.5 V=9: mean k_a=%.3f min=%d max=%d events=%zu forced=%llu paired=%llu "
                "snapped=%llu phraseCuts=%llu lambda=%.3f\n",
                occ.mean, occ.minK, occ.maxK, ev.size(), (unsigned long long)pl.stats().forcedStarts,
                (unsigned long long)pl.stats().pairedStarts, (unsigned long long)pl.stats().snapped,
                (unsigned long long)pl.stats().phraseCuts, pl.lambda());
    CHECK(std::fabs(occ.mean - 6.5) <= 0.05 * 6.5);
    CHECK(occ.minK >= static_cast<int>(p.effectiveMin()));
    CHECK(occ.maxK <= 9);
    CHECK(pl.stats().forcedLate == 0);

    // Anti-synchrony: starts (and ends) >= 40 ms apart.
    std::vector<std::int64_t> starts, ends;
    for (const auto& pe : ev) {
        if (!(pe.ev.flags & kEvResidual)) starts.push_back(pe.ev.startSample);
        ends.push_back(pe.ev.endSample);
    }
    REQUIRE(std::is_sorted(starts.begin(), starts.end()));
    std::sort(ends.begin(), ends.end());
    std::int64_t minStartGap = INT64_MAX, minEndGap = INT64_MAX;
    for (std::size_t i = 1; i < starts.size(); ++i) minStartGap = std::min(minStartGap, starts[i] - starts[i - 1]);
    for (std::size_t i = 1; i < ends.size(); ++i) minEndGap = std::min(minEndGap, ends[i] - ends[i - 1]);
    CHECK(minStartGap >= pl.guardSamples());
    CHECK(minEndGap >= pl.guardSamples());

    // Slot re-entry cooldown and slot exclusivity.
    std::map<std::uint32_t, std::int64_t> lastEnd;
    const std::int64_t cd = static_cast<std::int64_t>(p.reEntryCooldownMs * 48.0);
    bool cooldownOk = true;
    for (const auto& pe : ev) {
        auto it = lastEnd.find(pe.ev.slot);
        if (it != lastEnd.end() && pe.ev.startSample < it->second + cd) cooldownOk = false;
        lastEnd[pe.ev.slot] = pe.ev.endSample;
    }
    CHECK(cooldownOk);

    // Event geometry.
    for (const auto& pe : ev) {
        REQUIRE(pe.ev.fadeOutStart >= pe.ev.startSample + pe.ev.fadeInLen);
        REQUIRE(pe.ev.endSample > pe.ev.fadeOutStart);
    }
}

TEST_CASE("Planner: min/max never violated over 1e4 s planned", "[planner]") {
    const auto snap = metaCorpus();
    for (std::uint64_t seed : {7ULL, 8ULL}) {
        auto p = balanced(seed);
        p.minActive = 5;  // tighter than default
        SegmentSelector sel(snap, selCfg(p));
        TalkerPlanner pl(snap, sel, p);
        pl.setLayoutRetention(TalkerPlanner::LayoutRetention::None);
        const std::int64_t T = 10000 * kFs;
        pl.planUntil(T);
        const auto occ = occupancy(pl.events(), 0, T);
        std::printf("[planner] 1e4 s seed %llu min=5: mean=%.3f min=%d max=%d forced=%llu\n",
                    (unsigned long long)seed, occ.mean, occ.minK, occ.maxK,
                    (unsigned long long)pl.stats().forcedStarts);
        CHECK(occ.minK >= 5);
        CHECK(occ.maxK <= 9);
        CHECK(std::fabs(occ.mean - 6.5) <= 0.05 * 6.5);
    }
}

TEST_CASE("Planner: forced starts hold min when the renewal process alone would not", "[planner]") {
    const auto snap = metaCorpus();
    TalkerPlanParams p = balanced(3);
    p.mean = 3.0;
    p.minActive = 3;  // min == mean: impossible without forced starts
    p.cvrMinFloor = 0;
    SegmentSelector sel(snap, selCfg(p));
    TalkerPlanner pl(snap, sel, p);
    const std::int64_t T = 600 * kFs;
    pl.planUntil(T);
    const auto occ = occupancy(pl.events(), 0, T);
    std::size_t forcedFlagged = 0;
    for (const auto& pe : pl.events()) forcedFlagged += (pe.ev.flags & kEvForced) ? 1u : 0u;
    CHECK(pl.stats().forcedStarts > 50);
    CHECK(forcedFlagged == pl.stats().forcedStarts);
    CHECK(occ.minK >= 3);
    // Forced starts overlap the ending talker (start before the drop, never after).
    CHECK(pl.stats().forcedLate == 0);
}

TEST_CASE("Planner: planUntil granularity, same seed and different seed", "[planner][determinism]") {
    const auto snap = metaCorpus();
    auto run = [&](std::uint64_t seed, std::int64_t step) {
        auto p = balanced(seed);
        SegmentSelector sel(snap, selCfg(p));
        TalkerPlanner pl(snap, sel, p);
        const std::int64_t T = 300 * kFs;
        for (std::int64_t t = step; t < T + step; t += step) pl.planUntil(std::min(t, T));
        return pl.eventsJsonString();
    };
    const auto a = run(11, 300 * kFs);
    CHECK(a == run(11, 1024));
    CHECK(a == run(11, 48000 * 7 + 13));
    CHECK(a != run(12, 300 * kFs));
}

TEST_CASE("Planner: epoch re-plan keeps frozen events", "[planner]") {
    const auto snap = metaCorpus();
    auto p = balanced(5);
    SegmentSelector sel(snap, selCfg(p));
    TalkerPlanner pl(snap, sel, p);
    pl.planUntil(60 * kFs);
    const std::vector<PlannedEvent> before = pl.events();
    const std::int64_t now = 30 * kFs;
    auto p2 = p;
    p2.mean = 4.0;
    p2.minActive = 3;
    p2.maxActive = 6;
    const auto discarded = pl.replan(p2, now);
    CHECK(pl.epoch() == 1);
    CHECK(!discarded.empty());
    std::size_t kept = 0;
    for (const auto& pe : before) {
        if (pe.ev.startSample < now + pl.freezeSamples()) {
            REQUIRE(kept < pl.events().size());
            const auto& k = pl.events()[kept].ev;
            CHECK(k.eventId == pe.ev.eventId);
            CHECK(k.startSample == pe.ev.startSample);
            CHECK(k.epoch == 0);
            // Hand-over (MASK_STRATEGIES §7): kept talkers finish with their own fade-out by
            // freeze + 1.5 s; those ending earlier are unchanged.
            const auto& h = pl.lastHandover();
            const std::int64_t handoverEnd = now + pl.freezeSamples() + h.span;
            CHECK(h.epoch == 1);
            CHECK(h.span == static_cast<std::int64_t>(TalkerPlanner::kHandoverS * kFs));
            CHECK(k.endSample <= pe.ev.endSample);
            CHECK(k.endSample <= std::max(handoverEnd, pe.ev.startSample + pe.ev.fadeInLen + (pe.ev.endSample - pe.ev.fadeOutStart)));
            CHECK(k.endSample - k.fadeOutStart == pe.ev.endSample - pe.ev.fadeOutStart);
            if (pe.ev.endSample <= now + pl.freezeSamples()) CHECK(k.endSample == pe.ev.endSample);
            if (k.endSample < pe.ev.endSample) CHECK(k.fadeOutStart >= now + pl.freezeSamples());
            ++kept;
        } else {
            CHECK(std::find(discarded.begin(), discarded.end(), pe.ev.eventId) != discarded.end());
        }
    }
    CHECK(kept == pl.events().size());
    pl.planUntil(600 * kFs);
    for (std::size_t i = kept; i < pl.events().size(); ++i) {
        CHECK(pl.events()[i].ev.epoch == 1);
        CHECK(pl.events()[i].ev.startSample >= now + pl.freezeSamples());
        CHECK(pl.events()[i].ev.slot < 6);
    }
    const auto occ = occupancy(pl.events(), 120 * kFs, 600 * kFs);
    std::printf("[planner] after re-plan to m=4: mean k_a=%.3f min=%d max=%d\n", occ.mean, occ.minK, occ.maxK);
    CHECK(std::fabs(occ.mean - 4.0) < 0.4);
    CHECK(occ.minK >= 3);
    CHECK(occ.maxK <= 6);
}

TEST_CASE("Planner: MultiVoice keeps K constant with speaker changes", "[planner]") {
    const auto snap = metaCorpus();
    TalkerPlanParams p;
    p.mode = PlanMode::FixedK;
    p.pool = 10;
    p.maxActive = 5;
    p.minActive = 5;
    p.mean = 5;
    p.medianS = 8.0;
    p.sigmaLn = 0.4;
    p.seed = 9;
    SegmentSelector sel(snap, selCfg(p));
    TalkerPlanner pl(snap, sel, p);
    const std::int64_t T = 600 * kFs;
    pl.planUntil(T);
    const auto [mn, mx] = slotOccupancyRange(pl.events(), 0, T - 30 * kFs);
    CHECK(mn == 5);
    CHECK(mx == 5);
    std::map<std::uint32_t, const TalkerEvent*> prev;
    bool chained = true, speakerChange = true;
    for (const auto& pe : pl.events()) {
        CHECK(pe.ev.fadeInLen == 7200);
        auto it = prev.find(pe.ev.slot);
        if (it != prev.end()) {
            if (pe.ev.startSample != it->second->fadeOutStart) chained = false;
            if (pe.ev.speaker == it->second->speaker) speakerChange = false;
        }
        prev[pe.ev.slot] = &pe.ev;
    }
    CHECK(chained);
    CHECK(speakerChange);
    // No speaker in two slots at the same time.
    bool exclusive = true;
    const auto& ev = pl.events();
    for (std::size_t i = 0; i < ev.size(); ++i)
        for (std::size_t j = i + 1; j < ev.size() && ev[j].ev.startSample < ev[i].ev.endSample; ++j)
            if (ev[j].ev.speaker == ev[i].ev.speaker && ev[j].ev.slot != ev[i].ev.slot) exclusive = false;
    CHECK(exclusive);
}

TEST_CASE("Planner: continuousN keeps one distinct speaker per slot with 20 ms joins", "[planner]") {
    const auto snap = metaCorpus();
    TalkerPlanParams p;
    p.mode = PlanMode::ContinuousN;
    p.pool = 8;
    p.maxActive = 4;
    p.minActive = 4;
    p.mean = 4;
    p.maxGapMs = 100.0;
    p.medianS = 8.0;
    p.sigmaLn = 0.4;
    p.seed = 10;
    SegmentSelector sel(snap, selCfg(p));
    TalkerPlanner pl(snap, sel, p);
    const std::int64_t T = 300 * kFs;
    pl.planUntil(T);
    const auto [mn, mx] = slotOccupancyRange(pl.events(), 0, T - 30 * kFs);
    CHECK(mn == 4);
    CHECK(mx == 4);
    std::map<std::uint32_t, SpeakerId> spk;
    bool constant = true;
    for (const auto& pe : pl.events()) {
        CHECK(pe.ev.fadeInLen == 960);
        auto [it, inserted] = spk.emplace(pe.ev.slot, pe.ev.speaker);
        if (!inserted && it->second != pe.ev.speaker) constant = false;
    }
    CHECK(constant);
    std::set<SpeakerId> distinct;
    for (auto& [s, id] : spk) distinct.insert(id);
    CHECK(distinct.size() == 4);
}

TEST_CASE("Planner: CVR mechanisms engage", "[planner]") {
    const auto snap = metaCorpus();
    auto p = balanced(21);
    p.mean = 3.0;
    p.minActive = 1;
    p.maxActive = 6;
    p.cvrMinFloor = 0;
    p.gainSigmaDb = 4.0;
    p.cvrLevelSigmaMult = 1.0;
    SegmentSelector sel(snap, selCfg(p));
    TalkerPlanner pl(snap, sel, p);
    pl.planUntil(1200 * kFs);
    std::printf("[planner] CVR m=3: paired=%llu capped=%llu phraseCuts=%llu\n",
                (unsigned long long)pl.stats().pairedStarts, (unsigned long long)pl.stats().capped,
                (unsigned long long)pl.stats().phraseCuts);
    CHECK(pl.stats().pairedStarts > 0);
    CHECK(pl.stats().capped > 0);
    // Dominance cap: no event exceeds the power-mean level of the talkers active at its start
    // by more than the cap.
    const auto& ev = pl.events();
    for (std::size_t i = 0; i < ev.size(); ++i) {
        double sum = 0.0;
        int n = 0;
        for (std::size_t j = 0; j < i; ++j)
            if (ev[j].ev.startSample <= ev[i].ev.startSample && ev[j].ev.endSample > ev[i].ev.startSample) {
                sum += std::pow(10.0, ev[j].ev.levelVarDb / 10.0);
                ++n;
            }
        if (n == 0 || (ev[i].ev.flags & kEvResidual)) continue;
        REQUIRE(ev[i].ev.levelVarDb <= 10.0 * std::log10(sum / n) + p.cvrDominanceCapDb + 1e-3);
    }
}
