#pragma once
// Plain configuration data types. No JSON types appear in this header.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bf {

// Recorded whenever a value is changed to satisfy a range or invariant.
struct Adjustment {
  std::string fieldPath;
  double from = 0.0;
  double to = 0.0;
  std::string reason;
};

// ---- Data-set (resources/data) types --------------------------------------

struct AreaAdvisory {
  std::string when;
  std::string text;
};

struct AreaModel {
  std::string id, displayName, description, evidenceLabel, notes;
  struct Talkers {
    double meanActive = 0;
    int pool = 0, minActive = 0, maxActive = 0;
    std::vector<double> recommendedRange;
  } talkers;
  double balancedStationaryFraction = 0.0;
  struct Macros {
    double character = 0.5, voiceAmount = 0.5, clearVoiceReduction = 1.0;
    std::string voiceDiversity = "high";
  } macros;
  struct Spectrum {
    std::string target;
    double lfLimitHz = 80, lfTrimDb125 = 0;
    bool useSpeechMatchedIfAvailable = false;
  } spectrum;
  struct Spatial {
    std::string algorithm = "auto";
    double spread = 0.5, motion = 0.3;
    std::string speakerVariation = "medium";
    int neighborhoodSize = 3;
  } spatial;
  struct Outputs {
    int minRecommended = 2, minWithoutAdvisory = 1;
  } outputs;
  struct Level {
    double defaultStrengthDb = 0.0, maxSimpleStrengthDb = 9.0;
  } level;
  std::vector<AreaAdvisory> advisories;
};

struct StrategyDef {
  std::string id, klass, displayName, descriptionSimple, evidenceLabel;
  struct Overrides {
    std::optional<double> babbleFraction;
    std::optional<double> character;
  } overrides;
  struct Forced {
    std::optional<std::pair<double, double>> characterRange;
  } forced;
};

struct CharacterAnchor {
  double c = 0;
  std::string label;
  double meanActiveFactor = 1, maxInternalGapMs = 0, segmentDurationMedianS = 0, segmentDurationSigmaLn = 0,
         perSegmentLevelSigmaDb = 0, reEntryCooldownS = 0, overlapOnHandoverMs = 0,
         stationaryFractionOffset = 0, spatialMotionRate = 0, fadeInMs = 0, fadeOutMs = 0;
  std::string interp;
};

struct CvrMapping {
  double r = 0;
  std::string label;
  double minActiveFloor = 0, dominanceCapDb = 0, levelSigmaMultiplier = 1, forcedOverlapAtHandoverMs = 0,
         onsetMaskingWindowMs = 0, segmentSelectionWeightForSoloRisk = 0, minimumStationaryFraction = 0,
         maxPhraseContinuityS = 0;  // -1 means "unlimited"/unset in the data files
};

struct VoiceAmountAnchor {
  double v = 0;
  std::string label;
  std::optional<double> meanActiveTalkers;  // nullopt: taken from the Area
  bool fromArea = false;
  std::string interp;
};

struct SpectrumTargetDef {
  std::string id, displayName, notes;
  std::vector<double> bandCentersHz;
  std::vector<double> levelsDb;
};

struct EngineDefaults {
  double lRefDbfs = -26.0;
  double simpleMinDb = -18.0, simpleMaxDb = 9.0;
  std::map<std::string, double> strengthLabels;
  double advancedMinDb = -40.0, advancedMaxDb = 12.0;
  double limiterCeilingDbtp = -1.0;
  std::string fallbackPolicy = "continuous";
  std::map<std::string, double> correctionTimeConstantS;
  double maxCorrectionDb = 6.0;
  double lfLimitHz = 80.0;
};

// ---- Preset types (PRESETS.md §6.2) ----------------------------------------

struct PresetBasedOn {
  std::optional<std::string> area, strategy, factoryPresetVersion;
};

struct PresetMacros {
  std::optional<double> strengthDb, character, voiceAmount, clearVoiceReduction;
  std::optional<std::string> voiceDiversity;
  std::optional<double> babbleFraction;  // macros.mix.babbleFraction
};

struct PresetTalkers {
  std::optional<int> pool;
  std::optional<double> meanActive;
  std::optional<int> minActive, maxActive;
  std::optional<double> segmentMinS, segmentMaxS, segmentMedianS, maxInternalGapMs, gainVariationDb, fadeInMs,
      fadeOutMs, reEntryCooldownS, segmentCooldownMin, poolRotationMin;
  std::optional<bool> languageAware;
  std::optional<std::string> targetVoiceProfile;
  std::optional<int> multiVoiceK;
};

struct PresetStationary {
  std::optional<bool> enabled;
  std::optional<std::string> spectrum, seedMode;
};

struct PresetCorrection {
  std::optional<bool> enabled;
  std::optional<std::string> speed;
  std::optional<double> maxDb;
  std::optional<bool> rememberCorrection;
};

struct PresetSpectrum {
  std::optional<std::string> target, speechMatchedProfile;
  std::optional<SpectrumTargetDef> custom;
  std::optional<double> lfLimitHz, hfLimitHz, lfTrimDb125;
  PresetCorrection correction;
};

struct PresetSpatial {
  std::optional<std::string> algorithm;
  std::optional<double> spread, motion;
  std::optional<std::string> speakerVariation;
  std::optional<int> neighborhoodSize;
};

struct OutputChannel {
  int index = 0, deviceChannel = 0;
  std::string label;
  double azimuthDeg = 0.0;
  int zone = 0;
  bool enabled = true;
  double gainDb = 0.0;
  bool mute = false, polarityInvert = false;
  double delayMs = 0.0;
  std::optional<std::string> calEqProfile;
};

struct OutputZone {
  int id = 0;
  std::string name;
  bool enabled = true;
  double levelDb = 0.0, babbleFractionOffset = 0.0;
};

struct PresetOutputs {
  bool rememberDevice = false;
  std::optional<std::string> device, layout;
  std::vector<OutputChannel> channels;
  std::vector<OutputZone> zones;
  std::optional<bool> limiterEnabled;
  std::optional<double> limiterCeilingDbtp;
};

struct Preset {
  std::string schema = "babbleforge.preset";
  std::string schemaVersion = "1.0";
  std::string name;
  std::optional<PresetBasedOn> basedOn;
  std::string created, appVersion, notes;
  std::string area, strategy;
  PresetMacros macros;
  PresetTalkers talkers;
  PresetStationary stationary;
  PresetSpectrum spectrum;
  PresetSpatial spatial;
  PresetOutputs outputs;
  std::optional<std::int64_t> seed;
  bool deterministic = false;
  std::optional<std::string> fallbackPolicy;
  std::string contentHash;  // as read from the file; serialize() always writes a fresh one

  // Original document (canonical JSON) from which unknown fields and "x-" keys,
  // and the opaque "overrides" block, are preserved on re-serialization.
  // Empty for presets built in code.
  std::string rawJson;
};

}  // namespace bf
