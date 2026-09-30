// Hybrid mixer (docs/MASK_STRATEGIES.md section 4).
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/engine/HybridMixer.h"
#include "core/random/Random.h"

using namespace bf;

namespace {

constexpr double kFs = 48000.0;

double powerDb(const float* x, std::size_t n) {
  double s = 0;
  for (std::size_t i = 0; i < n; ++i) s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
  return 10.0 * std::log10(s / static_cast<double>(n));
}

std::vector<float> unitNoise(std::size_t n, const char* name) {
  RngStream r(4242, name, 0);
  std::vector<float> v(n);
  for (auto& x : v) x = r.uniformPM1f() * 1.7320508f;  // unit variance
  return v;
}

}  // namespace

TEST_CASE("Hybrid mixer: constant-power law table", "[hybrid]") {
  const double bs[] = {0.0, 0.25, 0.5, 0.75, 1.0};
  const double gb[] = {0.0, 0.5, 0.7071, 0.8660, 1.0};
  const double gs[] = {1.0, 0.8660, 0.7071, 0.5, 0.0};
  for (int i = 0; i < 5; ++i) {
    const auto g = HybridMixer::gainsFor(bs[i]);
    CHECK(static_cast<double>(g.babble) == Catch::Approx(gb[i]).margin(1e-4));
    CHECK(static_cast<double>(g.stationary) == Catch::Approx(gs[i]).margin(1e-4));
  }
  // Regression guard: the linear-amplitude crossfade (g_b = b, g_s = 1 - b) is forbidden.
  for (double b = 0.05; b < 0.99; b += 0.05) {
    const auto g = HybridMixer::gainsFor(b);
    CHECK(static_cast<double>(g.babble) * static_cast<double>(g.babble) +
              static_cast<double>(g.stationary) * static_cast<double>(g.stationary) ==
          Catch::Approx(1.0).margin(1e-6));
    if (std::fabs(b - 0.5) < 1e-9) {
      CHECK(static_cast<double>(g.babble) > 0.7);  // linear law would give 0.5
    }
    const double linearPower = b * b + (1 - b) * (1 - b);
    CHECK(std::fabs(linearPower - 1.0) > 1e-3);  // sanity: the linear law is not constant power
  }
  CHECK(static_cast<double>(HybridMixer::gainsFor(0.5).babble) != 0.5);
}

TEST_CASE("Hybrid mixer: power constancy over 60 s with independent noises", "[hybrid]") {
  const std::size_t len = static_cast<std::size_t>(kFs * 60.0);
  const auto bab = unitNoise(len, "babble");
  const auto sta = unitNoise(len, "stationary");
  std::vector<float> out(len);
  for (double b : {0.0, 0.25, 0.5, 0.75, 1.0}) {
    HybridMixer m;
    m.prepare(kFs, {0}, b);
    const float* pb = bab.data();
    const float* ps = sta.data();
    float* po = out.data();
    int pos = 0;
    while (static_cast<std::size_t>(pos) < len) {  // 480-sample blocks
      const int n = static_cast<int>(std::min<std::size_t>(480, len - static_cast<std::size_t>(pos)));
      const float* ib = pb + pos;
      const float* is = ps + pos;
      float* o = po + pos;
      m.process(&ib, &is, &o, n);
      pos += n;
    }
    const double refDb = powerDb(bab.data(), len);
    CHECK(std::fabs(powerDb(out.data(), len) - refDb) <= 0.05);
  }
}

TEST_CASE("Hybrid mixer: ramp shape (linear in b, 2 s) and power during the ramp", "[hybrid]") {
  const std::size_t len = static_cast<std::size_t>(kFs * 3.0);
  // Orthogonal, exactly-uncorrelated inputs: random-sign babble and the same times (-1)^n.
  RngStream r(5, "ramp", 0);
  std::vector<float> bab(len), sta(len);
  for (std::size_t i = 0; i < len; ++i) {
    bab[i] = r.uniformPM1f() < 0.0f ? -1.0f : 1.0f;
    sta[i] = (i % 2 == 0) ? bab[i] : -bab[i];
  }
  HybridMixer m;
  m.prepare(kFs, {0}, 0.0);
  m.setBabbleFraction(1.0);  // ramp 0 -> 1 over 2 s starting at sample 0
  std::vector<float> out(len);
  const float* ib = bab.data();
  const float* is = sta.data();
  float* o = out.data();
  m.process(&ib, &is, &o, static_cast<int>(len));
  const std::size_t win = static_cast<std::size_t>(kFs * 0.05);
  double worst = 0.0;
  for (std::size_t s = 0; s + win <= len; s += win) worst = std::max(worst, std::fabs(powerDb(out.data() + s, win)));
  CHECK(worst <= 0.1);

  // Linear-in-b ramp: with babble = 1 and stationary = 0, out = sqrt(b(t)).
  std::vector<float> one(len, 1.0f), zero(len, 0.0f);
  HybridMixer m2;
  m2.prepare(kFs, {0}, 0.0);
  m2.setBabbleFraction(1.0);
  const float* i1 = one.data();
  const float* i0 = zero.data();
  float* o2 = out.data();
  m2.process(&i1, &i0, &o2, static_cast<int>(len));
  for (double t : {0.0, 0.5, 1.0, 1.5}) {
    const std::size_t n = static_cast<std::size_t>(t * kFs);
    const double bt = t / 2.0;
    CHECK(static_cast<double>(out[n]) * static_cast<double>(out[n]) == Catch::Approx(bt).margin(2e-4));
  }
  CHECK(out[static_cast<std::size_t>(2.0 * kFs)] == 1.0f);
  CHECK(out[len - 1] == 1.0f);
  CHECK(m2.zoneFraction(0) == 1.0);
}

TEST_CASE("Hybrid mixer: zone offsets and clamping", "[hybrid]") {
  HybridMixer m;
  m.prepare(kFs, {0, 1, 1, 2}, 0.5);
  m.setZoneOffset(1, 0.25);
  m.setZoneOffset(2, -0.5);
  m.setBabbleFraction(0.75);  // zone 0: .75, zone 1: 1.0 (clamped), zone 2: .25
  CHECK(m.zoneTarget(0) == 0.75);
  CHECK(m.zoneTarget(1) == 1.0);
  CHECK(m.zoneTarget(2) == Catch::Approx(0.25));
  const std::size_t len = static_cast<std::size_t>(kFs * 2.5);
  std::vector<std::vector<float>> bab(4, std::vector<float>(len, 1.0f)), sta(4, std::vector<float>(len, 0.0f)),
      out(4, std::vector<float>(len));
  const float* ib[4];
  const float* is[4];
  float* io[4];
  for (int c = 0; c < 4; ++c) {
    ib[c] = bab[static_cast<std::size_t>(c)].data();
    is[c] = sta[static_cast<std::size_t>(c)].data();
    io[c] = out[static_cast<std::size_t>(c)].data();
  }
  m.jumpToTarget();
  m.process(ib, is, io, static_cast<int>(len));
  CHECK(static_cast<double>(out[0][100]) == Catch::Approx(std::sqrt(0.75)).margin(1e-6));
  CHECK(out[1][100] == 1.0f);
  CHECK(out[2][100] == 1.0f);
  CHECK(static_cast<double>(out[3][100]) == Catch::Approx(0.5).margin(1e-6));
}

TEST_CASE("Hybrid mixer: unused-component hints", "[hybrid]") {
  HybridMixer m;
  m.prepare(kFs, {0}, 0.0);
  std::vector<float> z(static_cast<std::size_t>(kFs), 0.0f), o(z.size());
  const float* pz = z.data();
  float* po = o.data();
  const int sec = static_cast<int>(kFs);
  CHECK_FALSE(m.babbleUnused(0));
  for (int i = 0; i < 5; ++i) m.process(&pz, &pz, &po, sec);
  CHECK_FALSE(m.babbleUnused(0));  // needs strictly more than 5 s
  m.process(&pz, &pz, &po, 1);
  CHECK(m.babbleUnused(0));
  CHECK_FALSE(m.stationaryUnused(0));
  CHECK(m.babbleUnusedInAllZones());
  // Leaving b = 0 clears the hint; reaching b = 1 needs the ramp + 5 s.
  m.setBabbleFraction(1.0);
  CHECK_FALSE(m.babbleUnused(0));
  for (int i = 0; i < 6; ++i) m.process(&pz, &pz, &po, sec);
  CHECK_FALSE(m.stationaryUnused(0));  // ramp (2 s) + 5 s not yet elapsed
  for (int i = 0; i < 2; ++i) m.process(&pz, &pz, &po, sec);
  CHECK(m.stationaryUnused(0));
  CHECK(m.stationaryUnusedInAllZones());
  // A null input array (paused generator) equals silence.
  std::vector<float> one(z.size(), 1.0f);
  const float* p1 = one.data();
  m.process(&p1, nullptr, &po, 10);
  CHECK(o[0] == 1.0f);
}

TEST_CASE("Hybrid mixer: block-size independence (bit-exact)", "[hybrid]") {
  const int len = 3 * 48000;
  const auto bab = unitNoise(static_cast<std::size_t>(len), "b");
  const auto sta = unitNoise(static_cast<std::size_t>(len), "s");
  auto run = [&](const std::vector<int>& blocks) {
    HybridMixer m;
    m.prepare(kFs, {0, 1}, 0.3);
    std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<std::size_t>(len)));
    // Parameter events at fixed absolute samples; blocks are split at events.
    const std::vector<std::pair<int, int>> events{{1000, 0}, {50001, 1}, {90000, 2}};
    int pos = 0;
    std::size_t bi = 0, ev = 0;
    while (pos < len) {
      while (ev < events.size() && events[ev].first == pos) {
        if (events[ev].second == 0) m.setBabbleFraction(0.9);
        if (events[ev].second == 1) m.setZoneOffset(1, -0.4);
        if (events[ev].second == 2) m.setBabbleFraction(0.1);
        ++ev;
      }
      int n = std::min(blocks[bi++ % blocks.size()], len - pos);
      if (ev < events.size()) n = std::min(n, events[ev].first - pos);
      const float* ib[2] = {bab.data() + pos, bab.data() + pos};
      const float* is[2] = {sta.data() + pos, sta.data() + pos};
      float* io[2] = {out[0].data() + pos, out[1].data() + pos};
      m.process(ib, is, io, n);
      pos += n;
    }
    return out;
  };
  const auto ref = run({len});
  for (const auto& blocks : {std::vector<int>{1}, {7, 33, 480, 1}, {32}, {31, 65}, {4096}}) {
    const auto o = run(blocks);
    for (int c = 0; c < 2; ++c)
      CHECK(std::memcmp(ref[static_cast<std::size_t>(c)].data(), o[static_cast<std::size_t>(c)].data(),
                        sizeof(float) * static_cast<std::size_t>(len)) == 0);
  }
  CHECK(ref[0] != ref[1]);  // different zone fractions after the offset event
}
