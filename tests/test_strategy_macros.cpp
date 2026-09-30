#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

#include "core/strategy/MacroTables.h"
#include "test_strategy_common.h"

using Catch::Approx;
using bftest::dataSet;
using bftest::plan;

namespace {
bool hasAdj(const bf::MaskRenderPlan& p, const std::string& path) {
  return std::any_of(p.adjustments.begin(), p.adjustments.end(),
                     [&](const bf::Adjustment& a) { return a.fieldPath == path; });
}
}  // namespace

TEST_CASE("macro data files load with per-parameter interpolation", "[strategy][macros]") {
  const auto& ds = dataSet();
  CHECK(ds.characterInterp.at("maxInternalGapMs") == "log");
  CHECK(ds.characterInterp.at("meanActiveFactor") == "linear");
  CHECK(ds.characterInterp.at("minActive") == "step");
  CHECK(ds.characterInterp.at("fadeOutMs") == "log");
  CHECK(ds.characterInterp.at("segmentDurationSigmaLn") == "linear");
  CHECK(ds.cvrInterp.at("minActiveFloor") == "step");
  CHECK(ds.cvrInterp.at("maxPhraseContinuityS") == "log");
  CHECK(ds.cvrStepThresholds == std::vector<double>{0.33, 0.66});
  CHECK(ds.characterAnchors[2].minActiveFloor == 3);
  CHECK(ds.characterAnchors[2].maxActiveFactor == Approx(1.2));
  CHECK(ds.voiceAmountRules.poolFactor == Approx(1.8));
  CHECK(ds.levelVarTruncationSigma == Approx(2.0));
}

TEST_CASE("Voice Amount anchors are exact and log-interpolated (§9)", "[strategy][macros]") {
  bf::MacroTables mt(dataSet());
  CHECK(mt.voiceAmountMean(0.0, 6.5) == Approx(3.5).epsilon(1e-12));
  CHECK(mt.voiceAmountMean(0.5, 6.5) == Approx(6.5).epsilon(1e-12));
  CHECK(mt.voiceAmountMean(1.0, 6.5) == Approx(16.0).epsilon(1e-12));
  CHECK(mt.voiceAmountMean(0.5, 12.0) == Approx(12.0).epsilon(1e-12));
  CHECK(mt.voiceAmountMean(0.25, 6.5) == Approx(std::sqrt(3.5 * 6.5)).epsilon(1e-9));
  CHECK(mt.voiceAmountMean(0.75, 6.5) == Approx(std::sqrt(6.5 * 16.0)).epsilon(1e-9));
  // Pool = max(ceil(1.8 m) + 2, area pool).
  CHECK(mt.voiceAmountPool(6.5, 14) == 14);
  CHECK(mt.voiceAmountPool(16.0, 14) == 31);
  CHECK(mt.voiceAmountPool(3.5, 14) == 14);

  // Through the plan: v = 1 in Office at Character 0.5 -> m = 16, pool 31.
  bf::MacroState m;
  m.voiceAmount = 1.0;
  m.character = 0.5;
  auto p = plan("balanced", "office", m);
  CHECK(p.talkers.mean == Approx(16.0));
  CHECK(p.talkers.pool == 31u);
  m.voiceAmount = 0.0;
  p = plan("balanced", "office", m);
  CHECK(p.talkers.mean == Approx(3.5));
  CHECK(p.talkers.pool == 14u);
  m.voiceAmount = 0.5;
  p = plan("balanced", "office", m);
  CHECK(p.talkers.mean == Approx(6.5));
}

TEST_CASE("Character anchor rows (§5.1)", "[strategy][macros]") {
  bf::MacroTables mt(dataSet());
  const auto n = mt.character(0.0), b = mt.character(0.5), d = mt.character(1.0);
  CHECK(n.meanActiveFactor == Approx(0.70));
  CHECK(b.meanActiveFactor == Approx(1.00));
  CHECK(d.meanActiveFactor == Approx(1.45));
  CHECK(n.maxInternalGapMs == Approx(600));
  CHECK(b.maxInternalGapMs == Approx(250));
  CHECK(d.maxInternalGapMs == Approx(100));
  CHECK(n.segmentMedianS == Approx(7.0));
  CHECK(d.segmentMedianS == Approx(4.5));
  CHECK(n.segmentSigmaLn == Approx(0.55));
  CHECK(d.levelSigmaDb == Approx(1.0));
  CHECK(b.reEntryCooldownS == Approx(0.8));
  CHECK(d.overlapOnHandoverMs == Approx(400));
  CHECK(n.stationaryOffset == Approx(-0.10));
  CHECK(d.stationaryOffset == Approx(0.15));
  CHECK(b.motionRate == Approx(0.6));
  CHECK(n.fadeInMs == Approx(250));
  CHECK(d.fadeOutMs == Approx(150));
  // Interpolation modes.
  CHECK(mt.character(0.55).maxInternalGapMs == Approx(250.0 * std::pow(100.0 / 250.0, 0.1)).epsilon(1e-9));  // log
  CHECK(mt.character(0.25).segmentMedianS == Approx(std::sqrt(7.0 * 5.5)).epsilon(1e-9));                   // log
  CHECK(mt.character(0.25).levelSigmaDb == Approx(2.5));                                                     // linear
  CHECK(mt.character(0.75).meanActiveFactor == Approx(1.225));                                               // linear
  CHECK(mt.character(0.75).stationaryOffset == Approx(0.075));
  CHECK(mt.character(0.25).fadeInMs == Approx(std::sqrt(250.0 * 150.0)).epsilon(1e-9));
  // Step formula rows (nearest anchor): min/max formulas of the Balanced anchor at 0.55.
  CHECK(mt.formulaMin(0.55, 6.5) == 4);
  CHECK(mt.formulaMax(0.55, 6.5) == 9);
  CHECK(mt.formulaMin(0.0, 6.5) == 3);   // max(1, round(2.6))
  CHECK(mt.formulaMax(0.0, 6.5) == 10);  // round(9.75)
  CHECK(mt.formulaMin(1.0, 2.0) == 3);   // floor 3
  CHECK(mt.formulaMax(1.0, 6.5) == 8);   // round(7.8)
}

TEST_CASE("Area-specified bounds scale with the anchor formula ratio (§5.1)", "[strategy][macros]") {
  bf::MacroTables mt(dataSet());
  // Office: m 6.5, 4/9 at c = 0.5.
  auto t = mt.talkerCounts(6.5, 0.5, 6.5, 4, 9, 14);
  CHECK(t.mean == Approx(6.5));
  CHECK(t.minActive == 4);
  CHECK(t.maxActive == 9);
  CHECK(t.pool == 14);
  // Dense: m = 9.425; min = 4 * max(3, round(7.07)) / 4 = 7; max = 9 * round(11.31) / 9 = 11.
  t = mt.talkerCounts(6.5, 1.0, 6.5, 4, 9, 14);
  CHECK(t.mean == Approx(9.425));
  CHECK(t.minActive == 7);
  CHECK(t.maxActive == 11);
  // Natural: m = 4.55; min = 4 * max(1, round(1.82)) / 4 = 2; max = 9 * round(6.825) / 9 = 7.
  t = mt.talkerCounts(6.5, 0.0, 6.5, 4, 9, 14);
  CHECK(t.mean == Approx(4.55));
  CHECK(t.minActive == 2);
  CHECK(t.maxActive == 7);
  // Without explicit Area bounds the formula rows apply directly.
  t = mt.talkerCounts(6.5, 0.5, 6.5, 0, 0, 14);
  CHECK(t.minActive == 4);  // max(2, round(3.9))
  CHECK(t.maxActive == 9);  // round(8.775)
  // Clamping: min <= floor(m), max >= ceil(m).
  t = mt.talkerCounts(1.2, 0.0, 6.5, 4, 9, 14);
  CHECK(t.minActive <= static_cast<int>(std::floor(t.mean)));
  CHECK(t.maxActive >= static_cast<int>(std::ceil(t.mean)));
}

TEST_CASE("Character monotonicity through buildPlan", "[strategy][macros]") {
  for (const char* areaId : {"office", "open_office", "common_area", "small_room"}) {
    bf::MaskRenderPlan prev;
    bool first = true;
    for (double c : {0.0, 0.5, 1.0}) {
      bf::MacroState m;
      m.character = c;
      m.clearVoiceReduction = 0.0;  // isolate Character
      const auto p = plan("balanced", areaId, m);
      if (!first) {
        CHECK(p.talkers.mean > prev.talkers.mean);
        CHECK(p.talkers.maxGapMs < prev.talkers.maxGapMs);
        CHECK(p.talkers.gainSigmaDb < prev.talkers.gainSigmaDb);
        CHECK(p.talkers.sigmaLn < prev.talkers.sigmaLn);
        CHECK(p.talkers.fadeInMs < prev.talkers.fadeInMs);
        CHECK(p.talkers.fadeOutMs < prev.talkers.fadeOutMs);
        CHECK(p.talkers.medianS < prev.talkers.medianS);
        CHECK(p.talkers.reEntryCooldownMs < prev.talkers.reEntryCooldownMs);
        CHECK(p.talkers.minActive >= prev.talkers.minActive);
        CHECK(p.mix.stationaryFraction() > prev.mix.stationaryFraction());  // Δs
        CHECK(p.spatial.params.motion < prev.spatial.params.motion);
      }
      prev = p;
      first = false;
    }
  }
}

TEST_CASE("CVR mapping table (§6.2)", "[strategy][macros][cvr]") {
  bf::MacroTables mt(dataSet());
  auto lo = mt.cvr(0.0), med = mt.cvr(0.5), hi = mt.cvr(1.0);
  CHECK(lo.minFloor == 1);
  CHECK(med.minFloor == 2);
  CHECK(hi.minFloor == 3);
  CHECK(mt.cvr(0.32).minFloor == 1);
  CHECK(mt.cvr(0.33).minFloor == 2);
  CHECK(mt.cvr(0.65).minFloor == 2);
  CHECK(mt.cvr(0.66).minFloor == 3);
  CHECK(lo.dominanceCapDb == Approx(6.0));
  CHECK(mt.cvr(0.75).dominanceCapDb == Approx(3.25));
  CHECK(hi.levelSigmaMult == Approx(0.6));
  CHECK(med.forcedOverlapMs == Approx(150));
  CHECK(lo.onsetWindowMs == 0.0);  // no rule
  CHECK(med.onsetWindowMs == Approx(300));
  CHECK(hi.onsetWindowMs == Approx(100));
  CHECK(hi.soloRiskWeight == Approx(0.3));
  CHECK(med.minStationaryFraction == Approx(0.05));
  CHECK(lo.maxPhraseS == 0.0);  // off
  CHECK(mt.cvr(0.2).maxPhraseS == 0.0);
  CHECK(mt.cvr(0.4).maxPhraseS == Approx(10.0));
  CHECK(mt.cvr(0.75).maxPhraseS == Approx(std::sqrt(60.0)).epsilon(1e-9));
  CHECK(hi.maxPhraseS == Approx(6.0));
}

TEST_CASE("CVR r=1 gives min floor 3 when satisfiable and never raises the mean", "[strategy][macros][cvr]") {
  // Office, Character 0 (Natural anchor): m = 4.55, Character min = 2.
  bf::MacroState m;
  m.character = 0.0;
  m.clearVoiceReduction = 0.0;
  const auto low = plan("balanced", "office", m);
  m.clearVoiceReduction = 1.0;
  const auto high = plan("balanced", "office", m);
  CHECK(low.talkers.minActive == 2u);
  CHECK(high.talkers.minActive == 3u);
  CHECK(high.talkers.cvrMinFloor == 3u);
  CHECK(high.talkers.effectiveMin() == 3u);
  CHECK(high.talkers.mean == Approx(low.talkers.mean));
  CHECK(high.talkers.maxActive == low.talkers.maxActive);
  CHECK(high.talkers.overlapMinMs == Approx(400));
  CHECK(high.talkers.cvrLevelSigmaMult == Approx(0.6));
  CHECK(high.talkers.gainSigmaDb == Approx(low.talkers.gainSigmaDb));  // sigma stays the Character value
  CHECK_FALSE(hasAdj(high, "talkers.cvrMinFloor"));

  // Never raises the mean at any r / c / v.
  for (double v : {0.0, 0.5, 1.0})
    for (double c : {0.0, 0.5, 1.0}) {
      bf::MacroState a;
      a.voiceAmount = v;
      a.character = c;
      a.clearVoiceReduction = 0.0;
      const double m0 = plan("balanced", "office", a).talkers.mean;
      for (double r : {0.5, 1.0}) {
        a.clearVoiceReduction = r;
        CHECK(plan("balanced", "office", a).talkers.mean == Approx(m0));
      }
    }

  // Unsatisfiable: v = 0, c = 0 -> m = 2.45; floor 3 > m - 1, lowered and reported.
  bf::MacroState u;
  u.voiceAmount = 0.0;
  u.character = 0.0;
  u.clearVoiceReduction = 1.0;
  const auto p = plan("balanced", "office", u);
  CHECK(p.talkers.mean == Approx(2.45));
  CHECK(p.talkers.minActive == 1u);
  CHECK(static_cast<double>(p.talkers.minActive) <= p.talkers.mean - 1.0);
  CHECK(hasAdj(p, "talkers.cvrMinFloor"));
}

TEST_CASE("CVR minimum stationary fraction and orthogonality", "[strategy][macros][cvr]") {
  // Common area natural-leaning: stationary 0.15, Character 0 -> Δs -0.10 -> 0.05; CVR High -> 0.10.
  bf::MacroState m;
  m.character = 0.0;
  m.clearVoiceReduction = 1.0;
  CHECK(plan("balanced", "common_area", m).mix.stationaryFraction() == Approx(0.10));
  m.clearVoiceReduction = 0.0;
  CHECK(plan("balanced", "common_area", m).mix.stationaryFraction() == Approx(0.05));
  // Hybrid (explicit mix slider): Δs and the CVR minimum are ignored.
  m.clearVoiceReduction = 1.0;
  bf::StrategyParams p;
  p.babbleFraction = 1.0;
  CHECK(plan("hybrid", "common_area", m, p).mix.babbleFraction == Approx(1.0));
  // MultiVoice: only dominance cap, level sigma and selection weighting apply.
  const auto mv = plan("multi_voice", "office", m);
  CHECK(mv.talkers.cvrMinFloor == 0u);
  CHECK(mv.talkers.cvrOnsetWindowMs == 0.0);
  CHECK(mv.talkers.cvrMaxPhraseS == 0.0);
  CHECK(mv.talkers.cvrDominanceCapDb == Approx(2.5));
  CHECK(mv.talkers.cvrLevelSigmaMult == Approx(0.6));
  CHECK(mv.talkers.cvrSoloRiskWeight == Approx(0.3));
  CHECK(mv.mix.babbleFraction == Approx(1.0));
}
