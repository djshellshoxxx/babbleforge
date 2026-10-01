#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <complex>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/analysis/SpectrumAnalyzer.h"
#include "core/dsp/Fft.h"
#include "core/engine/CalibrationBus.h"
#include "core/engine/MaskEngine.h"
#include "core/ext/Extensions.h"
#include "core/spectrum/FirDesigner.h"

using namespace bf;

namespace {
constexpr double kFs = 48000.0;
constexpr double kPi = 3.141592653589793238462643383279502884;

struct Rendered {
    std::vector<std::vector<float>> ch;
};

// Runs the bus for `frames` in blocks of `block` (start() already called).
Rendered run(CalibrationBus& bus, int nOut, std::size_t frames, std::size_t block = 480) {
    Rendered r;
    r.ch.assign(static_cast<std::size_t>(nOut), std::vector<float>(frames));
    std::vector<float*> p(static_cast<std::size_t>(nOut));
    for (std::size_t off = 0; off < frames; off += block) {
        const std::size_t n = std::min(block, frames - off);
        for (int c = 0; c < nOut; ++c) p[static_cast<std::size_t>(c)] = r.ch[static_cast<std::size_t>(c)].data() + off;
        bus.process(p.data(), static_cast<int>(n));
    }
    return r;
}

double rmsDb(const std::vector<float>& x, std::size_t a, std::size_t b) {
    double e = 0.0;
    for (std::size_t i = a; i < b; ++i) e += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(std::max(e / static_cast<double>(b - a), 1e-30));
}
}  // namespace

TEST_CASE("sine: exact level and frequency", "[calibration]") {
    CalibrationBus bus;
    bus.prepare(kFs, 2);
    auto p = CalibrationParams::sine(1000.0, -20.0);
    REQUIRE(bus.start(p));
    const auto r = run(bus, 2, 48000);
    // Whole periods (1 kHz -> 48 samples), after the 10 ms ramp.
    CHECK(std::abs(rmsDb(r.ch[0], 4800, 4800 + 48 * 500) - (-20.0)) < 0.01);
    CHECK(r.ch[0] == r.ch[1]);
    // Frequency: DFT phase advance, and zero crossings spacing.
    std::size_t crossings = 0;
    std::size_t first = 0, last = 0;
    for (std::size_t i = 4801; i < 40000; ++i)
        if (r.ch[0][i - 1] < 0.0f && r.ch[0][i] >= 0.0f) {
            if (!crossings) first = i;
            last = i;
            ++crossings;
        }
    const double f = static_cast<double>(crossings - 1) * kFs / static_cast<double>(last - first);
    CHECK(std::abs(f - 1000.0) < 0.5);
    // 10 ms raised-cosine start ramp.
    CHECK(std::abs(r.ch[0][0]) < 1e-6f);
    double peakRamp = 0.0;
    for (std::size_t i = 0; i < 480; ++i) peakRamp = std::max<double>(peakRamp, std::abs(r.ch[0][i]));
    CHECK(peakRamp < std::pow(10.0, -20.0 / 20.0) * std::sqrt(2.0) * 1.0001);
    // A non-trivial frequency stays exact too.
    bus.stop();
    run(bus, 2, 4800);
    p = CalibrationParams::sine(440.0, -30.0);
    REQUIRE(bus.start(p));
    const auto q = run(bus, 2, 48000);
    CHECK(std::abs(rmsDb(q.ch[0], 4800, 4800 + 48000 / 2) - (-30.0)) < 0.02);
}

TEST_CASE("pink noise: 1/3-octave flat, level, seeded", "[calibration]") {
    constexpr std::size_t kFirst125 = 4, kLast8k = 22;  // 125 Hz .. 8 kHz (FIR HP/LP corners at 80 Hz / 12.5 kHz)
    CalibrationBus bus;
    bus.prepare(kFs, 1);
    CalibrationParams p;
    p.signal = CalSignal::PinkNoise;
    p.seed = 7;
    REQUIRE(bus.start(p));
    const std::size_t N = 20 * 48000;
    const auto r = run(bus, 1, N, 512);
    CHECK(std::abs(rmsDb(r.ch[0], 4800, N) - (-26.0)) < 0.75);
    const float* ptr = r.ch[0].data() + 4800;
    const auto a = analyzeSpectrum(&ptr, 1, N - 4800, kFs);
    double mean = 0.0;
    for (std::size_t b = kFirst125; b <= kLast8k; ++b) mean += a.overallDb[b];
    mean /= static_cast<double>(kLast8k - kFirst125 + 1);
    for (std::size_t b = kFirst125; b <= kLast8k; ++b) CHECK(std::abs(a.overallDb[b] - mean) < 1.0);
    // The designed kernel itself is flat (band levels 0 dB relative).
    const auto k = CalibrationBus::designPinkKernel(kFs);
    const auto resp = firThirdOctResponseDb(std::vector<double>(k.begin(), k.end()), kFs);
    double m2 = 0.0;
    for (std::size_t b = kFirst125; b <= kLast8k; ++b) m2 += resp[b];
    m2 /= static_cast<double>(kLast8k - kFirst125 + 1);
    for (std::size_t b = kFirst125; b <= kLast8k; ++b) CHECK(std::abs(resp[b] - m2) < 1.0);
}

TEST_CASE("speech-shaped and octave-band noise", "[calibration]") {
    CalibrationBus bus;
    bus.prepare(kFs, 1);
    CalibrationParams p;
    p.signal = CalSignal::SpeechNoise;
    REQUIRE(bus.start(p));
    auto r = run(bus, 1, 10 * 48000);
    CHECK(std::abs(rmsDb(r.ch[0], 4800, r.ch[0].size()) - (-26.0)) < 1.0);
    bus.stop();
    run(bus, 1, 4800);
    p.signal = CalSignal::OctaveNoise;
    p.octaveBandHz = 1000;
    REQUIRE(bus.start(p));
    r = run(bus, 1, 10 * 48000);
    CHECK(std::abs(rmsDb(r.ch[0], 4800, r.ch[0].size()) - (-26.0)) < 1.0);
    const float* ptr = r.ch[0].data() + 4800;
    const auto a = analyzeSpectrum(&ptr, 1, r.ch[0].size() - 4800, kFs);
    const double inBand = a.octaveDb[3];  // 1 kHz octave (125..8k index 3)
    CHECK(std::abs(inBand - (-26.0)) < 1.0);
    CHECK(a.octaveDb[0] < inBand - 30.0);
    CHECK(a.octaveDb[6] < inBand - 30.0);
}

TEST_CASE("sweep: instantaneous frequency and inverse-filter deconvolution", "[calibration]") {
    const double f1 = 100.0, f2 = 12000.0, T = 2.0;
    CalibrationBus bus;
    bus.prepare(kFs, 1);
    CalibrationParams p;
    p.signal = CalSignal::Sweep;
    p.sweepF1Hz = f1;
    p.sweepF2Hz = f2;
    p.sweepDurationS = 2.0;
    p.sweepFadeS = 0.05;
    p.sweepTailS = 1.0;
    p.sweepPeakDbfs = -20.0;
    // Duration below the 1 s limit is rejected, 2 s is fine.
    REQUIRE(bus.start(p));
    const std::size_t total = static_cast<std::size_t>(3.0 * kFs);
    const auto r = run(bus, 1, total + 4800, 333);
    const auto& x = r.ch[0];
    // Peak is -20 dBFS; tail is silent.
    float pk = 0.0f;
    for (float v : x) pk = std::max(pk, std::abs(v));
    CHECK(std::abs(20.0 * std::log10(pk) - (-20.0)) < 0.1);
    for (std::size_t i = static_cast<std::size_t>(2.0 * kFs) + 10; i < x.size(); ++i) REQUIRE(x[i] == 0.0f);
    // Instantaneous frequency from zero-crossing intervals vs the closed form.
    int checked = 0;
    std::size_t prev = 0;
    for (std::size_t i = 4800; i < static_cast<std::size_t>(1.9 * kFs); ++i) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            if (prev) {
                const double t = 0.5 * (static_cast<double>(i) + static_cast<double>(prev)) / kFs;
                const double fm = kFs / static_cast<double>(i - prev);
                const double fe = CalibrationBus::sweepInstantaneousHz(f1, f2, T, t);
                if (fe < 1000.0) {  // chirp curvature + sample quantisation keep this exact only at low f
                    CHECK(std::abs(fm / fe - 1.0) < 0.03);
                    ++checked;
                }
            }
            prev = i;
        }
    }
    CHECK(checked > 100);
    CHECK(std::abs(CalibrationBus::sweepInstantaneousHz(f1, f2, T, 0.0) - f1) < 1e-9);
    CHECK(std::abs(CalibrationBus::sweepInstantaneousHz(f1, f2, T, T) - f2) < 1e-6);

    // Deconvolution with the inverse filter -> near delta.
    const std::vector<float> sw = makeSweepSignal(kFs, f1, f2, T, 0.05);
    const std::vector<float> inv = makeSweepInverseFilter(kFs, f1, f2, T, 0.05);
    REQUIRE(sw.size() == inv.size());
    const std::size_t L = sw.size() + inv.size() - 1;
    std::size_t M = 1;
    while (M < L) M <<= 1;
    FftD fft(M);
    std::vector<double> a(M, 0.0), b(M, 0.0), c(M);
    for (std::size_t i = 0; i < sw.size(); ++i) a[i] = sw[i], b[i] = inv[i];
    std::vector<std::complex<double>> A(M / 2 + 1), B(M / 2 + 1);
    fft.forward(a.data(), A.data());
    fft.forward(b.data(), B.data());
    for (std::size_t k = 0; k < A.size(); ++k) A[k] *= B[k];
    fft.inverse(A.data(), c.data());
    std::size_t ip = 0;
    for (std::size_t i = 0; i < L; ++i)
        if (std::abs(c[i]) > std::abs(c[ip])) ip = i;
    CHECK(ip == sw.size() - 1);
    double eAll = 0.0, eNear = 0.0;
    for (std::size_t i = 0; i < L; ++i) {
        eAll += c[i] * c[i];
        if (i + 24 >= ip && i <= ip + 24) eNear += c[i] * c[i];
    }
    CHECK(eNear / eAll > 0.9);
    // Unity in-band gain: peak of the band-limited delta ~ 2 (f2 - f1) / fs.
    CHECK(std::abs(c[ip] / (2.0 * (f2 - f1) / kFs) - 1.0) < 0.25);
}

TEST_CASE("channel-id: sequence routes only to the intended output", "[calibration]") {
    const int nOut = 4;
    CalibrationBus bus;
    bus.prepare(kFs, nOut);
    CalibrationParams p;
    p.signal = CalSignal::ChannelId;
    p.outputs = {0, 1, 3};  // output 2 disabled
    p.codedTones = true;
    REQUIRE(bus.start(p));
    const std::size_t slot = static_cast<std::size_t>(1.5 * kFs);
    const auto r = run(bus, nOut, 3 * slot + 2000, 700);
    const std::size_t expectOut[3] = {0, 1, 3};
    for (int s = 0; s < 3; ++s) {
        const std::size_t a = static_cast<std::size_t>(s) * slot;
        for (std::size_t c = 0; c < nOut; ++c) {
            const bool intended = c == expectOut[s];
            // burst window [a+0.1 s, a+0.9 s]; gap [a+1.0 s, a+1.5 s)
            const double burst = rmsDb(r.ch[c], a + 4800, a + 43200);
            if (intended) {
                CHECK(std::abs(burst - (-26.0)) < 1.5);
                CHECK(rmsDb(r.ch[c], a + 48000 + 10, a + slot) < -150.0);
            } else {
                // Only this slot is checked for silence on the other outputs.
                CHECK(burst < -150.0);
            }
        }
    }
    for (std::size_t i = 0; i < r.ch[2].size(); ++i) REQUIRE(r.ch[2][i] == 0.0f);
    CHECK(rmsDb(r.ch[3], 3 * slot + 100, r.ch[3].size()) < -150.0);  // sequence over
    CHECK_FALSE(bus.running());
    // Coded tones: the burst of output 1 holds its two tones (500 Hz + 1250 Hz).
    const auto tones = CalibrationBus::channelIdTones(1);
    auto toneLevel = [&](double f) {
        double re = 0.0, im = 0.0;
        for (std::size_t i = slot + 4800; i < slot + 4800 + 48000 / 2; ++i) {
            const double ph = 2.0 * kPi * f * static_cast<double>(i - slot) / kFs;
            re += r.ch[1][i] * std::cos(ph);
            im += r.ch[1][i] * std::sin(ph);
        }
        return 20.0 * std::log10(std::hypot(re, im) * 2.0 / (48000.0 / 2) / std::sqrt(2.0));
    };
    CHECK(toneLevel(tones.first) > -26.0 - 12.0 - 3.0);
    CHECK(toneLevel(tones.first) > toneLevel(1111.0) + 6.0);
    // Distinct pairs for 32 outputs.
    std::vector<std::pair<double, double>> seen;
    for (int o = 0; o < 32; ++o) seen.push_back(CalibrationBus::channelIdTones(o));
    std::sort(seen.begin(), seen.end());
    CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}

TEST_CASE("safety: confirm guard, timeout and stop", "[calibration]") {
    CalibrationBus bus;
    bus.prepare(kFs, 1);
    std::string err;
    auto p = CalibrationParams::sine(1000.0, -10.0);
    CHECK_FALSE(bus.start(p, &err));
    CHECK_FALSE(err.empty());
    p.confirmLoud = true;
    CHECK(bus.start(p, &err));
    bus.stop();
    run(bus, 1, 4800);
    CHECK_FALSE(bus.running());
    CalibrationParams q;
    q.levelDbfs = -26.0;
    CHECK(q.levelDbfs == CalibrationBus::kDefaultLevelDbfs);
    q.levelDbfs = -12.0;
    CHECK(bus.start(q));  // exactly -12 does not need confirmation
    bus.stop();
    q.levelDbfs = -2.0;
    q.confirmLoud = true;
    CHECK_FALSE(bus.start(q));  // hard ceiling
    CHECK_FALSE(bus.start(CalibrationParams::sine(10.0, -30.0)));
    CalibrationParams bad;
    bad.outputs = {5};
    CHECK_FALSE(bus.start(bad));

    // Stop: silent within 100 ms (50 ms fade).
    REQUIRE(bus.start(CalibrationParams::sine(1000.0, -20.0)));
    run(bus, 1, 24000);
    bus.stop();
    const auto s = run(bus, 1, 6000, 256);
    CHECK(rmsDb(s.ch[0], 0, 480) > -30.0);  // still fading in the first 10 ms
    CHECK(rmsDb(s.ch[0], 4800, 6000) < -150.0);
    CHECK_FALSE(bus.running());

    // Hard 5 minute timeout.
    REQUIRE(bus.start(CalibrationParams::sine(1000.0, -26.0)));
    const std::size_t T = static_cast<std::size_t>(300.0 * kFs);
    auto t1 = run(bus, 1, T - 4800, 4800);
    CHECK(bus.running());
    const auto t2 = run(bus, 1, 4800 + 2400 + 4800, 4800);
    CHECK(rmsDb(t2.ch[0], 0, 2400) > -40.0);        // still playing just before 300 s
    CHECK(rmsDb(t2.ch[0], 4800 + 2400 + 100, t2.ch[0].size()) < -150.0);  // ended by 300 s + 50 ms
    CHECK_FALSE(bus.running());
}

TEST_CASE("source selector: equal-power crossfade", "[calibration]") {
    SourceSelector sel;
    sel.prepare(kFs);
    sel.request(OutputSource::Calibration, 0.5);
    sel.poll();
    const int n = 24000;
    std::vector<float> gm(n), gc(n);
    sel.gains(n, gm.data(), gc.data());
    for (int i = 0; i < n; ++i) REQUIRE(std::abs(double(gm[i]) * gm[i] + double(gc[i]) * gc[i] - 1.0) < 1e-5);
    CHECK(gm[0] > 0.999f);
    CHECK(gm[n - 1] == 0.0f);
    CHECK(gc[n - 1] == 1.0f);
    CHECK(std::abs(gm[n / 2] - std::sqrt(0.5)) < 0.01);
    // Back to masker in 500 ms; to mute fades the active source.
    sel.request(OutputSource::Masker, 0.5);
    sel.poll();
    sel.gains(n, gm.data(), gc.data());
    CHECK(sel.steadyMasker());
    sel.request(OutputSource::Mute, 0.5);
    sel.poll();
    sel.gains(n, gm.data(), gc.data());
    CHECK(gm[n - 1] == 0.0f);
    CHECK(gc[n - 1] == 0.0f);
}

TEST_CASE("stage: masker untouched in MASKER, crossfade and stop return", "[calibration]") {
    OutputSourceStage st;
    st.prepare(kFs, 2, 256);
    auto fill = [](std::vector<std::vector<float>>& b, float v) {
        for (auto& c : b) std::fill(c.begin(), c.end(), v);
    };
    std::vector<std::vector<float>> buf(2, std::vector<float>(256, 0.25f));
    std::vector<float*> ptr{buf[0].data(), buf[1].data()};
    st.process(ptr.data(), 256);
    for (float v : buf[0]) REQUIRE(v == 0.25f);

    std::string err;
    REQUIRE(st.startTest(CalibrationParams::sine(1000.0, -20.0), &err));
    // After the 500 ms crossfade the masker is silent and the sine is present.
    for (int blk = 0; blk < 100; ++blk) {
        fill(buf, 0.25f);
        st.process(ptr.data(), 256);
    }
    fill(buf, 0.25f);
    st.process(ptr.data(), 256);
    CHECK(rmsDb(buf[0], 0, 256) < -15.0);
    CHECK(rmsDb(buf[0], 0, 256) > -25.0);  // ~ -20 dBFS sine, masker (DC 0.25) gone
    st.stopTest();
    for (int blk = 0; blk < 30; ++blk) {  // 160 ms
        fill(buf, 0.25f);
        st.process(ptr.data(), 256);
    }
    for (float v : buf[1]) REQUIRE(v == 0.25f);
    CHECK(st.selector().steadyMasker());
}

TEST_CASE("test speakers: enabled outputs and sequence", "[calibration]") {
    std::vector<OutputChannel> ch(4);
    for (int i = 0; i < 4; ++i) ch[i].index = i;
    ch[1].enabled = false;
    ch[2].mute = true;
    CHECK(enabledOutputs(ch, {}, 4) == std::vector<int>{0, 3});
    std::vector<OutputZone> zones(2);
    zones[0].id = 0;
    zones[1].id = 1;
    zones[1].enabled = false;
    ch[3].zone = 1;
    CHECK(enabledOutputs(ch, zones, 4) == std::vector<int>{0});

    OutputSourceStage st;
    st.prepare(kFs, 4, 256);
    TestSpeakersOptions o;
    CHECK_FALSE(startTestSpeakers(st, {}, o));
    o.levelDbfs = -6.0;
    CHECK_FALSE(startTestSpeakers(st, {0, 3}, o));
    o.levelDbfs = -26.0;
    CHECK(startTestSpeakers(st, {0, 3}, o));
}

TEST_CASE("determinism: same seed same bits, any block size", "[calibration]") {
    auto gen = [](std::uint64_t seed, std::size_t block, CalSignal sig) {
        CalibrationBus bus;
        bus.prepare(kFs, 2);
        CalibrationParams p;
        p.signal = sig;
        p.seed = seed;
        REQUIRE(bus.start(p));
        return run(bus, 2, 30000, block);
    };
    for (CalSignal sig : {CalSignal::PinkNoise, CalSignal::SpeechNoise, CalSignal::ChannelId, CalSignal::OctaveNoise}) {
        const auto a = gen(5, 256, sig), b = gen(5, 256, sig), c = gen(5, 100, sig), d = gen(6, 256, sig);
        CHECK(a.ch == b.ch);
        CHECK(a.ch == c.ch);
        CHECK(a.ch != d.ch);
    }
}

TEST_CASE("V1 null interfaces never report SPL", "[calibration][ext]") {
    DigitalOnlyLevel lvl;
    CHECK(lvl.confidence().cls == CalibrationConfidence::None);
    CHECK_FALSE(lvl.estimatedSplDbA(0, -26.0f).has_value());
    CHECK_FALSE(lvl.digitalForTargetSplDbA(0, 45.0f).has_value());
    IdentitySpeakerCalibration id;
    const OutputCalibration oc = id.forOutput(3);
    CHECK(oc.gainDb == 0.0);
    CHECK(oc.delayMs == 0.0);
    CHECK_FALSE(oc.polarityInvert);
    CHECK_FALSE(oc.eqKernel.has_value());
    CHECK(id.revision() == 0);
    ConstantLevel cl;
    MaskStatistics st;
    CHECK(cl.updateGainOffsetDb(nullptr, st, 1.0) == 0.0f);
    NullIntelligibilityEstimator est;
    CHECK_FALSE(est.estimateSii({}, {}, nullptr).has_value());
    CHECK_FALSE(est.estimateSti({}, {}).has_value());
    NullMeasurementInput mi;
    CHECK_FALSE(mi.available());
    NullAmbientMonitor am;
    CHECK_FALSE(am.current().has_value());

    // Engine statistics JSON carries no SPL / dBA fields.
    std::function<void(const nlohmann::json&)> scan = [&](const nlohmann::json& j) {
        if (j.is_object())
            for (auto it = j.begin(); it != j.end(); ++it) {
                const std::string k = it.key();
                CHECK(k.find("SPL") == std::string::npos);
                CHECK(k.find("Spl") == std::string::npos);
                CHECK(k.find("dBA") == std::string::npos);
                CHECK(k.find("dba") == std::string::npos);
                scan(it.value());
            }
        else if (j.is_array())
            for (const auto& e : j) scan(e);
    };
    scan(toJson(MaskStatistics{}));
}
