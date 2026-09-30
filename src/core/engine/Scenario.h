#pragma once
// Research scenario (deterministic render) documents (docs/PRESETS.md §6.3) and plan building
// for the offline renderer, including the LaboratoryMask block (MASK_STRATEGIES.md §8).
//
//   { "schema": "babbleforge.scenario", "schemaVersion": "1.0",
//     "preset": { ...inline preset... } | "path/to/preset.json",
//     "corpusVersion": "...", "seed": 182731, "durationS": 300, "sampleRate": 48000,
//     "outputFormat": { "container": "wav", "sampleFormat": "float32" },
//     "events": [ { "atS": 120.0, "set": { "macros.character": 0.9 } } ],
//     "laboratory": { "mode": "continuousN", "talkers": 8, "maxGapMs": 100,
//                     "rmsNormalization": "two-pass", "spectrumMatch": "strict",
//                     "babbleFraction": 1.0, "limiter": false } }
//
// Event "set" keys are dotted paths into the preset document (e.g. "macros.mix.babbleFraction",
// "strategy", "area"); every event re-composes the plan, which takes effect at round(atS * fs).
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config/DataSet.h"
#include "core/config/Types.h"
#include "core/spatial/OutputLayout.h"
#include "core/strategy/StrategyTypes.h"

namespace bf {

struct LabSpec {
    LabSubMode mode = LabSubMode::ContinuousN;
    int talkers = 4;
    std::optional<double> maxGapMs;
    std::optional<double> babbleFraction;  // hybrid sub-mode / explicit b
    bool twoPassRms = true;                // "rmsNormalization": "two-pass" | "none"
    bool strictSpectrum = false;           // "spectrumMatch": "strict" | "none" (ltassMatchedBabble: strict)
    bool limiter = false;                  // research renders: limiter off unless requested
};

struct ScenarioEvent {
    double atS = 0.0;
    nlohmann::json set;  // object: dotted preset path -> value
};

struct Scenario {
    nlohmann::json doc;     // the scenario document with the preset inlined
    nlohmann::json preset;  // preset document
    std::uint64_t seed = 1;
    bool seedGiven = false;
    double durationS = 60.0;
    double sampleRate = 48000.0;
    std::string corpusVersion;  // expected corpus version (informational)
    std::vector<ScenarioEvent> events;  // sorted by atS
    std::optional<LabSpec> lab;
};

std::optional<LabSubMode> labSubModeFromString(const std::string& s) noexcept;

// Sets doc[a][b][c] = v for path "a.b.c" (intermediate objects are created).
void setDottedPath(nlohmann::json& doc, const std::string& path, const nlohmann::json& v);

bool parseScenario(const nlohmann::json& doc, const std::filesystem::path& baseDir, Scenario& out,
                   std::string* error = nullptr);
bool loadScenarioFile(const std::filesystem::path& file, Scenario& out, std::string* error = nullptr);

// Output layout of a preset: explicit channel list, else the named layout (mono, stereo,
// ring4, ring6, ring8); default stereo.
OutputLayout layoutFromPreset(const Preset& preset);

struct ScenarioPlan {
    bool ok = false;
    std::string error;
    Preset preset;
    OutputLayout layout;
    MaskRenderPlan plan;
    std::vector<std::string> conflicts;
};

// compose + validate + buildPlan (PRESETS.md §2) for a preset document; with a LaboratoryMask
// spec the strategy is forced to "research" and the sub-mode parameters are applied.
ScenarioPlan buildScenarioPlan(const DataSet& ds, const nlohmann::json& presetDoc, const std::optional<LabSpec>& lab,
                               const CorpusSummary& corpus, std::uint64_t seed);

}  // namespace bf
