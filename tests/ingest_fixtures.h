#pragma once
// Synthetic audio fixtures for the ingestion tests (generated at test time, never stored).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/corpus/CorpusSnapshot.h"
#include "core/corpus/SyntheticCorpus.h"
#include "core/corpus/ingest/IngestTypes.h"
#include "core/io/WavWriter.h"

namespace bftest {

// Per-process directory so concurrently running test processes (ctest -j) never share paths.
inline std::filesystem::path fixturePath(const std::string& name) {
#ifdef _WIN32
    const auto pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
    const auto pid = static_cast<unsigned long>(::getpid());
#endif
    return std::filesystem::current_path() / "ingest_fixtures" / ("p" + std::to_string(pid)) / name;
}

inline std::filesystem::path fixtureDir(const std::string& name) {
    auto d = fixturePath(name);
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

inline double rmsDb(const std::vector<float>& x) {
    double e = 0;
    for (float v : x) e += static_cast<double>(v) * v;
    return 10.0 * std::log10(std::max(e / static_cast<double>(std::max<std::size_t>(x.size(), 1)), 1e-20));
}

// Speech-like 48 kHz mono. Timing (speech regions, pauses) and the aspiration noise come from
// SyntheticCorpus; the voiced part is a sawtooth (1/h harmonics up to Nyquist, speaker F0 taken
// from the synthetic speaker) with 4 Hz syllable modulation, so YIN sees a periodic signal and
// the LTASS extends to the Nyquist limit. Pauses are digital silence. Speech-region level is
// scaled to `aslDb`; a continuous white noise floor at `noiseFloorDb` is added.
inline std::vector<float> speechLike(double seconds, std::uint64_t seed, double aslDb = -26.0,
                                     double noiseFloorDb = -75.0, double aspirationDb = -20.0,
                                     double pauseMedianS = 0.9) {
    bf::SyntheticCorpusParams p;
    p.numSpeakers = 1;
    p.recordingsPerSpeaker = 1;
    p.recordingSeconds = seconds;
    p.seed = seed;
    p.pauseMedianS = pauseMedianS;
    bf::SyntheticCorpus c(p);
    const auto snap = c.snapshot();
    const auto n = static_cast<std::size_t>(snap->recording(0).length);
    std::vector<float> asp(n);
    c.read(0, 0, asp.data(), n);
    const double f0 = 100.0 * std::pow(2.0, static_cast<double>(snap->speaker(0).features[bf::kFeatF0MedianSt]) / 12.0);
    std::vector<float> x(n, 0.0f);
    double phase = 0.0;
    double eTone = 0, eAsp = 0, cnt = 0;
    for (const auto& r : snap->regionsOf(0)) {
        for (auto i = r.start; i < r.end; ++i) {
            const auto k = static_cast<std::size_t>(i);
            phase += f0 / 48000.0;
            phase -= std::floor(phase);
            const double t = static_cast<double>(i - r.start) / 48000.0;
            const double sm = std::sin(M_PI * 4.0 * t);
            double env = 0.15 + 0.85 * sm * sm;
            const auto d = std::min<std::int64_t>({i - r.start, r.end - 1 - i, 240});
            env *= 0.5 - 0.5 * std::cos(M_PI * static_cast<double>(d) / 240.0);
            x[k] = static_cast<float>(env * (2.0 * phase - 1.0));
            eTone += static_cast<double>(x[k]) * x[k];
            eAsp += static_cast<double>(asp[k]) * asp[k];
            cnt += 1;
        }
    }
    const float ga = static_cast<float>(std::sqrt(eTone / std::max(eAsp, 1e-30)) * std::pow(10.0, aspirationDb / 20.0));
    for (std::size_t i = 0; i < n; ++i) x[i] += ga * asp[i];
    double e = 0;
    for (const auto& r : snap->regionsOf(0))
        for (auto i = r.start; i < r.end; ++i) e += static_cast<double>(x[static_cast<std::size_t>(i)]) * x[static_cast<std::size_t>(i)];
    const double cur = 10.0 * std::log10(e / std::max(cnt, 1.0));
    const float s = static_cast<float>(std::pow(10.0, (aslDb - cur) / 20.0));
    for (auto& v : x) v *= s;
    std::mt19937 rng(static_cast<std::uint32_t>(seed) * 2654435761u + 1u);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    const float nf = static_cast<float>(std::pow(10.0, noiseFloorDb / 20.0));
    for (auto& v : x) v += nf * nd(rng);
    return x;
}

inline std::vector<float> addWhiteNoise(std::vector<float> x, double levelDb, std::uint32_t seed = 7) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    const float g = static_cast<float>(std::pow(10.0, levelDb / 20.0));
    for (auto& v : x) v += g * nd(rng);
    return x;
}

inline std::vector<float> clipped(std::vector<float> x, float gain) {
    for (auto& v : x) v = std::clamp(v * gain, -1.0f, 1.0f);
    return x;
}

// Windowed-sinc low-pass (Blackman, 511 taps), zero-delay compensated.
inline std::vector<float> lowpass(const std::vector<float>& x, double fc, double fs = 48000.0) {
    const int N = 511, M = N / 2;
    std::vector<double> h(N);
    const double pi = std::acos(-1.0);
    double sum = 0;
    for (int i = 0; i < N; ++i) {
        const double t = i - M;
        const double s = t == 0 ? 2.0 * fc / fs : std::sin(2.0 * pi * fc * t / fs) / (pi * t);
        const double w = 0.42 - 0.5 * std::cos(2.0 * pi * i / (N - 1)) + 0.08 * std::cos(4.0 * pi * i / (N - 1));
        h[static_cast<std::size_t>(i)] = s * w;
        sum += s * w;
    }
    for (auto& v : h) v /= sum;
    std::vector<float> y(x.size(), 0.0f);
    for (std::size_t n = 0; n < x.size(); ++n) {
        double a = 0;
        for (int k = 0; k < N; ++k) {
            const auto idx = static_cast<std::int64_t>(n) + M - k;
            if (idx >= 0 && idx < static_cast<std::int64_t>(x.size())) a += h[static_cast<std::size_t>(k)] * x[static_cast<std::size_t>(idx)];
        }
        y[n] = static_cast<float>(a);
    }
    return y;
}

inline bf::ingest::DecodedAudio toDecoded(const std::vector<float>& mono, std::uint32_t rate = 48000) {
    bf::ingest::DecodedAudio d;
    d.sampleRate = rate;
    d.channels = 1;
    d.bitDepth = 24;
    d.format = "wav";
    d.data = mono;
    return d;
}

inline bf::ingest::DecodedAudio toStereo(const std::vector<float>& l, const std::vector<float>& r) {
    bf::ingest::DecodedAudio d;
    d.sampleRate = 48000;
    d.channels = 2;
    d.bitDepth = 24;
    d.format = "wav";
    d.data.resize(l.size() * 2);
    for (std::size_t i = 0; i < l.size(); ++i) { d.data[2 * i] = l[i]; d.data[2 * i + 1] = r[i]; }
    return d;
}

inline bool writeWav(const std::filesystem::path& p, const std::vector<float>& x, std::uint32_t rate = 48000) {
    bf::WavWriter w;
    if (!w.open(p.string(), rate, 1)) return false;
    w.writeInterleaved(x.data(), x.size());
    return w.close();
}

}  // namespace bftest
