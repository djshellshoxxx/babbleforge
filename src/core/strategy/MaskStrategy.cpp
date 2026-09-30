#include "core/strategy/MaskStrategy.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "core/strategy/TalkerCountModes.h"

namespace bf {

namespace {

constexpr std::uint64_t kDefaultSeed = 1;
constexpr double kDefaultSegMinS = 2.0;   // MASK_STRATEGIES.md §10
constexpr double kDefaultSegMaxS = 15.0;  // MASK_STRATEGIES.md §10

template <class T>
bool clampRec(T& v, double lo, double hi, const std::string& path, const char* reason, std::vector<Adjustment>& adj) {
  const double d = static_cast<double>(v);
  const double c = std::min(std::max(d, lo), hi);
  if (c == d) return false;
  adj.push_back({path, d, c, reason});
  v = static_cast<T>(c);
  return true;
}

template <class T>
void clampOpt(std::optional<T>& v, double lo, double hi, const std::string& path, const char* reason,
              std::vector<Adjustment>& adj) {
  if (v) clampRec(*v, lo, hi, path, reason, adj);
}

SpatialAlgorithm resolveAlgorithm(const std::string& req, const OutputLayout& layout) {
  const int n = layout.activeCount();
  const SpatialAlgorithm autoAlg = selectAlgorithm(layout);
  if (n <= 1) return SpatialAlgorithm::Mono;
  if (req == "mono") return SpatialAlgorithm::Mono;
  if (req == "stereo") return n == 2 ? SpatialAlgorithm::DistributedStereo : autoAlg;
  if (req == "small" || req == "vbap") return n >= 3 ? SpatialAlgorithm::SmallMultichannel : autoAlg;
  if (req == "large") return n >= 3 ? SpatialAlgorithm::LargeDistributed : autoAlg;
  if (req == "distributed") return n >= 4 ? SpatialAlgorithm::LargeDistributed : autoAlg;  // PRESETS §3.1
  return autoAlg;  // "auto" and unknown values
}

// Resolves a target id: exact, else a data-set id that starts with "<id>_" (e.g. the Area
// value "ltass_universal" names the file target "ltass_universal_byrne1994").
const SpectrumTargetDef* findTarget(const std::map<std::string, SpectrumTargetDef>& targets, const std::string& id) {
  if (auto it = targets.find(id); it != targets.end()) return &it->second;
  for (const auto& [k, v] : targets)
    if (k.rfind(id + "_", 0) == 0) return &v;
  return nullptr;
}

void setStationaryOnly(MaskRenderPlan& p, const std::string& code) {
  p.babbleEnabled = false;
  p.strategyClass = StrategyClass::StationarySpeechMask;
  p.mix.babbleFraction = 0.0;
  p.mix.zoneBabbleFraction.fill(0.0);
  p.stationary.enabled = true;
  p.stationary.keepHot = false;
  p.speakersNeeded = 0;
  p.status.runnable = true;
  p.status.degraded.push_back(code);
  p.status.degraded.push_back("fallback.stationary");
}

void setError(MaskRenderPlan& p, const std::string& code) {
  p.status.error = true;
  p.status.runnable = false;
  p.status.errorCode = code;
}

void shiftZones(MaskRenderPlan& p, double newB) {
  const double d = newB - p.mix.babbleFraction;
  for (double& z : p.mix.zoneBabbleFraction) z = std::clamp(z + d, 0.0, 1.0);
  p.mix.babbleFraction = newB;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

std::shared_ptr<const StrategyEnv> StrategyEnv::fromDataSet(const DataSet& ds) {
  auto e = std::make_shared<StrategyEnv>();
  e->engineDefaults = ds.engineDefaults;
  e->macros = MacroTables(ds);
  e->targets = ds.targets;
  return e;
}

MaskStrategy::MaskStrategy(StrategyDef def, std::shared_ptr<const StrategyEnv> env)
    : def_(std::move(def)), env_(std::move(env)) {}

StrategyId MaskStrategy::id() const {
  auto id = strategyIdFromString(def_.id);
  return id ? *id : StrategyId::Balanced;
}

bool MaskStrategy::allowsStationary() const {
  // A stationary component can be added unless the strategy fixes b (range upper == lower).
  if (def_.forced.babbleFractionRange)
    return def_.forced.babbleFractionRange->first < 1.0 && strategyClass() != StrategyClass::MultiVoiceMask;
  return strategyClass() != StrategyClass::MultiVoiceMask;
}

double MaskStrategy::resolveCharacter(const AreaModel& area, const MacroState& m) const {
  double c = m.character ? *m.character
                         : (def_.overrides.characterDefault ? *def_.overrides.characterDefault : area.macros.character);
  c = std::clamp(c, 0.0, 1.0);
  if (def_.forced.characterRange)
    c = std::clamp(c, def_.forced.characterRange->first, def_.forced.characterRange->second);
  return c;
}

// ---- validate -------------------------------------------------------------------------------

ValidationResult MaskStrategy::validate(StrategyParams& p, MacroState& m, const CorpusSummary& corpus) const {
  ValidationResult r;
  auto& a = r.adjustments;
  const char* range = "outside the allowed range (MASK_STRATEGIES.md §10)";
  const char* forced = "outside the strategy's forced range (PRESETS.md §5)";
  const EngineDefaults& ed = env_->engineDefaults;

  clampOpt(m.voiceAmount, 0, 1, "macros.voiceAmount", range, a);
  clampOpt(m.clearVoiceReduction, 0, 1, "macros.clearVoiceReduction", range, a);
  clampOpt(m.character, 0, 1, "macros.character", range, a);
  if (m.character && def_.forced.characterRange)
    clampRec(*m.character, def_.forced.characterRange->first, def_.forced.characterRange->second, "macros.character",
             forced, a);

  clampOpt(p.babbleFraction, 0, 1, "mix.babbleFraction", range, a);
  if (p.babbleFraction && def_.forced.babbleFractionRange)
    clampRec(*p.babbleFraction, def_.forced.babbleFractionRange->first, def_.forced.babbleFractionRange->second,
             "mix.babbleFraction", forced, a);
  for (std::size_t z = 0; z < p.zoneBabbleOffsets.size(); ++z)
    clampRec(p.zoneBabbleOffsets[z], -1, 1, "zones[" + std::to_string(z) + "].babbleFractionOffset", range, a);
  if (p.zoneBabbleOffsets.size() > kMaxZones) p.zoneBabbleOffsets.resize(kMaxZones);

  const auto kr = def_.forced.multiVoiceKRange ? *def_.forced.multiVoiceKRange : std::make_pair(1.0, 16.0);
  clampOpt(p.multiVoiceK, kr.first, kr.second, "talkers.multiVoiceK", range, a);

  clampOpt(p.pool, 2, 64, "talkers.pool", range, a);
  clampOpt(p.meanActive, 1, 32, "talkers.meanActive", range, a);
  clampOpt(p.maxActive, 1, 48, "talkers.maxActive", range, a);
  clampOpt(p.minActive, 0, 48, "talkers.minActive", range, a);
  // Invariants: min <= mean <= max <= pool <= available.
  if (p.pool && corpus.present())
    clampRec(*p.pool, 2, std::max(2, corpus.availableSpeakers), "talkers.pool",
             "pool exceeds the available speakers", a);
  if (p.maxActive && p.pool) clampRec(*p.maxActive, 1, *p.pool, "talkers.maxActive", "max exceeds pool", a);
  if (p.maxActive && !p.pool && corpus.present())
    clampRec(*p.maxActive, 1, std::max(1, corpus.availableSpeakers), "talkers.maxActive",
             "max exceeds the available speakers", a);
  if (p.meanActive && p.maxActive) clampRec(*p.meanActive, 1, *p.maxActive, "talkers.meanActive", "mean exceeds max", a);
  if (p.minActive && p.meanActive)
    clampRec(*p.minActive, 0, std::floor(*p.meanActive), "talkers.minActive", "min exceeds mean", a);
  if (p.minActive && p.maxActive && !p.meanActive)
    clampRec(*p.minActive, 0, *p.maxActive, "talkers.minActive", "min exceeds max", a);

  clampOpt(p.maxInternalGapMs, 50, 1000, "talkers.maxInternalGapMs", range, a);
  clampOpt(p.segmentMinS, 1, 20, "talkers.segmentMinS", range, a);
  clampOpt(p.segmentMaxS, 3, 60, "talkers.segmentMaxS", range, a);
  {
    const double segMin = p.segmentMinS.value_or(kDefaultSegMinS);
    const double segMax = p.segmentMaxS.value_or(kDefaultSegMaxS);
    if (segMin >= segMax) {
      if (p.segmentMaxS) {
        clampRec(*p.segmentMaxS, std::min(60.0, segMin + 1.0), 60.0, "talkers.segmentMaxS", "segMin >= segMax", a);
        if (p.segmentMinS && *p.segmentMinS >= *p.segmentMaxS)
          clampRec(*p.segmentMinS, 1.0, *p.segmentMaxS - 1.0, "talkers.segmentMinS", "segMin >= segMax", a);
      } else {
        clampRec(*p.segmentMinS, 1.0, segMax - 1.0, "talkers.segmentMinS", "segMin >= segMax", a);
      }
    }
  }
  clampOpt(p.gainVariationDb, 0, 6, "talkers.gainVariationDb", range, a);
  clampOpt(p.fadeInMs, 20, 1000, "talkers.fadeInMs", range, a);
  clampOpt(p.fadeOutMs, 20, 1000, "talkers.fadeOutMs", range, a);
  if (p.fadeInMs && p.fadeOutMs) {
    const double segMinMs = 1000.0 * p.segmentMinS.value_or(kDefaultSegMinS);
    const double sum = *p.fadeInMs + *p.fadeOutMs;
    if (sum >= segMinMs) {
      const double k = 0.99 * segMinMs / sum;
      clampRec(*p.fadeInMs, 0, *p.fadeInMs * k, "talkers.fadeInMs", "fadeIn + fadeOut >= segMin", a);
      clampRec(*p.fadeOutMs, 0, *p.fadeOutMs * k, "talkers.fadeOutMs", "fadeIn + fadeOut >= segMin", a);
    }
  }
  clampOpt(p.reEntryCooldownS, 0, 30, "talkers.reEntryCooldownS", range, a);

  clampOpt(p.spread, 0, 1, "spatial.spread", range, a);
  clampOpt(p.motion, 0, 1, "spatial.motion", range, a);
  clampOpt(p.neighborhoodSize, 2, 5, "spatial.neighborhoodSize", range, a);
  clampOpt(p.lfLimitHz, 20, 250, "spectrum.lfLimitHz", range, a);
  clampOpt(p.hfLimitHz, 4000, 20000, "spectrum.hfLimitHz", range, a);
  clampOpt(p.lfTrimDb125, -12, 6, "spectrum.lfTrimDb125", range, a);
  clampOpt(p.strengthDb, ed.advancedMinDb, ed.advancedMaxDb, "level.strengthDb", range, a);
  clampOpt(p.limiterCeilingDbtp, -6, -0.1, "level.limiterCeilingDbtp", range, a);

  if (def_.forced.fallbackPolicy) {
    const auto f = fallbackPolicyFromString(*def_.forced.fallbackPolicy);
    if (f && p.fallback && *p.fallback != *f) {
      a.push_back({"reliability.fallbackPolicy", static_cast<double>(*p.fallback), static_cast<double>(*f),
                   "fallback policy forced by the strategy (RELIABILITY.md §4)"});
      p.fallback = f;
    }
  }
  if (def_.forced.seedRequired && !p.seed) {
    a.push_back({"seed", 0.0, static_cast<double>(kDefaultSeed), "seed required by the strategy; default seed used"});
    p.seed = kDefaultSeed;
  }
  return r;
}

ValidationResult LaboratoryMask::validate(StrategyParams& p, MacroState& m, const CorpusSummary& corpus) const {
  ValidationResult r = MaskStrategy::validate(p, m, corpus);
  clampOpt(p.labN, 1, 64, "research.n", "outside the allowed range (MASK_STRATEGIES.md §8.1)", r.adjustments);
  const bool cn = p.labMode == LabSubMode::ContinuousN || p.labMode == LabSubMode::LtassMatchedBabble;
  const int n = p.labN.value_or(def_.overrides.continuousNDefault.value_or(4));
  if (cn && corpus.present() && n > corpus.availableSpeakers)
    r.conflicts.push_back("research.n: " + std::to_string(n) + " distinct speakers required, " +
                          std::to_string(corpus.availableSpeakers) + " available (Strict: render aborts)");
  return r;
}

// ---- plan building --------------------------------------------------------------------------

MaskRenderPlan MaskStrategy::basePlan(const AreaModel& area, const StrategyParams& p, const OutputLayout& layout) const {
  const EngineDefaults& ed = env_->engineDefaults;
  MaskRenderPlan plan;
  plan.strategyId = id();
  plan.strategyClass = strategyClass();
  plan.areaId = area.id;
  plan.levelVarTruncationSigma = env_->macros.levelVarTruncationSigma();

  // Spectrum target (shared by both components).
  const float trim = static_cast<float>(p.lfTrimDb125.value_or(area.spectrum.lfTrimDb125));
  std::string tid = p.spectrumTarget.value_or(area.spectrum.target);
  std::optional<SpectrumTarget> tgt;
  if (p.customTarget && (tid == "custom" || tid == p.customTarget->id))
    tgt = buildSpectrumTarget(*p.customTarget, trim).target;
  if (!tgt) {
    if (const SpectrumTargetDef* d = findTarget(env_->targets, tid)) tgt = buildSpectrumTarget(*d, trim).target;
  }
  if (!tgt) {
    plan.adjustments.push_back({"spectrum.target", 0, 0, "unknown target '" + tid + "'; using ltass_universal"});
    tid = "ltass_universal";
    if (const SpectrumTargetDef* d = findTarget(env_->targets, tid)) tgt = buildSpectrumTarget(*d, trim).target;
  }
  if (tgt) plan.target = *tgt;
  plan.target.id = tid;
  plan.target.lfTrimDb125 = trim;
  plan.target.lfLimitHz = static_cast<float>(p.lfLimitHz.value_or(area.spectrum.lfLimitHz));
  if (p.hfLimitHz) plan.target.hfLimitHz = static_cast<float>(*p.hfLimitHz);
  plan.stationary.spectrumId = tid;

  // Spatial.
  plan.spatial.algorithmRequest = p.spatialAlgorithm.value_or(area.spatial.algorithm);
  plan.spatial.algorithm = resolveAlgorithm(plan.spatial.algorithmRequest, layout);
  plan.spatial.activeOutputs = layout.activeCount();
  if (plan.spatial.algorithmRequest != "auto") plan.spatial.params.algorithm = plan.spatial.algorithm;
  plan.spatial.params.spread = static_cast<float>(std::clamp(p.spread.value_or(area.spatial.spread), 0.0, 1.0));
  plan.spatial.params.motion = static_cast<float>(std::clamp(area.spatial.motion, 0.0, 1.0));
  plan.spatial.params.variation =
      speakerVariationFromString(p.speakerVariation.value_or(area.spatial.speakerVariation))
          .value_or(SpeakerVariation::Medium);
  plan.spatial.params.neighbourhoodSize = std::clamp(p.neighborhoodSize.value_or(area.spatial.neighborhoodSize), 2, 5);

  // Level (ENGINE.md §3.1).
  plan.level.lRefDbfs = ed.lRefDbfs;
  plan.level.strengthDb =
      std::clamp(p.strengthDb.value_or(area.level.defaultStrengthDb), ed.advancedMinDb, ed.advancedMaxDb);
  plan.level.limiterEnabled = p.limiterEnabled.value_or(true);
  plan.level.limiterCeilingDbtp = std::clamp(p.limiterCeilingDbtp.value_or(ed.limiterCeilingDbtp), -6.0, -0.1);

  // Fallback policy (RELIABILITY.md §4): strategy-forced, else user, else engine default.
  FallbackPolicy fb = fallbackPolicyFromString(ed.fallbackPolicy).value_or(FallbackPolicy::Continuous);
  if (p.fallback) fb = *p.fallback;
  if (def_.forced.fallbackPolicy)
    if (auto f = fallbackPolicyFromString(*def_.forced.fallbackPolicy)) fb = *f;
  plan.fallback = fb;

  plan.talkers.seed = p.seed.value_or(kDefaultSeed);
  plan.talkers.talkerRefDbfs = ed.lRefDbfs;
  plan.selector.diversity = p.voiceDiversity.value_or(area.macros.voiceDiversity);
  plan.selector.languageAware = p.languageAware.value_or(false);
  plan.selector.poolRotation = def_.forced.poolRotation;
  return plan;
}

double MaskStrategy::baseBabbleFraction(const AreaModel& area, const StrategyParams& p, bool& locked) const {
  double b = 1.0 - area.balancedStationaryFraction;
  if (def_.overrides.babbleFraction)
    b = *def_.overrides.babbleFraction;
  else if (def_.overrides.babbleFractionDelta)
    b += *def_.overrides.babbleFractionDelta;
  locked = def_.forced.mixUserLocked;
  if (p.babbleFraction) {
    b = *p.babbleFraction;
    locked = true;
  }
  if (p.stationaryEnabled && !*p.stationaryEnabled && strategyClass() != StrategyClass::StationarySpeechMask) {
    b = 1.0;
    locked = true;
  }
  b = std::clamp(b, 0.0, 1.0);
  if (def_.forced.babbleFractionRange)
    b = std::clamp(b, def_.forced.babbleFractionRange->first, def_.forced.babbleFractionRange->second);
  return b;
}

void MaskStrategy::applyMix(MaskRenderPlan& plan, const StrategyParams& p, double baseB, bool locked,
                            double stationaryOffset, double cvrMinStationary) const {
  plan.mix.baseBabbleFraction = baseB;
  plan.mix.userLocked = locked;
  double s = 1.0 - baseB;
  if (!locked && allowsStationary()) {
    s += stationaryOffset;               // Character Δs (§5.1)
    s = std::max(s, cvrMinStationary);   // CVR minimum stationary fraction (§6.2)
    double lo = 0.0, hi = 1.0;
    if (def_.forced.stationaryFractionRange) {
      lo = def_.forced.stationaryFractionRange->first;
      hi = def_.forced.stationaryFractionRange->second;
    }
    s = std::clamp(s, lo, hi);
  }
  plan.mix.babbleFraction = std::clamp(1.0 - s, 0.0, 1.0);
  for (std::size_t z = 0; z < kMaxZones; ++z) {
    const double off = z < p.zoneBabbleOffsets.size() ? p.zoneBabbleOffsets[z] : 0.0;
    plan.mix.zoneBabbleFraction[z] = std::clamp(plan.mix.babbleFraction + off, 0.0, 1.0);
  }
}

void MaskStrategy::applyMotion(MaskRenderPlan& plan, const AreaModel& area, const StrategyParams& p, double c) const {
  double motion;
  if (p.motion) {
    motion = *p.motion;
  } else {
    motion = area.spatial.motion * def_.overrides.motionScale.value_or(1.0);
    if (def_.forced.motionRange) motion = std::clamp(motion, def_.forced.motionRange->first, def_.forced.motionRange->second);
    motion *= env_->macros.character(c).motionRate;
  }
  plan.spatial.params.motion = static_cast<float>(std::clamp(motion, 0.0, 1.0));
}

void MaskStrategy::buildStochasticTalkers(MaskRenderPlan& plan, const AreaModel& area, const StrategyParams& p,
                                          double v, double c, double r, const CorpusSummary& corpus) const {
  const MacroTables& mt = env_->macros;
  const VoiceAmountRules& vr = mt.voiceAmountRules();
  auto& adj = plan.adjustments;
  TalkerPlanParams& t = plan.talkers;
  t.mode = PlanMode::Stochastic;

  // 1. Voice Amount (§9) and 2. Character (§5.1) talker counts.
  const double mArea = area.talkers.meanActive;
  const double mV = mt.voiceAmountMean(v, mArea);
  TalkerCounts tc = mt.talkerCounts(mV, c, mArea, area.talkers.minActive, area.talkers.maxActive, area.talkers.pool);
  double mean = std::clamp(tc.mean, vr.meanMin, vr.meanMax);
  int mn = tc.minActive, mx = tc.maxActive, pool = tc.pool;
  // Step 5: user overrides. A user mean replaces the macro result; min/max are re-derived.
  if (p.meanActive) {
    mean = *p.meanActive;
    std::tie(mn, mx) = mt.bounds(mean, c, mArea, area.talkers.minActive, area.talkers.maxActive);
    mx = std::max(mx, static_cast<int>(std::ceil(mean - 1e-9)));
    pool = std::max(mt.voiceAmountPool(mean, area.talkers.pool), mx);
  }
  if (p.minActive) mn = *p.minActive;
  if (p.maxActive) mx = *p.maxActive;
  if (p.pool) pool = *p.pool;
  else pool = std::max(pool, mx);
  // §9: m <= available speakers - margin. Applied only while the corpus covers max (a
  // smaller corpus is handled by degrade(), which rescales the counts itself).
  if (corpus.present() && corpus.availableSpeakers > mx) {
    const double cap = std::max(1.0, static_cast<double>(corpus.availableSpeakers - vr.availableSpeakerMargin));
    clampRec(mean, vr.meanMin, std::max(vr.meanMin, cap), "talkers.meanActive", "Voice Amount: available speakers - 2",
             adj);
  }
  if (!p.minActive) mn = std::min(mn, static_cast<int>(std::floor(mean)));

  // Character durations / levels.
  const CharacterValues cv = mt.character(c);
  t.maxGapMs = p.maxInternalGapMs.value_or(cv.maxInternalGapMs);
  t.medianS = p.segmentMedianS.value_or(cv.segmentMedianS);
  t.sigmaLn = cv.segmentSigmaLn;
  t.gainSigmaDb = p.gainVariationDb.value_or(cv.levelSigmaDb);
  t.fadeInMs = p.fadeInMs.value_or(cv.fadeInMs);
  t.fadeOutMs = p.fadeOutMs.value_or(cv.fadeOutMs);
  t.reEntryCooldownMs = 1000.0 * p.reEntryCooldownS.value_or(cv.reEntryCooldownS);
  t.overlapMinMs = cv.overlapOnHandoverMs;
  t.segMinS = p.segmentMinS.value_or(kDefaultSegMinS);
  t.segMaxS = p.segmentMaxS.value_or(kDefaultSegMaxS);

  // 3. Clear Voice Reduction (§6.2). Never raises the mean.
  const CvrValues cr = mt.cvr(r);
  t.cvrDominanceCapDb = cr.dominanceCapDb;
  t.cvrLevelSigmaMult = cr.levelSigmaMult;
  t.overlapMinMs = std::max(t.overlapMinMs, cr.forcedOverlapMs);
  t.cvrOnsetWindowMs = cr.onsetWindowMs;
  t.cvrSoloRiskWeight = cr.soloRiskWeight;
  t.cvrMaxPhraseS = cr.maxPhraseS;
  plan.selector.soloRiskWeight = cr.soloRiskWeight;
  int floorV = cr.minFloor;
  if (floorV > mn) {
    if (static_cast<double>(floorV) > mean - 1.0) {
      const int lowered = std::max(mn, static_cast<int>(std::floor(mean - 1.0)));
      adj.push_back({"talkers.cvrMinFloor", static_cast<double>(floorV), static_cast<double>(lowered),
                     "CVR floor lowered: min <= mean - 1 not satisfiable (MASK_STRATEGIES.md §6.2)"});
      floorV = lowered;
    }
    mn = std::max(mn, floorV);
  }
  t.cvrMinFloor = static_cast<std::uint32_t>(std::max(0, floorV));

  // Final invariants: min <= mean <= max <= pool (<= 64), ceil(mean) <= max where possible.
  clampRec(pool, 2, 64, "talkers.pool", "outside 2..64", adj);
  clampRec(mx, 1, pool, "talkers.maxActive", "max exceeds pool", adj);
  clampRec(mean, 1.0, static_cast<double>(mx), "talkers.meanActive", "mean exceeds max", adj);
  clampRec(mn, 0, static_cast<int>(std::floor(mean)), "talkers.minActive", "min exceeds mean", adj);
  if (t.segMinS >= t.segMaxS) t.segMaxS = t.segMinS + 1.0;
  const double segMinMs = 1000.0 * t.segMinS;
  if (t.fadeInMs + t.fadeOutMs >= segMinMs) {
    const double k = 0.99 * segMinMs / (t.fadeInMs + t.fadeOutMs);
    t.fadeInMs *= k;
    t.fadeOutMs *= k;
  }
  t.cvrMinFloor = std::min<std::uint32_t>(t.cvrMinFloor, static_cast<std::uint32_t>(mn));
  t.mean = mean;
  t.minActive = static_cast<std::uint32_t>(mn);
  t.maxActive = static_cast<std::uint32_t>(mx);
  t.pool = static_cast<std::uint32_t>(pool);
}

int speakersNeededFor(const MaskRenderPlan& plan) {
  if (!plan.babbleEnabled) return 0;
  if (plan.talkers.mode == PlanMode::ContinuousN) return static_cast<int>(plan.talkers.maxActive);
  return std::max(static_cast<int>(plan.talkers.pool), static_cast<int>(plan.talkers.maxActive) + 4);
}

int minCorpusSpeakersFor(const MaskRenderPlan& plan) {
  if (!plan.babbleEnabled) return 0;
  switch (plan.talkers.mode) {
    case PlanMode::FixedK: return static_cast<int>(plan.talkers.maxActive) + 3;
    case PlanMode::ContinuousN: return static_cast<int>(plan.talkers.maxActive);
    case PlanMode::Stochastic: break;
  }
  return std::max(static_cast<int>(std::ceil(plan.talkers.mean - 1e-9)) + 4, 8);
}

void MaskStrategy::finalize(MaskRenderPlan& plan) const {
  plan.speakersNeeded = speakersNeededFor(plan);
  const bool lab = plan.strategyClass == StrategyClass::LaboratoryMask;
  // §4.2: components the fallback may need stay hot (Continuous/Safe); Laboratory keeps the
  // stationary seed position.
  plan.stationary.keepHot = lab || (plan.babbleEnabled && plan.fallback != FallbackPolicy::Strict);
  bool anyStationary = plan.mix.babbleFraction < 1.0;
  for (double z : plan.mix.zoneBabbleFraction) anyStationary = anyStationary || z < 1.0;
  plan.stationary.enabled = anyStationary || plan.stationary.keepHot;
  plan.rehash();
}

MaskRenderPlan StochasticBabbleStrategy::buildPlan(const AreaModel& area, const StrategyParams& p,
                                                   const MacroState& m, const CorpusSummary& corpus,
                                                   const OutputLayout& layout) const {
  MaskRenderPlan plan = basePlan(area, p, layout);
  const double v = std::clamp(m.voiceAmount.value_or(area.macros.voiceAmount), 0.0, 1.0);
  const double c = resolveCharacter(area, m);
  const double r = std::clamp(m.clearVoiceReduction.value_or(area.macros.clearVoiceReduction), 0.0, 1.0);
  plan.voiceAmount = v;
  plan.character = c;
  plan.clearVoiceReduction = r;
  plan.babbleEnabled = true;
  buildStochasticTalkers(plan, area, p, v, c, r, corpus);
  bool locked = false;
  const double b = baseBabbleFraction(area, p, locked);
  const CharacterValues cv = env_->macros.character(c);
  applyMix(plan, p, b, locked, cv.stationaryOffset, env_->macros.cvr(r).minStationaryFraction);
  applyMotion(plan, area, p, c);
  finalize(plan);
  return plan;
}

MaskRenderPlan StationarySpeechMask::buildPlan(const AreaModel& area, const StrategyParams& p, const MacroState& m,
                                               const CorpusSummary&, const OutputLayout& layout) const {
  MaskRenderPlan plan = basePlan(area, p, layout);
  plan.voiceAmount = m.voiceAmount.value_or(area.macros.voiceAmount);
  plan.character = resolveCharacter(area, m);
  plan.clearVoiceReduction = m.clearVoiceReduction.value_or(area.macros.clearVoiceReduction);
  plan.babbleEnabled = false;
  plan.talkers.pool = 0;
  plan.talkers.minActive = plan.talkers.maxActive = 0;
  plan.talkers.mean = 0.0;
  plan.mix.baseBabbleFraction = 0.0;
  plan.mix.babbleFraction = 0.0;
  plan.mix.zoneBabbleFraction.fill(0.0);
  plan.mix.userLocked = true;
  applyMotion(plan, area, p, 0.5);
  finalize(plan);
  return plan;
}

int MultiVoiceMask::defaultK() const { return def_.overrides.multiVoiceK.value_or(7); }

MaskRenderPlan MultiVoiceMask::buildPlan(const AreaModel& area, const StrategyParams& p, const MacroState& m,
                                         const CorpusSummary&, const OutputLayout& layout) const {
  MaskRenderPlan plan = basePlan(area, p, layout);
  const MacroTables& mt = env_->macros;
  const auto kr = def_.forced.multiVoiceKRange ? *def_.forced.multiVoiceKRange : std::make_pair(1.0, 16.0);
  const int k = std::clamp(p.multiVoiceK.value_or(defaultK()), static_cast<int>(kr.first), static_cast<int>(kr.second));
  const double r = std::clamp(m.clearVoiceReduction.value_or(area.macros.clearVoiceReduction), 0.0, 1.0);
  plan.voiceAmount = m.voiceAmount.value_or(area.macros.voiceAmount);  // not used: K is fixed
  plan.character = 0.5;  // Character not exposed: Balanced anchor for gaps, level sigma, fades
  plan.clearVoiceReduction = r;
  plan.babbleEnabled = true;

  TalkerPlanParams& t = plan.talkers;
  t.mode = PlanMode::FixedK;
  t.maxActive = t.minActive = static_cast<std::uint32_t>(k);
  t.mean = k;
  t.pool = static_cast<std::uint32_t>(std::clamp(p.pool.value_or(multiVoicePool(k)), k, 64));
  const CharacterValues cv = mt.character(plan.character);
  t.maxGapMs = p.maxInternalGapMs.value_or(cv.maxInternalGapMs);
  t.medianS = p.segmentMedianS.value_or(def_.overrides.segmentDurationMedianS.value_or(8.0));
  t.sigmaLn = def_.overrides.segmentDurationSigmaLn.value_or(0.4);
  t.gainSigmaDb = p.gainVariationDb.value_or(cv.levelSigmaDb);
  t.fadeInMs = p.fadeInMs.value_or(cv.fadeInMs);
  t.fadeOutMs = p.fadeOutMs.value_or(cv.fadeOutMs);
  t.reEntryCooldownMs = 1000.0 * p.reEntryCooldownS.value_or(cv.reEntryCooldownS);
  t.overlapMinMs = def_.overrides.handoverOverlapMs.value_or(150.0);
  t.segMinS = p.segmentMinS.value_or(kDefaultSegMinS);
  t.segMaxS = p.segmentMaxS.value_or(kDefaultSegMaxS);
  if (t.segMinS >= t.segMaxS) t.segMaxS = t.segMinS + 1.0;
  // §6.2: only the dominance cap, level sigma and selection weighting apply (count is fixed).
  const CvrValues cr = mt.cvr(r);
  t.cvrMinFloor = 0;
  t.cvrDominanceCapDb = cr.dominanceCapDb;
  t.cvrLevelSigmaMult = cr.levelSigmaMult;
  t.cvrSoloRiskWeight = cr.soloRiskWeight;
  t.cvrOnsetWindowMs = 0.0;
  t.cvrMaxPhraseS = 0.0;
  plan.selector.soloRiskWeight = cr.soloRiskWeight;

  bool locked = false;
  const double b = baseBabbleFraction(area, p, locked);
  applyMix(plan, p, b, true, 0.0, 0.0);
  plan.mix.userLocked = locked;
  applyMotion(plan, area, p, plan.character);
  finalize(plan);
  return plan;
}

MaskRenderPlan LaboratoryMask::buildPlan(const AreaModel& area, const StrategyParams& p, const MacroState& m,
                                         const CorpusSummary& corpus, const OutputLayout& layout) const {
  StrategyParams q = p;
  if (p.labMode == LabSubMode::Pink) q.spectrumTarget = "pink";
  MaskRenderPlan plan = basePlan(area, q, layout);
  plan.labMode = p.labMode;
  // All parameters explicit: macros default to the Balanced anchor / Area mean, and CVR is
  // disabled (r = 0) unless explicitly configured (§6.2).
  const double v = std::clamp(m.voiceAmount.value_or(0.5), 0.0, 1.0);
  const double c = std::clamp(m.character.value_or(0.5), 0.0, 1.0);
  const double r = def_.forced.cvrDisabledUnlessExplicit ? std::clamp(m.clearVoiceReduction.value_or(0.0), 0.0, 1.0)
                                                         : m.clearVoiceReduction.value_or(0.0);
  plan.voiceAmount = v;
  plan.character = c;
  plan.clearVoiceReduction = r;

  double b = 1.0;
  switch (p.labMode) {
    case LabSubMode::Ssn:
    case LabSubMode::Pink:
      plan.babbleEnabled = false;
      plan.talkers.pool = plan.talkers.minActive = plan.talkers.maxActive = 0;
      plan.talkers.mean = 0.0;
      b = 0.0;
      break;
    case LabSubMode::ContinuousN:
    case LabSubMode::LtassMatchedBabble: {
      plan.babbleEnabled = true;
      const int n = std::clamp(p.labN.value_or(def_.overrides.continuousNDefault.value_or(4)), 1, 64);
      TalkerPlanParams& t = plan.talkers;
      t.mode = PlanMode::ContinuousN;
      t.maxActive = t.minActive = static_cast<std::uint32_t>(n);
      t.mean = n;
      t.pool = static_cast<std::uint32_t>(n);  // exactly N distinct speakers
      const double xfade = def_.overrides.continuousNOverlapMs.value_or(20.0);
      t.maxGapMs = p.maxInternalGapMs.value_or(def_.overrides.continuousNMaxGapMs.value_or(100.0));
      t.medianS = p.segmentMedianS.value_or(def_.overrides.segmentDurationMedianS.value_or(8.0));
      t.sigmaLn = def_.overrides.segmentDurationSigmaLn.value_or(0.4);
      t.gainSigmaDb = p.gainVariationDb.value_or(0.0);  // equal active speech level per talker
      t.fadeInMs = t.fadeOutMs = xfade;
      t.overlapMinMs = xfade;
      t.segMinS = p.segmentMinS.value_or(kDefaultSegMinS);
      t.segMaxS = p.segmentMaxS.value_or(kDefaultSegMaxS);
      if (t.segMinS >= t.segMaxS) t.segMaxS = t.segMinS + 1.0;
      const CvrValues cr = env_->macros.cvr(r);
      t.cvrMinFloor = 0;
      t.cvrDominanceCapDb = cr.dominanceCapDb;
      t.cvrLevelSigmaMult = cr.levelSigmaMult;
      t.cvrSoloRiskWeight = cr.soloRiskWeight;
      plan.selector.soloRiskWeight = cr.soloRiskWeight;
      plan.strictSpectralMatch = p.labMode == LabSubMode::LtassMatchedBabble;
      b = p.babbleFraction.value_or(1.0);
      break;
    }
    case LabSubMode::Stochastic:
    case LabSubMode::Hybrid:
      plan.babbleEnabled = true;
      buildStochasticTalkers(plan, area, p, v, c, r, corpus);
      b = p.babbleFraction.value_or(p.labMode == LabSubMode::Hybrid ? 0.5 : 1.0);
      break;
  }
  StrategyParams mixParams = p;
  mixParams.babbleFraction = std::clamp(b, 0.0, 1.0);
  applyMix(plan, mixParams, *mixParams.babbleFraction, true, 0.0, 0.0);
  applyMotion(plan, area, p, c);
  finalize(plan);
  return plan;
}

// ---- degrade --------------------------------------------------------------------------------

MaskRenderPlan MaskStrategy::degrade(const MaskRenderPlan& in, const DegradeReason& reason) const {
  MaskRenderPlan p = in;
  if (!p.babbleEnabled) return p;  // stationary-only plans need no corpus
  const FallbackPolicy policy =
      p.strategyClass == StrategyClass::LaboratoryMask ? FallbackPolicy::Strict : p.fallback;
  const int S = reason.availableSpeakers;
  const int needed = speakersNeededFor(p);

  if (reason.kind == DegradeReason::Kind::SourceFailures) {
    const bool major = reason.failedPoolFraction > 0.10;
    if (policy == FallbackPolicy::Strict) {
      setError(p, "source.failure");
    } else if (policy == FallbackPolicy::Safe && major) {
      setStationaryOnly(p, "corpus.reduced");
    } else if (major) {
      p.status.degraded.push_back("corpus.reduced");  // Continuous: substitute and continue
    }
    p.rehash();
    return p;
  }

  const bool none = reason.kind == DegradeReason::Kind::CorpusNone || S <= 1;
  if (!none && S >= needed) return p;  // normal plan
  const std::string code = none ? "corpus.none" : "corpus.insufficient";
  if (policy == FallbackPolicy::Strict) {
    setError(p, code);
    p.rehash();
    return p;
  }
  if (none || policy == FallbackPolicy::Safe) {
    setStationaryOnly(p, code);
    p.rehash();
    return p;
  }

  // Continuous: reduced plan (RELIABILITY.md §3).
  TalkerPlanParams& t = p.talkers;
  const int maxOrig = static_cast<int>(t.maxActive);
  p.selector.relaxRecentSpeakerRule = true;
  p.status.degraded.push_back(code);
  t.pool = static_cast<std::uint32_t>(S);
  if (S <= maxOrig) {
    const int newMax = S - 1;  // never the same speaker in two slots
    const double ratio = static_cast<double>(newMax) / static_cast<double>(maxOrig);
    if (t.mode == PlanMode::Stochastic) {
      t.mean = std::min(t.mean * ratio, static_cast<double>(newMax));
      t.maxActive = static_cast<std::uint32_t>(newMax);
      const int mn = static_cast<int>(std::lround(t.minActive * ratio));
      t.minActive = static_cast<std::uint32_t>(std::clamp(mn, 0, static_cast<int>(std::floor(t.mean))));
      t.cvrMinFloor = std::min(t.cvrMinFloor, t.minActive);
    } else {
      t.maxActive = t.minActive = static_cast<std::uint32_t>(newMax);
      t.mean = newMax;
    }
    // Stationary compensation: +0.5 (1 - ratio), total stationary capped at 0.6.
    const double s = 1.0 - p.mix.babbleFraction;
    if (s < 0.6) {
      const double s2 = std::min(s + 0.5 * (1.0 - ratio), 0.6);
      p.mix.stationaryCompensation = s2 - s;
      shiftZones(p, 1.0 - s2);
    }
    p.stationary.enabled = true;
  }
  p.rehash();
  return p;
}

// ---- capabilities ---------------------------------------------------------------------------

StrategyCaps HybridMask::capabilities() const {
  StrategyCaps c;
  c.needsCorpus = true;
  c.exposesMixSlider = def_.forced.mixUserLocked;  // Hybrid (explicit mix slider), not Balanced
  c.exposesCharacter = true;
  c.exposesVoiceAmount = true;
  c.minCorpusSpeakers = 8;
  return c;
}

StrategyCaps BabbleMask::capabilities() const {
  StrategyCaps c;
  c.exposesCharacter = true;
  c.exposesVoiceAmount = true;
  c.minCorpusSpeakers = 8;
  return c;
}

StrategyCaps NaturalBabbleMask::capabilities() const {
  StrategyCaps c;
  c.exposesCharacter = true;
  c.exposesVoiceAmount = true;
  c.minCorpusSpeakers = 8;
  return c;
}

StrategyCaps StationarySpeechMask::capabilities() const {
  StrategyCaps c;
  c.needsCorpus = false;
  c.minCorpusSpeakers = 0;
  return c;
}

StrategyCaps MultiVoiceMask::capabilities() const {
  StrategyCaps c;
  c.exposesVoiceAmount = false;  // the count is K (the Multi-Voice selector replaces Voice Amount)
  c.minCorpusSpeakers = defaultK() + 3;
  return c;
}

StrategyCaps LaboratoryMask::capabilities() const {
  StrategyCaps c;
  c.needsCorpus = true;  // false only for the ssn / pink sub-modes (see minCorpusSpeakersFor)
  c.exposesMixSlider = true;
  c.exposesVoiceAmount = true;
  c.lockedForResearch = def_.forced.lockedForResearch;
  c.minCorpusSpeakers = def_.overrides.continuousNDefault.value_or(4);
  return c;
}

// ---- factory --------------------------------------------------------------------------------

std::unique_ptr<MaskStrategy> makeStrategy(const StrategyDef& def, std::shared_ptr<const StrategyEnv> env) {
  const auto cls = strategyClassFromString(def.klass);
  if (!cls || !strategyIdFromString(def.id)) return nullptr;
  switch (*cls) {
    case StrategyClass::StationarySpeechMask: return std::make_unique<StationarySpeechMask>(def, std::move(env));
    case StrategyClass::MultiVoiceMask: return std::make_unique<MultiVoiceMask>(def, std::move(env));
    case StrategyClass::BabbleMask: return std::make_unique<BabbleMask>(def, std::move(env));
    case StrategyClass::HybridMask: return std::make_unique<HybridMask>(def, std::move(env));
    case StrategyClass::NaturalBabbleMask: return std::make_unique<NaturalBabbleMask>(def, std::move(env));
    case StrategyClass::LaboratoryMask: return std::make_unique<LaboratoryMask>(def, std::move(env));
  }
  return nullptr;
}

std::unique_ptr<MaskStrategy> makeStrategy(const DataSet& ds, const std::string& strategyId) {
  auto it = ds.strategies.find(strategyId);
  if (it == ds.strategies.end()) return nullptr;
  return makeStrategy(it->second, StrategyEnv::fromDataSet(ds));
}

StrategyLibrary::StrategyLibrary(const DataSet& ds) : env_(StrategyEnv::fromDataSet(ds)) {
  for (const auto& [id, def] : ds.strategies)
    if (auto s = makeStrategy(def, env_)) strategies_[id] = std::move(s);
}

const MaskStrategy* StrategyLibrary::find(const std::string& strategyId) const {
  auto it = strategies_.find(strategyId);
  return it == strategies_.end() ? nullptr : it->second.get();
}

std::vector<std::string> StrategyLibrary::ids() const {
  std::vector<std::string> v;
  for (const auto& [id, s] : strategies_) v.push_back(id);
  return v;
}

}  // namespace bf
