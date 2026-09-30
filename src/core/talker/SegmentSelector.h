#pragma once
// Segment selector (docs/TALKER_ENGINE.md §6-7): speaker choice, persistent per-speaker
// anchor shuffle with cycle constraint, cooldowns (incl. adaptive T_seg,eff), diversity-mode
// pool construction and pool rotation. Deterministic: all draws use named RngStreams
// ("selector", "selector.shuffle.<speakerId>.<cycle>"); candidate lists are ordered by id.
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "core/corpus/CorpusSnapshot.h"
#include "core/random/Random.h"

namespace bf {

enum class DiversityMode : std::uint8_t { Low, Balanced, High, Matched };

struct TargetVoiceProfile {
    std::vector<double> f0MedianQuantilesHz;  // ascending
};

struct SelectorConfig {
    std::uint32_t poolSize = 12;             // P
    std::uint32_t voiceSlots = 9;            // V (for R_spk)
    double meanActive = 6.5;                 // r_consume for T_seg,eff
    DiversityMode diversity = DiversityMode::High;
    bool languageAware = false;
    TargetVoiceProfile target;               // Matched mode
    std::uint64_t seed = 1;
    double segMinS = 1.5;                    // anchors need segMin + 0.5 s of material
    double ageTS = 60.0;                     // T_age
    double speakerReuseS = 10.0;             // per-speaker time-based reuse cooldown
    double rotationPeriodS = 1200.0;         // pool rotation (0 = off, e.g. LaboratoryMask)
    double soloRiskWeight = 1.0;             // CVR selection weight of solo-risk anchors
    double segCooldownOverrideS = -1.0;      // >= 0 overrides T_seg,eff (tests)
};

struct SegmentPick {
    SpeakerId speaker = 0;
    RecordingId recording = 0;
    SegmentId segment = 0;
    std::int64_t anchor = 0;
};

struct SelectorStats {
    std::uint64_t picks = 0;
    std::uint64_t relaxed[3] = {0, 0, 0};    // eligibility stage used: 0 strict, 1 R/2, 2 R=0
    std::uint64_t cooldownSkips = 0;
    std::uint64_t cooldownRelaxed = 0;       // all anchors cooled: cooldown ignored
    std::uint64_t failures = 0;              // no eligible speaker at all
    std::uint64_t rotations = 0;
};

class SegmentSelector {
public:
    SegmentSelector(std::shared_ptr<const CorpusSnapshot> snap, const SelectorConfig& cfg);

    // Re-derives the "selector" stream from (seed, epoch, planHash) (plan change, §4.6).
    void reseed(std::uint64_t epoch, std::uint64_t planHash);
    // Rebuilds the pool (diversity change / new P). Uses the "selector" stream.
    void setPoolSize(std::uint32_t p);
    void setVoiceSlots(std::uint32_t v, double meanActive);
    void setSoloRiskWeight(double w) { cfg_.soloRiskWeight = w; }

    // Picks a speaker (§6.2) and its next anchor (§6.3) for an event starting at t.
    std::optional<SegmentPick> pick(std::int64_t t, std::span<const SpeakerId> active);
    // Next anchor of a given speaker (ContinuousN).
    std::optional<SegmentPick> pickForSpeaker(SpeakerId s, std::int64_t t);
    // Registers the played material region [anchor, srcEnd) and the event time span.
    void commit(const SegmentPick& p, std::int64_t srcEnd, std::int64_t tStart, std::int64_t tEnd);
    // Pool rotation (§7.3), evaluated at planned time t (every rotationPeriodS).
    void maybeRotatePool(std::int64_t t, std::span<const SpeakerId> active);
    // n distinct pool speakers in random order (stream "lab.speakerset").
    std::vector<SpeakerId> chooseSpeakerSet(std::uint32_t n, RngStream& rng) const;

    const std::vector<SpeakerId>& pool() const noexcept { return pool_; }
    const std::vector<SpeakerId>& eligibleSpeakers() const noexcept { return eligible_; }
    std::uint32_t recentWindow() const noexcept;           // R_spk
    double segmentCooldownS() const noexcept { return segCooldownS_; }
    bool smallForCooldownWarning() const noexcept { return segCooldownS_ < 1800.0; }
    const SelectorStats& stats() const noexcept { return stats_; }
    std::uint32_t cycleOf(SpeakerId s) const { return spk_[s].cycle; }
    std::size_t shuffleSize(SpeakerId s) const { return spk_[s].perm.size(); }
    const std::vector<std::uint32_t>& shuffleOf(SpeakerId s) const { return spk_[s].perm; }
    bool anchorInCooldown(SegmentId g, std::int64_t t) const;
    double featureDistance(SpeakerId a, SpeakerId b) const;
    const CorpusSnapshot& snapshot() const noexcept { return *snap_; }

    // Persistence (non-deterministic mode, §6.3): shuffle (cycle, position, permutation),
    // speaker usage and the global cooldown table; times stored relative to `now`.
    nlohmann::json exportState(std::int64_t now) const;
    bool importState(const nlohmann::json& j, std::int64_t now);

private:
    struct SpeakerState {
        std::vector<std::uint32_t> perm;  // local anchor indices (eligible anchors only)
        std::uint32_t pos = 0, cycle = 0;
        std::uint32_t soloRemaining = 0;
        bool hasUse = false;
        std::int64_t lastUseStart = 0, lastUseEnd = 0;
    };
    struct CoolRegion { std::int64_t start, end, expiry; };

    void buildFeatureSpace();
    void buildEligible();
    void buildPool();
    void newCycle(SpeakerId s);
    std::optional<std::uint32_t> nextAnchor(SpeakerId s, std::int64_t t);
    bool eligibleAt(SpeakerId s, std::int64_t t, std::span<const SpeakerId> active,
                    std::uint32_t recentN, bool timeRule) const;
    double weightOf(SpeakerId s, std::int64_t t) const;
    SpeakerId nearestNonPool(SpeakerId ref, const std::vector<bool>& inPool,
                             const std::vector<bool>& taken) const;

    std::shared_ptr<const CorpusSnapshot> snap_;
    SelectorConfig cfg_;
    RngStream rng_;
    std::vector<std::vector<float>> z_;           // z-scored features per speaker
    std::vector<SpeakerId> eligible_;             // eligible under the diversity mode
    std::vector<SpeakerId> pool_;                 // sorted by id
    std::vector<SpeakerState> spk_;
    std::vector<std::vector<std::uint32_t>> eligibleAnchors_;  // per speaker, local indices
    std::vector<std::vector<CoolRegion>> cool_;   // per recording
    std::deque<SpeakerId> recent_;
    double segCooldownS_ = 2700.0;
    std::int64_t nextRotation_ = 0;
    SelectorStats stats_;
};

}  // namespace bf
