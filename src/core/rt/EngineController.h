#pragma once
// Engine Control (docs/RELIABILITY.md §1-§6, REALTIME_ARCHITECTURE.md §2, §3, §10).
//
// Owns the control thread ("bf.control"), the watchdog thread ("bf.watchdog", 1 Hz), the
// logger (unless an external one is given) and the RealtimeEngine. All state changes happen on
// the control thread through the RELIABILITY §1.3 transition table; commands from other
// threads are queued (mutex-protected, never touched by RT) and return whether the transition
// they request is legal (IllegalTransition: rejected, logged at WARN, state unchanged).
//
//  start()      STOPPED -> PREPARING -> READY -> (play, unless prepareOnly) -> STARTING -> RUNNING
//  stop()       READY/STARTING/RUNNING/DEGRADED -> STOPPING (300 ms fade) -> STOPPED;
//               PREPARING -> STOPPED (abort); DEVICE_LOST -> STOPPED
//  play()       READY -> STARTING (500 ms fade-in) -> RUNNING
//  reconnect()  DEVICE_LOST -> PREPARING (same device only)
//  reset()      ERROR -> STOPPED
//  setPreset()  validated; Strength / mix / limiter ceiling applied live, anything else
//               rebuilds (STOPPING -> PREPARING)
//
// Device handling: the engine opens only cfg.deviceId and never switches devices. Device lost
// (event or no callback for 2 s) -> DEVICE_LOST; the same device returning with
// autoReconnectSameDevice -> PREPARING. A device-initiated sample-rate change rebuilds at the
// new rate (STOPPING -> PREPARING). Babble runs at 44.1 / 48 / 88.2 / 96 kHz (per-event preload
// resampling from the 48 kHz corpus); at any other rate a babble plan runs stationary-only with
// DEGRADED `engine.rateUnsupported` (Strict / LaboratoryMask: ERROR).
//
// Fallback policy (RELIABILITY §3-4) through MaskStrategy::degrade(): at prepare for the corpus
// (reduced plan / stationary / ERROR), at run time for source failures (Continuous: substitute
// and DEGRADED corpus.reduced above 10 % failed pool recordings; Safe: switch to stationary;
// Strict: ERROR). DEGRADED reasons clear with a 30 s hysteresis.
//
// Persistence (PRESETS §9): session.json every 30 s if changed and on stop, last_known_good.json
// after 60 s RUNNING/DEGRADED without ERROR, correction_memory.json every 10 min and on stop,
// selector_state.json every 60 s and on stop. Startup: explicit preset / session.json /
// last_known_good.json / factory default (RELIABILITY §2 "Invalid config at startup").
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config/DataSet.h"
#include "core/rt/AudioBackend.h"
#include "core/rt/Diagnostics.h"
#include "core/rt/EngineState.h"
#include "core/rt/Logging.h"
#include "core/rt/Persistence.h"
#include "core/rt/RealtimeEngine.h"
#include "core/strategy/MaskStrategy.h"

namespace bf::rt {

struct EngineControllerConfig {
    const DataSet* dataSet = nullptr;
    std::shared_ptr<const CorpusSnapshot> corpus;  // null: no voice library
    IAudioSource* audio = nullptr;
    bool audioThreadSafe = false;
    IAudioBackend* backend = nullptr;
    std::string deviceId;
    double sampleRate = 48000.0;
    int bufferFrames = 512;
    bool autoReconnectSameDevice = true;
    bool prepareOnly = false;
    std::optional<std::uint64_t> seed;       // else random (logged)
    std::optional<nlohmann::json> preset;    // explicit preset, else session / LKG / factory
    std::filesystem::path stateDir;          // persistence directory (empty: off)
    Logger* logger = nullptr;                // external logger, else one from `log`
    LoggerConfig log;
    RealtimeEngineConfig engine;             // tuning; corpus / audio / seed / logger are filled in
    bool redactPathsInExports = true;
    // Timing (defaults per RELIABILITY / PRESETS; tests shorten them).
    double degradedHysteresisS = 30.0;
    double sessionSaveS = 30.0, lkgPromoteS = 60.0, correctionSaveS = 600.0, selectorSaveS = 60.0;
    double watchdogPeriodS = 1.0;
    double callbackStallS = 2.0, analysisStallS = 10.0, plannerStallS = 5.0, preloadQueueAgeS = 5.0;
    int controlTickMs = 10;
    double commandTimeoutS = 10.0;
};

class EngineController {
public:
    explicit EngineController(EngineControllerConfig cfg);
    ~EngineController();
    EngineController(const EngineController&) = delete;
    EngineController& operator=(const EngineController&) = delete;

    // Commands (any non-RT thread).
    CommandResult start();
    CommandResult stop();
    CommandResult play();
    CommandResult reconnect();
    CommandResult reset();
    CommandResult setPreset(const nlohmann::json& presetDoc);  // "setPlan"
    CommandResult setStrength(double db);

    // Device events (backend handler; any non-RT thread).
    void onDeviceEvent(const DeviceEvent& e);

    // Observation (any thread).
    EngineState state() const noexcept { return state_.load(std::memory_order_acquire); }
    std::uint32_t degradedReasons() const noexcept { return reasons_.load(std::memory_order_acquire); }
    std::optional<EngineStatusEvent> pollStatus();  // coalesced latest status (UI timer)
    std::vector<EngineStatusEvent> statusHistory() const;
    bool waitForState(EngineState s, double timeoutS) const;
    nlohmann::json diagnostics() const;
    nlohmann::json currentPreset() const;
    MaskRenderPlan currentPlan() const;
    std::uint64_t sessionSeed() const noexcept { return seed_; }
    const StartupConfig& startupConfig() const noexcept { return startup_; }
    Logger& logger() noexcept { return *log_; }
    RtMetrics metrics() const;
    MaskStatistics statistics() const;
    double sampleRate() const noexcept { return fs_.load(std::memory_order_acquire); }
    const SessionStore& store() const noexcept { return store_; }
    const EngineControllerConfig& config() const noexcept { return cfg_; }

private:
    struct FallbackOutcome {
        MaskRenderPlan plan;
        std::uint32_t bits = 0;
        std::string errorCode;
        std::string message;
    };

    void controlLoop();
    void watchdogLoop();
    void post(std::function<void()> task);
    CommandResult rejectCommand(const char* command, EngineState target);
    CommandResult call(std::function<CommandResult()> fn);
    bool go(EngineState to, const std::string& reason, const std::string& detail = {});
    void pushStatus(EngineState prev, EngineState now, const std::string& detail, std::vector<std::string> newCodes);

    void doPrepare();
    void doPlay();
    void beginStop(bool rebuild, const std::string& reason, bool immediate);
    void finishStop();
    void releaseEngine();
    void enterError(const std::string& code, const std::string& msg);
    void deviceFailure(const std::string& reason);
    void periodic();
    void watchdogCheck();
    void evaluateDegraded(double nowS);
    void handleSourceFailures(const RtMetrics& m, double nowS);
    void persist(bool force);
    FallbackOutcome applyFallback(const MaskRenderPlan& plan, double fs) const;
    int availableSpeakers() const;
    CorpusSummary corpusSummary() const;
    double nowS() const;
    std::unique_ptr<MaskStrategy> strategyFor(const MaskRenderPlan& p) const;

    EngineControllerConfig cfg_;
    std::unique_ptr<Logger> ownLog_;
    Logger* log_ = nullptr;
    SessionStore store_;
    StartupConfig startup_;
    std::uint64_t seed_ = 1;
    std::chrono::steady_clock::time_point t0_;

    // Control-thread state.
    EngineStateMachine sm_;
    DegradedTracker tracker_;
    nlohmann::json preset_;
    MaskRenderPlan basePlan_, plan_;
    OutputLayout layout_;
    bool havePlan_ = false;
    bool rebuildPending_ = false, stoppedByAbort_ = false, safeFallback_ = false;
    double stopDeadlineS_ = 0.0;
    double runningSinceS_ = -1.0;
    bool lkgPromoted_ = false, sessionDirty_ = true;
    double lastSessionSaveS_ = 0.0, lastCorrectionSaveS_ = 0.0, lastSelectorSaveS_ = 0.0;
    std::uint64_t lastCallbacks_ = 0;
    double lastCallbackAdvanceS_ = 0.0;
    std::uint64_t handledFailures_ = 0;
    std::uint32_t lastReasons_ = 0;
    std::deque<std::pair<double, std::uint64_t>> starveHist_, xrunHist_;
    std::uint64_t lastClip_ = 0;
    bool plannerStallLogged_ = false, analysisStallLogged_ = false, preloadStallLogged_ = false,
         loggerStallLogged_ = false;
    std::string deviceType_;
    int bufferFrames_ = 0, deviceOutputs_ = 0;
    std::uint64_t planEpoch_ = 0;

    // Engine (created / destroyed on the control thread; readers lock engineMutex_).
    mutable CheckedMutex engineMutex_;
    std::unique_ptr<RealtimeEngine> engine_;

    // Shared observation state.
    std::atomic<EngineState> state_{EngineState::Stopped};
    std::atomic<std::uint32_t> reasons_{0};
    std::atomic<double> fs_{0.0};
    std::atomic<bool> abort_{false};
    std::atomic<std::int64_t> enginePos_{0};
    mutable CheckedMutex statusMutex_;
    mutable std::condition_variable_any statusCv_;
    std::vector<EngineStatusEvent> history_;
    std::optional<EngineStatusEvent> pendingStatus_;
    std::uint64_t statusSeq_ = 0;
    mutable CheckedMutex presetMutex_;  // preset_/plan_ copies for readers
    nlohmann::json presetCopy_;
    MaskRenderPlan planCopy_;
    OutputLayout layoutCopy_;
    int bufferFramesCopy_ = 0, deviceOutputsCopy_ = 0;
    std::uint64_t planEpochCopy_ = 0;

    // Queue / threads.
    CheckedMutex qMutex_;
    std::condition_variable_any qCv_;
    std::deque<std::function<void()>> queue_;
    std::atomic<bool> quit_{false};
    std::thread control_, watchdog_;
    std::thread::id controlId_;
    double lastCpuWallS_ = 0.0, lastCpuS_ = 0.0;
    mutable std::atomic<double> cpuPct_{0.0};
};

}  // namespace bf::rt
