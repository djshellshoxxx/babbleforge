#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include "test_strategy_common.h"

using Catch::Approx;
using bftest::plan;
using bftest::strategy;

namespace {

bool hasCode(const bf::MaskRenderPlan& p, const std::string& c) {
  return std::find(p.status.degraded.begin(), p.status.degraded.end(), c) != p.status.degraded.end();
}

bf::DegradeReason insufficient(int s) {
  bf::DegradeReason r;
  r.kind = bf::DegradeReason::Kind::CorpusInsufficient;
  r.availableSpeakers = s;
  return r;
}

// Office / balanced at c = 0.5: pool 14, mean 6.5, min 4, max 9, b 0.70, needed 14.
bf::MaskRenderPlan officeBalanced(bf::FallbackPolicy policy = bf::FallbackPolicy::Continuous) {
  bf::MacroState m;
  m.character = 0.5;
  bf::StrategyParams p;
  p.fallback = policy;
  return plan("balanced", "office", m, p);
}

}  // namespace

TEST_CASE("degrade: reduced plans for S available speakers (RELIABILITY §3)", "[strategy][degrade]") {
  const auto& s = strategy("balanced");
  const auto base = officeBalanced();
  REQUIRE(base.speakersNeeded == 14);  // max(P = 14, max + 4 = 13)
  REQUIRE(base.talkers.maxActive == 9u);

  SECTION("S >= speakersNeeded: normal") {
    for (int S : {14, 20, 200}) {
      const auto d = s.degrade(base, insufficient(S));
      CHECK(d.canonicalJson() == base.canonicalJson());
      CHECK(d.status.degraded.empty());
    }
  }
  SECTION("maxActive + 1 <= S < speakersNeeded: P = S, recent-speaker rule relaxed") {
    for (int S : {10, 12, 13}) {
      const auto d = s.degrade(base, insufficient(S));
      CHECK(d.talkers.pool == static_cast<std::uint32_t>(S));
      CHECK(d.talkers.maxActive == 9u);
      CHECK(d.talkers.mean == Approx(6.5));
      CHECK(d.talkers.minActive == 4u);
      CHECK(d.selector.relaxRecentSpeakerRule);
      CHECK(hasCode(d, "corpus.insufficient"));
      CHECK(d.mix.babbleFraction == Approx(base.mix.babbleFraction));
      CHECK(d.planHash != base.planHash);
      CHECK(d.planHash == d.computeHash());
      CHECK(d.status.runnable);
    }
  }
  SECTION("2 <= S <= maxActive: max = S - 1, mean/min scaled, stationary compensation") {
    struct Row {
      int S;
      int max;
      double mean;
      int min;
      double stationary;
    };
    // ratio = (S - 1) / 9; stationary = min(0.30 + 0.5 (1 - ratio), 0.6).
    const Row rows[] = {
        {9, 8, 6.5 * 8 / 9, 4, 0.30 + 0.5 * (1.0 / 9)},   // min round(3.56) = 4
        {6, 5, 6.5 * 5 / 9, 2, 0.30 + 0.5 * (4.0 / 9)},   // min round(2.22) = 2
        {4, 3, 6.5 * 3 / 9, 1, 0.6},                      // 0.30 + 0.333 -> capped 0.6
        {2, 1, 6.5 * 1 / 9, 0, 0.6},
    };
    for (const Row& r : rows) {
      INFO("S = " << r.S);
      const auto d = s.degrade(base, insufficient(r.S));
      CHECK(d.talkers.maxActive == static_cast<std::uint32_t>(r.max));
      CHECK(d.talkers.pool == static_cast<std::uint32_t>(r.S));
      CHECK(d.talkers.mean == Approx(r.mean));
      CHECK(d.talkers.minActive == static_cast<std::uint32_t>(r.min));
      CHECK(d.talkers.minActive <= d.talkers.mean);
      CHECK(d.talkers.mean <= d.talkers.maxActive);
      CHECK(d.talkers.maxActive < d.talkers.pool);  // no speaker in two slots at once
      CHECK(d.mix.stationaryFraction() == Approx(r.stationary));
      CHECK(d.mix.stationaryCompensation == Approx(r.stationary - 0.30));
      CHECK(d.mix.zoneBabbleFraction[0] == Approx(d.mix.babbleFraction));
      CHECK(d.babbleEnabled);
      CHECK(hasCode(d, "corpus.insufficient"));
    }
  }
  SECTION("S <= 1: babble impossible -> stationary (Continuous)") {
    for (int S : {0, 1}) {
      const auto d = s.degrade(base, insufficient(S));
      CHECK_FALSE(d.babbleEnabled);
      CHECK(d.strategyClass == bf::StrategyClass::StationarySpeechMask);
      CHECK(d.mix.babbleFraction == 0.0);
      CHECK(d.stationary.enabled);
      CHECK(hasCode(d, "corpus.none"));
      CHECK(d.status.runnable);
      CHECK(d.target.id == base.target.id);            // same spectrum target
      CHECK(d.level.strengthDb == base.level.strengthDb);  // same level
    }
    bf::DegradeReason none;
    none.kind = bf::DegradeReason::Kind::CorpusNone;
    CHECK_FALSE(s.degrade(base, none).babbleEnabled);
  }
}

TEST_CASE("degrade: fallback policies (RELIABILITY §4)", "[strategy][degrade]") {
  const auto& s = strategy("balanced");
  SECTION("Safe switches to stationary on any insufficient corpus") {
    const auto base = officeBalanced(bf::FallbackPolicy::Safe);
    for (int S : {12, 6, 1}) {
      const auto d = s.degrade(base, insufficient(S));
      CHECK_FALSE(d.babbleEnabled);
      CHECK(d.mix.babbleFraction == 0.0);
      CHECK(hasCode(d, "fallback.stationary"));
      CHECK(d.status.runnable);
    }
    CHECK(s.degrade(base, insufficient(14)).babbleEnabled);
    bf::DegradeReason f;
    f.kind = bf::DegradeReason::Kind::SourceFailures;
    f.availableSpeakers = 200;
    f.failedPoolFraction = 0.05;
    CHECK(s.degrade(base, f).babbleEnabled);
    f.failedPoolFraction = 0.2;
    CHECK_FALSE(s.degrade(base, f).babbleEnabled);
  }
  SECTION("Strict marks an error") {
    const auto base = officeBalanced(bf::FallbackPolicy::Strict);
    CHECK_FALSE(base.stationary.keepHot);
    for (int S : {12, 6, 1}) {
      const auto d = s.degrade(base, insufficient(S));
      CHECK(d.status.error);
      CHECK_FALSE(d.status.runnable);
      CHECK(d.status.errorCode == (S <= 1 ? "corpus.none" : "corpus.insufficient"));
    }
    CHECK_FALSE(s.degrade(base, insufficient(20)).status.error);
    bf::DegradeReason f;
    f.kind = bf::DegradeReason::Kind::SourceFailures;
    f.failedPoolFraction = 0.01;
    CHECK(s.degrade(base, f).status.errorCode == "source.failure");
  }
  SECTION("Continuous: source failures substitute and continue") {
    const auto base = officeBalanced();
    bf::DegradeReason f;
    f.kind = bf::DegradeReason::Kind::SourceFailures;
    f.failedPoolFraction = 0.2;
    const auto d = s.degrade(base, f);
    CHECK(d.babbleEnabled);
    CHECK(hasCode(d, "corpus.reduced"));
  }
  SECTION("Laboratory is always Strict") {
    bf::StrategyParams p;
    p.labMode = bf::LabSubMode::ContinuousN;
    p.labN = 8;
    p.fallback = bf::FallbackPolicy::Continuous;
    const auto base = plan("research", "office", {}, p);
    CHECK(base.fallback == bf::FallbackPolicy::Strict);
    CHECK(base.speakersNeeded == 8);
    CHECK_FALSE(strategy("research").degrade(base, insufficient(8)).status.error);
    const auto d = strategy("research").degrade(base, insufficient(7));
    CHECK(d.status.error);
    CHECK(d.status.errorCode == "corpus.insufficient");
  }
  SECTION("MultiVoice reduces K and compensates") {
    const auto base = plan("multi_voice", "office");  // K 7, pool 12
    REQUIRE(base.speakersNeeded == 12);
    auto d = strategy("multi_voice").degrade(base, insufficient(10));
    CHECK(d.talkers.maxActive == 7u);
    CHECK(d.talkers.pool == 10u);
    d = strategy("multi_voice").degrade(base, insufficient(5));
    CHECK(d.talkers.maxActive == 4u);
    CHECK(d.talkers.minActive == 4u);
    CHECK(d.talkers.mean == Approx(4.0));
    CHECK(d.mix.stationaryFraction() == Approx(0.5 * (1.0 - 4.0 / 7.0)));
  }
  SECTION("stationary-only strategies are unaffected") {
    const auto base = plan("speech_noise", "office");
    const auto d = strategy("speech_noise").degrade(base, insufficient(0));
    CHECK(d.canonicalJson() == base.canonicalJson());
    CHECK(d.status.runnable);
  }
}
