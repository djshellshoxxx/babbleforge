#pragma once
// EngineBridge: the only GUI object that talks to the engine (docs/GUI_ARCHITECTURE.md).
//
// Owns the EngineController and (unless one is injected) the JuceAudioBackend. Nothing here
// blocks the message thread:
//  - Preset edits (AppState) are debounced (150 ms) and handed to a worker thread
//    ("bf.gui.engine"), which composes the plan (bf::composePlan, to validate it and report
//    adjustments) and calls EngineController::setPreset() — a blocking command.
//    Only the newest pending preset is pushed (latest wins).
//  - start / stop / reconnect / reset and device changes run on the same worker.
//  - A status thread ("bf.gui.status") samples state, coalesced status events, metrics and
//    statistics; a 30 Hz message-thread timer copies the snapshot and notifies listeners.
//
// Construction order: EngineBridge (creates the controller, which resolves the startup preset
// from session.json / last_known_good.json / factory) -> AppState(startupPreset()) -> attach().
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <juce_events/juce_events.h>
#include <nlohmann/json.hpp>

#include "core/config/DataSet.h"
#include "core/rt/AudioBackend.h"
#include "core/rt/EngineController.h"
#include "model/AppState.h"

namespace bf::gui {

struct EngineStatus {
    rt::EngineState state = rt::EngineState::Stopped;
    std::uint32_t degradedReasons = 0;
    std::string detailCode;
    std::uint64_t statusSeq = 0;  // EngineStatusEvent sequence seen so far
    // Device / audio.
    std::string deviceId, deviceName, driver;
    double sampleRate = 0.0;
    int bufferFrames = 0;
    int outputs = 0;
    double cpuPct = 0.0;  // DSP load p99 (% of the callback budget)
    std::uint64_t xruns = 0;
    bool limiterEnabled = true;
    // Activity.
    bool haveStats = false;
    double voicesActive = 0.0;  // measured mean active talkers (or the plan target)
    double outputLevelDb = -200.0;  // short-term loudness of the output
    bool babble = false;
    // Plan pushes.
    std::uint64_t pushes = 0;
    std::string lastPushError;
    // OUTPUT page telemetry (ENGINE §5 meters on the output, TEST SPEAKERS state).
    struct OutputTelemetry {
        std::vector<double> rmsFastDb, truePeakDb;  // per output channel, dBFS / dBTP
        double rmsDb = -200.0, lufsS = -200.0, truePeakDbMax = -200.0, leq60Db = -200.0;
        double limiterGrMaxDb = 0.0, limiterAbove05 = 0.0, latencyMs = 0.0;
        bool testRunning = false;
        double testElapsedS = 0.0;
        int testOutputs = 0;
    } out;
};

class EngineBridge final : private juce::Timer, private AppState::Listener {
public:
    struct Options {
        const DataSet* dataSet = nullptr;
        std::shared_ptr<const CorpusSnapshot> corpus;
        IAudioSource* audio = nullptr;
        bool audioThreadSafe = false;
        rt::IAudioBackend* backend = nullptr;  // null: the bridge owns a JuceAudioBackend
        std::string deviceId;                  // empty: first device on first start
        std::filesystem::path stateDir;        // persistence (session.json, ...); empty: off
        std::optional<nlohmann::json> preset;  // explicit startup preset
        int debounceMs = 150;
        int uiHz = 30;
        int statusPollMs = 50;
    };

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void engineStatusChanged(const EngineStatus& s) = 0;
    };

    explicit EngineBridge(Options o);
    ~EngineBridge() override;

    // Startup configuration resolved by the controller (as a parsed preset).
    Preset startupPreset() const;
    std::string startupSource() const;
    void attach(AppState& state);  // starts listening to preset edits

    // Commands (message thread; never block).
    void start();
    void stop();
    void toggleStartStop();
    void reconnect();
    void reset();
    void setDevice(const std::string& deviceId);  // stops, recreates the controller on the new device
    void refreshDevices();                          // async; listeners get a status update
    // TEST SPEAKERS / STOP TEST (GUI §23). Starts the engine first when it is stopped and stops
    // it again when the test ends.
    void testSpeakers(bool on);

    // Plan pushes.
    void schedulePush();  // (re)starts the debounce timer
    void flushPush();     // push now if the preset changed since the last push (skips the debounce)
    std::uint64_t pushCount() const noexcept { return pushes_.load(); }
    std::optional<Preset> lastPushedPreset() const;
    bool pushPending() const noexcept { return debounce_.isTimerRunning(); }

    // Status (message thread copy, refreshed at uiHz).
    const EngineStatus& status() const noexcept { return ui_; }
    bool masking() const noexcept;      // STARTING / RUNNING / DEGRADED / PREPARING
    double elapsedSeconds() const;      // since masking started (0 when stopped)
    std::vector<rt::AudioDeviceInfo> deviceList() const;
    std::string deviceId() const;

    // Test helpers: block until the worker queue is empty and no push is pending (the
    // debounce is flushed first). Pump the message loop around this in tests.
    bool waitIdle(int timeoutMs);
    void pollNow();  // copy the status snapshot immediately (normally the 30 Hz timer)

    void addListener(Listener* l) { listeners_.add(l); }
    void removeListener(Listener* l) { listeners_.remove(l); }

    std::shared_ptr<rt::EngineController> controller() const;

private:
    void timerCallback() override;
    void appStateChanged(unsigned changes) override;
    void post(std::function<void()> fn);
    void workerLoop();
    void statusLoop();
    void createController(const std::string& deviceId);
    void destroyController();
    void doPush(const Preset& p);
    std::string ensureDevice();
    void sample();

    Options opt_;
    std::unique_ptr<rt::IAudioBackend> ownBackend_;
    rt::IAudioBackend* backend_ = nullptr;
    AppState* state_ = nullptr;

    mutable std::mutex ctlMutex_;  // guards the pointer only (never held across calls)
    std::shared_ptr<rt::EngineController> ctl_;
    nlohmann::json startupPreset_;
    std::string startupSource_;

    // Worker.
    std::mutex qMutex_;
    std::condition_variable qCv_;
    std::deque<std::function<void()>> queue_;
    std::optional<Preset> pendingPreset_;
    bool workerBusy_ = false;
    std::atomic<bool> quit_{false};
    std::thread worker_, status_;

    // Shared snapshot (status thread -> UI).
    mutable std::mutex snapMutex_;
    EngineStatus snap_;
    std::string deviceIdShared_;
    std::vector<rt::AudioDeviceInfo> devices_;
    std::optional<Preset> lastPushed_;
    std::atomic<std::uint64_t> pushes_{0};
    std::string pushError_;
    std::atomic<bool> testAutoStarted_{false}, testWasRunning_{false};

    // UI state.
    EngineStatus ui_;
    double maskingSinceMs_ = -1.0;
    std::uint64_t queuedRevision_ = ~std::uint64_t{0};  // AppState revision last handed to the worker
    juce::TimedCallback debounce_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    juce::ListenerList<Listener> listeners_;
};

juce::String statusHeadline(const EngineStatus& s);       // RUN page text (GUI §4)
juce::String statusSubline(const EngineStatus& s);        // e.g. "Reduced Voice Library" (GUI §59)
juce::String formatElapsed(double seconds);               // 02:34:18

}  // namespace bf::gui
