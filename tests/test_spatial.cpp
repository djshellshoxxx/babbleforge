// Spatial policy, layouts and channel balance (docs/SPATIAL_ENGINE.md sections 2-4).
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <set>

#include "core/spatial/ChannelBalance.h"
#include "core/spatial/OutputLayout.h"
#include "core/spatial/SpatialPolicy.h"

using namespace bf;
using Catch::Matchers::WithinAbs;

namespace {

double sumSq(const GainVector& g) {
  double s = 0;
  for (float v : g) s += static_cast<double>(v) * static_cast<double>(v);
  return s;
}
double db(double x) { return 20.0 * std::log10(x); }

}  // namespace

TEST_CASE("Layout factories and automatic algorithm selection", "[spatial]") {
  CHECK(selectAlgorithm(makeMonoLayout()) == SpatialAlgorithm::Mono);
  CHECK(selectAlgorithm(makeStereoLayout()) == SpatialAlgorithm::DistributedStereo);
  CHECK(selectAlgorithm(makeRing4Layout()) == SpatialAlgorithm::SmallMultichannel);
  CHECK(selectAlgorithm(makeRing6Layout()) == SpatialAlgorithm::SmallMultichannel);
  CHECK(selectAlgorithm(makeRing8Layout()) == SpatialAlgorithm::SmallMultichannel);
  CHECK(selectAlgorithm(makeCustomRingLayout(3)) == SpatialAlgorithm::SmallMultichannel);
  CHECK(selectAlgorithm(makeCustomRingLayout(12)) == SpatialAlgorithm::LargeDistributed);
  CHECK(selectAlgorithm(makeRegularGridLayout(2, 2, 2.0f)) == SpatialAlgorithm::LargeDistributed);
  const std::vector<Vec2> three{{0, 0}, {1, 0}, {2, 0}};
  CHECK(selectAlgorithm(makeGridLayout(three)) == SpatialAlgorithm::LargeDistributed);

  const auto r8 = makeRing8Layout();
  REQUIRE(r8.size() == 8);
  std::set<float> az;
  for (const auto& o : r8.outputs) az.insert(std::fabs(o.azimuthDeg));
  CHECK(az == std::set<float>{22.5f, 67.5f, 112.5f, 157.5f});
  CHECK(makeRing6Layout().size() == 6);
  CHECK(makeStereoLayout().outputs[0].azimuthDeg == 30.0f);
  const auto eq5 = makeCustomRingLayout(5);
  CHECK(eq5.outputs[0].azimuthDeg == Catch::Approx(-144.0f));
  CHECK_THROWS(makeCustomRingLayout(2));
  CHECK_THROWS(makeCustomRingLayout(17));
  auto disabled = makeRing6Layout();
  for (int i = 2; i < 6; ++i) disabled.outputs[static_cast<std::size_t>(i)].enabled = false;
  CHECK(selectAlgorithm(disabled) == SpatialAlgorithm::DistributedStereo);
}

TEST_CASE("Mono policy is unity", "[spatial]") {
  SpatialPolicy p(makeMonoLayout(), {}, 1);
  const auto g = p.placeTalker(0);
  REQUIRE(g.size() == 1);
  CHECK(g[0] == 1.0f);
}

TEST_CASE("Distributed stereo: constant power, far channel floor, no hard panning", "[spatial]") {
  SpatialParams sp;
  sp.spread = 1.0f;  // p_max = 0.70
  sp.motion = 1.0f;
  SpatialPolicy p(makeStereoLayout(), sp, 42);
  REQUIRE(p.algorithm() == SpatialAlgorithm::DistributedStereo);
  CHECK(p.stereoMaxPan() == Catch::Approx(0.70));
  for (int slot = 0; slot < 40; ++slot) {
    auto g = p.placeTalker(slot);
    for (int step = 0; step < 300; ++step) {
      REQUIRE_THAT(sumSq(g), WithinAbs(1.0, 1e-6));
      const double hi = std::max(g[0], g[1]), lo = std::min(g[0], g[1]);
      REQUIRE(db(static_cast<double>(lo) / static_cast<double>(hi)) >= -12.6);
      g = p.updateMotion(slot, 48000);
    }
  }
  // Default spread 0.6 -> p_max 0.42.
  SpatialPolicy d(makeStereoLayout(), SpatialParams{}, 42);
  CHECK(d.stereoMaxPan() == Catch::Approx(0.42));
  // High speaker variation widens by 1.1 but never beyond 0.7.
  SpatialParams hi;
  hi.variation = SpeakerVariation::High;
  hi.spread = 1.0f;
  CHECK(SpatialPolicy(makeStereoLayout(), hi, 1).stereoMaxPan() == Catch::Approx(0.70));
  hi.spread = 0.6f;
  CHECK(SpatialPolicy(makeStereoLayout(), hi, 1).stereoMaxPan() == Catch::Approx(0.462));
}

TEST_CASE("Distributed stereo: balancing and motion rate", "[spatial]") {
  SpatialPolicy p(makeStereoLayout(), SpatialParams{}, 7);
  const double e[2] = {10.0, 1.0};  // left is loud -> talkers should go right
  p.setRecentEnergy(e);
  int right = 0;
  const int n = 2000;
  for (int i = 0; i < n; ++i) {
    p.placeTalker(i % 16);
    right += p.slotPan(i % 16) > 0.0 ? 1 : 0;
  }
  CHECK(right > static_cast<int>(0.78 * n));  // expected 0.5 + 0.7/2 = 0.85

  SpatialParams sp;
  sp.motion = 0.6f;
  SpatialPolicy m(makeStereoLayout(), sp, 9);
  m.placeTalker(0);
  double prev = m.slotPan(0), maxRate = 0.0;
  int dirChanges = 0;
  double prevDir = 0.0;
  for (int i = 0; i < 600; ++i) {
    m.updateMotion(0, 4800);  // 0.1 s
    const double cur = m.slotPan(0);
    maxRate = std::max(maxRate, std::fabs(cur - prev) / 0.1);
    const double dir = cur - prev;
    if (dir != 0.0 && prevDir != 0.0 && (dir > 0) != (prevDir > 0)) ++dirChanges;
    if (dir != 0.0) prevDir = dir;
    prev = cur;
  }
  CHECK(maxRate <= 0.02 * 0.6 + 1e-9);
  CHECK(dirChanges <= 60 / 4);  // at most one reversal per target interval (>= 4 s)
}

TEST_CASE("VBAP pair selection and 2x2 inverse", "[spatial]") {
  SpatialPolicy p(makeRing4Layout(), SpatialParams{}, 1);
  // Ring4 outputs: 135, 45, -45, -135. A source on a speaker feeds only that speaker.
  auto g = p.vbapGains(45.0);
  CHECK(g[1] == Catch::Approx(1.0f).margin(1e-5));
  CHECK(g[0] == Catch::Approx(0.0f).margin(1e-5));
  // Midway between two speakers: equal gains (symmetric), 1/sqrt2 each.
  g = p.vbapGains(90.0);
  CHECK(g[0] == Catch::Approx(g[1]).margin(1e-5));
  CHECK(g[0] == Catch::Approx(std::sqrt(0.5)).margin(1e-4));
  CHECK(g[2] == 0.0f);
  // Tangent law check off-centre: gains ratio follows the 2D VBAP solution (unit vectors).
  g = p.vbapGains(60.0);  // between 45 (out 1) and 135 (out 0)
  const double gi = std::sin((135.0 - 60.0) * 3.14159265358979 / 180.0);
  const double gj = std::sin((60.0 - 45.0) * 3.14159265358979 / 180.0);
  CHECK(static_cast<double>(g[0]) / static_cast<double>(g[1]) == Catch::Approx(gj / gi).epsilon(0.01));
  // Wrap-around pair (rear): between 135 and -135.
  g = p.vbapGains(180.0);
  CHECK(g[0] == Catch::Approx(g[3]).margin(1e-5));
}

TEST_CASE("Small multichannel: power normalisation and minimum spread rule", "[spatial]") {
  const std::vector<OutputLayout> layouts{makeRing4Layout(), makeRing6Layout(), makeRing8Layout(),
                                          makeCustomRingLayout(3), makeCustomRingLayout(5),
                                          makeCustomRingLayout(7)};
  for (const auto& l : layouts) {
    for (float spread : {0.0f, 0.3f, 0.6f, 1.0f}) {
      SpatialParams sp;
      sp.spread = spread;
      SpatialPolicy p(l, sp, 5);
      REQUIRE(p.algorithm() == SpatialAlgorithm::SmallMultichannel);
      for (int a = -180; a < 180; a += 1) {
        const auto g = p.mdapGains(a, spread * 45.0);
        REQUIRE_THAT(sumSq(g), WithinAbs(1.0, 1e-5));
        std::vector<float> s(g);
        std::sort(s.rbegin(), s.rend());
        REQUIRE(s[1] > 0.0f);
        REQUIRE(db(static_cast<double>(s[1]) / static_cast<double>(s[0])) >= -9.0);
      }
    }
  }
}

TEST_CASE("Small multichannel: every placed and moving talker feeds >= 2 outputs", "[spatial]") {
  for (const auto& l : {makeRing4Layout(), makeRing6Layout(), makeRing8Layout()}) {
    for (auto var : {SpeakerVariation::Low, SpeakerVariation::Medium, SpeakerVariation::High}) {
      SpatialParams sp;
      sp.spread = 0.2f;
      sp.motion = 1.0f;
      sp.variation = var;
      SpatialPolicy p(l, sp, 11);
      for (int slot = 0; slot < 24; ++slot) {
        auto g = p.placeTalker(slot);
        for (int step = 0; step < 40; ++step) {
          REQUIRE_THAT(sumSq(g), WithinAbs(1.0, 1e-5));
          std::vector<float> s(g);
          std::sort(s.rbegin(), s.rend());
          REQUIRE(s[1] > 0.0f);
          REQUIRE(db(static_cast<double>(s[1]) / static_cast<double>(s[0])) >= -9.0);
          g = p.updateMotion(slot, 48000 * 3);
        }
      }
    }
  }
}

TEST_CASE("Small multichannel: balancing, motion rate and determinism", "[spatial]") {
  SpatialParams sp;
  sp.motion = 1.0f;
  SpatialPolicy p(makeRing8Layout(), sp, 3);
  std::vector<double> energy(8, 1.0);
  energy[3] = 0.0;  // least loaded
  p.setRecentEnergy(energy);
  const double target = static_cast<double>(makeRing8Layout().outputs[3].azimuthDeg);
  int near = 0;
  const int n = 1500;
  for (int i = 0; i < n; ++i) {
    p.placeTalker(0);
    double d = std::fabs(p.slotAzimuthDeg(0) - target);
    d = std::min(d, 360.0 - d);
    near += d <= 15.0 + 1e-9 ? 1 : 0;
  }
  CHECK(near > static_cast<int>(0.62 * n));  // expected ~0.7 (+ uniform hits)

  p.placeTalker(1);
  double prev = p.slotAzimuthDeg(1), maxRate = 0.0;
  for (int i = 0; i < 1200; ++i) {
    p.updateMotion(1, 4800);
    double d = std::fabs(p.slotAzimuthDeg(1) - prev);
    d = std::min(d, 360.0 - d);
    maxRate = std::max(maxRate, d / 0.1);
    prev = p.slotAzimuthDeg(1);
  }
  CHECK(maxRate <= 3.0 + 1e-6);

  SpatialPolicy a(makeRing6Layout(), sp, 99), b(makeRing6Layout(), sp, 99), c(makeRing6Layout(), sp, 100);
  bool differs = false;
  for (int slot = 0; slot < 6; ++slot) {
    auto ga = a.placeTalker(slot), gb = b.placeTalker(slot), gc = c.placeTalker(slot);
    CHECK(ga == gb);
    differs = differs || ga != gc;
    CHECK(a.updateMotion(slot, 480000) == b.updateMotion(slot, 480000));
  }
  CHECK(differs);
}

TEST_CASE("Large distributed: neighbourhoods on a grid", "[spatial]") {
  // 4x4 grid, 2 m pitch; zones: columns 0-1 = zone 0, columns 2-3 = zone 1.
  std::vector<Vec2> pos;
  std::vector<std::uint8_t> zones;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      pos.push_back({2.0f * static_cast<float>(c), 2.0f * static_cast<float>(r)});
      zones.push_back(c < 2 ? 0 : 1);
    }
  const auto layout = makeGridLayout(pos, zones);
  SpatialParams sp;
  sp.motion = 1.0f;
  SpatialPolicy p(layout, sp, 21);
  REQUIRE(p.algorithm() == SpatialAlgorithm::LargeDistributed);
  CHECK(p.effectiveNeighbourhoodSize() == 3);
  for (int o = 0; o < 16; ++o) {
    const auto nb = p.neighbourhood(o);
    REQUIRE(nb.size() == 3);
    CHECK(nb[0] == o);
    for (int q : nb) CHECK(layout.outputs[static_cast<std::size_t>(q)].zone == layout.outputs[static_cast<std::size_t>(o)].zone);
    // Neighbours are the nearest same-zone outputs: distance <= 2*sqrt2 for the grid.
    for (int q : nb) {
      const auto a = *layout.outputs[static_cast<std::size_t>(o)].posM, b = *layout.outputs[static_cast<std::size_t>(q)].posM;
      CHECK(std::hypot(a.x - b.x, a.y - b.y) <= 2.0 * std::sqrt(2.0) + 1e-4);
    }
  }
  // Corner output 0 at (0,0): nearest are (2,0) = 1 and (0,2) = 4 (tie broken by index).
  CHECK(std::vector<int>(p.neighbourhood(0).begin(), p.neighbourhood(0).end()) == std::vector<int>{0, 1, 4});
}

TEST_CASE("Large distributed: gains, zone confinement, migration", "[spatial]") {
  std::vector<Vec2> pos;
  std::vector<std::uint8_t> zones;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      pos.push_back({2.0f * static_cast<float>(c), 2.0f * static_cast<float>(r)});
      zones.push_back(c < 2 ? 0 : 1);
    }
  const auto layout = makeGridLayout(pos, zones);
  SpatialParams sp;
  sp.motion = 1.0f;
  SpatialPolicy p(layout, sp, 77);
  int migrated = 0;
  for (int slot = 0; slot < 96; ++slot) {
    const int zone = slot % 2;
    auto g = p.placeTalker(slot, 20.0, zone);
    const int home = p.slotHome(slot);
    REQUIRE(layout.outputs[static_cast<std::size_t>(home)].zone == zone);
    auto checkGains = [&](const GainVector& gv) {
      REQUIRE_THAT(sumSq(gv), WithinAbs(1.0, 1e-5));
      for (std::size_t o = 0; o < gv.size(); ++o)
        if (gv[o] > 0.0f) REQUIRE(layout.outputs[o].zone == zone);
    };
    checkGains(g);
    // Home 0 dB, neighbours -3..-6 dB relative to home.
    for (std::size_t o = 0; o < g.size(); ++o) {
      if (static_cast<int>(o) == home || g[o] == 0.0f) continue;
      const double rel = db(static_cast<double>(g[o]) / static_cast<double>(g[static_cast<std::size_t>(home)]));
      REQUIRE(rel <= -3.0 + 1e-3);
      REQUIRE(rel >= -6.0 - 1e-3);
    }
    for (int step = 0; step < 24; ++step) {
      g = p.updateMotion(slot, 48000);
      checkGains(g);  // also during the 3 s crossfade
    }
    if (p.slotMigrated(slot)) {
      ++migrated;
      CHECK(p.slotHome(slot) != home);
      CHECK(layout.outputs[static_cast<std::size_t>(p.slotHome(slot))].zone == zone);
    }
  }
  // probability 0.3 per segment -> ~29 of 96
  CHECK(migrated > 10);
  CHECK(migrated < 60);
}

TEST_CASE("Large distributed on a large ring uses angular neighbours", "[spatial]") {
  SpatialPolicy p(makeCustomRingLayout(12), SpatialParams{}, 4);
  REQUIRE(p.algorithm() == SpatialAlgorithm::LargeDistributed);
  const auto nb = p.neighbourhood(0);
  REQUIRE(nb.size() == 3);
  std::set<int> s(nb.begin(), nb.end());
  CHECK(s == std::set<int>{0, 1, 11});
}

TEST_CASE("Density scaling for large distributed layouts", "[spatial]") {
  auto d = scaleDensity(8.0, 4, 10, 16, 3);
  CHECK(d.mTotal == Catch::Approx(42.67).margin(0.01));
  CHECK(d.vTotal == 54);
  CHECK_FALSE(d.capped);
  CHECK(d.mEffective == 8.0);
  CHECK(totalPoolSize(20, d.vTotal) == 62);
  CHECK(totalPoolSize(100, d.vTotal) == 100);

  d = scaleDensity(8.0, 4, 20, 32, 3);  // V = 214 -> capped at 96
  CHECK(d.capped);
  CHECK(d.vTotal == 96);
  CHECK(d.mEffective == Catch::Approx(8.0 * 96.0 / 214.0).margin(0.02));
  CHECK(d.mTotal == Catch::Approx(8.0 * 32.0 / 3.0 * 96.0 / 214.0).margin(0.1));
}

TEST_CASE("Channel balance: feed-forward normalisation", "[spatial][balance]") {
  const std::vector<double> act{1.0, 0.5, 1.0};
  const std::vector<GainVector> gv{{1.0f, 0.0f}, {std::sqrt(0.5f), std::sqrt(0.5f)}, {1.0f, 0.0f}};
  const auto e = ChannelBalance::expectedPowers(act, gv, 2);
  CHECK(e[0] == Catch::Approx(2.25));
  CHECK(e[1] == Catch::Approx(0.25));
  ChannelBalance cb;
  cb.prepare(2);
  cb.updateFeedForward(e, 1.0);
  CHECK(cb.gain(0) * cb.gain(0) * e[0] == Catch::Approx(1.0));
  CHECK(cb.gain(1) * cb.gain(1) * e[1] == Catch::Approx(1.0));
  // Smoothing tau = 10 s: a step in E moves the gain by 1 - exp(-1/10) per second.
  const std::vector<double> e2{1.0, 1.0};
  const double before = cb.gain(0);
  cb.updateFeedForward(e2, 1.0);
  const double a = 1.0 - std::exp(-0.1);
  CHECK(cb.gain(0) == Catch::Approx(before + a * (1.0 - before)));
}

TEST_CASE("Channel balance: feedback trim limits, slew and freeze", "[spatial][balance]") {
  ChannelBalance cb;
  cb.prepare(3);
  // Channel 0 is +6 dB hot, channel 2 is -6 dB cold (power).
  const std::vector<double> loud{4.0, 1.0, 0.25};
  double prev0 = 0.0, maxSlew = 0.0;
  for (int t = 0; t < 600; ++t) {
    // measured after the trim
    std::vector<double> m(3);
    for (int c = 0; c < 3; ++c) {
      const double g = std::pow(10.0, cb.trimDb(c) / 20.0);
      m[static_cast<std::size_t>(c)] = loud[static_cast<std::size_t>(c)] * g * g;
    }
    cb.updateFeedback(m, 1.0);
    maxSlew = std::max(maxSlew, std::fabs(cb.trimDb(0) - prev0));
    prev0 = cb.trimDb(0);
    for (int c = 0; c < 3; ++c) CHECK(std::fabs(cb.trimDb(c)) <= 3.0 + 1e-12);
  }
  CHECK(maxSlew <= 0.25 + 1e-9);
  CHECK(cb.trimDb(0) == Catch::Approx(-3.0).margin(1e-6));  // wants ~-4.3 dB, clamped
  CHECK(cb.trimDb(2) == Catch::Approx(3.0).margin(1e-6));
  CHECK(cb.trimDb(1) > 0.0);  // mean power is above channel 1

  // Frozen: nothing moves.
  cb.setFrozen(true);
  const double held = cb.trimDb(0);
  const std::vector<double> flat{1.0, 1.0, 1.0};
  for (int t = 0; t < 100; ++t) cb.updateFeedback(flat, 1.0);
  CHECK(cb.trimDb(0) == held);

  // Balanced input keeps the trim at zero and is a fixed point.
  ChannelBalance eq;
  eq.prepare(4);
  const std::vector<double> ones(4, 2.0);
  for (int t = 0; t < 300; ++t) eq.updateFeedback(ones, 1.0);
  for (int c = 0; c < 4; ++c) CHECK(eq.trimDb(c) == 0.0);
}

TEST_CASE("Channel balance: feedback slew limit is exactly 0.25 dB/s", "[spatial][balance]") {
  ChannelBalance cb;
  cb.prepare(2);
  const std::vector<double> m{2.0, 0.5};  // large error -> slew-limited
  cb.updateFeedback(m, 1.0);
  CHECK(std::fabs(cb.trimDb(0)) <= 0.25 + 1e-12);
  cb.updateFeedback(m, 4.0);
  CHECK(std::fabs(cb.trimDb(0)) <= 1.25 + 1e-12);
}
