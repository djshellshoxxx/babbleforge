#pragma once
// Mask strategy layer types (docs/MASK_STRATEGIES.md §2, ENGINE.md §2.3, RELIABILITY.md §3-4).
// Control-thread only. A MaskRenderPlan is an immutable value produced by a MaskStrategy.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/config/Types.h"
#include "core/spatial/SpatialPolicy.h"
#include "core/spectrum/SpectrumTarget.h"
#include "core/talker/TalkerPlanParams.h"

namespace bf {

class CorpusSnapshot;

enum class StrategyId : std::uint8_t { Balanced, Natural, Dense, SpeechNoise, MultiVoice, Hybrid, Research };
enum class StrategyClass : std::uint8_t {
  StationarySpeechMask,
  MultiVoiceMask,
  BabbleMask,
  HybridMask,
  NaturalBabbleMask,
  LaboratoryMask
};
enum class FallbackPolicy : std::uint8_t { Strict, Safe, Continuous };
// LaboratoryMask sub-modes (MASK_STRATEGIES.md §8.1).
enum class LabSubMode : std::uint8_t { ContinuousN, Stochastic, Ssn, Pink, Hybrid, LtassMatchedBabble };

inline constexpr std::size_t kMaxZones = 8;

std::string_view toString(StrategyId) noexcept;
std::string_view toString(StrategyClass) noexcept;
std::string_view toString(FallbackPolicy) noexcept;
std::string_view toString(LabSubMode) noexcept;
std::optional<StrategyId> strategyIdFromString(std::string_view) noexcept;
std::optional<StrategyClass> strategyClassFromString(std::string_view) noexcept;
std::optional<FallbackPolicy> fallbackPolicyFromString(std::string_view) noexcept;
std::optional<SpeakerVariation> speakerVariationFromString(std::string_view) noexcept;

// Capability flags (MASK_STRATEGIES.md §2.3).
struct StrategyCaps {
  bool needsCorpus = true;
  bool exposesMixSlider = false;
  bool exposesCharacter = false;
  bool exposesVoiceAmount = false;
  bool lockedForResearch = false;
  int minCorpusSpeakers = 0;  // for the strategy's default parameters; see minCorpusSpeakersFor()
};

// Macro state (GUI macros). nullopt = default (strategy default, else Area default).
// For LaboratoryMask an explicitly set clearVoiceReduction enables CVR (§6.2).
struct MacroState {
  std::optional<double> voiceAmount, character, clearVoiceReduction;
};

// User / advanced parameters (PRESETS.md §2 step 5, MASK_STRATEGIES.md §10). nullopt = derive.
struct StrategyParams {
  // talkers
  std::optional<int> pool, minActive, maxActive;
  std::optional<double> meanActive;
  std::optional<int> multiVoiceK;
  std::optional<double> maxInternalGapMs, segmentMinS, segmentMaxS, segmentMedianS, gainVariationDb, fadeInMs,
      fadeOutMs, reEntryCooldownS;
  std::optional<std::string> voiceDiversity;  // low | balanced | high | matched
  std::optional<bool> languageAware;
  // mix (user-set b locks the mix: Δs and the CVR stationary minimum are not applied)
  std::optional<double> babbleFraction;
  std::vector<double> zoneBabbleOffsets;  // index = zone id (0..7)
  // spectrum
  std::optional<std::string> spectrumTarget;
  std::optional<SpectrumTargetDef> customTarget;
  std::optional<double> lfLimitHz, hfLimitHz, lfTrimDb125;
  std::optional<bool> stationaryEnabled;
  // spatial
  std::optional<std::string> spatialAlgorithm;  // auto | distributed | mono | stereo | small | large
  std::optional<double> spread, motion;
  std::optional<std::string> speakerVariation;
  std::optional<int> neighborhoodSize;
  // level
  std::optional<double> strengthDb;
  std::optional<bool> limiterEnabled;
  std::optional<double> limiterCeilingDbtp;
  // reliability / determinism
  std::optional<FallbackPolicy> fallback;
  std::optional<std::uint64_t> seed;
  // LaboratoryMask
  LabSubMode labMode = LabSubMode::Stochastic;
  std::optional<int> labN;
};

// What the strategy may assume about the corpus.
struct CorpusSummary {
  int availableSpeakers = 0;  // healthy speakers
  std::string corpusVersion;
  bool present() const noexcept { return availableSpeakers > 0; }
  static CorpusSummary from(const CorpusSnapshot& snap);
};

struct ValidationResult {
  std::vector<Adjustment> adjustments;
  std::vector<std::string> conflicts;  // e.g. CVR floor lowered
  bool changed() const noexcept { return !adjustments.empty(); }
};

struct DegradeReason {
  enum class Kind : std::uint8_t {
    CorpusInsufficient,  // fewer available speakers than the plan needs (RELIABILITY §3)
    CorpusNone,          // no usable corpus
    SourceFailures,      // individual source failures; failedPoolFraction is meaningful
  };
  Kind kind = Kind::CorpusInsufficient;
  int availableSpeakers = 0;
  double failedPoolFraction = 0.0;
};

struct StationaryParams {
  bool enabled = true;       // generator audible (b < 1) or kept hot for fallback
  bool keepHot = false;      // running at gain 0 so that fallback is instantaneous (§4.2)
  std::string spectrumId;    // target id (shared with the babble EQ)
  std::string seedStream = "noise.ch";
};

struct MixParams {
  double babbleFraction = 0.7;                // strategy b after macros (and degradation)
  double baseBabbleFraction = 0.7;            // strategy b before the Character/CVR macros
  std::array<double, kMaxZones> zoneBabbleFraction{};  // b + zone offset, clamped [0, 1]
  bool userLocked = false;                    // explicit mix slider: Δs / CVR minimum ignored
  double stationaryCompensation = 0.0;        // added by degrade() (RELIABILITY §3)
  double stationaryFraction() const noexcept { return 1.0 - babbleFraction; }
};

struct SpatialPlanParams {
  std::string algorithmRequest = "auto";      // as configured
  SpatialAlgorithm algorithm = SpatialAlgorithm::DistributedStereo;  // resolved for the layout
  SpatialParams params;                       // spread, motion, speakerVariation, k_n
  int activeOutputs = 0;
};

struct LevelParams {
  double lRefDbfs = -26.0;
  double strengthDb = 0.0;
  bool limiterEnabled = true;
  double limiterCeilingDbtp = -1.0;
  double outputRmsDbfs() const noexcept { return lRefDbfs + strengthDb; }
};

struct SelectorHints {
  std::string diversity = "high";
  bool languageAware = false;
  bool poolRotation = true;
  bool relaxRecentSpeakerRule = false;  // reduced plan (RELIABILITY §3)
  double soloRiskWeight = 1.0;
};

struct PlanStatus {
  bool runnable = true;
  bool error = false;                    // Strict fallback: enter ERROR, no audio
  std::string errorCode;                 // e.g. "corpus.insufficient"
  std::vector<std::string> degraded;     // DEGRADED reason codes
};

// ENGINE.md §2.3: immutable, built on the control thread, fully concrete.
struct MaskRenderPlan {
  std::uint64_t planId = 0;  // assigned by the engine when issued; not part of planHash
  StrategyId strategyId = StrategyId::Balanced;
  StrategyClass strategyClass = StrategyClass::HybridMask;
  std::string areaId;

  // Effective macro values (after defaults, forced ranges, Laboratory rules).
  double voiceAmount = 0.5, character = 0.5, clearVoiceReduction = 1.0;

  bool babbleEnabled = true;
  TalkerPlanParams talkers;
  double levelVarTruncationSigma = 2.0;
  double segmentLevelMaxDb() const noexcept {
    return levelVarTruncationSigma * talkers.gainSigmaDb * talkers.cvrLevelSigmaMult;
  }
  LabSubMode labMode = LabSubMode::Stochastic;  // LaboratoryMask only
  bool strictSpectralMatch = false;             // ltassMatchedBabble: correction converged then frozen
  SelectorHints selector;
  int speakersNeeded = 0;                       // RELIABILITY §3

  StationaryParams stationary;
  SpectrumTarget target;
  MixParams mix;
  SpatialPlanParams spatial;
  LevelParams level;
  FallbackPolicy fallback = FallbackPolicy::Continuous;

  PlanStatus status;
  std::vector<Adjustment> adjustments;  // macro/plan-time clamps and conflicts (not hashed)

  // Stable hash of the canonical content (everything except planId, status notes,
  // adjustments and the talker seed), for correction memory.
  std::uint64_t planHash = 0;
  std::string canonicalJson() const;
  std::uint64_t computeHash() const;
  void rehash() { planHash = computeHash(); }
};

}  // namespace bf
