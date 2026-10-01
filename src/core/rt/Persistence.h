#pragma once
// Configuration persistence of the real-time host (docs/PRESETS.md §9, RELIABILITY.md §2
// "Invalid config at startup"). Every write is atomic with a .bak of the previous version
// (config/AtomicFile).
//
//   session.json            current effective preset (+ seed); every 30 s if changed, on stop/exit
//   last_known_good.json    configuration that reached RUNNING and stayed >= 60 s without ERROR
//   correction_memory.json  spectral correction per (plan hash, corpus version, fs)
//   selector_state.json     persistent shuffle (SegmentSelector::exportState)
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/analysis/SpectralCorrection.h"
#include "core/config/DataSet.h"
#include "core/strategy/StrategyTypes.h"

namespace bf::rt {

class SessionStore {
public:
    explicit SessionStore(std::filesystem::path dir = {});
    bool enabled() const noexcept { return !dir_.empty(); }
    const std::filesystem::path& dir() const noexcept { return dir_; }
    std::filesystem::path sessionPath() const { return dir_ / "session.json"; }
    std::filesystem::path lkgPath() const { return dir_ / "last_known_good.json"; }
    std::filesystem::path correctionPath() const { return dir_ / "correction_memory.json"; }
    std::filesystem::path selectorPath() const { return dir_ / "selector_state.json"; }

    bool saveSession(const nlohmann::json& preset, std::uint64_t seed, std::string* error = nullptr);
    std::optional<nlohmann::json> loadSessionPreset(std::string* error = nullptr) const;
    bool saveLastKnownGood(const nlohmann::json& preset, std::string* error = nullptr);
    std::optional<nlohmann::json> loadLastKnownGood(std::string* error = nullptr) const;

    bool saveCorrection(std::uint64_t planHash, const std::string& corpusVersion, double fs, const OperatingBands& c,
                        std::string* error = nullptr);
    std::optional<OperatingBands> loadCorrection(std::uint64_t planHash, const std::string& corpusVersion, double fs) const;

    bool saveSelectorState(const nlohmann::json& state, const std::string& corpusVersion, std::string* error = nullptr);
    std::optional<nlohmann::json> loadSelectorState(const std::string& corpusVersion) const;

private:
    std::optional<nlohmann::json> readJson(const std::filesystem::path& p, std::string* error) const;
    bool writeJson(const std::filesystem::path& p, const nlohmann::json& j, std::string* error);
    std::filesystem::path dir_;
};

// PRESETS.md §10 first-launch default (office / balanced / stereo, Continuous).
nlohmann::json factoryDefaultPreset();

// A preset document is valid when it composes into a runnable plan.
bool validatePresetDoc(const DataSet& ds, const nlohmann::json& doc, const CorpusSummary& corpus,
                       std::string* error = nullptr);

struct StartupConfig {
    nlohmann::json preset;
    std::string source;                 // "explicit" | "session" | "lkg" | "factory"
    std::vector<std::string> problems;  // why earlier candidates were rejected
    bool lkgRestored = false;
};

// Explicit preset (if given), else session.json, else last_known_good.json, else the factory
// default; the first one that validates wins.
StartupConfig resolveStartupConfig(const DataSet& ds, const SessionStore& store, const CorpusSummary& corpus,
                                   const std::optional<nlohmann::json>& explicitPreset);

}  // namespace bf::rt
