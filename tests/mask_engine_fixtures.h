#pragma once
// Shared fixtures for tests/test_mask_engine.cpp and tests/test_render.cpp.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config/DataSet.h"
#include "core/corpus/SyntheticCorpus.h"
#include "core/engine/OfflineRenderer.h"
#include "core/engine/Scenario.h"

namespace bftest {

inline const bf::DataSet& dataSet() {
    static const bf::DataSet ds = [] {
        auto r = bf::loadDataSet(BF_DATA_DIR);
        REQUIRE(r.ok);
        return r.data;
    }();
    return ds;
}

// Speech-like synthetic talkers: SyntheticCorpus(WhiteNoise) speech regions (exact VAD and ASL
// metadata, pauses of digital silence) through
//  - a 32-tap FIR (truncated one-pole low-pass at 500 Hz, power-normalised): band levels rise
//    +3 dB/oct below and fall ~-3 dB/oct above 500 Hz, within a few dB of the LTASS targets, so
//    the babble EQ never clamps;
//  - a syllabic amplitude modulation 0.15 + 0.85 sin^2(pi f_m t), f_m in 3..6 Hz per recording,
//    power-normalised (mean square 1), evaluated at the absolute source sample.
// Every output sample depends only on (recording, sample index), so reads are exact for any
// chunking (bit-exact renders, REALTIME §8.4).
class ShapedWhiteSource final : public bf::IAudioSource {
public:
    static constexpr std::size_t kTaps = 32;
    explicit ShapedWhiteSource(std::uint32_t speakers, double recordingSeconds = 60.0,
                               std::uint32_t recordingsPerSpeaker = 2) {
        bf::SyntheticCorpusParams p;
        p.numSpeakers = speakers;
        p.recordingsPerSpeaker = recordingsPerSpeaker;
        p.recordingSeconds = recordingSeconds;
        p.signal = bf::SyntheticCorpusParams::Signal::WhiteNoise;
        p.measureAsl = false;  // white noise: the nominal speaker level is the exact ASL
        inner_ = std::make_unique<bf::SyntheticCorpus>(p);
        const double a = std::exp(-2.0 * 3.14159265358979323846 * 500.0 / 48000.0);
        double e = 0.0;
        h_.resize(kTaps);
        for (std::size_t k = 0; k < kTaps; ++k) {
            h_[k] = static_cast<float>(std::pow(a, static_cast<double>(k)));
            e += static_cast<double>(h_[k]) * h_[k];
        }
        for (float& v : h_) v = static_cast<float>(v / std::sqrt(e));
        // E[(0.15 + 0.85 s^2)^2] with s = sin(uniform phase) = 0.0225 + 0.1275 + 0.7225 * 3/8.
        envNorm_ = 1.0 / std::sqrt(0.0225 + 0.1275 + 0.7225 * 0.375);
    }
    std::shared_ptr<const bf::CorpusSnapshot> snapshot() const { return inner_->snapshot(); }
    bf::SyntheticCorpus& inner() { return *inner_; }
    bool read(bf::RecordingId rec, std::uint64_t start, float* dst, std::size_t n) override {
        const std::uint64_t hist = kTaps - 1;
        const std::uint64_t a0 = start >= hist ? start - hist : 0;
        const std::size_t pre = static_cast<std::size_t>(start - a0);
        tmp_.assign(hist - pre, 0.0f);  // zeros before the recording start
        const std::size_t off = tmp_.size();
        tmp_.resize(off + pre + n);
        if (!inner_->read(rec, a0, tmp_.data() + off, pre + n)) return false;
        const double fm = 3.0 + 3.0 * static_cast<double>((rec * 2654435761u) % 1000u) / 1000.0;
        const double w = 3.14159265358979323846 * fm / 48000.0;
        for (std::size_t i = 0; i < n; ++i) {
            const float* x = tmp_.data() + hist + i;
            float y = 0.0f;
            for (std::size_t k = 0; k < kTaps; ++k) y += h_[k] * x[-static_cast<std::ptrdiff_t>(k)];
            const double s = std::sin(w * static_cast<double>(start + i));
            dst[i] = static_cast<float>(static_cast<double>(y) * envNorm_ * (0.15 + 0.85 * s * s));
        }
        return true;
    }

private:
    std::unique_ptr<bf::SyntheticCorpus> inner_;
    std::vector<float> tmp_;
    std::vector<float> h_;
    double envNorm_ = 1.0;
};

inline ShapedWhiteSource& shapedCorpus() {
    static ShapedWhiteSource s(20);
    return s;
}

inline bf::CorpusHandle handle(ShapedWhiteSource& s) { return {s.snapshot(), &s}; }
inline bf::CorpusHandle handle(bf::SyntheticCorpus& s) { return {s.snapshot(), &s}; }

inline nlohmann::json scenarioDoc(const std::string& area, const std::string& strategy, const std::string& layout,
                                  double durationS, std::uint64_t seed,
                                  const nlohmann::json& macros = nlohmann::json::object()) {
    nlohmann::json preset = {{"name", "test"}, {"area", area}, {"strategy", strategy},
                             {"outputs", {{"layout", layout}}}};
    if (!macros.empty()) preset["macros"] = macros;
    return {{"schema", "babbleforge.scenario"},
            {"schemaVersion", "1.0"},
            {"preset", preset},
            {"seed", seed},
            {"durationS", durationS},
            {"sampleRate", 48000}};
}

inline bf::Scenario scenario(const nlohmann::json& doc) {
    bf::Scenario sc;
    std::string err;
    INFO(err);
    REQUIRE(bf::parseScenario(doc, ".", sc, &err));
    return sc;
}

inline bf::RenderResult render(const nlohmann::json& doc, const bf::CorpusHandle& corpus,
                               const bf::RenderOptions& opt = {}) {
    bf::RenderResult r = bf::renderScenario(dataSet(), scenario(doc), corpus, opt);
    INFO(r.error);
    REQUIRE(r.ok);
    return r;
}

// Energy mean over channels of [a, b), dBFS.
inline double rmsDb(const std::vector<std::vector<float>>& x, std::size_t a, std::size_t b) {
    double e = 0.0;
    for (const auto& ch : x)
        for (std::size_t i = a; i < b; ++i) e += static_cast<double>(ch[i]) * ch[i];
    return 10.0 * std::log10(e / static_cast<double>((b - a) * x.size()));
}
inline double rmsDb(const std::vector<float>& ch, std::size_t a, std::size_t b) {
    double e = 0.0;
    for (std::size_t i = a; i < b; ++i) e += static_cast<double>(ch[i]) * ch[i];
    return 10.0 * std::log10(e / static_cast<double>(b - a));
}

}  // namespace bftest
