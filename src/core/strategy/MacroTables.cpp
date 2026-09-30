#include "core/strategy/MacroTables.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "core/math/DetMath.h"

namespace bf {

namespace {

Interp parseInterp(const std::map<std::string, std::string>& m, const std::string& key, Interp def) {
  auto it = m.find(key);
  if (it == m.end()) return def;
  if (it->second == "log") return Interp::Log;
  if (it->second == "step") return Interp::Step;
  return Interp::Linear;
}

template <class Anchor, class Get>
double tableAt(const std::vector<Anchor>& anchors, double Anchor::*key, Get get, double q, Interp mode,
               const std::vector<double>& steps) {
  std::vector<double> x, y;
  x.reserve(anchors.size());
  y.reserve(anchors.size());
  for (const auto& a : anchors) {
    x.push_back(a.*key);
    y.push_back(get(a));
  }
  return interpAnchors(x, y, q, mode, steps);
}

}  // namespace

double interpAnchors(const std::vector<double>& x, const std::vector<double>& y, double q, Interp mode,
                     const std::vector<double>& stepThresholds) {
  const std::size_t n = x.size();
  if (n == 0) return 0.0;
  if (n == 1) return y[0];
  if (mode == Interp::Step) {
    std::size_t k = 0;
    if (!stepThresholds.empty()) {
      for (double t : stepThresholds)
        if (q >= t) ++k;
    } else {
      while (k + 1 < n && q >= x[k + 1]) ++k;  // nearest lower anchor
    }
    return y[std::min(k, n - 1)];
  }
  q = std::clamp(q, x.front(), x.back());
  std::size_t i = 0;
  while (i + 2 < n && q > x[i + 1]) ++i;
  const double span = x[i + 1] - x[i];
  const double t = span > 0 ? (q - x[i]) / span : 0.0;
  if (t <= 0.0) return y[i];
  if (t >= 1.0) return y[i + 1];
  if (mode == Interp::Log && y[i] > 0 && y[i + 1] > 0)
    return detexp(detlog(y[i]) + t * (detlog(y[i + 1]) - detlog(y[i])));
  return y[i] + t * (y[i + 1] - y[i]);
}

MacroTables::MacroTables(const DataSet& ds)
    : charAnchors_(ds.characterAnchors),
      charInterp_(ds.characterInterp),
      charSteps_(ds.characterStepThresholds),
      truncSigma_(ds.levelVarTruncationSigma),
      cvr_(ds.cvrMappings),
      cvrInterp_(ds.cvrInterp),
      cvrSteps_(ds.cvrStepThresholds),
      vaAnchors_(ds.voiceAmountAnchors),
      va_(ds.voiceAmountRules) {
  auto byC = [](const CharacterAnchor& a, const CharacterAnchor& b) { return a.c < b.c; };
  std::sort(charAnchors_.begin(), charAnchors_.end(), byC);
  std::sort(cvr_.begin(), cvr_.end(), [](const CvrMapping& a, const CvrMapping& b) { return a.r < b.r; });
  std::sort(vaAnchors_.begin(), vaAnchors_.end(),
            [](const VoiceAmountAnchor& a, const VoiceAmountAnchor& b) { return a.v < b.v; });
}

Interp MacroTables::charInterp(const std::string& key) const { return parseInterp(charInterp_, key, Interp::Linear); }
Interp MacroTables::cvrInterp(const std::string& key) const { return parseInterp(cvrInterp_, key, Interp::Linear); }

double MacroTables::voiceAmountMean(double v, double mArea) const {
  if (vaAnchors_.empty()) return mArea;
  std::vector<double> x, y;
  for (const auto& a : vaAnchors_) {
    x.push_back(a.v);
    y.push_back(a.fromArea || !a.meanActiveTalkers ? mArea : *a.meanActiveTalkers);
  }
  const Interp mode = va_.interp == "linear" ? Interp::Linear : Interp::Log;
  return interpAnchors(x, y, std::clamp(v, 0.0, 1.0), mode, {});
}

int MacroTables::voiceAmountPool(double m, int areaPool) const {
  const int p = static_cast<int>(std::ceil(va_.poolFactor * m - 1e-9)) + static_cast<int>(std::lround(va_.poolOffset));
  return std::max(p, areaPool);
}

CharacterValues MacroTables::character(double c) const {
  CharacterValues v;
  if (charAnchors_.empty()) return v;
  c = std::clamp(c, 0.0, 1.0);
  using A = CharacterAnchor;
  auto at = [&](const char* key, double A::*field) {
    return tableAt(charAnchors_, &A::c, [field](const A& a) { return a.*field; }, c, charInterp(key), charSteps_);
  };
  v.meanActiveFactor = at("meanActiveFactor", &A::meanActiveFactor);
  v.minFloor = at("minActive", &A::minActiveFloor);
  v.minFactor = at("minActive", &A::minActiveFactor);
  v.maxFactor = at("maxActive", &A::maxActiveFactor);
  v.maxInternalGapMs = at("maxInternalGapMs", &A::maxInternalGapMs);
  v.segmentMedianS = at("segmentDurationMedianS", &A::segmentDurationMedianS);
  v.segmentSigmaLn = at("segmentDurationSigmaLn", &A::segmentDurationSigmaLn);
  v.levelSigmaDb = at("perSegmentLevelSigmaDb", &A::perSegmentLevelSigmaDb);
  v.reEntryCooldownS = at("reEntryCooldownS", &A::reEntryCooldownS);
  v.overlapOnHandoverMs = at("overlapOnHandoverMs", &A::overlapOnHandoverMs);
  v.stationaryOffset = at("stationaryFractionOffset", &A::stationaryFractionOffset);
  v.motionRate = at("spatialMotionRate", &A::spatialMotionRate);
  v.fadeInMs = at("fadeInMs", &A::fadeInMs);
  v.fadeOutMs = at("fadeOutMs", &A::fadeOutMs);
  return v;
}

int MacroTables::formulaMin(double c, double m) const {
  const CharacterValues v = character(c);
  return std::max(static_cast<int>(std::lround(v.minFloor)), static_cast<int>(std::lround(v.minFactor * m)));
}

int MacroTables::formulaMax(double c, double m) const {
  return std::max(1, static_cast<int>(std::lround(character(c).maxFactor * m)));
}

std::pair<int, int> MacroTables::bounds(double m, double c, double mArea, int areaMin, int areaMax) const {
  if (areaMin > 0 && areaMax > 0) {
    const double dMin = std::max(1, formulaMin(0.5, mArea));
    const double dMax = std::max(1, formulaMax(0.5, mArea));
    return {static_cast<int>(std::lround(areaMin * formulaMin(c, m) / dMin)),
            static_cast<int>(std::lround(areaMax * formulaMax(c, m) / dMax))};
  }
  return {formulaMin(c, m), formulaMax(c, m)};
}

TalkerCounts MacroTables::talkerCounts(double mBase, double c, double mArea, int areaMin, int areaMax,
                                       int areaPool) const {
  TalkerCounts t;
  t.meanAfterVoiceAmount = mBase;
  const double f05 = characterMeanFactor(0.5);
  t.mean = mBase * characterMeanFactor(c) / (f05 > 0 ? f05 : 1.0);
  std::tie(t.minActive, t.maxActive) = bounds(t.mean, c, mArea, areaMin, areaMax);
  t.minActive = std::clamp(t.minActive, 0, static_cast<int>(std::floor(t.mean)));
  t.maxActive = std::max(t.maxActive, static_cast<int>(std::ceil(t.mean - 1e-9)));
  t.pool = std::max(voiceAmountPool(mBase, areaPool), t.maxActive);
  return t;
}

CvrValues MacroTables::cvr(double r) const {
  CvrValues v;
  if (cvr_.empty()) return v;
  r = std::clamp(r, 0.0, 1.0);
  using M = CvrMapping;
  // -1 in the data means "off": a curve segment touching an "off" anchor behaves as a step.
  auto at = [&](const char* key, double M::*field) {
    Interp mode = cvrInterp(key);
    if (mode != Interp::Step) {
      std::size_t i = 0;
      while (i + 2 < cvr_.size() && r > cvr_[i + 1].r) ++i;
      if (cvr_[i].*field < 0 || cvr_[i + 1].*field < 0) mode = Interp::Step;
    }
    return tableAt(cvr_, &M::r, [field](const M& m) { return m.*field; }, r, mode, cvrSteps_);
  };
  v.minFloor = static_cast<int>(std::lround(at("minActiveFloor", &M::minActiveFloor)));
  v.dominanceCapDb = at("dominanceCapDb", &M::dominanceCapDb);
  v.levelSigmaMult = at("levelSigmaMultiplier", &M::levelSigmaMultiplier);
  v.forcedOverlapMs = at("forcedOverlapAtHandoverMs", &M::forcedOverlapAtHandoverMs);
  v.onsetWindowMs = std::max(0.0, at("onsetMaskingWindowMs", &M::onsetMaskingWindowMs));
  v.soloRiskWeight = at("segmentSelectionWeightForSoloRisk", &M::segmentSelectionWeightForSoloRisk);
  v.minStationaryFraction = at("minimumStationaryFraction", &M::minimumStationaryFraction);
  v.maxPhraseS = std::max(0.0, at("maxPhraseContinuityS", &M::maxPhraseContinuityS));
  return v;
}

}  // namespace bf
