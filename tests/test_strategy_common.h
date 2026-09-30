#pragma once
// Shared fixtures for tests/test_strategy_*.cpp.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "core/config/DataSet.h"
#include "core/spatial/OutputLayout.h"
#include "core/strategy/MaskStrategy.h"

namespace bftest {

inline const bf::DataSet& dataSet() {
  static const bf::DataSet ds = [] {
    auto r = bf::loadDataSet(BF_DATA_DIR);
    REQUIRE(r.ok);
    return r.data;
  }();
  return ds;
}

inline const bf::StrategyLibrary& library() {
  static const bf::StrategyLibrary lib(dataSet());
  return lib;
}

inline const bf::MaskStrategy& strategy(const char* id) {
  const bf::MaskStrategy* s = library().find(id);
  REQUIRE(s != nullptr);
  return *s;
}

inline const bf::AreaModel& area(const std::string& id) { return dataSet().areas.at(id); }

inline bf::CorpusSummary bigCorpus() {
  bf::CorpusSummary c;
  c.availableSpeakers = 200;
  c.corpusVersion = "test";
  return c;
}

inline bf::MaskRenderPlan plan(const std::string& strategyId, const std::string& areaId, bf::MacroState m = {},
                               bf::StrategyParams p = {}, bf::CorpusSummary c = bigCorpus(),
                               const bf::OutputLayout& layout = bf::makeStereoLayout()) {
  const bf::MaskStrategy& s = strategy(strategyId.c_str());
  s.validate(p, m, c);
  return s.buildPlan(area(areaId), p, m, c, layout);
}

// min <= mean <= max <= pool (babble plans) and basic value sanity.
inline void checkPlanValid(const bf::MaskRenderPlan& pl, int available) {
  CHECK(pl.planHash == pl.computeHash());
  CHECK(pl.planHash != 0);
  CHECK(pl.mix.babbleFraction >= 0.0);
  CHECK(pl.mix.babbleFraction <= 1.0);
  for (double z : pl.mix.zoneBabbleFraction) CHECK((z >= 0.0 && z <= 1.0));
  CHECK(pl.target.thirdOctDb[bf::kBand1k] == 0.0f);
  CHECK(pl.level.outputRmsDbfs() == pl.level.lRefDbfs + pl.level.strengthDb);
  CHECK(pl.status.runnable);
  if (!pl.babbleEnabled) {
    CHECK(pl.mix.babbleFraction == 0.0);
    CHECK(pl.stationary.enabled);
    return;
  }
  const auto& t = pl.talkers;
  CHECK(static_cast<double>(t.minActive) <= t.mean);
  CHECK(t.mean <= static_cast<double>(t.maxActive));
  CHECK(t.maxActive <= t.pool);
  CHECK(static_cast<int>(t.pool) <= available);
  CHECK(t.mean >= 1.0);
  CHECK(t.mean <= 32.0);
  CHECK(t.pool <= 64u);
  CHECK(t.segMinS < t.segMaxS);
  CHECK(t.fadeInMs + t.fadeOutMs < 1000.0 * t.segMinS);
  CHECK(t.maxGapMs > 0.0);
  CHECK(t.medianS > 0.0);
  CHECK(pl.speakersNeeded >= static_cast<int>(t.maxActive));
}

}  // namespace bftest
