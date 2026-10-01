#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/analysis/Meters.h"
#include "core/dsp/TruePeakDetector.h"

using namespace bf;
using Catch::Approx;

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;

double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

// Feeds `seconds` of a 1 kHz sine of peak level `dbfs` to all channels, keeping phase continuous.
struct SineFeeder {
  double phase = 0.0;
  void feed(Meters& m, int nCh, double dbfs, double seconds, double freq = 1000.0) {
    const int total = static_cast<int>(seconds * kFs);
    const double amp = dbToLin(dbfs);
    std::vector<std::vector<float>> buf(static_cast<size_t>(nCh), std::vector<float>(480));
    std::vector<const float*> ptr(static_cast<size_t>(nCh));
    for (size_t c = 0; c < ptr.size(); ++c) ptr[c] = buf[c].data();
    for (int done = 0; done < total; done += 480) {
      const int n = std::min(480, total - done);
      for (int i = 0; i < n; ++i) {
        const float v = static_cast<float>(amp * std::sin(phase));
        phase += 2.0 * kPi * freq / kFs;
        for (auto& b : buf) b[static_cast<size_t>(i)] = v;
      }
      m.process(ptr.data(), n);
    }
  }
};
}  // namespace

TEST_CASE("K-weighting coefficients at 48 kHz match BS.1770", "[meters][kweight]") {
  const auto s1 = kWeightStage1(48000.0);
  CHECK(s1.b[0] == Approx(1.53512485958697).margin(1e-6));
  CHECK(s1.b[1] == Approx(-2.69169618940638).margin(1e-6));
  CHECK(s1.b[2] == Approx(1.19839281085285).margin(1e-6));
  CHECK(s1.a[1] == Approx(-1.69065929318241).margin(1e-6));
  CHECK(s1.a[2] == Approx(0.73248077421585).margin(1e-6));
  const auto s2 = kWeightStage2(48000.0);
  CHECK(s2.b[0] == Approx(1.0).margin(1e-6));
  CHECK(s2.b[1] == Approx(-2.0).margin(1e-6));
  CHECK(s2.b[2] == Approx(1.0).margin(1e-6));
  CHECK(s2.a[1] == Approx(-1.99004745483398).margin(1e-6));
  CHECK(s2.a[2] == Approx(0.99007225036621).margin(1e-6));
}

TEST_CASE("Loudness: Tech 3341 tests 1 and 2 (1 kHz sine, stereo)", "[meters][lufs]") {
  for (double level : {-23.0, -33.0}) {
    Meters m;
    m.prepare(kFs, 2);
    SineFeeder f;
    f.feed(m, 2, level, 20.0);
    const auto s = m.snapshot();
    CHECK(s.lufsM == Approx(level).margin(0.1));
    CHECK(s.lufsS == Approx(level).margin(0.1));
    CHECK(s.lufsI == Approx(level).margin(0.1));
    // Sine RMS is 3.01 dB below peak.
    CHECK(s.rmsFastDb[0] == Approx(level - 3.0103).margin(0.02));
    CHECK(s.leq60Db[1] == Approx(level - 3.0103).margin(0.02));
    CHECK(s.samplePeakDb[0] == Approx(level).margin(0.01));
    CHECK(s.truePeakDb[0] == Approx(level).margin(0.1));
    CHECK(s.crestDb == Approx(3.0103).margin(0.15));
  }
}

TEST_CASE("Loudness: single channel is 3.01 LU below stereo", "[meters][lufs]") {
  Meters m;
  m.prepare(kFs, 1);
  SineFeeder f;
  f.feed(m, 1, -23.0, 10.0);
  CHECK(m.snapshot().lufsI == Approx(-26.01).margin(0.1));
}

TEST_CASE("Loudness: gating (Tech 3341 test 3/4/5 structures)", "[meters][lufs][gating]") {
  // Test 3: -36 dBFS 10 s, -23 dBFS 60 s, -36 dBFS 10 s (stereo 1 kHz sine).
  // Expected integrated loudness: -23.0 ±0.1 LUFS per EBU Tech 3341.
  {
    Meters m;
    m.prepare(kFs, 2);
    SineFeeder f;
    f.feed(m, 2, -36.0, 10.0);
    f.feed(m, 2, -23.0, 60.0);
    f.feed(m, 2, -36.0, 10.0);
    CHECK(m.snapshot().lufsI == Approx(-23.0).margin(0.1));
  }
  // Test 4: -72 dBFS 10 s, -36 dBFS 10 s, -23 dBFS 60 s, -36 dBFS 10 s, -72 dBFS 10 s.
  // -72 blocks filtered by absolute gate; remaining structure is like Test 3.
  // Expected integrated loudness: -23.0 ±0.1 LUFS per EBU Tech 3341.
  {
    Meters m;
    m.prepare(kFs, 2);
    SineFeeder f;
    f.feed(m, 2, -72.0, 10.0);
    f.feed(m, 2, -36.0, 10.0);
    f.feed(m, 2, -23.0, 60.0);
    f.feed(m, 2, -36.0, 10.0);
    f.feed(m, 2, -72.0, 10.0);
    CHECK(m.snapshot().lufsI == Approx(-23.0).margin(0.1));
  }
  // Test 5: -26 dBFS 20 s, -20 dBFS 20.1 s, -26 dBFS 20 s (stereo 1 kHz sine).
  // All blocks pass the absolute and relative gates.
  // Expected integrated loudness: -23.0 ±0.1 LUFS per EBU Tech 3341.
  {
    Meters m;
    m.prepare(kFs, 2);
    SineFeeder f;
    f.feed(m, 2, -26.0, 20.0);
    f.feed(m, 2, -20.0, 20.1);
    f.feed(m, 2, -26.0, 20.0);
    CHECK(m.snapshot().lufsI == Approx(-23.0).margin(0.1));
  }
  // Only a -72 LUFS signal: everything is below the absolute gate.
  Meters q;
  q.prepare(kFs, 2);
  SineFeeder g;
  g.feed(q, 2, -72.0, 5.0);
  CHECK(q.snapshot().lufsI <= -199.0);
}

TEST_CASE("Loudness: works at 44.1 kHz and 96 kHz", "[meters][lufs]") {
  for (double fs : {44100.0, 96000.0}) {
    Meters m;
    m.prepare(fs, 2);
    const int n = static_cast<int>(6 * fs);
    std::vector<float> b(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      b[static_cast<size_t>(i)] = static_cast<float>(dbToLin(-23.0) * std::sin(2 * kPi * 1000.0 * i / fs));
    const float* p[2] = {b.data(), b.data()};
    m.process(p, n);
    CHECK(m.snapshot().lufsI == Approx(-23.0).margin(0.1));
  }
}

TEST_CASE("Meters: block-size independence and reset", "[meters]") {
  const int n = 48000 * 5;
  std::vector<float> a(static_cast<size_t>(n)), b(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    a[static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(0.05 * i) + 0.2 * std::sin(0.31 * i));
    b[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(0.11 * i));
  }
  auto run = [&](int block) {
    Meters m;
    m.prepare(kFs, 2);
    for (int i = 0; i < n; i += block) {
      const int k = std::min(block, n - i);
      const float* p[2] = {a.data() + i, b.data() + i};
      m.process(p, k);
    }
    return m.snapshot();
  };
  const auto s1 = run(1), s2 = run(777);
  CHECK(s1.lufsI == s2.lufsI);
  CHECK(s1.leq60Db[0] == s2.leq60Db[0]);
  CHECK(s1.truePeakMaxDb[1] == s2.truePeakMaxDb[1]);
  CHECK(s1.rmsFastDb[1] == Approx(s2.rmsFastDb[1]).margin(1e-9));
}

TEST_CASE("Meters: RMS-fast window and silence", "[meters]") {
  Meters m;
  m.prepare(kFs, 1);
  std::vector<float> loud(48000, 0.5f), quiet(48000, 0.0f);
  const float* pl[1] = {loud.data()};
  const float* pq[1] = {quiet.data()};
  m.process(pl, 48000);
  CHECK(m.snapshot().rmsFastDb[0] == Approx(-6.0206).margin(0.01));
  m.process(pq, 48000);  // 1 s of silence: fast window (300 ms) is fully silent
  const auto s = m.snapshot();
  CHECK(s.rmsFastDb[0] < -150.0);
  CHECK(s.leq60Db[0] == Approx(-6.0206 - 3.0103).margin(0.02));  // half of 2 s at 0.25 power
}

TEST_CASE("True peak: fs/4 sine with 45 degree phase", "[meters][truepeak]") {
  // Samples alternate +-1.0 (sample peak 0 dBFS) while the continuous wave peaks at sqrt(2).
  TruePeakDetector d;
  d.prepare(48000.0, 1);
  float peak = 0.0f;
  for (int i = 0; i < 2000; ++i) {
    const double v = std::sqrt(2.0) * std::sin(kPi / 2.0 * i + kPi / 4.0);
    const float p = d.processSample(0, static_cast<float>(v));
    if (i > 100) peak = std::max(peak, p);
  }
  CHECK(20.0 * std::log10(peak) == Approx(3.01).margin(0.3));
}

TEST_CASE("True peak: known cases", "[meters][truepeak]") {
  TruePeakDetector d;
  d.prepare(48000.0, 2);
  CHECK(d.oversampling() == 4);
  // Two equal adjacent samples: mid-point overshoot for a band-limited reconstruction.
  float p0 = 0;
  for (int i = 0; i < 200; ++i) {
    const float x = (i == 100 || i == 101) ? 1.0f : 0.0f;
    p0 = std::max(p0, d.processSample(0, x));
  }
  CHECK(p0 > 1.05f);  // sinc reconstruction overshoots (~1.27)
  CHECK(p0 < 1.4f);
  // DC / silent input: no false peaks.
  float pdc = 0;
  for (int i = 0; i < 400; ++i) pdc = std::max(pdc, d.processSample(1, 0.0f));
  CHECK(pdc == 0.0f);
  // Low-frequency sine: TP equals sample peak within 0.05 dB.
  TruePeakDetector d2;
  d2.prepare(48000.0, 1);
  float pk = 0;
  for (int i = 0; i < 4000; ++i) pk = std::max(pk, d2.processSample(0, static_cast<float>(0.5 * std::sin(2 * kPi * 100.0 * i / 48000.0))));
  CHECK(pk == Approx(0.5f).epsilon(0.006));
  // Oversampling factors.
  CHECK(TruePeakDetector::oversamplingForRate(44100.0) == 4);
  CHECK(TruePeakDetector::oversamplingForRate(96000.0) == 2);
}

TEST_CASE("Meters: true peak reported through snapshot", "[meters][truepeak]") {
  Meters m;
  m.prepare(kFs, 1);
  const int n = 48000;
  std::vector<float> b(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    b[static_cast<size_t>(i)] = static_cast<float>(std::sqrt(2.0) * 0.5 * std::sin(kPi / 2.0 * i + kPi / 4.0));
  const float* p[1] = {b.data()};
  m.process(p, n);
  const auto s = m.snapshot();
  CHECK(s.samplePeakDb[0] == Approx(-6.02).margin(0.02));
  CHECK(s.truePeakDb[0] == Approx(-3.01).margin(0.3));
  CHECK(s.truePeakMaxDb[0] >= s.truePeakDb[0]);
}
