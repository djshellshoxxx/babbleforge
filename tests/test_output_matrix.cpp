// Output matrix (docs/SPATIAL_ENGINE.md section 7).
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/engine/OutputMatrix.h"
#include "core/random/Random.h"

using namespace bf;
using Catch::Approx;

namespace {

constexpr double kFs = 48000.0;
using Buf = std::vector<std::vector<float>>;

struct Io {
  Buf in, out;
  std::vector<const float*> pi;
  std::vector<float*> po;
  Io(int nIn, int nOut, int len, float fill)
      : in(static_cast<std::size_t>(nIn), std::vector<float>(static_cast<std::size_t>(len), fill)),
        out(static_cast<std::size_t>(nOut), std::vector<float>(static_cast<std::size_t>(len), -9.0f)) {
    for (auto& v : in) pi.push_back(v.data());
    for (auto& v : out) po.push_back(v.data());
  }
  void run(OutputMatrix& m, int off, int n) {
    std::vector<const float*> a;
    std::vector<float*> b;
    for (auto& v : in) a.push_back(v.data() + off);
    for (auto& v : out) b.push_back(v.data() + off);
    m.process(a.data(), b.data(), n);
  }
};

OutputMatrix make(int nOut, const std::vector<int>& zones = {}, const std::vector<int>& dev = {},
                  int nDev = -1, int maxBlock = 4096) {
  OutputMatrix m;
  std::vector<int> z = zones, d = dev;
  if (z.empty()) z.assign(static_cast<std::size_t>(nOut), 0);
  if (d.empty())
    for (int i = 0; i < nOut; ++i) d.push_back(i);
  m.prepare(kFs, maxBlock, z, d, nDev < 0 ? nOut : nDev);
  return m;
}

}  // namespace

TEST_CASE("Output matrix: identity by default", "[matrix]") {
  auto m = make(2);
  Io io(2, 2, 1000, 0.0f);
  RngStream r(1, "x", 0);
  for (auto& ch : io.in)
    for (auto& v : ch) v = r.uniformPM1f();
  io.run(m, 0, 1000);
  CHECK(io.out == io.in);
}

TEST_CASE("Output matrix: gain smoothing (20 ms) and range", "[matrix]") {
  auto m = make(1);
  Io io(1, 1, 3000, 1.0f);
  m.setOutputGainDb(0, -6.0);
  io.run(m, 0, 3000);
  const double target = std::pow(10.0, -6.0 / 20.0);
  CHECK(static_cast<double>(io.out[0][479]) == Approx((1.0 + target) / 2.0).margin(1e-3));  // halfway
  CHECK(static_cast<double>(io.out[0][959]) == Approx(target).margin(1e-6));               // 20 ms = 960 samples
  CHECK(static_cast<double>(io.out[0][2999]) == Approx(target).margin(1e-6));
  for (int i = 1; i < 960; ++i) CHECK(io.out[0][static_cast<std::size_t>(i)] < io.out[0][static_cast<std::size_t>(i - 1)]);
  // Range: +12 dB request is clamped to +6, -80 to -40.
  m.setOutputGainDb(0, 12.0);
  m.applyImmediately();
  Io hi(1, 1, 10, 1.0f);
  hi.run(m, 0, 10);
  CHECK(static_cast<double>(hi.out[0][9]) == Approx(std::pow(10.0, 6.0 / 20.0)).margin(1e-5));
  m.setOutputGainDb(0, -80.0);
  m.applyImmediately();
  hi.run(m, 0, 10);
  CHECK(static_cast<double>(hi.out[0][9]) == Approx(0.01).margin(1e-6));
}

TEST_CASE("Output matrix: mute ramps over 20 ms", "[matrix]") {
  auto m = make(1);
  Io io(1, 1, 4000, 1.0f);
  m.setMute(0, true);
  io.run(m, 0, 2000);
  CHECK(static_cast<double>(io.out[0][479]) == Approx(0.5).margin(1e-3));
  CHECK(io.out[0][959] == 0.0f);
  CHECK(io.out[0][1999] == 0.0f);
  CHECK(io.out[0][958] > 0.0f);
  m.setMute(0, false);
  io.run(m, 2000, 2000);
  CHECK(static_cast<double>(io.out[0][2000 + 479]) == Approx(0.5).margin(1e-3));
  CHECK(io.out[0][2000 + 959] == 1.0f);
  m.setOutputEnabled(0, false);  // a disabled output behaves like a mute
  Io io2(1, 1, 1000, 1.0f);
  io2.run(m, 0, 1000);
  CHECK(io2.out[0][999] == 0.0f);
}

TEST_CASE("Output matrix: polarity change fades out, flips, fades in (20 ms each)", "[matrix]") {
  auto m = make(1);
  Io io(1, 1, 4000, 1.0f);
  m.setPolarityInvert(0, true);
  CHECK(m.fading(0));
  io.run(m, 0, 4000);
  const auto& o = io.out[0];
  for (int i = 1; i < 960; ++i) CHECK(o[static_cast<std::size_t>(i)] < o[static_cast<std::size_t>(i - 1)]);
  CHECK(o[959] == 0.0f);
  CHECK(o[479] > 0.0f);               // still the old polarity while fading out
  CHECK(o[960] < 0.0f);               // new polarity fading in
  CHECK(static_cast<double>(o[960 + 479]) == Approx(-0.5).margin(1e-3));
  CHECK(o[1919] == -1.0f);
  CHECK(o[3999] == -1.0f);
  CHECK_FALSE(m.fading(0));
  CHECK(m.activePolarityInverted(0));
}

TEST_CASE("Output matrix: integer-sample delay is sample exact", "[matrix]") {
  for (int D : {0, 1, 37, 480, 4800}) {
    auto m = make(1);
    m.setDelaySamples(0, D);
    m.applyImmediately();
    Io io(1, 1, 6000, 0.0f);
    io.in[0][3] = 1.0f;
    io.in[0][10] = -0.5f;
    io.run(m, 0, 6000);
    for (int i = 0; i < 6000; ++i) {
      float expect = 0.0f;
      if (i == 3 + D) expect = 1.0f;
      if (i == 10 + D) expect = -0.5f;
      REQUIRE(io.out[0][static_cast<std::size_t>(i)] == expect);
    }
  }
  auto m = make(1);
  CHECK(m.maxDelaySamples() == 4800);
  m.setDelayMs(0, 250.0);  // clamped to 100 ms
  m.applyImmediately();
  CHECK(m.activeDelaySamples(0) == 4800);
  m.setDelayMs(0, 10.0);
  m.applyImmediately();
  CHECK(m.activeDelaySamples(0) == 480);
}

TEST_CASE("Output matrix: delay change uses fade-out, jump, fade-in", "[matrix]") {
  auto m = make(1);
  const int len = 6000;
  Io io(1, 1, len, 0.0f);
  for (int i = 0; i < len; ++i) io.in[0][static_cast<std::size_t>(i)] = static_cast<float>(i + 1);
  m.setDelaySamples(0, 100);
  io.run(m, 0, len);
  const auto& o = io.out[0];
  CHECK(o[959] == 0.0f);  // silent at the switch
  CHECK(o[958] != 0.0f);
  CHECK(o[960] != 0.0f);
  // After both fades the output is exactly the delayed input.
  for (int i = 1920; i < len; ++i) REQUIRE(o[static_cast<std::size_t>(i)] == io.in[0][static_cast<std::size_t>(i - 100)]);
  // Before the switch: undelayed, scaled by the fade-out ramp.
  CHECK(static_cast<double>(o[100]) == Approx(101.0 * (1.0 - 101.0 / 960.0)).epsilon(1e-6));
  // After the switch: delayed signal scaled by the fade-in ramp.
  CHECK(static_cast<double>(o[960 + 100]) == Approx(static_cast<double>(io.in[0][960 + 100 - 100]) * 101.0 / 960.0).epsilon(1e-6));
}

TEST_CASE("Output matrix: zone gain and zone enable", "[matrix]") {
  auto m = make(3, {0, 1, 1});
  m.setZoneGainDb(1, -6.0);
  Io io(3, 3, 14000, 1.0f);
  io.run(m, 0, 2000);
  const double g = std::pow(10.0, -6.0 / 20.0);
  CHECK(io.out[0][1999] == 1.0f);
  CHECK(static_cast<double>(io.out[1][1999]) == Approx(g).margin(1e-6));
  CHECK(static_cast<double>(io.out[2][1999]) == Approx(g).margin(1e-6));
  CHECK(static_cast<double>(io.out[1][959]) == Approx(g).margin(1e-6));  // 20 ms
  m.setZoneGainDb(1, 0.0);
  io.run(m, 2000, 2000);
  m.setZoneEnabled(1, false);  // 200 ms ramp = 9600 samples
  io.run(m, 4000, 10000);
  CHECK(static_cast<double>(io.out[1][4000 + 4799]) == Approx(0.5).margin(1e-3));
  CHECK(io.out[1][4000 + 9599] == 0.0f);
  CHECK(io.out[1][13999] == 0.0f);
  CHECK(io.out[0][13999] == 1.0f);
  // Zone level limits: -24..+6 dB.
  m.setZoneGainDb(0, -60.0);
  m.applyImmediately();
  Io c(3, 3, 4, 1.0f);
  c.run(m, 0, 4);
  CHECK(static_cast<double>(c.out[0][3]) == Approx(std::pow(10.0, -24.0 / 20.0)).margin(1e-6));
}

TEST_CASE("Output matrix: sparse device channel map", "[matrix]") {
  // logical 0 -> device 5, logical 1 -> device 0, logical 2 unmapped, logical 3 -> device 5 (summed)
  auto m = make(4, {}, {5, 0, -1, 5}, 7);
  REQUIRE(m.numDeviceChannels() == 7);
  Io io(4, 7, 100, 0.0f);
  for (std::size_t c = 0; c < 4; ++c) std::fill(io.in[c].begin(), io.in[c].end(), static_cast<float>(c + 1));
  io.run(m, 0, 100);
  for (std::size_t i = 0; i < 100; ++i) {
    CHECK(io.out[0][i] == 2.0f);
    CHECK(io.out[5][i] == 1.0f + 4.0f);
    for (std::size_t d : {1u, 2u, 3u, 4u, 6u}) CHECK(io.out[d][i] == 0.0f);  // unmapped device channels are silent
  }
}

namespace {
struct HalfEq : ICalEq {
  int calls = 0;
  void process(float* b, int n) noexcept override {
    ++calls;
    for (int i = 0; i < n; ++i) b[i] *= 0.5f;
  }
};
}  // namespace

TEST_CASE("Output matrix: CalEQ slot", "[matrix]") {
  auto m = make(2);
  HalfEq eq;
  m.setCalEq(1, &eq);
  Io io(2, 2, 64, 1.0f);
  io.run(m, 0, 64);
  CHECK(io.out[0][10] == 1.0f);  // empty slot = identity
  CHECK(io.out[1][10] == 0.5f);
  CHECK(eq.calls == 1);
  m.setCalEq(1, nullptr);
  io.run(m, 0, 64);
  CHECK(io.out[1][10] == 1.0f);
}

TEST_CASE("Output matrix: block-size independence (bit-exact)", "[matrix]") {
  const int len = 30000;
  constexpr int nOut = 3;
  RngStream r(77, "in", 0);
  Buf in(nOut, std::vector<float>(static_cast<std::size_t>(len)));
  for (auto& ch : in)
    for (auto& v : ch) v = r.uniformPM1f();
  auto run = [&](const std::vector<int>& blocks, int maxBlock) {
    auto m = make(nOut, {0, 1, 1}, {2, 0, 1}, 3, maxBlock);
    Buf out(3, std::vector<float>(static_cast<std::size_t>(len), 0.0f));
    const std::vector<int> evAt{500, 2500, 4000, 9000, 9001, 15000, 20000};
    int pos = 0;
    std::size_t bi = 0, ev = 0;
    while (pos < len) {
      while (ev < evAt.size() && evAt[ev] == pos) {
        switch (ev) {
          case 0: m.setOutputGainDb(0, -9.0); break;
          case 1: m.setMute(1, true); break;
          case 2: m.setDelaySamples(2, 333); break;
          case 3: m.setPolarityInvert(0, true); break;
          case 4: m.setDelaySamples(0, 90); break;  // change while a fade is running
          case 5: m.setZoneEnabled(1, false); m.setMute(1, false); break;
          case 6: m.setZoneGainDb(1, -12.0); m.setPolarityInvert(2, true); break;
        }
        ++ev;
      }
      int n = std::min(blocks[bi++ % blocks.size()], len - pos);
      if (ev < evAt.size()) n = std::min(n, evAt[ev] - pos);
      const float* pi[nOut];
      float* po[nOut];
      for (int c = 0; c < nOut; ++c) {
        pi[c] = in[static_cast<std::size_t>(c)].data() + pos;
        po[c] = out[static_cast<std::size_t>(c)].data() + pos;
      }
      m.process(pi, po, n);
      pos += n;
    }
    return out;
  };
  const auto ref = run({len}, 4096);
  for (const auto& blocks : {std::vector<int>{1}, {7, 64, 513, 1}, {32}, {100000}})
    for (int maxBlock : {64, 4096}) {
      const auto o = run(blocks, maxBlock);
      for (std::size_t c = 0; c < 3; ++c)
        CHECK(std::memcmp(ref[c].data(), o[c].data(), sizeof(float) * static_cast<std::size_t>(len)) == 0);
    }
  // Sanity: the events did change the signal.
  CHECK(std::fabs(ref[2][29000]) > 0.0f);
}
