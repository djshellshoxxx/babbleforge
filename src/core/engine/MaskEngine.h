#pragma once
// Integrated N-channel mask engine (docs/ENGINE.md §2.2, §3, §6; MASK_STRATEGIES.md §4, §7;
// SPATIAL_ENGINE.md §4, §5; SPECTRUM_ENGINE.md §2.3, §6; REALTIME_ARCHITECTURE.md §8.4).
//
// Signal flow per output channel c (all buses N-channel):
//
//   BabbleEngine talkers ── SpatialPolicy gain vectors ──► babble bus
//     × g_c = g_btrim · ChannelBalance(c)       (slow trim + per-channel balance, smoothed)
//     → babble shaper FIR (T − LTASS_pool + C, babble-mode normalised, 100 ms kernel xfades)
//     → AllpassDecorrelator                                              ─► tap T1
//   StationaryMaskEngine (independent noise.ch.<c> → unit-energy target FIR)  ─► tap T2
//   HybridMixer (per zone: √b·babble + √(1−b)·stationary)                   ─► tap T3
//   strategy crossfader (plan epochs: talker epoch, 2 s b ramp, kernel crossfades, trim freeze)
//   master gain (Strength, one-pole τ = 200 ms per sample) × static gain (lab two-pass)
//   source select (MASKER; calibration path reserved, identity in V1)
//   OutputMatrix (zone/output gain, mute, polarity, delay, CalEQ slot)
//   Limiter (true peak, zone-linked) → safety clip ±1                        ─► tap T4 → out
//
// Determinism (D2): the engine works in cells of at most 256 frames on a grid anchored to the
// absolute sample counter; every sub-component is block-size independent; plan changes are
// applied at their scenario-stamped sample; analysis objects (babble trim, channel balance,
// spectral correction, meters, modulation analyzer) run synchronously at their nominal
// cadence (5 s blocks, 1 s feed-forward balance) and their results are applied at the next
// 256-frame grid boundary. Output is therefore bit-identical for any host block size.
//
// This is the offline (synchronous) engine used by bfrender and the tests: planning, source
// preparation and analysis run inside process(), which may allocate. The real-time host
// (REALTIME_ARCHITECTURE.md §2) moves those parts to their threads; the DSP graph is the same.
//
// The babble part requires fs = 48 kHz (the V1 talker planner clock); the stationary part
// works at every supported rate.
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/analysis/Meters.h"
#include "core/analysis/ModulationAnalyzer.h"
#include "core/analysis/SpectralCorrection.h"
#include "core/analysis/SpectrumAnalyzer.h"
#include "core/config/Types.h"
#include "core/corpus/CorpusSnapshot.h"
#include "core/dsp/AllpassDecorrelator.h"
#include "core/dsp/Limiter.h"
#include "core/dsp/PartitionedConvolver.h"
#include "core/engine/BabbleEngine.h"
#include "core/engine/HybridMixer.h"
#include "core/engine/OutputMatrix.h"
#include "core/engine/StationaryMaskEngine.h"
#include "core/spatial/ChannelBalance.h"
#include "core/spatial/OutputLayout.h"
#include "core/spatial/SpatialPolicy.h"
#include "core/strategy/StrategyTypes.h"

namespace bf {

inline constexpr int kNumTaps = 4;  // T1 babble, T2 stationary, T3 pre-master mix, T4 output

// Receives tap audio (planar, numChannels x n) for every processed cell, in stream order.
class ITapSink {
public:
    virtual ~ITapSink() = default;
    virtual void onTap(int tap, std::int64_t startSample, const float* const* channels, int numChannels, int n) = 0;
};

struct MaskEngineConfig {
    std::uint64_t seed = 1;
    std::shared_ptr<const CorpusSnapshot> corpus;  // null: stationary-only engine
    IAudioSource* audio = nullptr;                  // required with a corpus
    double lRefDbfs = -26.0;

    // Output matrix settings (index = logical output; missing entries use defaults).
    std::vector<OutputChannel> channels;
    std::vector<OutputZone> zones;

    // Limiter: `limiterStage = false` removes the limiter AND the safety clip (float research
    // renders, MASK_STRATEGIES §8.2). limiterOverride replaces plan.level.limiterEnabled.
    bool limiterStage = true;
    std::optional<bool> limiterOverride;

    // Babble level loops (ENGINE.md §3.2, SPATIAL_ENGINE.md §4).
    bool babbleTrim = true;
    bool channelBalance = true;

    // Spectral correction (SPECTRUM_ENGINE.md §6).
    bool correctionEnabled = true;
    CorrectionSpeed correctionSpeed = CorrectionSpeed::Normal;
    bool correctionStrict = false;                // LaboratoryMask pre-roll: tau_c = 20 s
    std::optional<OperatingBands> initialCorrection;
    bool freezeCorrection = false;                // keep C constant (Strict render pass)
    std::optional<ThirdOctArray> poolLtassDb;     // override of the measured pool LTASS

    double staticGainDb = 0.0;                    // two-pass RMS normalisation gain
    double babbleProbeSeconds = 900.0;            // planned-timeline probe for g_bnorm
};

struct MaskStatistics {
    double fs = 48000.0;
    int numChannels = 0;
    std::int64_t samples = 0;
    double seconds = 0.0;
    int latencySamples = 0;
    std::uint64_t planChanges = 0;
    std::uint64_t currentPlanId = 0;

    // Whole-run levels (dBFS RMS, energy mean over channels unless per channel).
    std::vector<double> outputRmsChDb;
    double outputRmsDb = -200.0;
    double babbleRmsDb = -200.0, stationaryRmsDb = -200.0, mixRmsDb = -200.0;
    std::vector<double> babbleRmsChDb;

    // Meters on T4 (ENGINE.md §5).
    double lufsS = -200.0, lufsI = -200.0, leq60Db = -200.0;
    double truePeakMaxDb = -200.0, samplePeakMaxDb = -200.0;
    double crestDb = 0.0;  // true-peak max - RMS, whole run

    // Mix (MASK_STRATEGIES §4.2).
    double configuredBabbleFraction = 0.0;
    double measuredBabbleFraction = 0.0;  // babble energy share of T3, whole run

    // Talkers (ground truth from the voice renderer).
    bool babbleActive = false;
    double meanActive = 0.0, meanSpeaking = 0.0;
    int minActive = 0, maxActive = 0;
    std::uint64_t occupancyFrames = 0;
    double countNorm = 1.0;
    double babbleTrimDb = 0.0;
    std::vector<double> channelBalanceDb;

    // Temporal statistics of T3 (SPECTRUM_ENGINE.md §8).
    std::size_t gapCount = 0;
    double gapMedianS = 0.0, gapP95S = 0.0, gapMaxS = 0.0, gapRatePerS = 0.0;
    double envelopeL10L90Db = 0.0;  // whole run, 10 ms frames
    double crest60sDb = 0.0;        // L10 - L90 of the last 60 s
    double modulationDepth10s = 0.0;
    bool haveModulation = false;
    ModBandArray modulation{};

    // Spectrum (shape deviations, dB; 1/3 octave 125 Hz..8 kHz and octave 125..8k).
    bool haveBabbleSpectrum = false;
    double babbleThirdOctMaxDevDb = 0.0, babbleOctaveMaxDevDb = 0.0;
    bool haveStationarySpectrum = false;
    double stationaryThirdOctMaxDevDb = 0.0, stationaryOctaveMaxDevDb = 0.0;
    OperatingBands correctionDb{};
    std::uint64_t babbleKernelDesigns = 0;
    bool eqClamped = false;

    // Limiter / output safety.
    bool limiterEnabled = false;
    double limiterGrMaxDb = 0.0;
    double limiterActiveFraction = 0.0;    // fraction of samples in cells with GR > 0
    double limiterAbove05Fraction = 0.0;   // fraction of samples with GR > 0.5 dB
    std::uint64_t clipEvents = 0;

    // Reliability.
    std::uint64_t underflows = 0, droppedEvents = 0;
    bool sourceErrors = false;
    bool babbleUnavailable = false;  // a plan needed babble but no corpus / fs != 48 kHz
};

nlohmann::json toJson(const MaskStatistics& s);

// Active-speech-weighted pool LTASS shape (SPECTRUM_ENGINE.md §2.3), measured from the audio of
// the given speakers (up to 4 anchors x 3 s each, every speaker normalised to unit power).
// Result: 26 band levels in dB, 1 kHz band = 0 dB.
ThirdOctArray estimatePoolLtassDb(const CorpusSnapshot& snap, IAudioSource& audio,
                                  const std::vector<SpeakerId>& speakers, bool* readFailure = nullptr);

class MaskEngine {
public:
    static constexpr int kCell = 256;

    explicit MaskEngine(MaskEngineConfig cfg);
    ~MaskEngine();
    MaskEngine(const MaskEngine&) = delete;
    MaskEngine& operator=(const MaskEngine&) = delete;

    // Allocates the graph for `layout` (N = layout.size() outputs). maxBlock: largest host block.
    bool prepare(double fs, const OutputLayout& layout, int maxBlock, std::string* error = nullptr);
    // Schedules a plan at an absolute sample (>= current position). The first plan must take
    // effect at the current position before the first process() call.
    bool setPlan(const MaskRenderPlan& plan, std::int64_t effectiveSample, std::string* error = nullptr);
    // out[c], c < N. Any nFrames >= 0.
    void process(float* const* out, int nFrames);

    void setTapSink(ITapSink* sink) noexcept { tapSink_ = sink; }

    MaskStatistics statistics() const;
    std::int64_t position() const noexcept { return pos_; }
    int numChannels() const noexcept { return nCh_; }
    double sampleRate() const noexcept { return fs_; }
    int latencySamples() const noexcept;
    const BabbleEngine* babble() const noexcept { return babble_.get(); }
    std::int64_t babbleStartSample() const noexcept { return babbleStart_; }
    const OperatingBands& correction() const noexcept { return correction_.correction(); }
    const ThirdOctArray& poolLtassDb() const noexcept { return poolLtassDb_; }
    bool sourceFailure() const noexcept { return babble_ && (babble_->sourceErrors() || poolReadFailure_); }
    const MaskRenderPlan* currentPlan() const noexcept { return current_ ? &*current_ : nullptr; }
    // Plan epochs: [{planId, sample, strategy, planHash, babbleFraction, strengthDb}].
    nlohmann::json planHistoryJson() const;
    // Talker events (D1 timeline) with absolute engine sample times, events starting < endSample.
    nlohmann::json eventsJson(std::int64_t endSample) const;

private:
    struct Pending { MaskRenderPlan plan; std::int64_t at; };

    void processCell(float* const* out, int offset, int n);
    void applyPlan(const MaskRenderPlan& plan, std::int64_t at);
    bool createBabble(const MaskRenderPlan& plan, std::int64_t at);
    void designStationary(const MaskRenderPlan& plan, std::int64_t at);
    void designBabbleKernel(std::int64_t at);
    void placeNewTalkers(std::int64_t cellStart);
    void updateMotion(std::int64_t dt);
    void feedForwardBalance();
    void endOfBlock();              // 5 s analysis block (trim, balance feedback)
    void correctionStep(const SpectrumBlock& blk);
    void applyStamped();
    std::int64_t nextSplit(std::int64_t t) const;
    bool crossfading(std::int64_t t) const noexcept { return t < crossfadeUntil_; }

    MaskEngineConfig cfg_;
    double fs_ = 48000.0;
    int nCh_ = 0;
    OutputLayout layout_;
    std::vector<int> zoneOf_;
    bool prepared_ = false;

    // plans
    std::vector<Pending> pending_;  // sorted by `at`
    std::optional<MaskRenderPlan> current_;
    std::uint64_t planCounter_ = 0;
    nlohmann::json history_ = nlohmann::json::array();
    std::map<std::uint64_t, double> trimMemory_;  // talker-plan hash -> trim dB
    std::uint64_t currentTalkerHash_ = 0;

    // babble
    std::unique_ptr<BabbleEngine> babble_;
    std::int64_t babbleStart_ = 0;
    std::unique_ptr<SpatialPolicy> spatial_;
    std::size_t eventCursor_ = 0;
    std::vector<std::uint8_t> slotPlaced_;     // slot carries a talker now
    std::vector<std::uint8_t> slotUsed_;       // slot has a placement (current or last)
    std::vector<std::int64_t> slotEnd_;       // babble-time end of the slot's current event
    ChannelBalance balance_;
    std::vector<std::unique_ptr<PartitionedConvolver>> babbleConv_;
    AllpassDecorrelator decorrelator_;
    SpeakerVariation variation_ = SpeakerVariation::Medium;
    ThirdOctArray poolLtassDb_{};
    bool poolReadFailure_ = false;
    bool eqClamped_ = false;
    std::uint64_t babbleDesigns_ = 0;
    bool babbleUnavailable_ = false;

    // stationary
    StationaryMaskEngine stationary_;

    // mix / master / output
    HybridMixer mixer_;
    double masterCur_ = 1.0, masterTarget_ = 1.0, masterCoef_ = 0.0, staticGain_ = 1.0;
    OutputMatrix matrix_;
    Limiter limiter_;
    bool limiterEnabled_ = true;
    std::int64_t crossfadeUntil_ = 0;

    // babble gain stage (per channel): target applied at grid boundaries, one-pole per sample.
    std::vector<double> gainCur_, gainTarget_, gainStamped_;
    bool gainStampPending_ = false;
    double gainCoef_ = 0.0;
    double trimDb_ = 0.0;
    std::int64_t trimFrozenUntil_ = 0;

    // analysis
    SpectrumAnalyzer babbleAnalyzer_, stationaryAnalyzer_;
    SpectralCorrection correction_;
    ThirdOctArray referenceDb_{};  // ideal band levels of the target (incl. HP/LP)
    bool haveReference_ = false;
    Meters meters_;
    ModulationAnalyzer modulation_{false};
    std::vector<std::uint64_t> levelHist_;  // 10 ms frame levels of T3, 0.1 dB bins
    int frameLen_ = 480, framePos_ = 0;
    double frameAcc_ = 0.0;
    std::int64_t blockLen_ = 240000, nextBlock_ = 240000;
    std::int64_t ffLen_ = 48000, nextFf_ = 48000;
    std::int64_t motionLen_ = 12288, nextMotion_ = 12288;
    std::vector<double> blockPowT1_;        // current 5 s block, per channel
    std::vector<std::vector<double>> recentT1_;  // last two blocks per channel
    std::int64_t blockN_ = 0;

    // whole-run accumulators
    std::vector<double> sumT1_, sumT2_, sumT3_, sumT4_;
    double babbleInMix_ = 0.0;
    double configuredB_ = 0.0;
    std::uint64_t grActiveSamples_ = 0;

    // buffers (N x kCell)
    std::vector<std::vector<float>> bab_, sta_, mix_, dev_;
    std::vector<float*> pBab_, pSta_, pMix_, pDev_, pOut_;

    ITapSink* tapSink_ = nullptr;
    std::int64_t pos_ = 0;
};

}  // namespace bf
