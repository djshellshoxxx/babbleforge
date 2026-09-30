#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/dsp/Limiter.h"
#include "core/dsp/TruePeakDetector.h"

using namespace bf;
using Catch::Approx;

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;
double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

std::vector<std::vector<float>> runLimiter(Limiter& lim, const std::vector<std::vector<float>>& in, int block) {
  const size_t nCh = in.size(), n = in[0].size();
  std::vector<std::vector<float>> out(nCh, std::vector<float>(n));
  std::vector<const float*> ip(nCh);
  std::vector<float*> op(nCh);
  for (size_t i = 0; i < n; i += static_cast<size_t>(block)) {
    const size_t k = std::min(static_cast<size_t>(block), n - i);
    for (size_t c = 0; c < nCh; ++c) { ip[c] = in[c].data() + i; op[c] = out[c].data() + i; }
    lim.process(ip.data(), op.data(), static_cast<int>(k));
  }
  return out;
}

double truePeakDb(const std::vector<float>& x, size_t from = 0) {
  TruePeakDetector d;
  d.prepare(kFs, 1);
  float pk = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    const float p = d.processSample(0, x[i]);
    if (i >= from) pk = std::max(pk, p);
  }
  return 20.0 * std::log10(std::max(pk, 1e-9f));
}

std::vector<float> square(size_t n, double amp, int half = 60) {
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i) x[i] = static_cast<float>(((i / static_cast<size_t>(half)) % 2 ? -1.0 : 1.0) * amp);
  return x;
}
// fs/4 sine at 45 degrees: samples +-a, inter-sample peak a*sqrt(2).
std::vector<float> isp(size_t n, double amp) {
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i)
    x[i] = static_cast<float>(amp * std::sqrt(2.0) * std::sin(kPi / 2.0 * double(i) + kPi / 4.0));
  return x;
}
}  // namespace

TEST_CASE("Limiter: full-scale +12 dB square and inter-sample-peak signals stay under ceiling", "[limiter]") {
  const size_t n = 48000;
  for (int kind = 0; kind < 2; ++kind) {
    Limiter lim;
    lim.prepare(kFs, 1, 480);
    auto x = kind == 0 ? square(n, dbToLin(12.0)) : isp(n, dbToLin(12.0));
    auto y = runLimiter(lim, {x}, 480);
    const double tp = truePeakDb(y[0], 2000);
    INFO("kind " << kind << " tp " << tp);
    CHECK(tp <= -1.0 + 0.2);
    CHECK(lim.stats().clipEvents == 0);
    CHECK(lim.stats().grDbMax > 10.0);
    // Loud but not absurdly over-attenuated.
    CHECK(tp > -1.6);
  }
}

TEST_CASE("Limiter: ceiling range and 44.1/96 kHz", "[limiter]") {
  for (double fs : {44100.0, 96000.0}) {
    for (double ceil : {-6.0, -0.1}) {
      Limiter lim;
      lim.prepare(fs, 2, 256);
      lim.setCeilingDb(ceil);
      const size_t n = static_cast<size_t>(fs);
      std::vector<float> a(n), b(n);
      for (size_t i = 0; i < n; ++i) {
        a[i] = static_cast<float>(3.0 * std::sin(2 * kPi * 997.0 * double(i) / fs));
        b[i] = static_cast<float>(1.5 * std::sin(2 * kPi * 15000.0 * double(i) / fs + 0.3));
      }
      auto y = runLimiter(lim, {a, b}, 256);
      TruePeakDetector d;
      d.prepare(fs, 1);
      for (int c = 0; c < 2; ++c) {
        d.reset();
        float pk = 0;
        for (size_t i = 3000; i < n; ++i) pk = std::max(pk, d.processSample(0, y[static_cast<size_t>(c)][i]));
        CHECK(20 * std::log10(pk) <= ceil + 0.25);
      }
      CHECK(lim.stats().clipEvents == 0);
    }
  }
  Limiter l;
  l.prepare(kFs, 1, 64);
  l.setCeilingDb(-20.0);
  CHECK(l.ceilingDb() == -6.0);
  l.setCeilingDb(5.0);
  CHECK(l.ceilingDb() == -0.1);
  CHECK(l.latencySamples() == 240);
}

TEST_CASE("Limiter: below ceiling is a bit-exact delay with zero GR", "[limiter]") {
  Limiter lim;
  lim.prepare(kFs, 2, 480);
  const size_t n = 20000;
  std::vector<std::vector<float>> x(2, std::vector<float>(n));
  const double amp = dbToLin(-11.0);  // 10 dB below the -1 dBTP ceiling
  for (size_t i = 0; i < n; ++i) {
    x[0][i] = static_cast<float>(amp * std::sin(0.0173 * double(i)));
    x[1][i] = static_cast<float>(amp * std::sin(2.1 * double(i)));
  }
  auto y = runLimiter(lim, x, 480);
  const size_t d = static_cast<size_t>(lim.latencySamples());
  for (size_t c = 0; c < 2; ++c) {
    for (size_t i = 0; i < d; ++i) REQUIRE(y[c][i] == 0.0f);
    for (size_t i = d; i < n; ++i) REQUIRE(y[c][i] == x[c][i - d]);
  }
  CHECK(lim.stats().grDbMax == 0.0f);
  CHECK(lim.stats().grDbCurrent == 0.0f);
  CHECK(lim.stats().samplesAbove05Db == 0);
  CHECK(lim.stats().clipEvents == 0);
}

TEST_CASE("Limiter: bypass keeps only the safety clip", "[limiter]") {
  Limiter lim;
  lim.prepare(kFs, 1, 100);
  lim.setBypass(true);
  std::vector<float> x(2000, 2.0f);
  auto y = runLimiter(lim, {x}, 100);
  const size_t d = static_cast<size_t>(lim.latencySamples());
  CHECK(y[0][d + 10] == 1.0f);
  CHECK(lim.stats().clipEvents == 2000 - d);
  CHECK(lim.stats().grDbMax == 0.0f);
}

TEST_CASE("Limiter: release timing follows the time constants", "[limiter]") {
  auto deficitAt = [](const std::vector<float>& out, size_t i) {
    // Gain seen on a constant 0.5 tone: the input is a DC-ish low level after the burst.
    return 1.0 - double(out[i]) / 0.05;
  };
  auto run = [&](size_t burstSamples, double burstAmp, std::vector<float>& out) {
    Limiter lim;
    lim.prepare(kFs, 1, 480);
    const size_t n = 48000 * 3;
    std::vector<float> x(n, 0.05f);  // DC at -26 dBFS far below the ceiling
    for (size_t i = 1000; i < 1000 + burstSamples; ++i) x[i] = static_cast<float>(burstAmp);
    out = runLimiter(lim, {x}, 480)[0];
  };
  std::vector<float> o;
  // Long event (300 ms, ~ -6 dB): slow release, tau = 600 ms.
  run(14400, 1.78 * 0.891 * 1.0, o);  // ~6 dB over the ceiling
  {
    const size_t end = 1000 + 14400 + 240;  // burst leaves the output
    // Find when release begins: hold 10 ms + window; sample deficits at two later points.
    const size_t t1 = end + 3000, t2 = t1 + 28800;  // 600 ms apart
    const double d1 = deficitAt(o, t1), d2 = deficitAt(o, t2);
    REQUIRE(d1 > 0.01);
    CHECK(d2 / d1 == Approx(std::exp(-1.0)).epsilon(0.1));
  }
  // Short event (10 ms, ~ -1.5 dB): fast release, tau = 80 ms.
  run(480, 0.95, o);  // ~1.5 dB GR incl. inter-sample overshoot
  {
    const size_t end = 1000 + 480 + 240;
    const size_t t1 = end + 2000, t2 = t1 + 3840;  // 80 ms apart
    const double d1 = deficitAt(o, t1), d2 = deficitAt(o, t2);
    REQUIRE(d1 > 0.003);
    CHECK(d2 / d1 == Approx(std::exp(-1.0)).epsilon(0.1));
  }
}

TEST_CASE("Limiter: link groups apply identical gain", "[limiter]") {
  Limiter lim;
  lim.prepare(kFs, 3, 480, {0, 0, 1});
  const size_t n = 24000;
  std::vector<std::vector<float>> x(3, std::vector<float>(n, 0.1f));
  for (size_t i = 5000; i < 9000; ++i) x[0][i] = 3.0f;  // only channel 0 is hot
  auto y = runLimiter(lim, x, 480);
  bool anyReduced = false;
  for (size_t i = 240; i < n; ++i) {
    // channel 1 has the same gain as channel 0: y1 / 0.1 == y0 / x0(delayed)
    const float g0 = y[0][i] / x[0][i - 240];
    const float g1 = y[1][i] / x[1][i - 240];
    REQUIRE(g0 == g1);
    if (g1 < 0.99f) anyReduced = true;
    // channel 2 is in its own group and unaffected
    REQUIRE(y[2][i] == x[2][i - 240]);
  }
  CHECK(anyReduced);
}

TEST_CASE("Limiter: output independent of block size (bit-exact)", "[limiter]") {
  const size_t n = 48000;
  std::vector<std::vector<float>> x(2, std::vector<float>(n));
  for (size_t i = 0; i < n; ++i) {
    const double env = 0.2 + 3.0 * (std::sin(0.0007 * double(i)) > 0.6 ? 1.0 : 0.0);
    x[0][i] = static_cast<float>(env * std::sin(0.31 * double(i)));
    x[1][i] = static_cast<float>(0.7 * env * std::sin(1.9 * double(i)));
  }
  auto run = [&](int block) {
    Limiter lim;
    lim.prepare(kFs, 2, 1024, {0, 0});
    return runLimiter(lim, x, block);
  };
  const auto ref = run(480);
  for (int b : {1, 64, 1024}) {
    const auto y = run(b);
    for (size_t c = 0; c < 2; ++c)
      REQUIRE(std::memcmp(y[c].data(), ref[c].data(), n * sizeof(float)) == 0);
  }
  // In-place processing gives the same result.
  Limiter lim;
  lim.prepare(kFs, 2, 1024, {0, 0});
  auto z = x;
  std::vector<float*> p = {z[0].data(), z[1].data()};
  lim.process(p.data(), p.data(), static_cast<int>(n));
  CHECK(std::memcmp(z[0].data(), ref[0].data(), n * sizeof(float)) == 0);
}
