#pragma once
// Deterministic offline renderer (docs/VALIDATION.md §2.1, §2.3; MASK_STRATEGIES.md §8;
// REALTIME_ARCHITECTURE.md §8.4-8.5). Shared by `bfrender` and the tests.
//
// renderScenario() runs the MaskEngine synchronously (analysis stepped at its nominal cadence)
// over the scenario duration, applying scenario events at their sample. The engine latency
// is rendered and dropped, so out[0] corresponds to engine time 0 of the masker.
//
// Laboratory mode (scenario "laboratory" block):
//  - Strict fallback: any source failure aborts with exit code 3 (no fallback);
//  - limiter (and safety clip) off unless requested; true peak is reported;
//  - spectrumMatch "strict": a 120 s pre-roll converges the babble correction with
//    tau_c = 20 s, which is then frozen for the render;
//  - rmsNormalization "two-pass": pass 1 renders and measures the whole duration, pass 2
//    applies one static gain so that the RMS (energy mean over channels) equals
//    L_ref + Strength within +-0.01 dB;
//  - continuousN speaker sets are nested across N (prefix of the "lab.speakerset" draw).
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config/DataSet.h"
#include "core/corpus/CorpusSnapshot.h"
#include "core/engine/MaskEngine.h"
#include "core/engine/Scenario.h"

namespace bf {

struct CorpusHandle {
    std::shared_ptr<const CorpusSnapshot> snapshot;
    IAudioSource* audio = nullptr;
};

struct RenderOptions {
    int blockSize = 480;          // host block size (the output does not depend on it)
    bool noLimiter = false;       // --no-limiter: no limiter and no safety clip
    std::vector<int> taps;        // taps to capture: 0..3 = T1..T4
    double strictPrerollS = 120.0;
};

enum RenderExit : int { kRenderOk = 0, kRenderIoError = 1, kRenderConfigError = 2, kRenderSourceFailure = 3 };

struct RenderResult {
    bool ok = false;
    int exitCode = kRenderOk;
    std::string error;
    double fs = 48000.0;
    int numChannels = 0;
    std::vector<std::vector<float>> audio;                   // planar, duration frames
    std::map<int, std::vector<std::vector<float>>> taps;     // tap -> planar (engine time 0..)
    MaskStatistics stats;
    nlohmann::json events;        // babbleforge.events/1
    nlohmann::json planHistory;
    nlohmann::json laboratory;    // lab report (speakers, two-pass, strict correction)
    std::vector<std::string> warnings;
    std::string corpusVersion;
    double targetRmsDb = -26.0;   // L_ref + Strength of the initial plan
    std::string targetId;         // spectrum target of the initial plan
    std::optional<ThirdOctArray> referenceDb;  // its ideal band levels (incl. LF/HF limits)
};

RenderResult renderScenario(const DataSet& ds, const Scenario& sc, const CorpusHandle& corpus,
                            const RenderOptions& opt = {});

// SHA-256 over the interleaved little-endian float32 sample data (the WAV data chunk).
std::string audioSha256(const std::vector<std::vector<float>>& planar);

// Writes <wav>, <stem>.events.json, <stem>.sidecar.json and <stem>.T<k>.wav for captured taps.
// Returns the sidecar document through `sidecar` (optional).
bool writeRenderOutputs(const RenderResult& r, const Scenario& sc, const DataSet& ds,
                        const std::filesystem::path& wavPath, nlohmann::json* sidecar = nullptr,
                        std::string* error = nullptr, const std::filesystem::path& eventsPath = {});

// Experiment matrix (VALIDATION.md §2.3): renders the Cartesian product of the axes into
// outDir (cell_NNN.wav + sidecars) and writes matrix_manifest.json. Returns the worst exit code.
int runMatrix(const DataSet& ds, const nlohmann::json& matrix, const std::filesystem::path& baseDir,
              const CorpusHandle& corpus, const std::filesystem::path& outDir, const RenderOptions& opt,
              std::string* error = nullptr, nlohmann::json* manifestOut = nullptr);

}  // namespace bf
