#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <fstream>
#include <random>

#include "core/corpus/FlacCache.h"
#include "core/corpus/ingest/Analyzer.h"
#include "core/corpus/ingest/Decode.h"
#include "core/corpus/ingest/Features.h"
#include "core/corpus/ingest/Signal.h"
#include "ingest_fixtures.h"

using namespace bf;
using namespace bf::ingest;
using Catch::Approx;

namespace {

std::vector<std::uint8_t> flagsFromRuns(std::size_t n, std::initializer_list<std::pair<int, int>> runs) {
    std::vector<std::uint8_t> f(n, 0);
    for (auto [a, b] : runs)
        for (int i = a; i < b; ++i) f[static_cast<std::size_t>(i)] = 1;
    return f;
}

std::vector<float> sine(double f, double amp, double seconds, double fs) {
    std::vector<float> x(static_cast<std::size_t>(seconds * fs));
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(amp * std::sin(2.0 * M_PI * f * static_cast<double>(i) / fs));
    return x;
}

std::vector<float> harmonic(double f0, double seconds, double fs, double noiseDb = -200.0) {
    std::vector<float> x(static_cast<std::size_t>(seconds * fs));
    std::mt19937 rng(3);
    std::normal_distribution<double> nd(0.0, 1.0);
    for (std::size_t i = 0; i < x.size(); ++i) {
        double v = 0;
        for (int h = 1; h <= 6; ++h) v += std::sin(2.0 * M_PI * f0 * h * static_cast<double>(i) / fs) / h;
        x[i] = static_cast<float>(0.3 * v + std::pow(10.0, noiseDb / 20.0) * nd(rng));
    }
    return x;
}

}  // namespace

TEST_CASE("VAD post-processing: hangover, merge, isolated-short removal", "[ingest][vad]") {
    SECTION("hangover extends 50 ms before and 150 ms after") {
        const auto r = postProcessVadFlags(flagsFromRuns(300, {{100, 110}}));
        REQUIRE(r.size() == 1);
        CHECK(r[0].start == 95);
        CHECK(r[0].end == 125);
    }
    SECTION("start clamps at file start") {
        const auto r = postProcessVadFlags(flagsFromRuns(300, {{2, 30}}));
        REQUIRE(r.size() == 1);
        CHECK(r[0].start == 0);
    }
    SECTION("gaps < 80 ms are merged, >= 80 ms are kept (after hangover)") {
        // Run A ext -> [95,125); run B ext starts at B-5. gap = (B-5) - 125.
        auto merged = postProcessVadFlags(flagsFromRuns(400, {{100, 110}, {137, 160}}));  // gap 7 frames = 70 ms
        CHECK(merged.size() == 1);
        auto kept = postProcessVadFlags(flagsFromRuns(400, {{100, 110}, {138, 160}}));    // gap 8 frames = 80 ms
        CHECK(kept.size() == 2);
    }
    SECTION("short isolated regions (< 200 ms, pauses >= 500 ms) are dropped") {
        // A single 10 ms frame extends to 210 ms? no: 1 + 5 + 15 = 21 frames >= 20 -> kept.
        CHECK(postProcessVadFlags(flagsFromRuns(400, {{100, 101}})).size() == 1);
        // A run of 1 frame at the very end cannot get the after-hangover: [n-1-5, n) = 6 frames.
        CHECK(postProcessVadFlags(flagsFromRuns(300, {{299, 300}})).empty());
        // The same short region 300 ms from a neighbour is kept.
        const auto r = postProcessVadFlags(flagsFromRuns(400, {{50, 100}, {250, 251}, {300, 301}}));
        CHECK(r.size() >= 2);
        // Isolated short region far from everything: build with a clipped tail.
        const auto s = postProcessVadFlags(flagsFromRuns(400, {{50, 100}, {390, 400}}));
        REQUIRE(s.size() == 1);  // 385..400 = 15 frames < 20, gap to previous >= 500 ms
        CHECK(s[0].start == 45);
    }
    SECTION("no speech gives no regions") {
        CHECK(postProcessVadFlags(std::vector<std::uint8_t>(100, 0)).empty());
    }
}

TEST_CASE("ITU-T P.56 method B", "[ingest][p56]") {
    constexpr double fs = 48000.0;
    SECTION("continuous sine: ASL = RMS, activity 100 %") {
        const auto x = sine(1000.0, 0.1, 10.0, fs);
        const auto r = p56MethodB(x.data(), x.size(), fs);
        REQUIRE(r.valid);
        CHECK(r.aslDb == Approx(20.0 * std::log10(0.1 / std::sqrt(2.0))).margin(0.2));
        CHECK(r.activityPct > 97.0);
    }
    SECTION("white noise level is recovered") {
        std::mt19937 rng(1);
        std::normal_distribution<float> nd(0.0f, 0.05f);
        std::vector<float> x(static_cast<std::size_t>(fs * 8));
        for (auto& v : x) v = nd(rng);
        const auto r = p56MethodB(x.data(), x.size(), fs);
        CHECK(r.aslDb == Approx(20.0 * std::log10(0.05)).margin(0.2));
    }
    SECTION("sine bursts with digital silence: known level and activity") {
        // 10 s bursts / 10 s silence, three periods. Burst level -23.01 dBFS; the 0.2 s hangover
        // per burst gives an expected activity of 3 x 10.2 / 60 = 51 %.
        std::vector<float> x;
        const auto burst = sine(500.0, 0.1, 10.0, fs);
        for (int k = 0; k < 3; ++k) {
            x.insert(x.end(), burst.begin(), burst.end());
            x.insert(x.end(), burst.size(), 0.0f);
        }
        const auto r = p56MethodB(x.data(), x.size(), fs);
        REQUIRE(r.valid);
        CHECK(r.aslDb == Approx(20.0 * std::log10(0.1 / std::sqrt(2.0))).margin(0.2));
        CHECK(r.activityPct == Approx(51.0).margin(2.0));
    }
    SECTION("bursts over a -70 dBFS noise floor ignore the floor") {
        std::vector<float> x = sine(500.0, 0.1, 4.0, fs);
        std::mt19937 rng(9);
        std::normal_distribution<float> nd(0.0f, static_cast<float>(std::pow(10.0, -70.0 / 20.0)));
        std::vector<float> gap(static_cast<std::size_t>(4 * fs));
        for (auto& v : gap) v = nd(rng);
        std::vector<float> all;
        for (int k = 0; k < 3; ++k) { all.insert(all.end(), x.begin(), x.end()); all.insert(all.end(), gap.begin(), gap.end()); }
        const auto r = p56MethodB(all.data(), all.size(), fs);
        CHECK(r.aslDb == Approx(20.0 * std::log10(0.1 / std::sqrt(2.0))).margin(0.3));
    }
    SECTION("silence is invalid") {
        std::vector<float> z(48000, 0.0f);
        CHECK_FALSE(p56MethodB(z.data(), z.size(), fs).valid);
    }
}

TEST_CASE("YIN F0 on harmonic signals", "[ingest][yin]") {
    for (double f0 : {90.0, 120.0, 150.0, 220.0, 310.0}) {
        DYNAMIC_SECTION("f0 = " << f0) {
            const auto x = harmonic(f0, 3.0, 16000.0);
            const std::vector<std::uint8_t> mask(x.size() / 160, 1);
            const auto st = yinF0(x, mask);
            CHECK(st.medianHz == Approx(f0).epsilon(0.01));
            CHECK(st.meanHz == Approx(f0).epsilon(0.01));
            CHECK(st.voicedRatio > 0.95);
            CHECK(st.rangeSt < 0.5);
        }
    }
    SECTION("moderate noise (20 dB SNR) still within 1 %") {
        const auto x = harmonic(140.0, 3.0, 16000.0, -30.0);
        const std::vector<std::uint8_t> mask(x.size() / 160, 1);
        CHECK(yinF0(x, mask).medianHz == Approx(140.0).epsilon(0.01));
    }
    SECTION("noise is unvoiced") {
        std::mt19937 rng(5);
        std::normal_distribution<float> nd(0.0f, 0.1f);
        std::vector<float> x(48000);
        for (auto& v : x) v = nd(rng);
        const std::vector<std::uint8_t> mask(x.size() / 160, 1);
        CHECK(yinF0(x, mask).voicedRatio < 0.1);
    }
    SECTION("glide covers the expected range and histogram sums to 1") {
        std::vector<float> x(16000 * 3);
        double ph = 0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double f = 100.0 * std::pow(2.0, static_cast<double>(i) / static_cast<double>(x.size()));  // 100..200
            ph += 2.0 * M_PI * f / 16000.0;
            x[i] = static_cast<float>(0.3 * (std::sin(ph) + 0.5 * std::sin(2 * ph) + 0.3 * std::sin(3 * ph)));
        }
        const std::vector<std::uint8_t> mask(x.size() / 160, 1);
        const auto st = yinF0(x, mask);
        CHECK(st.rangeSt == Approx(10.8).margin(1.5));  // p5..p95 of a 12 st glide
        float s = 0;
        for (float h : st.hist) s += h;
        CHECK(s == Approx(1.0).margin(1e-4));
    }
}

TEST_CASE("clipping detector", "[ingest][clip]") {
    constexpr double fs = 48000.0;
    SECTION("clean sine") {
        const auto x = sine(3000.0, 0.5, 2.0, fs);
        const auto r = detectClipping(x.data(), x.size(), 1, fs);
        CHECK(r.ratio < 1e-5);
    }
    SECTION("smooth 50 Hz sine at -1 dBFS is not clipped (flat-top rule)") {
        const auto x = sine(50.0, 0.8913, 2.0, fs);
        const auto r = detectClipping(x.data(), x.size(), 1, fs);
        CHECK(r.ratio == Approx(0.0));
    }
    SECTION("hard-clipped 50 Hz sine is flagged") {
        auto x = sine(50.0, 1.5, 2.0, fs);
        for (auto& v : x) v = std::clamp(v, -0.8913f, 0.8913f);
        CHECK(detectClipping(x.data(), x.size(), 1, fs).ratio > 1e-3);
    }
    SECTION("hard-clipped sine is flagged with runs > 3 ms") {
        auto x = sine(120.0, 6.0, 2.0, fs);
        for (auto& v : x) v = std::clamp(v, -1.0f, 1.0f);
        const auto r = detectClipping(x.data(), x.size(), 1, fs);
        CHECK(r.ratio > 1e-4);
        CHECK(r.longestRunMs > 3.0);
    }
    SECTION("clipping at a non-full-scale level (3 samples within 0.01 dB of max)") {
        auto x = sine(120.0, 2.0, 2.0, fs);
        for (auto& v : x) v = std::clamp(v, -0.4f, 0.4f);
        const auto r = detectClipping(x.data(), x.size(), 1, fs);
        CHECK(r.ratio > 1e-3);
    }
    SECTION("|x| >= 0.999 counts even as a single sample; runs closer than 1 ms are merged") {
        std::vector<float> x(48000, 0.0f);
        x[1000] = 0.9995f;
        x[1010] = -0.9995f;    // 10 samples apart: merged (< 1 ms)
        x[5000] = 0.9995f;
        auto r = detectClipping(x.data(), x.size(), 1, fs);
        CHECK(r.ratio == Approx(3.0 / 48000.0));
        CHECK(r.longestRunMs == Approx(1000.0 * 11.0 / fs).margin(1e-6));
    }
    SECTION("strided (interleaved) input") {
        std::vector<float> x(2 * 4800, 0.1f);
        for (std::size_t i = 0; i < 4800; ++i) {
            x[2 * i] = 0.1f * std::sin(0.4f * static_cast<float>(i));
            x[2 * i + 1] = 1.0f;  // right channel fully clipped
        }
        CHECK(detectClipping(x.data(), 4800, 2, fs).ratio == Approx(0.0));
        CHECK(detectClipping(x.data() + 1, 4800, 2, fs).ratio == Approx(1.0));
    }
}

TEST_CASE("quality score and classes", "[ingest][quality]") {
    auto base = [] {
        RecordingAnalysis a;
        a.sourceRate = 48000;
        a.durationS = 120;
        a.speechRatio = 0.8;
        a.snrDb = 45;
        a.bandwidthHz = 15000;
        a.clipRatio = 0;
        return a;
    };
    SECTION("good") {
        auto a = base();
        scoreQuality(a);
        CHECK(a.qualityClass == QualityClass::Good);
        CHECK(a.qualityScore == Approx(100.0));
        CHECK(a.reasons.empty());
    }
    SECTION("penalty table") {
        auto a = base(); a.snrDb = 30; scoreQuality(a);       // 20
        CHECK(a.qualityScore == Approx(80.0));
        CHECK(a.hasReason("warn.snr"));
        a = base(); a.bandwidthHz = 9250; scoreQuality(a);   // 30 * 1750/3500 = 15
        CHECK(a.qualityScore == Approx(85.0));
        a = base(); a.clipRatio = 5.5e-5; scoreQuality(a);   // 20 * 0.5 = 10
        CHECK(a.qualityScore == Approx(90.0));
        CHECK(a.hasReason("warn.clipping"));
        a = base(); a.speechRatio = 0.45; scoreQuality(a);   // 10
        CHECK(a.qualityScore == Approx(90.0));
        a = base(); a.lossy = true; a.reverberant = true; a.multichannelSelected = true; scoreQuality(a);
        CHECK(a.qualityScore == Approx(75.0));
        CHECK(a.qualityClass == QualityClass::Usable);
    }
    SECTION("usable band 50-79") {
        auto a = base(); a.snrDb = 28; a.bandwidthHz = 9000; scoreQuality(a);  // 28 + 17.1 = 45.1 -> 54.9
        CHECK(a.qualityClass == QualityClass::Usable);
        CHECK(a.qualityScore == Approx(100.0 - 28.0 - 30.0 * 2000.0 / 3500.0).margin(1e-6));
    }
    SECTION("hard reject rules") {
        struct Case { const char* code; void (*mod)(RecordingAnalysis&); };
        const Case cases[] = {
            {"reject.sample_rate", [](RecordingAnalysis& a) { a.sourceRate = 22050; }},
            {"reject.bandwidth", [](RecordingAnalysis& a) { a.bandwidthHz = 4000; }},
            {"reject.clipping", [](RecordingAnalysis& a) { a.clipRatio = 2e-4; }},
            {"reject.clipping", [](RecordingAnalysis& a) { a.clipRunMs = 3.5; }},
            {"reject.snr", [](RecordingAnalysis& a) { a.snrDb = 20; }},
            {"reject.speech_ratio", [](RecordingAnalysis& a) { a.speechRatio = 0.2; }},
            {"reject.duration", [](RecordingAnalysis& a) { a.durationS = 8; }},
        };
        for (const auto& c : cases) {
            auto a = base();
            c.mod(a);
            scoreQuality(a);
            INFO(c.code);
            CHECK(a.qualityClass == QualityClass::Rejected);
            CHECK(a.hasReason(c.code));
        }
    }
    SECTION("score < 50 without a hard rule is rejected") {
        auto a = base();
        a.snrDb = 25.5; a.bandwidthHz = 8000; a.lossy = true; a.reverberant = true; a.multichannelSelected = true;
        scoreQuality(a);
        CHECK(a.qualityScore < 50.0);
        CHECK(a.qualityClass == QualityClass::Rejected);
        CHECK(a.hasReason("reject.quality"));
    }
    SECTION("warnings for 32 kHz sources and 10-30 s files") {
        auto a = base(); a.sourceRate = 32000; a.durationS = 20; scoreQuality(a);
        CHECK(a.hasReason("warn.sample_rate"));
        CHECK(a.hasReason("warn.duration"));
        CHECK(a.qualityClass == QualityClass::Good);
    }
}

TEST_CASE("resampling and DC removal", "[ingest][signal]") {
    SECTION("44.1 kHz -> 48 kHz keeps level, length and frequency") {
        const auto x = sine(1000.0, 0.5, 2.0, 44100.0);
        const auto y = resample(x.data(), x.size(), 44100.0, 48000.0);
        CHECK(y.size() == static_cast<std::size_t>(std::llround(static_cast<double>(x.size()) * 48000.0 / 44100.0)));
        std::vector<float> mid(y.begin() + 4800, y.end() - 4800);
        CHECK(bftest::rmsDb(mid) == Approx(20.0 * std::log10(0.5 / std::sqrt(2.0))).margin(0.01));
        // Phase alignment: sample n of y should equal sin(2 pi f n / 48000).
        double err = 0;
        for (std::size_t n = 4800; n < y.size() - 4800; ++n) err = std::max(err, std::fabs(static_cast<double>(y[n]) - 0.5 * std::sin(2.0 * M_PI * 1000.0 * static_cast<double>(n) / 48000.0)));
        CHECK(err < 1e-3);
    }
    SECTION("96 kHz -> 48 kHz rejects content above 24 kHz") {
        auto x = sine(30000.0, 0.5, 1.0, 96000.0);
        const auto y = resample(x.data(), x.size(), 96000.0, 48000.0);
        std::vector<float> mid(y.begin() + 4800, y.end() - 4800);
        CHECK(bftest::rmsDb(mid) < -100.0);
    }
    SECTION("same rate is a copy") {
        const auto x = sine(1000.0, 0.5, 0.1, 48000.0);
        CHECK(resample(x.data(), x.size(), 48000.0, 48000.0) == x);
    }
    SECTION("zero-phase 20 Hz high-pass removes DC and leaves speech band untouched") {
        auto x = sine(200.0, 0.2, 3.0, 48000.0);
        for (auto& v : x) v += 0.05f;
        removeDcZeroPhase(x, 48000.0);
        double m = 0, dev = 0;
        for (std::size_t n = 24000; n < x.size() - 24000; ++n) m += x[n];
        m /= static_cast<double>(x.size() - 48000);
        CHECK(std::fabs(m) < 1e-4);
        for (std::size_t n = 24000; n < x.size() - 24000; ++n)
            dev = std::max(dev, std::fabs(static_cast<double>(x[n]) - 0.2 * std::sin(2.0 * M_PI * 200.0 * static_cast<double>(n) / 48000.0)));
        CHECK(dev < 2e-3);  // zero phase: no delay
        CHECK(std::fabs(x[10]) < 0.05);  // no large start-up transient
    }
}

TEST_CASE("decoders: WAV, FLAC, AIFF", "[ingest][decode]") {
    const auto dir = bftest::fixtureDir("decode");
    const auto x = sine(440.0, 0.5, 0.5, 48000.0);
    SECTION("WAV") {
        REQUIRE(bftest::writeWav(dir / "a.wav", x));
        DecodedAudio d;
        std::string why;
        REQUIRE(decodeFile((dir / "a.wav").string(), d, why));
        CHECK(d.sampleRate == 48000);
        CHECK(d.channels == 1);
        CHECK(d.data == x);
    }
    SECTION("FLAC via the cache writer (24-bit)") {
        REQUIRE(writeFlacCache(dir / "a.flac", x.data(), x.size()));
        DecodedAudio d;
        std::string why;
        REQUIRE(decodeFile((dir / "a.flac").string(), d, why));
        CHECK(d.sampleRate == 48000);
        CHECK(d.bitDepth == 24);
        REQUIRE(d.data.size() == x.size());
        double err = 0;
        for (std::size_t i = 0; i < x.size(); ++i) err = std::max(err, std::fabs(static_cast<double>(d.data[i] - x[i])));
        CHECK(err <= 0.5 / 8388608.0 + 1e-9);
    }
    SECTION("AIFF 16-bit big-endian stereo") {
        std::vector<unsigned char> b;
        auto put32 = [&](std::uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<unsigned char>(v >> s)); };
        auto put16 = [&](std::uint16_t v) { b.push_back(static_cast<unsigned char>(v >> 8)); b.push_back(static_cast<unsigned char>(v)); };
        const std::uint32_t frames = 1000;
        b.insert(b.end(), {'F', 'O', 'R', 'M'});
        put32(4 + 8 + 18 + 8 + 8 + frames * 4);
        b.insert(b.end(), {'A', 'I', 'F', 'F', 'C', 'O', 'M', 'M'});
        put32(18);
        put16(2); put32(frames); put16(16);
        // 44100 as 80-bit extended: exponent 0x400E, mantissa 0xAC44000000000000
        b.insert(b.end(), {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0});
        b.insert(b.end(), {'S', 'S', 'N', 'D'});
        put32(8 + frames * 4);
        put32(0); put32(0);
        for (std::uint32_t i = 0; i < frames; ++i) {
            put16(static_cast<std::uint16_t>(static_cast<std::int16_t>(i * 10)));
            put16(static_cast<std::uint16_t>(static_cast<std::int16_t>(-static_cast<int>(i) * 10)));
        }
        std::ofstream(dir / "a.aiff", std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        DecodedAudio d;
        std::string why;
        REQUIRE(decodeFile((dir / "a.aiff").string(), d, why));
        CHECK(d.sampleRate == 44100);
        CHECK(d.channels == 2);
        REQUIRE(d.frames() == frames);
        CHECK(d.data[2 * 100] == Approx(1000.0 / 32768.0));
        CHECK(d.data[2 * 100 + 1] == Approx(-1000.0 / 32768.0));
    }
    SECTION("unsupported containers, missing and corrupt files") {
        std::ofstream(dir / "a.mp3", std::ios::binary) << "not really";
        DecodedAudio d;
        std::string why;
        CHECK_FALSE(decodeFile((dir / "a.mp3").string(), d, why));
        CHECK(why == "decode.unsupported");
        CHECK_FALSE(decodeFile((dir / "missing.wav").string(), d, why));
        CHECK(why == "decode.open");
        std::ofstream(dir / "bad.wav", std::ios::binary) << "RIFFxxxxWAVEjunk";
        CHECK_FALSE(decodeFile((dir / "bad.wav").string(), d, why));
        CHECK(why.rfind("decode.", 0) == 0);
        std::ofstream(dir / "bad.flac", std::ios::binary) << "garbage garbage garbage";
        CHECK_FALSE(decodeFile((dir / "bad.flac").string(), d, why));
        CHECK(why.rfind("decode.", 0) == 0);
    }
}
