#pragma once
// Offline synchronous babble engine: TalkerPlanner + SegmentSelector + SourcePreparer +
// VoiceRenderer (docs/TALKER_ENGINE.md §2, ENGINE.md §3.2).
//
// Runs at any supported engine rate (cfg.fs: 44.1 / 48 / 88.2 / 96 kHz): positions, plan
// changes and the planner clock are engine samples; the 48 kHz corpus audio is resampled per
// event at preparation time (SourcePreparer).
//
// render() plans ahead (horizon 6 s), prepares each event's processed audio synchronously
// into block chains just before it is needed, and renders the babble bus. Output is
// deterministic and bit-identical for any sequence of render() block sizes (D2): scheduled
// plan changes split the block at their sample, and planning never runs past the freeze
// point of a pending plan change (so a re-plan never discards already-planned events).
//
// Level: talkers are ASL-normalised to busLevelDbfs by the planner; the bus is scaled by the
// feed-forward g_bnorm = 1/sqrt(F), F = planned E[k_speech] incl. level variation and fades
// (TalkerPlanner::estimateBusPowerFactor). Optional slow trim (ENGINE.md §3.2 item 3),
// computed offline from the rendered bus at a 5 s cadence (REALTIME §8.4).
//
// External-feed mode (cfg.externalFeed, the real-time host): render() only runs the RT voice
// renderer; planning, event arming and source preparation are done by the host's planner and
// preload threads through planner(), selector(), pool(), chain() and renderer(), and the
// occupancy statistics are consumed by the analysis thread (consumeOccupancy()).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "core/corpus/CorpusSnapshot.h"
#include "core/talker/SegmentSelector.h"
#include "core/talker/SourcePreparer.h"
#include "core/talker/TalkerPlanner.h"
#include "core/talker/VoiceRenderer.h"

namespace bf {

struct BabbleEngineConfig {
    TalkerPlanParams plan;
    DiversityMode diversity = DiversityMode::High;
    TargetVoiceProfile target;
    bool languageAware = false;
    double rotationPeriodS = 1200.0;
    double segCooldownOverrideS = -1.0;
    std::size_t numChannels = 1;
    double busLevelDbfs = -26.0;       // L_ref (also the per-talker reference)
    std::size_t blockPoolBlocks = 0;   // 0 = automatic
    double horizonS = 6.0;
    double probeSeconds = 1800.0;      // planned-timeline probe for g_bnorm
    bool trimEnabled = false;
    bool retainLayouts = false;        // diagnostics: keep every event's layout in the planner
    bool externalFeed = false;         // real-time host: see the header comment
    double fs = 48000.0;               // engine rate (isSupportedBabbleRate()); planner clock unit
};

struct BabbleEngineStats {
    std::uint64_t frames = 0;          // occupancy frames (one per 256 samples)
    double sumActive = 0.0, sumSpeaking = 0.0;
    std::uint32_t minActive = 0xFFFF, maxActive = 0;
    double meanActive() const { return frames ? sumActive / static_cast<double>(frames) : 0.0; }
    double meanSpeaking() const { return frames ? sumSpeaking / static_cast<double>(frames) : 0.0; }
};

class BabbleEngine {
public:
    BabbleEngine(std::shared_ptr<const CorpusSnapshot> snap, IAudioSource& source,
                 const BabbleEngineConfig& cfg);
    ~BabbleEngine();

    // out[c] for c < nCh (must equal cfg.numChannels); overwrites.
    void render(float* const* out, int nCh, int nFrames);
    // Plan change at an absolute sample (>= current position).
    void scheduleReplan(const TalkerPlanParams& params, std::int64_t atSample);
    // Per-slot channel gain vector from the Spatial Renderer (VoiceRenderer::setGains). For
    // deterministic output call it between render() calls at fixed sample positions.
    void setSlotGains(std::uint32_t slot, const float* gains, std::size_t n) noexcept {
        renderer_.setGains(slot, gains, n);
    }

    std::int64_t position() const noexcept { return pos_; }
    const TalkerPlanner& planner() const noexcept { return *planner_; }
    const SegmentSelector& selector() const noexcept { return *selector_; }
    const BabbleEngineStats& stats() const noexcept { return stats_; }
    double countNorm() const noexcept { return gbnorm_; }
    double trimDb() const noexcept { return trimDb_; }
    std::uint64_t underflows() const noexcept { return renderer_.underflows(); }
    std::uint64_t droppedEvents() const noexcept { return renderer_.droppedEvents(); }
    bool sourceErrors() const noexcept { return sourceErrors_.load(std::memory_order_relaxed); }

    // ---- external-feed mode (real-time host) ----
    // Planner thread: planner / selector / renderer producer side; chains and the block pool are
    // shared with the preload threads (the host serialises pool access).
    TalkerPlanner& plannerMutable() noexcept { return *planner_; }
    SegmentSelector& selectorMutable() noexcept { return *selector_; }
    VoiceRenderer& renderer() noexcept { return renderer_; }
    BlockPool& pool() noexcept { return *pool_; }
    std::size_t numChains() const noexcept { return chainStore_.size(); }
    BlockChain* chain(std::size_t i) noexcept { return chainStore_[i].get(); }
    const BabbleEngineConfig& config() const noexcept { return cfg_; }
    // Planner thread: re-plan now (external-feed mode); updates g_bnorm.
    std::vector<std::uint64_t> applyReplanNow(const TalkerPlanParams& params, std::int64_t atSample);
    // Planner thread (external-feed mode): switches to a new corpus snapshot at `atSample`: the
    // selector is rebuilt over the snapshot with the surviving state migrated, the planner re-plans
    // from the new pool (TalkerPlanner::adoptSnapshot), the count normalisation is refreshed.
    // Returns the discarded (not yet started) event ids like applyReplanNow().
    std::vector<std::uint64_t> adoptCorpus(std::shared_ptr<const CorpusSnapshot> snap, const CorpusMigration& map,
                                           std::int64_t atSample);
    const CorpusSnapshot& snapshot() const noexcept { return *snap_; }
    void markSourceError() noexcept { sourceErrors_.store(true, std::memory_order_relaxed); }
    // Analysis thread: pops the renderer's occupancy frames into stats().
    void consumeOccupancy() noexcept;

private:
    struct Inflight {
        PlannedEvent pe;
        BlockChain* chain = nullptr;
        std::int64_t chainOffset = 0;
    };
    void step(float* const* out, std::size_t n);
    void applyReplan();
    void updateTrim(float* const* out, std::size_t n);
    double computeCountNorm() const;

    std::shared_ptr<const CorpusSnapshot> snap_;
    BabbleEngineConfig cfg_;
    std::unique_ptr<SegmentSelector> selector_;
    std::unique_ptr<TalkerPlanner> planner_;
    SourcePreparer preparer_;
    std::unique_ptr<BlockPool> pool_;
    std::vector<std::unique_ptr<BlockChain>> chainStore_;
    std::vector<BlockChain*> freeChains_;
    VoiceRenderer renderer_;
    std::deque<PlannedEvent> pending_;
    std::vector<Inflight> inflight_;
    std::vector<PlannedEvent> scratch_;
    std::vector<float> scratchAudio_;
    struct Replan { TalkerPlanParams params; std::int64_t at; };
    std::deque<Replan> replans_;
    std::int64_t pos_ = 0;
    std::int64_t fs_ = 48000, trimCadence_ = 5 * 48000, trimFreeze_ = 10 * 48000;
    double gbnorm_ = 1.0;
    std::atomic<bool> sourceErrors_{false};
    std::vector<float*> ptrs_;
    BabbleEngineStats stats_;
    // Offline slow trim state.
    double trimDb_ = 0.0, trimAcc_ = 0.0;
    std::int64_t trimAccN_ = 0, trimFrozenUntil_ = 0, nextTrimUpdate_ = 0;
    std::deque<double> trimWindows_;
};

}  // namespace bf
