#pragma once
// Deterministic discrete-event talker planner (docs/TALKER_ENGINE.md §4, MASK_STRATEGIES §6).
//
// Runs ahead of real time on its own int64 sample clock (48 kHz). Decisions depend only on
// the parameters, the seed, the corpus snapshot, the selector state and the planner's own
// history, never on how planUntil() calls are granulated: planUntil(t) commits exactly the
// events whose start is < t, in start order, and the sequence of committed events is the same
// for any sequence of increasing t.
//
// Stochastic mode: V slots with alternating-renewal ON/OFF (log-normal ON, shifted
// exponential OFF), steady-state initial phase, count controller lambda, forced starts for min,
// 40 ms anti-synchrony guards on starts and ends, CVR onset pairing, dominance cap and max
// phrase continuity. FixedK / ContinuousN: always-on chained slots (§4.7).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "core/corpus/CorpusSnapshot.h"
#include "core/random/Random.h"
#include "core/talker/SegmentSelector.h"
#include "core/talker/SourcePreparer.h"
#include "core/talker/TalkerPlanParams.h"

namespace bf {

enum TalkerEventFlag : std::uint16_t {
    kEvResidual = 1,     // initial steady-state event (virtual start before 0)
    kEvForced = 2,       // forced start (min enforcement)
    kEvPaired = 4,       // start pulled forward by CVR onset pairing
    kEvSnapped = 8,      // fade-out snapped to a pause boundary
    kEvPhraseCut = 16,   // fade-out forced by CVR max phrase continuity
    kEvCapped = 32,      // level limited by the CVR dominance cap
    kEvEndShifted = 64,  // end moved by the anti-synchrony guard
};

struct TalkerEvent {
    std::uint64_t eventId = 0;
    std::uint32_t epoch = 0;
    std::uint32_t slot = 0;
    std::int64_t startSample = 0;
    std::int64_t fadeInLen = 0;
    std::int64_t fadeOutStart = 0;  // absolute sample
    std::int64_t endSample = 0;     // absolute sample; fade-out = [fadeOutStart, endSample)
    SpeakerId speaker = 0;
    RecordingId recording = 0;
    SegmentId segment = 0;
    std::int64_t anchor = 0;
    float segGainLin = 1.0f;        // ASL normalisation x level variation (x dominance cap)
    float pan = 0.0f;               // placeholder position [-1, 1] (stream spatial.slot.<j>)
    float levelVarDb = 0.0f;
    std::uint16_t flags = 0;
    std::int64_t length() const noexcept { return endSample - startSample; }
};

struct PlannedEvent {
    TalkerEvent ev;
    std::shared_ptr<const ProcessedLayout> layout;  // processed audio + speech mask
};

struct PlannerStats {
    std::uint64_t events = 0, forcedStarts = 0, pairedStarts = 0, snapped = 0, phraseCuts = 0,
                  capped = 0, startShifts = 0, endShifts = 0, pickFailures = 0, forcedLate = 0;
};

class TalkerPlanner {
public:
    static constexpr std::int64_t kFs = 48000;
    static constexpr std::int64_t kGuard = 1920;       // 40 ms
    static constexpr std::int64_t kFreeze = 24000;     // 0.5 s re-plan freeze window

    TalkerPlanner(std::shared_ptr<const CorpusSnapshot> snap, SegmentSelector& selector,
                  const TalkerPlanParams& params);

    // Commits all events with startSample < t.
    void planUntil(std::int64_t t);
    // Plan change at `now` (§4.6): events starting before now + 0.5 s are kept, later ones
    // discarded (ids returned), epoch++, RNG streams re-derived from (seed, epoch, plan hash).
    std::vector<std::uint64_t> replan(const TalkerPlanParams& params, std::int64_t now);

    const std::vector<PlannedEvent>& events() const noexcept { return events_; }
    // Appends events committed since the last call.
    std::size_t takeNew(std::vector<PlannedEvent>& out);
    // Layouts of events that are no longer live: kept (All), dropped once taken (UntilTaken,
    // default) or dropped regardless (None; planner-only long runs).
    enum class LayoutRetention { All, UntilTaken, None };
    void setLayoutRetention(LayoutRetention r) noexcept { retention_ = r; }

    // Time-average over [t0, t1) of sum_events levelVar^2 * fade^2 * speech (events need
    // layouts). With ASL-normalised talkers the babble bus power is L_ref^2 * g_bnorm^2 * this.
    static double plannedBusPower(const std::vector<PlannedEvent>& events, std::int64_t t0, std::int64_t t1);

    const TalkerPlanParams& params() const noexcept { return p_; }
    std::uint32_t epoch() const noexcept { return epoch_; }
    double lambda() const noexcept { return lambda_; }
    double meanOnSeconds() const noexcept { return dOnS_; }
    double meanOffSeconds() const noexcept { return dOffS_; }
    std::int64_t frontier() const noexcept { return lastStart_; }
    const PlannerStats& stats() const noexcept { return stats_; }

    // Planned bus power factor F = time-average of sum_events (levelVar^2 * fade^2 * speech)
    // (E[k_speech] including level variation and fades) from a probe planner run of `seconds`
    // on a copy of the selector. Feed-forward g_bnorm = 1 / sqrt(F) (ENGINE.md §3.2).
    double estimateBusPowerFactor(double seconds = 600.0) const;

    nlohmann::json eventsJson() const;
    std::string eventsJsonString() const;

private:
    struct Slot {
        RngStream rng{0}, spatial{0};
        std::int64_t lastEnd = kNever;
        std::int64_t nextStart = 0;
        bool hasSpeaker = false;   // ContinuousN fixed speaker
        SpeakerId speaker = 0;
        bool chainFresh = true;    // fixed modes: no predecessor to cross-fade with
        bool pairedNext = false;   // next start was pulled forward by onset pairing
    };
    static constexpr std::int64_t kNever = INT64_MIN / 4;
    static constexpr std::int64_t kKeepEnded = 3 * kFs;

    void initStreams();
    void initialPhase();
    void recomputeRates();
    std::int64_t offDuration(Slot& s);
    bool construct(std::uint32_t slot, std::int64_t t, std::uint16_t flags, double residualFrac);
    void commit(PlannedEvent&& pe);
    void pruneLive();
    void tick();
    void addOccupancy(const TalkerEvent& ev, int sign);
    std::int64_t cooldownEnd(const Slot& s) const;
    std::int64_t overlapSamples() const;
    void planStochastic(std::int64_t t);
    void planContinuous(std::int64_t t);
    int speakingOthersAt(std::int64_t t) const;

    std::shared_ptr<const CorpusSnapshot> snap_;
    SegmentSelector& sel_;
    TalkerPlanParams p_;
    std::uint32_t epoch_ = 0;
    std::uint64_t planHash_ = 0;
    std::vector<Slot> slots_;
    RngStream global_{0}, gainvar_{0}, lab_{0};
    std::vector<PlannedEvent> events_;
    std::vector<std::size_t> live_;  // indices of events with end > lastStart_
    std::size_t taken_ = 0;
    std::uint64_t nextId_ = 0;
    std::int64_t lastStart_ = -kGuard;
    std::int64_t floorTime_ = 0;
    LayoutRetention retention_ = LayoutRetention::UntilTaken;
    // Count controller.
    double lambda_ = 1.0, dOnS_ = 5.0, dOffS_ = 2.0;
    std::int64_t nextTick_ = kFs;
    static constexpr std::size_t kRing = 1024;
    std::vector<std::int64_t> occ_ = std::vector<std::int64_t>(kRing, 0);
    // Forced start state.
    bool pendingOverlapValid_ = false;
    std::int64_t pendingOverlap_ = 0;
    std::int64_t forcedBlockedUntil_ = kNever;
    PlannerStats stats_;
};

}  // namespace bf
