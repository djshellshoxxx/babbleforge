#pragma once
// Diagnostic snapshot (docs/RELIABILITY.md §6.1) and health model (§6.2).
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/engine/MaskEngine.h"
#include "core/rt/EngineState.h"
#include "core/rt/RealtimeEngine.h"

namespace bf::rt {

enum class Health : std::uint8_t { Good, Warning, Bad };
std::string_view toString(Health h) noexcept;

enum class LimiterHealth : std::uint8_t { Inactive, Active, Sustained };  // Active: > 1 % of the time
enum class CorpusHealth : std::uint8_t { Healthy, Reduced, Insufficient };

struct HealthReport {
    Health dspLoad = Health::Good, preload = Health::Good, xruns = Health::Good, limiter = Health::Good,
           corpus = Health::Good;
    bool error = false;
    bool outputHealthy() const noexcept;  // no Bad indicator and no ERROR
};

// §6.2 thresholds: DSP load p99 < 50 % / 50-80 % / > 80 %; preload min buffered > 3 s / 1-3 s /
// < 1 s; xruns per minute 0 / 1-10 / > 10; limiter inactive / active > 1 % / sustained;
// corpus healthy / reduced / insufficient or none.
HealthReport evaluateHealth(double dspLoadP99, double preloadMinBufferedS, double xrunsPerMinute, LimiterHealth limiter,
                            CorpusHealth corpus, bool errorState);
nlohmann::json toJson(const HealthReport& h);

struct OutputInfo {
    int index = 0;
    std::string label;
    bool enabled = true, mute = false;
    int zone = 0;
    double gainDb = 0.0;
};

// Everything the snapshot reports; filled by the EngineController.
struct DiagnosticsInput {
    std::string appVersion, build;
    EngineState state = EngineState::Stopped;
    std::uint32_t degradedReasons = 0;
    double uptimeS = 0.0;
    std::uint64_t sessionSeed = 0;
    std::string corpusVersion, dataSetHash;
    std::uint64_t planEpoch = 0;
    // audio
    std::string driver, device;
    double sampleRate = 0.0;
    int bufferFrames = 0, outputs = 0;
    double latencyMs = 0.0;
    double processCpuPct = 0.0;
    // plan / corpus
    std::string strategy, target;
    double meanActiveTarget = 0.0;
    int slots = 0, pool = 0;
    int corpusSpeakers = 0;
    double usableSpeechH = 0.0;
    double segmentCooldownEffMin = 0.0;
    bool limiterEnabled = true;
    double limiterCeilingDbtp = -1.0;
    std::vector<OutputInfo> outputsInfo;
    // live
    RtMetrics metrics;
    MaskStatistics stats;
    bool haveStats = false;
    std::uint64_t logDropped = 0;
    double xrunsPerMinute = 0.0;
    LimiterHealth limiterHealth = LimiterHealth::Inactive;
    CorpusHealth corpusHealth = CorpusHealth::Healthy;
    std::vector<nlohmann::json> recentLog;
};

// babbleforge.diagnostics/1. redact: paths / user names in the recent log are redacted
// (redactPathsInExports, RELIABILITY §8).
nlohmann::json buildDiagnosticsSnapshot(const DiagnosticsInput& in, bool redact);

// Keys every snapshot carries (tests / consumers).
const std::vector<std::string>& diagnosticsRequiredPaths();

}  // namespace bf::rt
