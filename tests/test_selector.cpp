#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/corpus/SyntheticCorpus.h"
#include "core/talker/SegmentSelector.h"

using namespace bf;

namespace {
constexpr std::int64_t kFs = 48000;

std::shared_ptr<const CorpusSnapshot> corpus(std::uint32_t speakers = 24) {
    SyntheticCorpusParams p;
    p.numSpeakers = speakers;
    p.recordingsPerSpeaker = 2;
    p.recordingSeconds = 60.0;
    p.measureAsl = false;
    return SyntheticCorpus(p).snapshot();
}

SelectorConfig cfg(std::uint32_t pool, std::uint32_t slots, std::uint64_t seed = 3) {
    SelectorConfig c;
    c.poolSize = pool;
    c.voiceSlots = slots;
    c.meanActive = 2.0;
    c.seed = seed;
    c.segCooldownOverrideS = 0.0;
    c.rotationPeriodS = 0.0;
    return c;
}
}  // namespace

TEST_CASE("Snapshot: VAD pauses, anchors and segment records are consistent", "[selector][corpus]") {
    const auto snap = corpus(6);
    REQUIRE(snap->numSegments() > 0);
    for (RecordingId r = 0; r < snap->numRecordings(); ++r) {
        const auto regs = snap->regionsOf(r);
        const auto pauses = snap->pausesOf(r);
        REQUIRE(pauses.size() + 1 == regs.size());
        for (std::size_t i = 0; i < pauses.size(); ++i) {
            CHECK(pauses[i].start == regs[i].end);
            CHECK(pauses[i].end == regs[i + 1].start);
            CHECK(pauses[i].end - pauses[i].start >= kMinPauseSamples);
            CHECK(pauses[i].phraseBoundary == (pauses[i].end - pauses[i].start >= kPhraseBoundarySamples));
        }
    }
    for (SpeakerId s = 0; s < snap->numSpeakers(); ++s) {
        for (const auto& seg : snap->anchorsOf(s)) {
            CHECK(snap->recording(seg.recording).speaker == s);
            CHECK(snap->recording(seg.recording).length - seg.anchor >= 2 * kFs);
            CHECK(snap->speechSecondsIn(seg.recording, seg.anchor, seg.anchor + 3 * kFs) >= 1.0);
            bool onset = false;
            for (const auto& reg : snap->regionsOf(seg.recording)) onset |= reg.start == seg.anchor;
            CHECK(onset);
        }
    }
}

TEST_CASE("Selector: never picks an active speaker; recent-speaker rule", "[selector]") {
    const auto snap = corpus();
    SegmentSelector sel(snap, cfg(8, 2));
    REQUIRE(sel.pool().size() == 8);
    REQUIRE(sel.recentWindow() == 3);  // max(1, (8 - 2) / 2)
    std::vector<SpeakerId> history;
    std::int64_t t = 0;
    for (int i = 0; i < 200; ++i) {
        std::vector<SpeakerId> active;
        if (i % 3 == 0) active = {sel.pool()[static_cast<std::size_t>(i) % 8]};
        auto pk = sel.pick(t, active);
        REQUIRE(pk);
        CHECK(std::find(active.begin(), active.end(), pk->speaker) == active.end());
        // Not among the last R_spk = 3 selected speakers.
        for (std::size_t k = 0; k < 3 && k < history.size(); ++k) CHECK(history[history.size() - 1 - k] != pk->speaker);
        history.push_back(pk->speaker);
        sel.commit(*pk, pk->anchor + 2 * kFs, t, t + 2 * kFs);
        t += 20 * kFs;  // beyond the 10 s per-speaker time rule
    }
    CHECK(sel.stats().relaxed[0] == 200);
}

TEST_CASE("Selector: relaxation order R -> R/2 -> 0", "[selector]") {
    const auto snap = corpus();
    auto c = cfg(4, 0);  // R_spk = max(1, 4/2) = 2
    SegmentSelector sel(snap, c);
    const auto pool = sel.pool();
    REQUIRE(pool.size() == 4);
    REQUIRE(sel.recentWindow() == 2);
    std::int64_t t = 0;
    // Recent: pool[2], pool[3] (most recent last); active: pool[0], pool[1].
    for (SpeakerId s : {pool[2], pool[3]}) {
        auto pk = sel.pickForSpeaker(s, t);
        REQUIRE(pk);
        sel.commit(*pk, pk->anchor + kFs, t, t + kFs);
        t += 20 * kFs;
    }
    const std::vector<SpeakerId> active{pool[0], pool[1]};
    auto pk = sel.pick(t, active);
    REQUIRE(pk);
    CHECK(pk->speaker == pool[2]);  // stage 1: only the most recent speaker is excluded
    CHECK(sel.stats().relaxed[1] == 1);
    sel.commit(*pk, pk->anchor + kFs, t, t + kFs);
    // Now recent = [.., pool[3], pool[2]]; with the time rule both remain blocked (t unchanged)
    // until stage 2 drops all recency rules.
    const std::vector<SpeakerId> active3{pool[0], pool[1], pool[3]};
    auto pk2 = sel.pick(t, active3);
    REQUIRE(pk2);
    CHECK(pk2->speaker == pool[2]);
    CHECK(sel.stats().relaxed[2] == 1);
    // All pool speakers active: failure.
    CHECK_FALSE(sel.pick(t, pool));
    CHECK(sel.stats().failures == 1);
}

TEST_CASE("Selector: persistent shuffle covers all anchors before reuse; cycle constraint", "[selector]") {
    const auto snap = corpus();
    SegmentSelector sel(snap, cfg(8, 2));
    const SpeakerId s = sel.pool()[0];
    const std::size_t n = snap->anchorsOf(s).size();
    REQUIRE(n >= 10);
    std::vector<std::vector<SegmentId>> cycles;
    std::int64_t t = 0;
    for (int c = 0; c < 6; ++c) {
        std::vector<SegmentId> cyc;
        for (std::size_t i = 0; i < n; ++i) {
            auto pk = sel.pickForSpeaker(s, t);
            REQUIRE(pk);
            cyc.push_back(pk->segment);
            sel.commit(*pk, pk->anchor + kFs, t, t + kFs);
            t += kFs;
        }
        CHECK(sel.cycleOf(s) == static_cast<std::uint32_t>(c));
        std::set<SegmentId> uniq(cyc.begin(), cyc.end());
        CHECK(uniq.size() == n);  // every anchor exactly once per cycle
        cycles.push_back(std::move(cyc));
    }
    const std::size_t k = (n * 2) / 10;
    for (std::size_t c = 1; c < cycles.size(); ++c) {
        std::set<SegmentId> tail(cycles[c - 1].end() - static_cast<std::ptrdiff_t>(k), cycles[c - 1].end());
        for (std::size_t i = 0; i < k; ++i) CHECK(tail.count(cycles[c][i]) == 0);
    }
    // Different cycles are different permutations.
    CHECK(cycles[0] != cycles[1]);
}

TEST_CASE("Selector: global segment cooldown skips cooled material", "[selector]") {
    const auto snap = corpus();
    auto c = cfg(8, 2);
    c.segCooldownOverrideS = 3600.0;
    SegmentSelector sel(snap, c);
    const SpeakerId s = sel.pool()[1];
    auto pk = sel.pickForSpeaker(s, 0);
    REQUIRE(pk);
    const RecordingId cooled = pk->recording;
    // Mark the whole rest of that recording as played.
    sel.commit(*pk, snap->recording(cooled).length, 0, 10 * kFs);
    std::size_t sameRecLater = 0;
    for (const auto& seg : snap->anchorsOf(s)) sameRecLater += (seg.recording == cooled && seg.anchor > pk->anchor);
    REQUIRE(sameRecLater > 0);
    const std::size_t n = snap->anchorsOf(s).size();
    for (std::size_t i = 0; i + 1 < n / 2; ++i) {
        auto q = sel.pickForSpeaker(s, 20 * kFs);
        REQUIRE(q);
        CHECK(!(q->recording == cooled && q->anchor >= pk->anchor));
        CHECK_FALSE(sel.anchorInCooldown(q->segment, 20 * kFs));
    }
    CHECK(sel.stats().cooldownSkips > 0);
    // After the cooldown expires the region is available again.
    CHECK_FALSE(sel.anchorInCooldown(pk->segment, 10 * kFs + 3600 * kFs));
}

TEST_CASE("Selector: adaptive segment cooldown T_seg,eff", "[selector]") {
    const auto snap = corpus();
    auto c = cfg(8, 2);
    c.segCooldownOverrideS = -1.0;
    c.diversity = DiversityMode::Balanced;
    c.meanActive = 6.5;
    SegmentSelector sel(snap, c);
    double material = 0.0;
    for (SpeakerId s : sel.eligibleSpeakers()) material += snap->usableSpeechSeconds(s);
    const double expect = std::max(300.0, std::min(2700.0, 0.8 * material / 6.5));
    CHECK(std::fabs(sel.segmentCooldownS() - expect) < 1e-9);
    CHECK(sel.smallForCooldownWarning());
}

TEST_CASE("Selector: state persistence round-trip", "[selector]") {
    const auto snap = corpus();
    auto c = cfg(8, 2);
    c.segCooldownOverrideS = 600.0;
    SegmentSelector a(snap, c);
    std::int64_t t = 0;
    for (int i = 0; i < 40; ++i) {
        auto pk = a.pick(t, {});
        REQUIRE(pk);
        a.commit(*pk, pk->anchor + 3 * kFs, t, t + 3 * kFs);
        t += 2 * kFs;
    }
    const auto state = a.exportState(t);
    const auto text = state.dump();

    auto c2 = c;
    c2.seed = 999;  // a different session: state comes from the file, not the seed
    SegmentSelector b(snap, c2);
    REQUIRE(b.importState(nlohmann::json::parse(text), t));
    CHECK(b.exportState(t) == state);
    for (SpeakerId s : a.pool()) {
        for (int i = 0; i < 5; ++i) {
            auto pa = a.pickForSpeaker(s, t);
            auto pb = b.pickForSpeaker(s, t);
            REQUIRE(pa);
            REQUIRE(pb);
            CHECK(pa->segment == pb->segment);
        }
    }
    // Mismatching corpus version is rejected.
    auto bad = state;
    bad["corpusVersion"] = "other";
    SegmentSelector d(snap, c);
    CHECK_FALSE(d.importState(bad, t));
}

TEST_CASE("Selector: diversity-mode pools", "[selector]") {
    const auto snap = corpus(32);
    auto f0 = [&](SpeakerId s) { return snap->speaker(s).features[kFeatF0MedianSt]; };
    auto meanPairDist = [](const SegmentSelector& s) {
        double sum = 0.0;
        int n = 0;
        const auto& p = s.pool();
        for (std::size_t i = 0; i < p.size(); ++i)
            for (std::size_t j = i + 1; j < p.size(); ++j) { sum += s.featureDistance(p[i], p[j]); ++n; }
        return sum / n;
    };
    auto mk = [&](DiversityMode m) {
        auto c = cfg(8, 4, 17);
        c.diversity = m;
        return SegmentSelector(snap, c);
    };
    const auto low = mk(DiversityMode::Low);
    const auto high = mk(DiversityMode::High);
    const auto bal = mk(DiversityMode::Balanced);
    CHECK(low.pool().size() == 8);
    CHECK(high.pool().size() == 8);
    CHECK(bal.pool().size() == 8);
    CHECK(meanPairDist(low) < meanPairDist(high));

    // Balanced: 2 per F0 quartile -> 4 below / 4 above the corpus F0 median.
    std::vector<float> all;
    for (SpeakerId s : bal.eligibleSpeakers()) all.push_back(f0(s));
    std::sort(all.begin(), all.end());
    const float median = 0.5f * (all[all.size() / 2 - 1] + all[all.size() / 2]);
    int lower = 0;
    for (SpeakerId s : bal.pool()) lower += f0(s) < median;
    CHECK(lower == 4);

    // High excludes F0 outliers (> 2.5 SD) from the eligible set.
    {
        double m = 0.0, m2 = 0.0;
        for (SpeakerId s = 0; s < snap->numSpeakers(); ++s) { m += f0(s); m2 += f0(s) * f0(s); }
        const auto ns = static_cast<double>(snap->numSpeakers());
        m /= ns;
        const double sd = std::sqrt(m2 / ns - m * m);
        for (SpeakerId s : high.pool()) CHECK(std::fabs(f0(s) - m) / sd <= 2.5);
    }

    // Matched: low target voices -> pool F0 below corpus median.
    auto c = cfg(6, 2, 17);
    c.diversity = DiversityMode::Matched;
    c.target.f0MedianQuantilesHz = {85, 90, 95, 100, 105};
    SegmentSelector matched(snap, c);
    REQUIRE(matched.pool().size() == 6);
    for (SpeakerId s : matched.pool()) CHECK(f0(s) < median);
}

TEST_CASE("Selector: pool rotation replaces ceil(P/4) inactive least-recent speakers", "[selector]") {
    const auto snap = corpus(32);
    auto c = cfg(8, 4, 5);
    c.rotationPeriodS = 60.0;
    c.diversity = DiversityMode::Balanced;
    SegmentSelector sel(snap, c);
    const auto before = sel.pool();
    const std::vector<SpeakerId> active{before[0], before[1], before[2]};
    sel.maybeRotatePool(59 * kFs, active);
    CHECK(sel.pool() == before);
    sel.maybeRotatePool(61 * kFs, active);
    const auto after = sel.pool();
    CHECK(after.size() == 8);
    std::size_t kept = 0;
    for (SpeakerId s : after) kept += std::count(before.begin(), before.end(), s);
    CHECK(kept == 6);  // ceil(8/4) = 2 replaced
    for (SpeakerId s : active) CHECK(std::count(after.begin(), after.end(), s) == 1);
    CHECK(sel.stats().rotations == 1);
}
