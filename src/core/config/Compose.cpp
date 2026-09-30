#include "core/config/Compose.h"

#include <algorithm>
#include <cmath>

namespace bf {
namespace {

struct Clamper {
  std::vector<Adjustment>& adj;
  template <class T>
  void operator()(T& v, double lo, double hi, const char* path) {
    double d = static_cast<double>(v);
    double c = std::min(std::max(d, lo), hi);
    if (c != d) {
      adj.push_back({path, d, c, "out of range in composed configuration"});
      v = static_cast<T>(c);
    }
  }
  template <class T>
  void opt(std::optional<T>& v, double lo, double hi, const char* path) {
    if (v) (*this)(*v, lo, hi, path);
  }
};

template <class T>
void assign(T& dst, const std::optional<T>& src) {
  if (src) dst = *src;
}

}  // namespace

ComposeResult compose(const DataSet& ds, const Preset& p) {
  ComposeResult r;
  auto ai = ds.areas.find(p.area);
  if (ai == ds.areas.end()) {
    r.error = "area: unknown area \"" + p.area + "\"";
    return r;
  }
  auto si = ds.strategies.find(p.strategy);
  if (si == ds.strategies.end()) {
    r.error = "strategy: unknown strategy \"" + p.strategy + "\"";
    return r;
  }
  const AreaModel& area = ai->second;
  const StrategyDef& strat = si->second;
  const EngineDefaults& ed = ds.engineDefaults;
  EffectiveConfig& c = r.config;
  Clamper clamp{r.adjustments};

  // Step 1: engine built-in fallbacks.
  c.limiterCeilingDbtp = ed.limiterCeilingDbtp;
  c.fallbackPolicy = ed.fallbackPolicy;
  c.lfLimitHz = ed.lfLimitHz;
  c.correctionMaxDb = ed.maxCorrectionDb;

  // Step 2: Area Model.
  c.areaId = area.id;
  c.strengthDb = area.level.defaultStrengthDb;
  c.character = area.macros.character;
  c.voiceAmount = area.macros.voiceAmount;
  c.clearVoiceReduction = area.macros.clearVoiceReduction;
  c.voiceDiversity = area.macros.voiceDiversity;
  c.babbleFraction = 1.0 - area.balancedStationaryFraction;
  c.pool = area.talkers.pool;
  c.meanActive = area.talkers.meanActive;
  c.minActive = area.talkers.minActive;
  c.maxActive = area.talkers.maxActive;
  c.spectrumTarget = area.spectrum.target;
  c.stationarySpectrum = area.spectrum.target;
  c.useSpeechMatchedIfAvailable = area.spectrum.useSpeechMatchedIfAvailable;
  c.lfLimitHz = area.spectrum.lfLimitHz;
  c.lfTrimDb125 = area.spectrum.lfTrimDb125;
  c.spatialAlgorithm = area.spatial.algorithm;
  c.spread = area.spatial.spread;
  c.motion = area.spatial.motion;
  c.speakerVariation = area.spatial.speakerVariation;
  c.neighborhoodSize = area.spatial.neighborhoodSize;

  // Step 3: Strategy overrides.
  c.strategyId = strat.id;
  c.strategyClass = strat.klass;
  assign(c.babbleFraction, strat.overrides.babbleFraction);
  if (strat.overrides.babbleFractionDelta && !strat.overrides.babbleFraction)
    c.babbleFraction += *strat.overrides.babbleFractionDelta;
  if (strat.forced.babbleFractionRange)
    c.babbleFraction = std::clamp(c.babbleFraction, strat.forced.babbleFractionRange->first,
                                  strat.forced.babbleFractionRange->second);
  assign(c.character, strat.overrides.character);

  // Step 4 (macros) is applied by MaskStrategy::buildPlan (core/strategy), which consumes the
  // macro VALUES resolved here; see bf::composePlan() for the full pipeline.

  // Step 5: user (preset) non-null fields.
  assign(c.strengthDb, p.macros.strengthDb);
  assign(c.character, p.macros.character);
  assign(c.voiceAmount, p.macros.voiceAmount);
  assign(c.clearVoiceReduction, p.macros.clearVoiceReduction);
  assign(c.voiceDiversity, p.macros.voiceDiversity);
  assign(c.babbleFraction, p.macros.babbleFraction);

  const auto& t = p.talkers;
  assign(c.pool, t.pool);
  assign(c.meanActive, t.meanActive);
  assign(c.minActive, t.minActive);
  assign(c.maxActive, t.maxActive);
  c.segmentMinS = t.segmentMinS;
  c.segmentMaxS = t.segmentMaxS;
  c.segmentMedianS = t.segmentMedianS;
  c.maxInternalGapMs = t.maxInternalGapMs;
  c.gainVariationDb = t.gainVariationDb;
  c.fadeInMs = t.fadeInMs;
  c.fadeOutMs = t.fadeOutMs;
  c.reEntryCooldownS = t.reEntryCooldownS;
  c.segmentCooldownMin = t.segmentCooldownMin;
  c.poolRotationMin = t.poolRotationMin;
  assign(c.languageAware, t.languageAware);
  c.targetVoiceProfile = t.targetVoiceProfile;
  c.multiVoiceK = t.multiVoiceK;

  assign(c.stationaryEnabled, p.stationary.enabled);
  assign(c.stationarySpectrum, p.stationary.spectrum);
  assign(c.seedMode, p.stationary.seedMode);

  const auto& sp = p.spectrum;
  assign(c.spectrumTarget, sp.target);
  c.speechMatchedProfile = sp.speechMatchedProfile;
  c.customTarget = sp.custom;
  assign(c.lfLimitHz, sp.lfLimitHz);
  assign(c.hfLimitHz, sp.hfLimitHz);
  assign(c.lfTrimDb125, sp.lfTrimDb125);
  assign(c.correctionEnabled, sp.correction.enabled);
  assign(c.correctionSpeed, sp.correction.speed);
  assign(c.correctionMaxDb, sp.correction.maxDb);
  assign(c.rememberCorrection, sp.correction.rememberCorrection);

  assign(c.spatialAlgorithm, p.spatial.algorithm);
  assign(c.spread, p.spatial.spread);
  assign(c.motion, p.spatial.motion);
  assign(c.speakerVariation, p.spatial.speakerVariation);
  assign(c.neighborhoodSize, p.spatial.neighborhoodSize);

  c.channels = p.outputs.channels;
  c.zones = p.outputs.zones;
  assign(c.limiterEnabled, p.outputs.limiterEnabled);
  assign(c.limiterCeilingDbtp, p.outputs.limiterCeilingDbtp);
  assign(c.fallbackPolicy, p.fallbackPolicy);
  if (strat.forced.fallbackPolicy) c.fallbackPolicy = *strat.forced.fallbackPolicy;
  c.seed = p.seed;
  c.deterministic = p.deterministic;

  // Step 6: validation / clamping.
  clamp(c.strengthDb, ed.advancedMinDb, ed.advancedMaxDb, "effective.strengthDb");
  clamp(c.character, 0, 1, "effective.character");
  if (strat.forced.characterRange)
    clamp(c.character, strat.forced.characterRange->first, strat.forced.characterRange->second,
          "effective.character");
  clamp(c.voiceAmount, 0, 1, "effective.voiceAmount");
  clamp(c.clearVoiceReduction, 0, 1, "effective.clearVoiceReduction");
  clamp(c.babbleFraction, 0, 1, "effective.babbleFraction");
  clamp(c.spread, 0, 1, "effective.spread");
  clamp(c.motion, 0, 1, "effective.motion");
  clamp(c.limiterCeilingDbtp, -6, -0.1, "effective.limiterCeilingDbtp");
  clamp(c.pool, 1, 64, "effective.pool");
  clamp(c.maxActive, 1, c.pool, "effective.maxActive");
  clamp(c.meanActive, 1, c.maxActive, "effective.meanActive");
  clamp(c.minActive, 0, std::floor(c.meanActive), "effective.minActive");

  r.ok = true;
  return r;
}

}  // namespace bf
