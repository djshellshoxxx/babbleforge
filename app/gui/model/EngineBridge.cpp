#include "model/EngineBridge.h"

#include <chrono>

#include "backend/JuceAudioBackend.h"
#include "core/config/Preset.h"
#include "core/engine/MaskEngine.h"
#include "core/engine/Scenario.h"
#include "core/strategy/PlanComposer.h"

namespace bf::gui {

using rt::EngineState;

namespace {

nlohmann::json toJson(const Preset& p) { return nlohmann::json::parse(serializePreset(p)); }

bool isMaskingState(EngineState s) {
    return s == EngineState::Preparing || s == EngineState::Ready || s == EngineState::Starting ||
           s == EngineState::Running || s == EngineState::Degraded;
}

}  // namespace

EngineBridge::EngineBridge(Options o) : opt_(std::move(o)), debounce_([this] { flushPush(); }) {
    if (opt_.backend) {
        backend_ = opt_.backend;
    } else {
        ownBackend_ = std::make_unique<rt::JuceAudioBackend>();
        backend_ = ownBackend_.get();
    }
    deviceIdShared_ = opt_.deviceId;
    createController(opt_.deviceId);
    if (auto c = controller()) {
        startupPreset_ = c->startupConfig().preset;
        startupSource_ = c->startupConfig().source;
    }
    worker_ = std::thread([this] { workerLoop(); });
    status_ = std::thread([this] { statusLoop(); });
    startTimerHz(opt_.uiHz);
}

EngineBridge::~EngineBridge() {
    alive_->store(false);
    stopTimer();
    debounce_.stopTimer();
    if (state_) state_->removeListener(this);
    {
        std::lock_guard<std::mutex> lk(qMutex_);
        quit_ = true;
    }
    qCv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (status_.joinable()) status_.join();
    destroyController();
}

Preset EngineBridge::startupPreset() const {
    if (!startupPreset_.is_null()) {
        nlohmann::json d = startupPreset_;
        if (!d.contains("schema")) d["schema"] = "babbleforge.preset";
        if (!d.contains("schemaVersion")) d["schemaVersion"] = "1.0";
        const PresetParseResult r = parsePreset(d.dump());
        if (r.ok) return r.preset;
    }
    return AppState::factoryPreset(*opt_.dataSet, "office", "balanced");
}

std::string EngineBridge::startupSource() const { return startupSource_; }

void EngineBridge::attach(AppState& state) {
    if (state_) state_->removeListener(this);
    state_ = &state;
    state_->addListener(this);
    queuedRevision_ = state_->revision();  // the controller already holds the startup preset
}

std::shared_ptr<rt::EngineController> EngineBridge::controller() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return ctl_;
}

void EngineBridge::createController(const std::string& deviceId) {
    rt::EngineControllerConfig cfg;
    cfg.dataSet = opt_.dataSet;
    cfg.corpus = opt_.corpus;
    cfg.audio = opt_.audio;
    cfg.audioThreadSafe = opt_.audioThreadSafe;
    cfg.backend = backend_;
    cfg.deviceId = deviceId;
    cfg.stateDir = opt_.stateDir;
    cfg.log = opt_.log;
    if (cfg.log.dir.empty() && !opt_.stateDir.empty()) cfg.log.dir = opt_.stateDir / "logs";
    cfg.redactPathsInExports = opt_.redactPathsInExports;
    {
        std::lock_guard<std::mutex> lk(snapMutex_);
        if (lastPushed_) cfg.preset = toJson(*lastPushed_);
    }
    if (!cfg.preset && opt_.preset) cfg.preset = opt_.preset;
    auto c = std::make_shared<rt::EngineController>(cfg);
    std::weak_ptr<rt::EngineController> weak = c;
    backend_->setDeviceEventHandler([weak](const rt::DeviceEvent& e) {
        if (auto p = weak.lock()) p->onDeviceEvent(e);
    });
    std::lock_guard<std::mutex> lk(ctlMutex_);
    ctl_ = std::move(c);
}

void EngineBridge::destroyController() {
    std::shared_ptr<rt::EngineController> old;
    {
        std::lock_guard<std::mutex> lk(ctlMutex_);
        old = std::move(ctl_);
    }
    if (!old) return;
    backend_->setDeviceEventHandler(nullptr);
    if (old->state() != EngineState::Stopped) {
        old->stop();
        old->waitForState(EngineState::Stopped, 3.0);
    }
    // A device-event call in flight may still hold a reference: let it finish so that the
    // controller is never destroyed on an audio-device thread.
    for (int i = 0; i < 200 && old.use_count() > 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    old.reset();
}

// ---- commands -----------------------------------------------------------------------------

void EngineBridge::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(qMutex_);
        queue_.push_back(std::move(fn));
    }
    qCv_.notify_all();
}

std::string EngineBridge::ensureDevice() {
    {
        std::lock_guard<std::mutex> lk(snapMutex_);
        if (!deviceIdShared_.empty()) return deviceIdShared_;
    }
    // No device chosen yet: use the first device that has outputs (explicit, once). Device
    // loss never switches devices (GUI §58).
    const auto devs = backend_->devices();
    std::string id;
    for (const auto& d : devs)
        if (d.numOutputs > 0) {
            id = d.id;
            break;
        }
    if (id.empty() && !devs.empty()) id = devs.front().id;
    {
        std::lock_guard<std::mutex> lk(snapMutex_);
        devices_ = devs;
        deviceIdShared_ = id;
    }
    if (!id.empty()) {
        destroyController();
        createController(id);
    }
    return id;
}

void EngineBridge::start() {
    testAutoStarted_ = false;  // an explicit Start keeps masking after a test
    flushPush();
    post([this] {
        if (ensureDevice().empty()) {
            std::lock_guard<std::mutex> lk(snapMutex_);
            pushError_ = "No output device available";
            return;
        }
        auto c = controller();
        if (!c) return;
        switch (c->state()) {
        case EngineState::Stopped: c->start(); break;
        case EngineState::Ready: c->play(); break;
        case EngineState::DeviceLost: c->reconnect(); break;
        case EngineState::Error:
            c->reset();
            c->start();
            break;
        default: break;
        }
    });
}

void EngineBridge::stop() {
    post([this] {
        if (auto c = controller()) c->stop();
    });
}

void EngineBridge::toggleStartStop() {
    const EngineState s = ui_.state;
    if (isMaskingState(s) || s == EngineState::DeviceLost) stop();
    else if (s != EngineState::Stopping) start();
}

void EngineBridge::reconnect() {
    post([this] {
        if (auto c = controller()) c->reconnect();
    });
}

void EngineBridge::reset() {
    post([this] {
        if (auto c = controller()) c->reset();
    });
}

void EngineBridge::setDevice(const std::string& id) {
    flushPush();
    post([this, id] {
        {
            std::lock_guard<std::mutex> lk(snapMutex_);
            if (id == deviceIdShared_ && controller()) return;
            deviceIdShared_ = id;
        }
        auto c = controller();
        const bool wasMasking = c && isMaskingState(c->state());
        destroyController();
        createController(id);
        if (wasMasking)
            if (auto n = controller()) n->start();
    });
}

void EngineBridge::refreshDevices() {
    post([this] {
        auto devs = backend_->devices();
        std::lock_guard<std::mutex> lk(snapMutex_);
        devices_ = std::move(devs);
    });
}

void EngineBridge::testSpeakers(bool on) {
    post([this, on] {
        auto c = controller();
        if (!on) {
            if (c) c->testSpeakers(false);
            return;
        }
        if (ensureDevice().empty()) return;
        c = controller();
        if (!c) return;
        const EngineState st = c->state();
        if (st == EngineState::Stopped) {
            c->start();
            testAutoStarted_ = true;
            if (!c->waitForState(EngineState::Running, 20.0) && c->state() != EngineState::Degraded) {
                testAutoStarted_ = false;
                return;
            }
        } else if (!isMaskingState(st)) {
            return;  // device lost / error: nothing to test
        }
        if (c->testSpeakers(true) != rt::CommandResult::Ok) testAutoStarted_ = false;
    });
}

std::vector<rt::AudioDeviceInfo> EngineBridge::deviceList() const {
    std::lock_guard<std::mutex> lk(snapMutex_);
    return devices_;
}

std::string EngineBridge::deviceId() const {
    std::lock_guard<std::mutex> lk(snapMutex_);
    return deviceIdShared_;
}

// ---- plan pushes ----------------------------------------------------------------------------

void EngineBridge::appStateChanged(unsigned changes) {
    if (changes & AppState::kPresetChanged) schedulePush();
}

void EngineBridge::schedulePush() { debounce_.startTimer(opt_.debounceMs); }

void EngineBridge::flushPush() {
    debounce_.stopTimer();
    if (!state_ || state_->revision() == queuedRevision_) return;
    queuedRevision_ = state_->revision();
    {
        std::lock_guard<std::mutex> lk(qMutex_);
        pendingPreset_ = state_->preset();
    }
    qCv_.notify_all();
}

void EngineBridge::doPush(const Preset& p) {
    // Compose here (worker) to validate and to log adjustments; the controller rebuilds the
    // plan itself from the preset document.
    const ComposedPlan cp = composePlan(*opt_.dataSet, p, CorpusSummary{}, layoutFromPreset(p));
    std::string err;
    if (!cp.ok) {
        err = cp.error;
    } else if (auto c = controller()) {
        const rt::CommandResult r = c->setPreset(toJson(p));
        if (r != rt::CommandResult::Ok) err = "setPreset: " + std::string(rt::toString(r));
    }
    std::lock_guard<std::mutex> lk(snapMutex_);
    pushError_ = err;
    if (err.empty()) {
        lastPushed_ = p;
        pushes_.fetch_add(1);
    }
}

std::optional<Preset> EngineBridge::lastPushedPreset() const {
    std::lock_guard<std::mutex> lk(snapMutex_);
    return lastPushed_;
}

void EngineBridge::workerLoop() {
    for (;;) {
        std::function<void()> job;
        std::optional<Preset> p;
        {
            std::unique_lock<std::mutex> lk(qMutex_);
            qCv_.wait(lk, [this] { return quit_.load() || !queue_.empty() || pendingPreset_.has_value(); });
            if (quit_) return;
            if (pendingPreset_) {
                p = std::move(pendingPreset_);
                pendingPreset_.reset();
            } else {
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            workerBusy_ = true;
        }
        try {
            if (p) doPush(*p);
            else if (job) job();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(snapMutex_);
            pushError_ = e.what();
        }
        std::lock_guard<std::mutex> lk(qMutex_);
        workerBusy_ = false;
    }
}

bool EngineBridge::waitIdle(int timeoutMs) {
    flushPush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lk(qMutex_);
            if (queue_.empty() && !pendingPreset_ && !workerBusy_) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// ---- status -----------------------------------------------------------------------------------

void EngineBridge::sample() {
    auto c = controller();
    EngineStatus s;
    if (c) {
        while (auto ev = c->pollStatus()) {
            s.statusSeq = ev->seq;
            s.detailCode = ev->detailCode;
        }
        s.state = c->state();
        s.degradedReasons = c->degradedReasons();
        const rt::RtMetrics m = c->metrics();
        s.cpuPct = m.dspLoadP99 * 100.0;
        s.xruns = m.xruns;
        s.babble = m.babble;
        s.sampleRate = c->sampleRate() > 0.0 ? c->sampleRate() : c->config().sampleRate;
        s.bufferFrames = c->config().bufferFrames;
        const MaskRenderPlan plan = c->currentPlan();
        s.outputs = plan.spatial.activeOutputs;
        s.limiterEnabled = plan.level.limiterEnabled;
        s.voicesActive = plan.babbleEnabled ? plan.talkers.targetMean() : 0.0;
        const auto ts = c->testSpeakersState();
        s.out.testRunning = ts.running;
        s.out.testElapsedS = ts.elapsedS;
        s.out.testOutputs = ts.outputs;
        if (testWasRunning_.exchange(ts.running) && !ts.running && testAutoStarted_.exchange(false))
            post([this] {  // the test ended on its own: return to the state before it
                if (auto cc = controller()) cc->stop();
            });
        if (isMaskingState(s.state)) {
            const MaskStatistics st = c->statistics();
            if (st.samples > 0) {
                s.out.rmsFastDb = st.rmsFastChDb;
                s.out.truePeakDb = st.truePeakChDb;
                s.out.rmsDb = st.rmsFastAllDb;
                s.out.lufsS = st.lufsS;
                s.out.truePeakDbMax = st.truePeak10sDb;
                s.out.leq60Db = st.leq60Db;
                s.out.limiterGrMaxDb = st.limiterGrMaxDb;
                s.out.limiterAbove05 = st.limiterAbove05Fraction;
                if (st.fs > 0.0) s.out.latencyMs = 1000.0 * (st.latencySamples + s.bufferFrames) / st.fs;
                s.haveStats = true;
                s.stats = std::make_shared<const MaskStatistics>(st);
                if (st.babbleActive) s.voicesActive = st.meanActive;
                s.outputLevelDb = st.lufsS > -150.0 ? st.lufsS : st.outputRmsDb;
            }
        }
    }
    std::lock_guard<std::mutex> lk(snapMutex_);
    if (s.statusSeq == 0) {
        s.statusSeq = snap_.statusSeq;
        s.detailCode = snap_.detailCode;
    }
    s.deviceId = deviceIdShared_;
    const auto colon = s.deviceId.find(':');
    s.driver = colon == std::string::npos ? std::string() : s.deviceId.substr(0, colon);
    s.deviceName = colon == std::string::npos ? s.deviceId : s.deviceId.substr(colon + 1);
    s.pushes = pushes_.load();
    s.lastPushError = pushError_;
    snap_ = std::move(s);
}

void EngineBridge::statusLoop() {
    while (!quit_.load()) {
        sample();
        for (int i = 0; i < opt_.statusPollMs / 10 && !quit_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void EngineBridge::pollNow() {
    sample();
    timerCallback();
}

void EngineBridge::timerCallback() {
    {
        std::lock_guard<std::mutex> lk(snapMutex_);
        ui_ = snap_;
    }
    const double now = juce::Time::getMillisecondCounterHiRes();
    const EngineState s = ui_.state;
    if (s == EngineState::Starting || s == EngineState::Running || s == EngineState::Degraded) {
        if (maskingSinceMs_ < 0) maskingSinceMs_ = now;
    } else if (s == EngineState::Stopped || s == EngineState::Error) {
        maskingSinceMs_ = -1.0;
    }
    listeners_.call([this](Listener& l) { l.engineStatusChanged(ui_); });
}

bool EngineBridge::masking() const noexcept { return isMaskingState(ui_.state); }

double EngineBridge::elapsedSeconds() const {
    if (maskingSinceMs_ < 0) return 0.0;
    return (juce::Time::getMillisecondCounterHiRes() - maskingSinceMs_) / 1000.0;
}

// ---- texts ---------------------------------------------------------------------------------

juce::String statusHeadline(const EngineStatus& s) {
    switch (s.state) {
    case EngineState::Stopped: return "READY";
    case EngineState::Preparing:
    case EngineState::Ready:
    case EngineState::Starting: return "STARTING";
    case EngineState::Running:
    case EngineState::Degraded: return "MASKING ACTIVE";
    case EngineState::DeviceLost: return "OUTPUT DEVICE LOST";
    case EngineState::Stopping: return "STOPPING";
    case EngineState::Error: return "ERROR";
    }
    return {};
}

juce::String statusSubline(const EngineStatus& s) {
    if (s.state == EngineState::Degraded) {
        const std::uint32_t corpusBits = rt::kDegCorpusReduced | rt::kDegCorpusInsufficient | rt::kDegCorpusNone;
        if (s.degradedReasons & corpusBits) return "Reduced Voice Library";
        if (s.degradedReasons & rt::kDegFallbackStationary) return "Steady masking only";
        if (s.degradedReasons & rt::kDegAudioXruns) return "Audio dropouts";
        return "DEGRADED";
    }
    if (s.state == EngineState::DeviceLost) return "Waiting for the output device";
    if (s.state == EngineState::Error)
        return s.detailCode.empty() ? juce::String("Masking stopped") : juce::String(s.detailCode);
    if (s.state == EngineState::Stopped && !s.lastPushError.empty()) return juce::String(s.lastPushError);
    return {};
}

juce::String formatElapsed(double seconds) {
    const auto t = static_cast<long long>(seconds < 0 ? 0 : seconds);
    return juce::String::formatted("%02lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
}

}  // namespace bf::gui
