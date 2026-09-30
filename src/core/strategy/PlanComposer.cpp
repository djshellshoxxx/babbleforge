#include "core/strategy/PlanComposer.h"

namespace bf {

StrategyParams strategyParamsFromPreset(const Preset& p) {
  StrategyParams s;
  const auto& t = p.talkers;
  s.pool = t.pool;
  s.meanActive = t.meanActive;
  s.minActive = t.minActive;
  s.maxActive = t.maxActive;
  s.multiVoiceK = t.multiVoiceK;
  s.maxInternalGapMs = t.maxInternalGapMs;
  s.segmentMinS = t.segmentMinS;
  s.segmentMaxS = t.segmentMaxS;
  s.segmentMedianS = t.segmentMedianS;
  s.gainVariationDb = t.gainVariationDb;
  s.fadeInMs = t.fadeInMs;
  s.fadeOutMs = t.fadeOutMs;
  s.reEntryCooldownS = t.reEntryCooldownS;
  s.languageAware = t.languageAware;
  s.voiceDiversity = p.macros.voiceDiversity;
  s.babbleFraction = p.macros.babbleFraction;
  for (const auto& z : p.outputs.zones) {
    if (z.id < 0 || z.id >= static_cast<int>(kMaxZones)) continue;
    if (s.zoneBabbleOffsets.size() <= static_cast<std::size_t>(z.id)) s.zoneBabbleOffsets.resize(z.id + 1, 0.0);
    s.zoneBabbleOffsets[static_cast<std::size_t>(z.id)] = z.babbleFractionOffset;
  }
  s.spectrumTarget = p.spectrum.target;
  s.customTarget = p.spectrum.custom;
  s.lfLimitHz = p.spectrum.lfLimitHz;
  s.hfLimitHz = p.spectrum.hfLimitHz;
  s.lfTrimDb125 = p.spectrum.lfTrimDb125;
  s.stationaryEnabled = p.stationary.enabled;
  s.spatialAlgorithm = p.spatial.algorithm;
  s.spread = p.spatial.spread;
  s.motion = p.spatial.motion;
  s.speakerVariation = p.spatial.speakerVariation;
  s.neighborhoodSize = p.spatial.neighborhoodSize;
  s.strengthDb = p.macros.strengthDb;
  s.limiterEnabled = p.outputs.limiterEnabled;
  s.limiterCeilingDbtp = p.outputs.limiterCeilingDbtp;
  if (p.fallbackPolicy) s.fallback = fallbackPolicyFromString(*p.fallbackPolicy);
  if (p.seed) s.seed = static_cast<std::uint64_t>(*p.seed);
  return s;
}

MacroState macroStateFromPreset(const Preset& p) {
  MacroState m;
  m.voiceAmount = p.macros.voiceAmount;
  m.character = p.macros.character;
  m.clearVoiceReduction = p.macros.clearVoiceReduction;
  return m;
}

ComposedPlan composePlan(const DataSet& ds, const Preset& preset, const CorpusSummary& corpus,
                         const OutputLayout& layout) {
  ComposedPlan out;
  const ComposeResult c = compose(ds, preset);
  if (!c.ok) {
    out.error = c.error;
    return out;
  }
  auto strategy = makeStrategy(ds, preset.strategy);
  if (!strategy) {
    out.error = "strategy: cannot instantiate \"" + preset.strategy + "\"";
    return out;
  }
  const AreaModel& area = ds.areas.at(preset.area);
  out.params = strategyParamsFromPreset(preset);
  out.macros = macroStateFromPreset(preset);
  ValidationResult v = strategy->validate(out.params, out.macros, corpus);
  out.plan = strategy->buildPlan(area, out.params, out.macros, corpus, layout);
  out.adjustments = std::move(v.adjustments);
  out.adjustments.insert(out.adjustments.end(), out.plan.adjustments.begin(), out.plan.adjustments.end());
  out.conflicts = std::move(v.conflicts);
  out.ok = true;
  return out;
}

}  // namespace bf
