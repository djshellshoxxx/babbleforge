// All-pass decorrelator (docs/SPATIAL_ENGINE.md section 5.2).
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <set>
#include <vector>

#include "core/dsp/AllpassDecorrelator.h"
#include "core/random/Random.h"

using namespace bf;

namespace {

constexpr double kFs = 48000.0;

bool isPrime(int n) {
  if (n < 2) return false;
  for (int d = 2; d * d <= n; ++d)
    if (n % d == 0) return false;
  return true;
}

// Impulse response of every channel (one impulse per channel, run in a single call).
std::vector<std::vector<float>> impulseResponses(AllpassDecorrelator& d, int len) {
  const int nch = d.numChannels();
  std::vector<std::vector<float>> in(static_cast<std::size_t>(nch), std::vector<float>(static_cast<std::size_t>(len), 0.0f));
  auto out = in;
  std::vector<const float*> pi;
  std::vector<float*> po;
  for (int c = 0; c < nch; ++c) {
    in[static_cast<std::size_t>(c)][0] = 1.0f;
    pi.push_back(in[static_cast<std::size_t>(c)].data());
    po.push_back(out[static_cast<std::size_t>(c)].data());
  }
  d.reset();
  d.process(pi.data(), po.data(), len);
  return out;
}

double rho(const std::vector<float>& a, const std::vector<float>& b) {
  double s = 0, ea = 0, eb = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    s += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    ea += static_cast<double>(a[i]) * static_cast<double>(a[i]);
    eb += static_cast<double>(b[i]) * static_cast<double>(b[i]);
  }
  return s / std::sqrt(ea * eb);
}

}  // namespace

TEST_CASE("Decorrelator: stage counts and delay design", "[decorrelator]") {
  AllpassDecorrelator low, med, high;
  low.prepare(kFs, 4, SpeakerVariation::Low, 1);
  med.prepare(kFs, 8, SpeakerVariation::Medium, 1);
  high.prepare(kFs, 8, SpeakerVariation::High, 1);
  CHECK(low.numStages() == 0);
  CHECK(med.numStages() == 3);
  CHECK(high.numStages() == 5);

  for (auto* d : {&med, &high}) {
    const bool isHigh = d == &high;
    const int hiSamples = static_cast<int>(std::floor((isHigh ? 4.5e-3 : 3.0e-3) * kFs));
    for (int c = 0; c < d->numChannels(); ++c) {
      std::set<int> used;
      for (int s = 0; s < d->numStages(); ++s) {
        const int D = d->delaySamples(c, s);
        CHECK(isPrime(D));
        CHECK(D >= static_cast<int>(std::lround(0.4e-3 * kFs)));
        CHECK(D <= hiSamples);
        CHECK(used.insert(D).second);  // distinct within the channel
        const double g = std::fabs(static_cast<double>(d->gain(c, s)));
        CHECK(g <= 0.60 + 1e-6);
        CHECK(g >= 0.45 * d->gainScale(c) - 1e-6);
        // alternating sign with (channel + stage)
        CHECK((d->gain(c, s) > 0.0f) == ((c + s) % 2 == 0));
      }
      if (c > 0) {  // adjacent channels use disjoint primes
        for (int s = 0; s < d->numStages(); ++s)
          for (int t = 0; t < d->numStages(); ++t) CHECK(d->delaySamples(c, s) != d->delaySamples(c - 1, t));
      }
    }
  }
}

TEST_CASE("Decorrelator: deterministic per seed, delays scale with fs", "[decorrelator]") {
  AllpassDecorrelator a, b, c, d96;
  a.prepare(kFs, 4, SpeakerVariation::Medium, 5);
  b.prepare(kFs, 4, SpeakerVariation::Medium, 5);
  c.prepare(kFs, 4, SpeakerVariation::Medium, 6);
  d96.prepare(96000.0, 4, SpeakerVariation::Medium, 5);
  bool differs = false;
  for (int ch = 0; ch < 4; ++ch)
    for (int s = 0; s < 3; ++s) {
      CHECK(a.delaySamples(ch, s) == b.delaySamples(ch, s));
      CHECK(a.gain(ch, s) == b.gain(ch, s));
      differs = differs || a.delaySamples(ch, s) != c.delaySamples(ch, s);
      CHECK(d96.delaySamples(ch, s) >= 38);  // 0.4 ms at 96 kHz
      CHECK(d96.delaySamples(ch, s) <= 288);
    }
  CHECK(differs);
  CHECK(d96.maxDelaySamples() == 288);
}

TEST_CASE("Decorrelator: flat magnitude and -60 dB decay within 25 ms", "[decorrelator]") {
  for (auto level : {SpeakerVariation::Medium, SpeakerVariation::High}) {
    AllpassDecorrelator d;
    d.prepare(kFs, 8, level, 2024);
    const int len = 24000;  // 0.5 s
    const auto ir = impulseResponses(d, len);
    for (int c = 0; c < 8; ++c) {
      const auto& h = ir[static_cast<std::size_t>(c)];
      // Energy decay curve: remaining energy after 25 ms <= -60 dB.
      double tot = 0, tail = 0;
      for (int n = 0; n < len; ++n) {
        const double e = static_cast<double>(h[static_cast<std::size_t>(n)]) * static_cast<double>(h[static_cast<std::size_t>(n)]);
        tot += e;
        if (n >= 1200) tail += e;
      }
      CHECK(tot == Catch::Approx(1.0).margin(1e-4));
      CHECK(10.0 * std::log10(std::max(tail / tot, 1e-30)) <= -60.0);
      CHECK(d.tailEnergy(c) <= 1e-6);
      // Magnitude response (DFT of the measured IR) flat within +-0.01 dB.
      double worst = 0.0;
      for (int k = 1; k < 240; ++k) {
        const double w = 3.14159265358979323846 * static_cast<double>(k) / 240.0;
        double re = 0, im = 0;
        for (int n = 0; n < 4000; ++n) {
          const double x = static_cast<double>(h[static_cast<std::size_t>(n)]);
          re += x * std::cos(w * n);
          im -= x * std::sin(w * n);
        }
        worst = std::max(worst, std::fabs(20.0 * std::log10(std::hypot(re, im))));
      }
      CHECK(worst <= 0.01);
    }
  }
}

TEST_CASE("Decorrelator: shared white noise gives low adjacent correlation", "[decorrelator]") {
  const int len = static_cast<int>(kFs * 10.0);
  RngStream rng(123, "test.noise", 0);
  std::vector<float> noise(static_cast<std::size_t>(len));
  for (auto& v : noise) v = rng.uniformPM1f();
  struct Case { SpeakerVariation v; double limit; };
  for (const Case cs : {Case{SpeakerVariation::Medium, 0.5}, Case{SpeakerVariation::High, 0.3}}) {
    for (std::uint64_t seed : {1u, 2u, 3u}) {
      constexpr int nch = 8;
      AllpassDecorrelator d;
      d.prepare(kFs, nch, cs.v, seed);
      std::vector<const float*> pi(nch, noise.data());
      std::vector<std::vector<float>> out(nch, std::vector<float>(static_cast<std::size_t>(len)));
      std::vector<float*> po;
      for (auto& o : out) po.push_back(o.data());
      d.process(pi.data(), po.data(), len);
      for (int c = 0; c + 1 < nch; ++c) {
        const double r = rho(out[static_cast<std::size_t>(c)], out[static_cast<std::size_t>(c + 1)]);
        CHECK(std::fabs(r) <= cs.limit);
      }
      // Each channel keeps the input power (all-pass).
      double pin = 0, pout = 0;
      for (int i = 0; i < len; ++i) {
        pin += static_cast<double>(noise[static_cast<std::size_t>(i)]) * static_cast<double>(noise[static_cast<std::size_t>(i)]);
        pout += static_cast<double>(out[0][static_cast<std::size_t>(i)]) * static_cast<double>(out[0][static_cast<std::size_t>(i)]);
      }
      CHECK(10.0 * std::log10(pout / pin) == Catch::Approx(0.0).margin(0.02));
    }
  }
}

TEST_CASE("Decorrelator: block-size independence (bit-exact) and bypass", "[decorrelator]") {
  const int len = 20000;
  RngStream rng(9, "test.blocks", 0);
  std::vector<float> x(static_cast<std::size_t>(len));
  for (auto& v : x) v = rng.uniformPM1f();
  constexpr int nch = 3;
  auto run = [&](const std::vector<int>& blocks) {
    AllpassDecorrelator d;
    d.prepare(kFs, nch, SpeakerVariation::High, 77);
    std::vector<std::vector<float>> out(nch, std::vector<float>(static_cast<std::size_t>(len)));
    int pos = 0;
    std::size_t bi = 0;
    while (pos < len) {
      const int n = std::min(blocks[bi++ % blocks.size()], len - pos);
      const float* pi[nch] = {x.data() + pos, x.data() + pos, x.data() + pos};
      float* po[nch] = {out[0].data() + pos, out[1].data() + pos, out[2].data() + pos};
      d.process(pi, po, n);
      pos += n;
    }
    return out;
  };
  const auto ref = run({len});
  for (const auto& blocks : {std::vector<int>{1}, {7, 64, 513, 1}, {32}, {4096, 3}}) {
    const auto o = run(blocks);
    for (int c = 0; c < nch; ++c)
      CHECK(std::memcmp(ref[static_cast<std::size_t>(c)].data(), o[static_cast<std::size_t>(c)].data(), sizeof(float) * static_cast<std::size_t>(len)) == 0);
  }
  // In-place processing equals out-of-place.
  AllpassDecorrelator d;
  d.prepare(kFs, 1, SpeakerVariation::High, 77);
  std::vector<float> buf = x;
  float* p = buf.data();
  const float* pi = buf.data();
  d.process(&pi, &p, len);
  CHECK(std::memcmp(buf.data(), ref[0].data(), sizeof(float) * static_cast<std::size_t>(len)) == 0);

  // Low = bypass.
  AllpassDecorrelator lo;
  lo.prepare(kFs, 1, SpeakerVariation::Low, 77);
  std::vector<float> y(static_cast<std::size_t>(len));
  const float* pin = x.data();
  float* pout = y.data();
  lo.process(&pin, &pout, len);
  CHECK(y == x);
}
