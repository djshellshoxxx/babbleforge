#pragma once
// Shared fixtures for the real-time host tests (tests/test_rt_*.cpp).
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/rt/NullBackend.h"
#include "core/strategy/StrategyTypes.h"
#include "mask_engine_fixtures.h"

namespace bftest {

inline nlohmann::json presetDoc(const std::string& area, const std::string& strategy, const std::string& layout = "stereo",
                                const std::string& fallback = "continuous") {
    return {{"schema", "babbleforge.preset"},
            {"schemaVersion", "1.0"},
            {"name", "rt-test"},
            {"area", area},
            {"strategy", strategy},
            {"outputs", {{"layout", layout}}},
            {"reliability", {{"fallbackPolicy", fallback}}}};
}

inline bf::ScenarioPlan planFor(const nlohmann::json& preset, const std::shared_ptr<const bf::CorpusSnapshot>& snap,
                                std::uint64_t seed) {
    const bf::CorpusSummary cs = snap ? bf::CorpusSummary::from(*snap) : bf::CorpusSummary{};
    bf::ScenarioPlan sp = bf::buildScenarioPlan(dataSet(), preset, std::nullopt, cs, seed);
    INFO(sp.error);
    REQUIRE(sp.ok);
    return sp;
}

// RT-safe output capture (preallocated): planar samples of the first `capacity` frames after
// arm(), plus running energy.
class OutputCapture final : public bf::rt::NullBackend::Observer {
public:
    OutputCapture(int channels, std::size_t capacity) : cap_(capacity) {
        data_.assign(static_cast<std::size_t>(channels), std::vector<float>(capacity, 0.0f));
    }
    void arm() noexcept { armed_.store(true, std::memory_order_release); }
    void onOutput(const float* const* out, int nOut, int n) noexcept override {
        if (!armed_.load(std::memory_order_acquire)) return;
        const std::size_t w = written_.load(std::memory_order_relaxed);
        const std::size_t m = std::min<std::size_t>(static_cast<std::size_t>(n), cap_ - w);
        const int nc = std::min<int>(nOut, static_cast<int>(data_.size()));
        for (int c = 0; c < nc; ++c)
            for (std::size_t i = 0; i < m; ++i) data_[static_cast<std::size_t>(c)][w + i] = out[c][i];
        written_.store(w + m, std::memory_order_release);
    }
    std::size_t frames() const noexcept { return written_.load(std::memory_order_acquire); }
    const std::vector<std::vector<float>>& data() const noexcept { return data_; }

private:
    std::size_t cap_;
    std::vector<std::vector<float>> data_;
    std::atomic<std::size_t> written_{0};
    std::atomic<bool> armed_{false};
};

inline bool waitUntil(const std::function<bool()>& pred, double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

}  // namespace bftest
