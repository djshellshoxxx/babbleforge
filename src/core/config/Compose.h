#pragma once
#include <optional>
#include <string>
#include <vector>

#include "core/config/DataSet.h"
#include "core/config/Preset.h"

namespace bf {

// Fully resolved parameters (no nulls) after PRESETS.md §2 steps 1-3, 5, 6.
struct EffectiveConfig {
  std::string areaId, strategyId, strategyClass;

  double strengthDb = 0.0;
  double character = 0.5, voiceAmount = 0.5, clearVoiceReduction = 1.0;
  std::string voiceDiversity = "high";
  double babbleFraction = 0.7;  // stationary fraction = 1 - babbleFraction
  double stationaryFraction() const { return 1.0 - babbleFraction; }

  int pool = 0, minActive = 0, maxActive = 0;
  double meanActive = 0.0;
  std::optional<double> segmentMinS, segmentMaxS, segmentMedianS, maxInternalGapMs, gainVariationDb, fadeInMs,
      fadeOutMs, reEntryCooldownS, segmentCooldownMin, poolRotationMin;
  bool languageAware = false;
  std::optional<std::string> targetVoiceProfile;
  std::optional<int> multiVoiceK;

  bool stationaryEnabled = true;
  std::string stationarySpectrum, seedMode = "session";

  std::string spectrumTarget;
  std::optional<std::string> speechMatchedProfile;
  std::optional<SpectrumTargetDef> customTarget;
  bool useSpeechMatchedIfAvailable = false;
  double lfLimitHz = 80, hfLimitHz = 12500, lfTrimDb125 = 0;
  bool correctionEnabled = true;
  std::string correctionSpeed = "normal";
  double correctionMaxDb = 6.0;
  bool rememberCorrection = true;

  std::string spatialAlgorithm = "auto";
  double spread = 0.5, motion = 0.3;
  std::string speakerVariation = "medium";
  int neighborhoodSize = 3;

  std::vector<OutputChannel> channels;
  std::vector<OutputZone> zones;
  bool limiterEnabled = true;
  double limiterCeilingDbtp = -1.0;
  std::string fallbackPolicy = "continuous";

  std::optional<std::int64_t> seed;
  bool deterministic = false;
};

struct ComposeResult {
  bool ok = false;
  std::string error;  // e.g. unknown area / strategy id
  EffectiveConfig config;
  std::vector<Adjustment> adjustments;
};

// compose() resolves PRESETS.md §2 steps 1-3, 5 and 6 into plain values. It does NOT apply
// step 4 (macros: Voice Amount -> Character -> Clear Voice Reduction, MASK_STRATEGIES.md
// §5, §6, §9): `character`, `voiceAmount`, `clearVoiceReduction` are the resolved macro
// VALUES and talker counts are the pre-macro Area/user values. The final, macro-applied
// parameters are produced by MaskStrategy::buildPlan(); bf::composePlan()
// (core/strategy/PlanComposer.h) runs compose + validate + buildPlan end to end.
ComposeResult compose(const DataSet& ds, const Preset& preset);

}  // namespace bf
