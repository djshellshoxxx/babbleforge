#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include "core/config/AtomicFile.h"
#include "core/config/Preset.h"
#include "core/strategy/PlanComposer.h"
#include "core/strategy/TalkerCountModes.h"
#include "test_strategy_common.h"

using Catch::Approx;
using bftest::area;
using bftest::bigCorpus;
using bftest::checkPlanValid;
using bftest::dataSet;
using bftest::plan;
using bftest::strategy;

namespace {
bool hasAdj(const std::vector<bf::Adjustment>& v, const std::string& path) {
  return std::any_of(v.begin(), v.end(), [&](const bf::Adjustment& a) { return a.fieldPath == path; });
}
}  // namespace

TEST_CASE("office / balanced plan matches the documented defaults", "[strategy][plan]") {
  // Area values hold at the Area's own default Character c_a = 0.55 (MASK_STRATEGIES.md §5.1).
  bf::MacroState m;
  m.character = 0.55;
  auto p = plan("balanced", "office", m);
  CHECK(p.strategyId == bf::StrategyId::Balanced);
  CHECK(p.strategyClass == bf::StrategyClass::HybridMask);
  CHECK(p.talkers.mode == bf::PlanMode::Stochastic);

  p = plan("balanced", "office");
  CHECK(p.character == Approx(0.55));
  CHECK(p.mix.baseBabbleFraction == Approx(0.70));
  CHECK(p.mix.babbleFraction == Approx(0.70));
  CHECK(p.talkers.mean == Approx(6.5));
  CHECK(p.talkers.minActive == 4u);
  CHECK(p.talkers.maxActive == 9u);
  CHECK(p.talkers.pool == 14u);
  CHECK(p.talkers.maxGapMs == Approx(250.0 * std::pow(0.4, 0.1)).epsilon(1e-9));
  CHECK(p.clearVoiceReduction == Approx(1.0));
  CHECK(p.talkers.cvrMinFloor == 3u);
  CHECK(p.target.id == "ltass_universal");
  CHECK(p.target.lfTrimDb125 == Approx(-1.5f));
  CHECK(p.target.lfLimitHz == Approx(80.0f));
  CHECK(p.spatial.algorithm == bf::SpatialAlgorithm::DistributedStereo);
  CHECK(p.spatial.params.spread == Approx(0.6f));
  CHECK(p.spatial.params.variation == bf::SpeakerVariation::Medium);
  CHECK(p.spatial.params.neighbourhoodSize == 3);
  CHECK(p.level.lRefDbfs == Approx(-26.0));
  CHECK(p.level.strengthDb == Approx(0.0));
  CHECK(p.fallback == bf::FallbackPolicy::Continuous);
  CHECK(p.stationary.enabled);
  CHECK(p.stationary.keepHot);
  CHECK(p.speakersNeeded == 14);
  CHECK(p.selector.diversity == "high");
}

TEST_CASE("strategy relative overrides (PRESETS.md §5)", "[strategy][plan]") {
  SECTION("natural") {
    auto p = plan("natural", "office");
    CHECK(p.strategyClass == bf::StrategyClass::NaturalBabbleMask);
    CHECK(p.character == Approx(0.2));
    CHECK(p.mix.baseBabbleFraction == Approx(0.80));
    CHECK(p.spatial.params.motion == Approx(0.45 * (1.0 - 0.4 * 0.4)).epsilon(1e-6));
    // Common area: b = min(0.95, 0.85 + 0.10); motion min(1, 0.8 * 1.5) = 1 -> x rate.
    p = plan("natural", "common_area");
    CHECK(p.mix.baseBabbleFraction == Approx(0.95));
    CHECK(p.mix.stationaryFraction() <= 0.9);
    CHECK(p.spatial.params.motion == Approx(1.0 * (1.0 - 0.4 * 0.4)).epsilon(1e-6));
    // Character > 0.35 is clamped.
    bf::MacroState m;
    m.character = 0.9;
    bf::StrategyParams sp;
    auto vr = strategy("natural").validate(sp, m, bigCorpus());
    CHECK(*m.character == Approx(0.35));
    CHECK(hasAdj(vr.adjustments, "macros.character"));
    m.character = 0.9;
    CHECK(strategy("natural").buildPlan(area("office"), sp, m, bigCorpus(), bf::makeStereoLayout()).character ==
          Approx(0.35));
  }
  SECTION("dense") {
    auto p = plan("dense", "office");
    CHECK(p.strategyClass == bf::StrategyClass::BabbleMask);
    CHECK(p.character == Approx(0.9));
    CHECK(p.mix.baseBabbleFraction == Approx(0.60));
    CHECK(p.talkers.maxGapMs < 150.0);
    bf::MacroState m;
    m.character = 0.2;
    CHECK(plan("dense", "office", m).character == Approx(0.75));
    // b = area b - 0.10 >= 0.4 (Open Office: 0.55 - 0.10 = 0.45; user 0.1 -> 0.4).
    CHECK(plan("dense", "open_office").mix.baseBabbleFraction == Approx(0.45));
    bf::StrategyParams sp;
    sp.babbleFraction = 0.1;
    CHECK(plan("dense", "open_office", {}, sp).mix.babbleFraction == Approx(0.4));
  }
  SECTION("speech_noise") {
    auto p = plan("speech_noise", "office");
    CHECK(p.strategyClass == bf::StrategyClass::StationarySpeechMask);
    CHECK_FALSE(p.babbleEnabled);
    CHECK(p.mix.babbleFraction == 0.0);
    CHECK(p.stationary.enabled);
    CHECK(p.speakersNeeded == 0);
    CHECK_FALSE(strategy("speech_noise").capabilities().needsCorpus);
    CHECK(p.target.id == "ltass_universal");
  }
  SECTION("multi_voice") {
    auto p = plan("multi_voice", "office");
    CHECK(p.strategyClass == bf::StrategyClass::MultiVoiceMask);
    CHECK(p.talkers.mode == bf::PlanMode::FixedK);
    CHECK(p.talkers.maxActive == 7u);
    CHECK(p.talkers.minActive == 7u);
    CHECK(p.talkers.mean == Approx(7.0));
    CHECK(p.talkers.pool == 12u);
    CHECK(p.talkers.targetMean() == Approx(7.0));
    CHECK(p.mix.babbleFraction == Approx(1.0));
    CHECK(p.talkers.medianS == Approx(8.0));
    CHECK(p.talkers.sigmaLn == Approx(0.4));
    CHECK(p.talkers.overlapMinMs == Approx(150.0));
    CHECK(strategy("multi_voice").capabilities().minCorpusSpeakers == 10);
    for (int k : {3, 5, 7, 9}) {
      bf::StrategyParams sp;
      sp.multiVoiceK = k;
      auto q = plan("multi_voice", "office", {}, sp);
      CHECK(q.talkers.maxActive == static_cast<std::uint32_t>(k));
      CHECK(static_cast<int>(q.talkers.pool) >= k + 3);
    }
    bf::StrategyParams sp;
    sp.multiVoiceK = 9;
    CHECK(plan("multi_voice", "office", {}, sp).talkers.pool == 20u);
    sp.babbleFraction = 0.5;  // user range 0.8 - 1.0
    CHECK(plan("multi_voice", "office", {}, sp).mix.babbleFraction == Approx(0.8));
  }
  SECTION("hybrid") {
    auto p = plan("hybrid", "office");
    CHECK(p.mix.babbleFraction == Approx(0.5));
    CHECK(p.mix.userLocked);
    CHECK(strategy("hybrid").capabilities().exposesMixSlider);
    CHECK_FALSE(strategy("balanced").capabilities().exposesMixSlider);
    bf::StrategyParams sp;
    sp.babbleFraction = 0.25;
    sp.zoneBabbleOffsets = {0.0, 0.5, -0.5};
    p = plan("hybrid", "office", {}, sp);
    CHECK(p.mix.babbleFraction == Approx(0.25));
    CHECK(p.mix.zoneBabbleFraction[0] == Approx(0.25));
    CHECK(p.mix.zoneBabbleFraction[1] == Approx(0.75));
    CHECK(p.mix.zoneBabbleFraction[2] == Approx(0.0));
    CHECK(p.mix.zoneBabbleFraction[7] == Approx(0.25));
  }
}

TEST_CASE("laboratory forces strict fallback and CVR off", "[strategy][plan][lab]") {
  bf::StrategyParams sp;
  sp.fallback = bf::FallbackPolicy::Continuous;
  bf::MacroState m;
  const auto& lab = strategy("research");
  auto vr = lab.validate(sp, m, bigCorpus());
  CHECK(*sp.fallback == bf::FallbackPolicy::Strict);
  CHECK(hasAdj(vr.adjustments, "reliability.fallbackPolicy"));
  CHECK(hasAdj(vr.adjustments, "seed"));  // seed required
  CHECK(sp.seed.has_value());
  CHECK(lab.capabilities().lockedForResearch);

  bf::StrategyParams raw;
  raw.fallback = bf::FallbackPolicy::Safe;  // even unvalidated, the plan is Strict
  auto p = lab.buildPlan(area("office"), raw, m, bigCorpus(), bf::makeStereoLayout());
  CHECK(p.strategyClass == bf::StrategyClass::LaboratoryMask);
  CHECK(p.fallback == bf::FallbackPolicy::Strict);
  CHECK(p.clearVoiceReduction == 0.0);
  CHECK(p.talkers.cvrMinFloor == 1u);  // r = 0 value; never raises min above the Character min
  CHECK(p.talkers.cvrDominanceCapDb == Approx(6.0));
  CHECK(p.talkers.cvrLevelSigmaMult == Approx(1.0));
  CHECK(p.talkers.cvrSoloRiskWeight == Approx(1.0));
  CHECK(p.talkers.cvrOnsetWindowMs == 0.0);
  CHECK(p.talkers.cvrMaxPhraseS == 0.0);
  CHECK_FALSE(p.selector.poolRotation);
  CHECK(p.mix.babbleFraction == Approx(1.0));
  CHECK(p.stationary.keepHot);

  // Explicit CVR enables it.
  m.clearVoiceReduction = 1.0;
  CHECK(lab.buildPlan(area("office"), raw, m, bigCorpus(), bf::makeStereoLayout()).talkers.cvrDominanceCapDb ==
        Approx(2.5));

  // continuousN: exactly N distinct speakers, 20 ms crossfades, 100 ms max gap.
  bf::StrategyParams cn;
  cn.labMode = bf::LabSubMode::ContinuousN;
  cn.labN = 8;
  p = lab.buildPlan(area("office"), cn, {}, bigCorpus(), bf::makeStereoLayout());
  CHECK(p.talkers.mode == bf::PlanMode::ContinuousN);
  CHECK(p.talkers.maxActive == 8u);
  CHECK(p.talkers.pool == 8u);
  CHECK(p.speakersNeeded == 8);
  CHECK(p.talkers.maxGapMs == Approx(100.0));
  CHECK(p.talkers.overlapMinMs == Approx(20.0));
  CHECK(p.talkers.gainSigmaDb == 0.0);
  // pink / ssn: no corpus.
  cn.labMode = bf::LabSubMode::Pink;
  p = lab.buildPlan(area("office"), cn, {}, bigCorpus(), bf::makeStereoLayout());
  CHECK_FALSE(p.babbleEnabled);
  CHECK(p.target.id == "pink");
  cn.labMode = bf::LabSubMode::Hybrid;
  CHECK(lab.buildPlan(area("office"), cn, {}, bigCorpus(), bf::makeStereoLayout()).mix.babbleFraction ==
        Approx(0.5));
}

TEST_CASE("validate clamps invariants min <= mean <= max <= pool <= available", "[strategy][validate]") {
  bf::CorpusSummary c;
  c.availableSpeakers = 6;
  bf::StrategyParams p;
  p.pool = 30;
  p.maxActive = 40;
  p.meanActive = 50.0;
  p.minActive = 45;
  p.segmentMinS = 12.0;
  p.segmentMaxS = 5.0;
  p.fadeInMs = 900.0;
  p.fadeOutMs = 900.0;
  p.babbleFraction = 1.4;
  p.spread = -1.0;
  p.strengthDb = 40.0;
  bf::MacroState m;
  m.character = 2.0;
  m.voiceAmount = -1.0;
  auto r = strategy("balanced").validate(p, m, c);
  CHECK(r.changed());
  CHECK(*p.pool <= 6);
  CHECK(*p.maxActive <= *p.pool);
  CHECK(*p.meanActive <= *p.maxActive);
  CHECK(*p.minActive <= *p.meanActive);
  CHECK(*p.segmentMinS < *p.segmentMaxS);
  CHECK(*p.fadeInMs + *p.fadeOutMs < 1000.0 * *p.segmentMinS);
  CHECK(*p.babbleFraction == 1.0);
  CHECK(*p.spread == 0.0);
  CHECK(*p.strengthDb == Approx(12.0));
  CHECK(*m.character == 1.0);
  CHECK(*m.voiceAmount == 0.0);
  for (const auto& a : r.adjustments) CHECK_FALSE(a.reason.empty());
  // The plan built from validated params satisfies the invariants too.
  auto pl = strategy("balanced").buildPlan(area("office"), p, m, c, bf::makeStereoLayout());
  checkPlanValid(pl, 6);

  // Valid parameters produce no adjustments.
  bf::StrategyParams ok;
  ok.pool = 14;
  ok.meanActive = 6.5;
  bf::MacroState mm;
  CHECK_FALSE(strategy("balanced").validate(ok, mm, bigCorpus()).changed());
}

TEST_CASE("buildPlan is deterministic and planHash is stable", "[strategy][plan][hash]") {
  bf::MacroState m;
  m.character = 0.7;
  const auto a = plan("balanced", "open_office", m);
  const auto b = plan("balanced", "open_office", m);
  CHECK(a.canonicalJson() == b.canonicalJson());
  CHECK(a.planHash == b.planHash);
  CHECK(a.planHash == a.computeHash());
  // Independent library / data-set instances give the same hash.
  auto ds2 = bf::loadDataSet(BF_DATA_DIR);
  REQUIRE(ds2.ok);
  bf::StrategyLibrary lib2(ds2.data);
  bf::StrategyParams sp;
  bf::MacroState m2 = m;
  lib2.find("balanced")->validate(sp, m2, bigCorpus());
  const auto c = lib2.find("balanced")->buildPlan(ds2.data.areas.at("open_office"), sp, m2, bigCorpus(),
                                                  bf::makeStereoLayout());
  CHECK(c.planHash == a.planHash);
  // Seed does not change the plan hash; content does.
  bf::StrategyParams seeded;
  seeded.seed = 12345;
  const auto d = plan("balanced", "open_office", m, seeded);
  CHECK(d.talkers.seed == 12345u);
  CHECK(d.planHash == a.planHash);
  m.character = 0.71;
  CHECK(plan("balanced", "open_office", m).planHash != a.planHash);
  CHECK(plan("dense", "open_office").planHash != plan("balanced", "open_office").planHash);
  CHECK(plan("balanced", "office", {}, {}, bigCorpus(), bf::makeRing4Layout()).planHash !=
        plan("balanced", "office").planHash);
}

TEST_CASE("all areas x strategies produce valid plans", "[strategy][plan]") {
  const auto& ds = dataSet();
  REQUIRE(ds.areas.size() == 9);
  REQUIRE(ds.strategies.size() == 7);
  const bf::OutputLayout layouts[] = {bf::makeMonoLayout(), bf::makeStereoLayout(), bf::makeRing4Layout(),
                                      bf::makeRegularGridLayout(2, 4, 3.0f)};
  for (const auto& [aid, ar] : ds.areas)
    for (const auto& [sid, sd] : ds.strategies)
      for (const auto& layout : layouts)
        for (double v : {0.0, 0.5, 1.0}) {
          INFO(aid << " / " << sid << " / " << layout.size() << " outputs / v=" << v);
          bf::MacroState m;
          m.voiceAmount = v;
          const auto p = plan(sid, aid, m, {}, bigCorpus(), layout);
          checkPlanValid(p, 200);
          CHECK(p.areaId == aid);
          CHECK(std::string(bf::toString(p.strategyId)) == sid);
          CHECK(std::string(bf::toString(p.strategyClass)) == sd.klass);
          CHECK(p.spatial.activeOutputs == layout.activeCount());
          if (layout.size() == 1) CHECK(p.spatial.algorithm == bf::SpatialAlgorithm::Mono);
        }
  // Distributed areas use the large algorithm with >= 4 outputs.
  CHECK(plan("balanced", "open_office", {}, {}, bigCorpus(), bf::makeRing4Layout()).spatial.algorithm ==
        bf::SpatialAlgorithm::LargeDistributed);
  CHECK(plan("balanced", "office", {}, {}, bigCorpus(), bf::makeRing4Layout()).spatial.algorithm ==
        bf::SpatialAlgorithm::SmallMultichannel);
}

TEST_CASE("§9 clamp: mean <= available speakers - 2", "[strategy][plan]") {
  bf::MacroState m;
  m.voiceAmount = 1.0;
  m.character = 1.0;  // m = 16 * 1.45 / 1.045, max 27, pool 31
  bf::CorpusSummary c;
  c.availableSpeakers = 40;
  auto p = plan("balanced", "office", m, {}, c);
  CHECK(p.talkers.mean == Approx(16.0 * 1.45 / 1.045));
  CHECK(p.talkers.maxActive == 27u);
  CHECK(p.talkers.pool == 31u);
  // Binding case: user mean = max = 32 with 33 speakers -> m <= 31.
  bf::StrategyParams sp;
  sp.meanActive = 32.0;
  sp.maxActive = 32;
  sp.pool = 33;
  c.availableSpeakers = 33;
  p = plan("balanced", "office", m, sp, c);
  CHECK(p.talkers.mean == Approx(31.0));
  checkPlanValid(p, 33);
}

TEST_CASE("talker-count mode table (TALKER_ENGINE.md §5)", "[strategy][modes]") {
  struct Row {
    int n, pool;
    double mean;
    int mn, mx;
    bool fixedK;
  };
  const Row rows[] = {{1, 4, 1, 1, 1, true},     {2, 6, 2, 2, 2, true},       {3, 7, 3, 3, 3, true},
                      {4, 8, 4, 4, 4, true},     {5, 10, 5, 5, 5, true},      {7, 12, 7, 7, 7, true},
                      {8, 18, 8, 6, 10, false},  {12, 26, 12, 9, 15, false},  {16, 34, 16, 12, 20, false},
                      {24, 48, 24, 18, 30, false}};
  for (const Row& r : rows) {
    const auto m = bf::talkerCountMode(r.n);
    CHECK(m.tabulated);
    CHECK(m.pool == r.pool);
    CHECK(m.mean == Approx(r.mean));
    CHECK(m.minActive == r.mn);
    CHECK(m.maxActive == r.mx);
    CHECK(m.fixedK == r.fixedK);
    CHECK(m.labContinuousN == r.n);
    if (!r.fixedK) {  // stochastic rule reproduces the table
      CHECK(r.mn == static_cast<int>(std::lround(0.75 * r.n)));
      CHECK(r.mx == static_cast<int>(std::lround(1.25 * r.n)));
      // The table gives P = 48 for N = 24 where the rule gives 50; the table row is used.
      if (r.n != 24) CHECK(r.pool == std::max(2 * r.n + 2, r.mx + 4));
    }
  }
  const auto c10 = bf::talkerCountMode(10);
  CHECK_FALSE(c10.tabulated);
  CHECK_FALSE(c10.fixedK);
  CHECK(c10.minActive == 8);
  CHECK(c10.maxActive == 13);
  CHECK(c10.pool == 22);
  CHECK(bf::talkerCountMode(24, 30).pool == 30);  // capped at the corpus
  CHECK(bf::talkerCountMode(6).fixedK);
  CHECK(bf::multiVoicePool(7) == 12);
  CHECK(bf::multiVoicePool(9) == 20);
}

TEST_CASE("composePlan wires compose + validate + buildPlan", "[strategy][compose]") {
  std::string text;
  REQUIRE(bf::readFile(std::string(BF_TEST_DATA_DIR) + "/example.bfpreset", text));
  auto pr = bf::parsePreset(text);
  REQUIRE(pr.ok);
  auto cp = bf::composePlan(dataSet(), pr.preset, bigCorpus(), bf::makeRing4Layout());
  REQUIRE(cp.ok);
  CHECK(cp.plan.strategyId == bf::StrategyId::Hybrid);
  CHECK(cp.plan.talkers.pool == 20u);
  CHECK(cp.plan.talkers.mean == Approx(8.5));  // user override replaces the macro mean
  CHECK(cp.plan.mix.babbleFraction == Approx(0.5));
  CHECK(cp.plan.character == Approx(0.65));
  CHECK(cp.plan.spatial.params.spread == Approx(0.85f));
  CHECK(cp.plan.talkers.segMaxS == Approx(15.0));
  checkPlanValid(cp.plan, 200);

  bf::Preset p;
  p.area = "office";
  p.strategy = "balanced";
  auto d = bf::composePlan(dataSet(), p, bigCorpus(), bf::makeStereoLayout());
  REQUIRE(d.ok);
  CHECK(d.plan.planHash == plan("balanced", "office").planHash);
  p.strategy = "nope";
  CHECK_FALSE(bf::composePlan(dataSet(), p, bigCorpus(), bf::makeStereoLayout()).ok);
}

TEST_CASE("every area/strategy spectrum target id resolves exactly", "[strategy][target]") {
  const auto& ds = dataSet();
  for (const auto& [id, a] : ds.areas)
    if (a.spectrum.target != "custom") CHECK(ds.targets.count(a.spectrum.target) == 1);
  CHECK(ds.targets.count("ltass_universal") == 1);
  // Unknown id: no prefix matching; adjustment + ltass_universal.
  bf::StrategyParams sp;
  sp.spectrumTarget = "ltass";
  const auto p = plan("balanced", "office", {}, sp);
  CHECK(hasAdj(p.adjustments, "spectrum.target"));
  CHECK(p.target.id == "ltass_universal");
}
