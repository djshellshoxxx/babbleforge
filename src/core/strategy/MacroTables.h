#pragma once
// Data-driven macro mappings (docs/MASK_STRATEGIES.md §5.1 Character, §6.2 Clear Voice
// Reduction, §9 Voice Amount), loaded from resources/data/macros/*.json via DataSet.
// Application order (PRESETS.md §2 step 4): Voice Amount -> Character -> CVR.
// All functions are pure.
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/config/DataSet.h"

namespace bf {

enum class Interp { Linear, Log, Step };

// Values of the Character table at one c (all rows of §5.1).
struct CharacterValues {
  double meanActiveFactor = 1.0;
  double minFloor = 2, minFactor = 0.6, maxFactor = 1.35;  // formula of the step anchor
  double maxInternalGapMs = 250, segmentMedianS = 5.5, segmentSigmaLn = 0.45, levelSigmaDb = 2.0,
         reEntryCooldownS = 0.8, overlapOnHandoverMs = 150, stationaryOffset = 0.0, motionRate = 0.6,
         fadeInMs = 150, fadeOutMs = 250;
};

// Values of the CVR table at one r (§6.2). Off values: onsetWindowMs = 0, maxPhraseS = 0.
struct CvrValues {
  int minFloor = 1;
  double dominanceCapDb = 6.0, levelSigmaMult = 1.0, forcedOverlapMs = 0.0, onsetWindowMs = 0.0,
         soloRiskWeight = 1.0, minStationaryFraction = 0.0, maxPhraseS = 0.0;
};

// Talker counts after Voice Amount + Character.
struct TalkerCounts {
  double mean = 0.0;
  int minActive = 0, maxActive = 0, pool = 0;
  double meanAfterVoiceAmount = 0.0;
};

class MacroTables {
public:
  MacroTables() = default;
  explicit MacroTables(const DataSet& ds);

  // §9: log interpolation 3.5 -> m_area -> 16 (anchors from data). Not clamped.
  double voiceAmountMean(double v, double mArea) const;
  // §9 pool rule: max(ceil(factor*m) + offset, areaPool).
  int voiceAmountPool(double m, int areaPool) const;
  const VoiceAmountRules& voiceAmountRules() const noexcept { return va_; }

  CharacterValues character(double c) const;
  double characterMeanFactor(double c) const { return character(c).meanActiveFactor; }
  // §5.1 formula rows: min = max(floor, round(factor*m)), max = round(factor*m) at c.
  int formulaMin(double c, double m) const;
  int formulaMax(double c, double m) const;
  // §5.1 counts incl. "Area-specified bounds" scaling rule (bounds apply at c = 0.5 for the
  // Area mean; they scale by the ratio of the anchor formulas evaluated at the new c and m).
  // areaMin/areaMax <= 0 means "no explicit bounds" (formula rows are used). The result obeys
  // min <= floor(mean), ceil(mean) <= max <= pool; pool follows the §9 rule on mBase.
  // min/max for mean m at c under the rule above (before the min <= m <= max clamp).
  // Area values hold at the Area's own default Character cArea (default overloads: 0.5);
  // everything scales by factor(c) / factor(cArea).
  std::pair<int, int> bounds(double m, double c, double mArea, int areaMin, int areaMax) const;
  std::pair<int, int> bounds(double m, double c, double mArea, int areaMin, int areaMax, double cArea) const;
  TalkerCounts talkerCounts(double mBase, double c, double mArea, int areaMin, int areaMax, int areaPool) const;
  TalkerCounts talkerCounts(double mBase, double c, double mArea, int areaMin, int areaMax, int areaPool,
                            double cArea) const;
  double levelVarTruncationSigma() const noexcept { return truncSigma_; }

  CvrValues cvr(double r) const;

  bool loaded() const noexcept { return !charAnchors_.empty(); }

private:
  Interp charInterp(const std::string& key) const;
  Interp cvrInterp(const std::string& key) const;

  std::vector<CharacterAnchor> charAnchors_;
  std::map<std::string, std::string> charInterp_;
  std::vector<double> charSteps_;
  double truncSigma_ = 2.0;
  std::vector<CvrMapping> cvr_;
  std::map<std::string, std::string> cvrInterp_;
  std::vector<double> cvrSteps_;
  std::vector<VoiceAmountAnchor> vaAnchors_;
  VoiceAmountRules va_;
};

// Generic piecewise interpolation over anchors x[i] -> y[i] (x ascending).
// Step: y[k] where k = number of thresholds <= x (clamped to the anchor count).
double interpAnchors(const std::vector<double>& x, const std::vector<double>& y, double q, Interp mode,
                     const std::vector<double>& stepThresholds);

}  // namespace bf
