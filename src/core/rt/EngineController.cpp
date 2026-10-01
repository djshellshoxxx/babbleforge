#include "core/rt/EngineController.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <future>
#include <random>

#include "BuildInfo.h"
#include "core/Version.h"
#include "core/config/AtomicFile.h"
#include "core/config/Preset.h"
#include "core/engine/Scenario.h"
#include "core/talker/SourcePreparer.h"

namespace bf::rt {

namespace {

CorrectionSpeed speedFrom(const std::optional<std::string>& s) {
    if (s && *s == "slow") return CorrectionSpeed::Slow;
    if (s && *s == "fast") return CorrectionSpeed::Fast;
    return CorrectionSpeed::Normal;
}

bool masking(EngineState s) {
    return s == EngineState::Starting || s == EngineState::Running || s == EngineState::Degraded;
}

// Changed top-level / second-level keys between two preset documents.
std::vector<std::string> presetDiff(const nlohmann::json& a, const nlohmann::json& b) {
    std::vector<std::string> r;
    if (!a.is_object() || !b.is_object()) return {"<document>"};
    std::vector<std::string> keys;
    for (auto it = a.begin(); it != a.end(); ++it) keys.push_back(it.key());
    for (auto it = b.begin(); it != b.end(); ++it)
        if (!a.contains(it.key())) keys.push_back(it.key());
    for (const auto& k : keys) {
        const nlohmann::json va = a.value(k, nlohmann::json()), vb = b.value(k, nlohmann::json());
        if (va == vb) continue;
        if (va.is_object() && vb.is_object()) {
            for (const auto& sub : presetDiff(va, vb)) r.push_back(k + "." + sub);
        } else {
            r.push_back(k);
        }
    }
    return r;
}

}  // namespace

EngineController::EngineController(EngineControllerConfig cfg) : cfg_(std::move(cfg)), store_(cfg_.stateDir) {
    t0_ = std::chrono::steady_clock::now();
    if (cfg_.logger) {
        log_ = cfg_.logger;
    } else {
        ownLog_ = std::make_unique<Logger>(cfg_.log);
        ownLog_->start();
        log_ = ownLog_.get();
    }
    log_->setEngineSampleSource([this] { return enginePos_.load(std::memory_order_relaxed); });
    tracker_.setHysteresis(cfg_.degradedHysteresisS);

    if (cfg_.seed) {
        seed_ = *cfg_.seed;
    } else {
        std::random_device rd;
        seed_ = (static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^
                static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
    }

    sm_.onTransition = [this](EngineState from, EngineState to, std::string_view reason) {
        state_.store(to, std::memory_order_release);
        log_->log(to == EngineState::Error ? LogLevel::Error : LogLevel::Info, logcode::kEngineState,
                  std::string(toString(from)) + " -> " + std::string(toString(to)),
                  {{"from", toString(from)}, {"to", toString(to)}, {"reason", std::string(reason)}});
    };
    sm_.onIllegal = [this](EngineState from, EngineState to, std::string_view reason) {
        log_->log(LogLevel::Warn, logcode::kEngineIllegal,
                  "illegal transition rejected: " + std::string(toString(from)) + " -> " + std::string(toString(to)),
                  {{"from", toString(from)}, {"to", toString(to)}, {"command", std::string(reason)}});
    };

    if (cfg_.corpus)
        log_->log(LogLevel::Info, logcode::kCorpusLoaded, "voice library loaded",
                  {{"corpusVersion", cfg_.corpus->corpusVersion()},
                   {"speakers", cfg_.corpus->numSpeakers()},
                   {"recordings", cfg_.corpus->numRecordings()}});
    if (cfg_.dataSet) {
        startup_ = resolveStartupConfig(*cfg_.dataSet, store_, corpusSummary(), cfg_.preset);
        if (!startup_.problems.empty())
            log_->log(LogLevel::Error, logcode::kConfigInvalid, "configuration rejected at startup",
                      {{"problems", startup_.problems}});
        if (startup_.lkgRestored)
            log_->log(LogLevel::Warn, logcode::kPresetLkgRestored,
                      "Previous settings were invalid; restored last working settings", {{"source", startup_.source}});
        preset_ = startup_.preset;
        log_->log(LogLevel::Info, logcode::kPresetLoad, "preset loaded",
                  {{"source", startup_.source}, {"name", preset_.value("name", "")}});
    } else {
        preset_ = factoryDefaultPreset();
        startup_.preset = preset_;
        startup_.source = "factory";
    }
    {
        std::lock_guard<CheckedMutex> lk(presetMutex_);
        presetCopy_ = preset_;
    }
    if (startup_.lkgRestored) pushStatus(EngineState::Stopped, EngineState::Stopped, logcode::kPresetLkgRestored, {});

    if (cfg_.backend)
        cfg_.backend->setDeviceEventHandler([this](const DeviceEvent& e) { onDeviceEvent(e); });
    control_ = std::thread([this] { controlLoop(); });
    controlId_ = control_.get_id();
    watchdog_ = std::thread([this] { watchdogLoop(); });
}

EngineController::~EngineController() {
    // Orderly shutdown on the control thread: stop audio, persist, release.
    abort_.store(true);  // a prepare in progress ends quickly
    call([this] {
        const EngineState s = sm_.state();
        if (s != EngineState::Stopped && s != EngineState::Error) {
            rebuildPending_ = false;
            if (s == EngineState::Stopping || masking(s) || s == EngineState::Ready) {
                if (s != EngineState::Stopping) sm_.transition(EngineState::Stopping, "shutdown");
                finishStop();
            } else {
                releaseEngine();
                sm_.transition(EngineState::Stopped, "shutdown");
            }
        } else {
            releaseEngine();
        }
        persist(true);
        return CommandResult::Ok;
    });
    quit_.store(true);
    qCv_.notify_all();
    if (watchdog_.joinable()) watchdog_.join();
    if (control_.joinable()) control_.join();
    releaseEngine();  // in case the shutdown command timed out behind a long prepare
    if (cfg_.backend) cfg_.backend->setDeviceEventHandler(nullptr);
    if (ownLog_) ownLog_->stop();
}

double EngineController::nowS() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
}

CorpusSummary EngineController::corpusSummary() const {
    CorpusSummary cs = cfg_.corpus ? CorpusSummary::from(*cfg_.corpus) : CorpusSummary{};
    cs.availableSpeakers = availableSpeakers();
    return cs;
}

int EngineController::availableSpeakers() const {
    if (!cfg_.corpus) return 0;
    int n = 0;
    for (SpeakerId s = 0; s < cfg_.corpus->numSpeakers(); ++s) n += cfg_.corpus->speakerHealthy(s) ? 1 : 0;
    return n;
}

std::unique_ptr<MaskStrategy> EngineController::strategyFor(const MaskRenderPlan& p) const {
    if (!cfg_.dataSet) return nullptr;
    return makeStrategy(*cfg_.dataSet, std::string(toString(p.strategyId)));
}

// ------------------------------------------------------------------------------ queue

void EngineController::post(std::function<void()> task) {
    {
        std::lock_guard<CheckedMutex> lk(qMutex_);
        queue_.push_back(std::move(task));
    }
    qCv_.notify_all();
}

CommandResult EngineController::call(std::function<CommandResult()> fn) {
    if (std::this_thread::get_id() == controlId_) return fn();
    if (quit_.load()) return CommandResult::Failed;
    auto prom = std::make_shared<std::promise<CommandResult>>();
    auto fut = prom->get_future();
    post([prom, fn = std::move(fn)]() mutable { prom->set_value(fn()); });
    if (fut.wait_for(std::chrono::duration<double>(cfg_.commandTimeoutS)) != std::future_status::ready)
        return CommandResult::Timeout;
    return fut.get();
}

void EngineController::controlLoop() {
    setCurrentThreadName("bf.control");
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<CheckedMutex> lk(qMutex_);
            qCv_.wait_for(lk, std::chrono::milliseconds(cfg_.controlTickMs),
                          [&] { return quit_.load() || !queue_.empty(); });
            if (!queue_.empty()) {
                task = std::move(queue_.front());
                queue_.pop_front();
            } else if (quit_.load()) {
                break;
            }
        }
        if (task) task();
        periodic();
    }
}

void EngineController::watchdogLoop() {
    setCurrentThreadName("bf.watchdog");
    auto next = std::chrono::steady_clock::now();
    while (!quit_.load()) {
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(cfg_.watchdogPeriodS));
        while (!quit_.load() && std::chrono::steady_clock::now() < next)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (quit_.load()) break;
        post([this] { watchdogCheck(); });  // the watchdog acts via Control (RELIABILITY §6.3)
    }
}

// ------------------------------------------------------------------------------ status

bool EngineController::go(EngineState to, const std::string& reason, const std::string& detail) {
    const EngineState from = sm_.state();
    if (!sm_.transition(to, reason)) return false;
    if (to == EngineState::Running && from == EngineState::Starting && runningSinceS_ < 0.0) runningSinceS_ = nowS();
    if (to == EngineState::Error || to == EngineState::Stopped || to == EngineState::Preparing ||
        to == EngineState::DeviceLost)
        runningSinceS_ = -1.0;
    pushStatus(from, to, detail.empty() ? reason : detail, {});
    return true;
}

void EngineController::pushStatus(EngineState prev, EngineState now, const std::string& detail,
                                  std::vector<std::string> newCodes) {
    EngineStatusEvent e;
    e.wall = std::chrono::system_clock::now();
    e.engineSample = static_cast<std::uint64_t>(std::max<std::int64_t>(0, enginePos_.load()));
    e.state = now;
    e.previous = prev;
    e.degradedReasons = reasons_.load();
    e.headline = std::string(headlineFor(now));
    e.detailCode = detail;
    e.newReasonCodes = std::move(newCodes);
    {
        std::lock_guard<CheckedMutex> lk(statusMutex_);
        e.seq = ++statusSeq_;
        history_.push_back(e);
        if (history_.size() > 2000) history_.erase(history_.begin(), history_.begin() + 1000);
        if (pendingStatus_) {  // coalesce: latest state + accumulated new reason codes
            std::vector<std::string> acc = pendingStatus_->newReasonCodes;
            for (const auto& c : e.newReasonCodes)
                if (std::find(acc.begin(), acc.end(), c) == acc.end()) acc.push_back(c);
            e.newReasonCodes = std::move(acc);
        }
        pendingStatus_ = std::move(e);
    }
    statusCv_.notify_all();
}

std::optional<EngineStatusEvent> EngineController::pollStatus() {
    std::lock_guard<CheckedMutex> lk(statusMutex_);
    auto r = std::move(pendingStatus_);
    pendingStatus_.reset();
    return r;
}

std::vector<EngineStatusEvent> EngineController::statusHistory() const {
    std::lock_guard<CheckedMutex> lk(statusMutex_);
    return history_;
}

bool EngineController::waitForState(EngineState s, double timeoutS) const {
    std::unique_lock<CheckedMutex> lk(statusMutex_);
    return statusCv_.wait_for(lk, std::chrono::duration<double>(timeoutS),
                              [&] { return state_.load(std::memory_order_acquire) == s; });
}

// ------------------------------------------------------------------------------ commands

CommandResult EngineController::rejectCommand(const char* command, EngineState target) {
    // Commands are triggers: a command whose trigger does not apply in the current state is an
    // illegal transition even when the target edge exists for another trigger (RELIABILITY §1.3).
    const EngineState s = sm_.state();
    log_->log(LogLevel::Warn, logcode::kEngineIllegal,
              std::string("illegal transition rejected: ") + command + " in " + std::string(toString(s)),
              {{"from", toString(s)}, {"to", toString(target)}, {"command", command}});
    return CommandResult::IllegalTransition;
}

CommandResult EngineController::start() {
    return call([this] {
        if (sm_.state() != EngineState::Stopped) return rejectCommand("start()", EngineState::Preparing);
        abort_.store(false);
        stoppedByAbort_ = false;
        if (!go(EngineState::Preparing, "start()")) return CommandResult::IllegalTransition;
        log_->log(LogLevel::Info, logcode::kEngineStart, "start requested");
        post([this] { doPrepare(); });
        return CommandResult::Ok;
    });
}

CommandResult EngineController::stop() {
    if (state() == EngineState::Preparing) {
        // The control thread may be busy preparing: abort asynchronously; doPrepare() ends in
        // STOPPED (PREPARING -> STOPPED) or, if it already reached READY, stops normally.
        abort_.store(true);
        return CommandResult::Ok;
    }
    return call([this] {
        const EngineState s = sm_.state();
        if (s == EngineState::Stopped && stoppedByAbort_) {
            stoppedByAbort_ = false;
            return CommandResult::Ok;
        }
        if (s == EngineState::DeviceLost) {
            releaseEngine();
            return go(EngineState::Stopped, "stop()") ? CommandResult::Ok : CommandResult::IllegalTransition;
        }
        if (s == EngineState::Ready || masking(s)) {
            beginStop(false, "stop()", false);
            return CommandResult::Ok;
        }
        // STOPPED / STOPPING / ERROR: illegal, logged.
        return rejectCommand("stop()", EngineState::Stopping);
    });
}

CommandResult EngineController::play() {
    return call([this] {
        if (sm_.state() != EngineState::Ready) return rejectCommand("play()", EngineState::Starting);
        doPlay();
        return CommandResult::Ok;
    });
}

CommandResult EngineController::reconnect() {
    return call([this] {
        if (sm_.state() != EngineState::DeviceLost) return rejectCommand("reconnect()", EngineState::Preparing);
        if (!go(EngineState::Preparing, "reconnect()")) return CommandResult::IllegalTransition;
        abort_.store(false);
        post([this] { doPrepare(); });
        return CommandResult::Ok;
    });
}

CommandResult EngineController::reset() {
    return call([this] {
        if (sm_.state() != EngineState::Error) return rejectCommand("reset()", EngineState::Stopped);
        releaseEngine();
        tracker_.clear();
        reasons_.store(0);
        return go(EngineState::Stopped, "reset()") ? CommandResult::Ok : CommandResult::IllegalTransition;
    });
}

CommandResult EngineController::setPreset(const nlohmann::json& doc) {
    return call([this, doc] {
        if (!cfg_.dataSet) return CommandResult::Failed;
        std::string err;
        if (!validatePresetDoc(*cfg_.dataSet, doc, corpusSummary(), &err)) {
            log_->log(LogLevel::Warn, logcode::kPresetInvalid, "Preset could not be loaded", {{"error", err}});
            return CommandResult::InvalidArgument;
        }
        const auto changed = presetDiff(preset_, doc);
        if (changed.empty()) return CommandResult::Ok;
        log_->log(LogLevel::Info, logcode::kPresetChange, "preset changed", {{"changed", changed}});
        const ScenarioPlan sp = buildScenarioPlan(*cfg_.dataSet, doc, std::nullopt, corpusSummary(), seed_);
        for (const auto& a : sp.plan.adjustments)
            log_->log(LogLevel::Info, logcode::kConfigClamped, a.reason,
                      {{"field", a.fieldPath}, {"from", a.from}, {"to", a.to}});
        preset_ = doc;
        sessionDirty_ = true;
        lkgPromoted_ = false;
        if (masking(sm_.state())) runningSinceS_ = sm_.state() == EngineState::Starting ? -1.0 : nowS();
        {
            std::lock_guard<CheckedMutex> lk(presetMutex_);
            presetCopy_ = preset_;
        }
        const EngineState s = sm_.state();
        if (!(s == EngineState::Ready || masking(s)) || !engine_ || !havePlan_) return CommandResult::Ok;
        // Live when only Strength / mix / limiter ceiling differ (same canonical plan otherwise).
        MaskRenderPlan a = basePlan_, b = sp.plan;
        b.level.strengthDb = a.level.strengthDb;
        b.level.limiterCeilingDbtp = a.level.limiterCeilingDbtp;
        b.mix = a.mix;
        a.rehash();
        b.rehash();
        if (a.planHash == b.planHash && sp.layout.size() == layout_.size()) {
            engine_->setStrengthDb(sp.plan.level.strengthDb);
            engine_->setLimiterCeilingDb(sp.plan.level.limiterCeilingDbtp);
            if (!safeFallback_ && plan_.babbleEnabled) engine_->setBabbleFraction(sp.plan.mix.babbleFraction);
            basePlan_.level = sp.plan.level;
            basePlan_.mix = sp.plan.mix;
            plan_.level = sp.plan.level;
            if (!safeFallback_ && plan_.babbleEnabled) plan_.mix = sp.plan.mix;
            std::lock_guard<CheckedMutex> lk(presetMutex_);
            planCopy_ = plan_;
            return CommandResult::Ok;
        }
        beginStop(true, "preset change requires a rebuild", false);
        return CommandResult::Ok;
    });
}

CommandResult EngineController::setStrength(double db) {
    nlohmann::json doc = currentPreset();
    if (!doc.contains("macros") || !doc["macros"].is_object()) doc["macros"] = nlohmann::json::object();
    doc["macros"]["strengthDb"] = db;
    return setPreset(doc);
}

CommandResult EngineController::setAudioFormat(double sampleRate, int bufferFrames) {
    if (!(sampleRate > 0.0) || bufferFrames < 16) return CommandResult::InvalidArgument;
    return call([this, sampleRate, bufferFrames] {
        if (cfg_.sampleRate == sampleRate && cfg_.bufferFrames == bufferFrames) return CommandResult::Ok;
        log_->log(LogLevel::Info, logcode::kDeviceBufferChange, "audio format changed",
                  {{"sampleRate", sampleRate}, {"bufferFrames", bufferFrames}});
        cfg_.sampleRate = sampleRate;
        cfg_.bufferFrames = bufferFrames;
        const EngineState s = sm_.state();
        if (s == EngineState::Ready || masking(s)) beginStop(true, "audio format change", false);
        return CommandResult::Ok;
    });
}

CommandResult EngineController::testSpeakers(bool on) {
    std::lock_guard<CheckedMutex> lk(engineMutex_);
    MaskEngine* me = engine_ ? engine_->engine() : nullptr;
    if (me == nullptr) return CommandResult::IllegalTransition;
    OutputSourceStage& stage = me->sourceStage();
    if (!on) {
        stage.stopTest();
        return CommandResult::Ok;
    }
    std::vector<OutputChannel> channels;
    std::vector<OutputZone> zones;
    int n = 1;
    {
        std::lock_guard<CheckedMutex> pl(presetMutex_);
        n = std::max(1, layoutCopy_.size());
        const auto r = parsePreset(presetCopy_.dump());
        if (r.ok) {
            channels = r.preset.outputs.channels;
            zones = r.preset.outputs.zones;
        }
    }
    const std::vector<int> outs = enabledOutputs(channels, zones, n);
    TestSpeakersOptions opt;
    std::string err;
    if (!startTestSpeakers(stage, outs, opt, &err)) return CommandResult::Failed;
    testOutputs_.store(static_cast<int>(outs.size()), std::memory_order_relaxed);
    return CommandResult::Ok;
}

EngineController::TestSpeakersState EngineController::testSpeakersState() const {
    std::lock_guard<CheckedMutex> lk(engineMutex_);
    TestSpeakersState s;
    MaskEngine* me = engine_ ? engine_->engine() : nullptr;
    if (me == nullptr) return s;
    CalibrationBus& bus = me->sourceStage().bus();
    s.running = bus.running();
    s.elapsedS = bus.elapsedSeconds();
    s.outputs = testOutputs_.load(std::memory_order_relaxed);
    return s;
}

void EngineController::onDeviceEvent(const DeviceEvent& e) {
    post([this, e] {
        const EngineState s = sm_.state();
        switch (e.kind) {
        case DeviceEventKind::Lost:
        case DeviceEventKind::Error:
            log_->log(LogLevel::Error, logcode::kDeviceLost, "OUTPUT DEVICE LOST",
                      {{"device", e.deviceId}, {"message", e.message}});
            if (s == EngineState::Stopping) {
                finishStop();
            } else if (s == EngineState::Ready || masking(s)) {
                deviceFailure("device.lost");
            }
            break;
        case DeviceEventKind::Returned:
            log_->log(LogLevel::Info, logcode::kDeviceReturned, "output device available again", {{"device", e.deviceId}});
            // Never another device: only the configured one, and only with autoReconnectSameDevice.
            if (s == EngineState::DeviceLost && cfg_.autoReconnectSameDevice && e.deviceId == cfg_.deviceId &&
                go(EngineState::Preparing, "device returned (autoReconnectSameDevice)")) {
                abort_.store(false);
                doPrepare();
            }
            break;
        case DeviceEventKind::RateChanged:
            log_->log(LogLevel::Info, logcode::kDeviceRateChange, "device sample rate changed",
                      {{"device", e.deviceId}, {"sampleRate", e.sampleRate}});
            if (s == EngineState::Ready || masking(s)) beginStop(true, "device.rateChange", true);
            break;
        case DeviceEventKind::BufferSizeChanged:
            log_->log(LogLevel::Info, logcode::kDeviceBufferChange, "device buffer size changed",
                      {{"device", e.deviceId}, {"bufferFrames", e.bufferFrames}});
            if (s == EngineState::Ready || masking(s)) beginStop(true, "device.bufferChange", true);
            break;
        }
    });
}

// ------------------------------------------------------------------------------ lifecycle

EngineController::FallbackOutcome EngineController::applyFallback(const MaskRenderPlan& in, double fs) const {
    FallbackOutcome o;
    o.plan = in;
    const auto strat = strategyFor(in);
    if (in.babbleEnabled && strat) {
        const int S = availableSpeakers();
        DegradeReason r;
        r.kind = (!cfg_.corpus || S <= 0 || !cfg_.audio) ? DegradeReason::Kind::CorpusNone
                                                          : DegradeReason::Kind::CorpusInsufficient;
        r.availableSpeakers = cfg_.audio ? S : 0;
        o.plan = strat->degrade(in, r);
    }
    if (o.plan.status.error) {
        o.errorCode = o.plan.status.errorCode.empty() ? "corpus.insufficient" : o.plan.status.errorCode;
        o.message = "fallback policy Strict: " + o.errorCode;
        return o;
    }
    for (const auto& c : o.plan.status.degraded) o.bits |= degradedReasonFromCode(c);
    if (o.plan.babbleEnabled && !isSupportedBabbleRate(fs)) {
        // Babble supports 44.1 / 48 / 88.2 / 96 kHz (TALKER_ENGINE §8.1). Report clearly.
        const bool strict = o.plan.fallback == FallbackPolicy::Strict || o.plan.strategyClass == StrategyClass::LaboratoryMask;
        if (strict) {
            o.errorCode = "engine.rateUnsupported";
            o.message = "babble requires a device rate of 44.1, 48, 88.2 or 96 kHz (device runs at " +
                        std::to_string(fs) + " Hz; fallback policy Strict)";
            return o;
        }
        MaskRenderPlan& p = o.plan;
        p.babbleEnabled = false;
        p.mix.babbleFraction = 0.0;
        p.mix.zoneBabbleFraction.fill(0.0);
        p.stationary.enabled = true;
        p.status.degraded.push_back("engine.rateUnsupported");
        p.status.degraded.push_back("fallback.stationary");
        p.rehash();
        o.bits |= kDegRateUnsupported | kDegFallbackStationary;
        o.message = "babble requires a device rate of 44.1, 48, 88.2 or 96 kHz (device runs at " +
                    std::to_string(fs) + " Hz): running stationary masking only";
    }
    return o;
}

void EngineController::doPrepare() {
    if (sm_.state() != EngineState::Preparing) return;
    auto aborted = [&] {
        if (!abort_.load()) return false;
        if (cfg_.backend) cfg_.backend->close();
        releaseEngine();
        stoppedByAbort_ = true;
        go(EngineState::Stopped, "stop() during prepare");
        return true;
    };
    if (!cfg_.dataSet || !cfg_.backend) {
        enterError("config.invalid", "controller without data set or audio backend");
        return;
    }
    const ScenarioPlan sp = buildScenarioPlan(*cfg_.dataSet, preset_, std::nullopt, corpusSummary(), seed_);
    if (!sp.ok) {
        enterError("config.invalid", sp.error);
        return;
    }
    if (aborted()) return;

    // Device: only the configured one (never another).
    const BackendOpenResult res = cfg_.backend->open(cfg_.deviceId, cfg_.sampleRate, cfg_.bufferFrames, sp.layout.size());
    if (!res.ok) {
        log_->log(LogLevel::Error, logcode::kDeviceOpenFailed, "device open failed",
                  {{"device", cfg_.deviceId}, {"error", res.error}});
        enterError("device.openFailed", res.error);
        return;
    }
    fs_.store(res.sampleRate);
    bufferFrames_ = res.bufferFrames;
    deviceOutputs_ = res.numOutputs;
    deviceType_ = cfg_.backend->typeName();
    log_->log(LogLevel::Info, logcode::kDeviceOpen, "device opened",
              {{"device", cfg_.deviceId}, {"sampleRate", res.sampleRate}, {"bufferFrames", res.bufferFrames},
               {"outputs", res.numOutputs}});

    FallbackOutcome fo = applyFallback(sp.plan, res.sampleRate);
    if (!fo.errorCode.empty()) {
        cfg_.backend->close();
        if (fo.errorCode == "engine.rateUnsupported")
            log_->log(LogLevel::Error, logcode::kEngineRateUnsupported, fo.message, {{"sampleRate", res.sampleRate}});
        enterError(fo.errorCode, fo.message);
        return;
    }
    if (fo.bits & kDegRateUnsupported)
        log_->log(LogLevel::Error, logcode::kEngineRateUnsupported, fo.message, {{"sampleRate", res.sampleRate}});
    if (fo.bits & kDegCorpusInsufficient)
        log_->log(LogLevel::Warn, logcode::kCorpusInsufficient, "Reduced Voice Library: reduced plan",
                  {{"availableSpeakers", availableSpeakers()}, {"needed", speakersNeededFor(sp.plan)}});
    basePlan_ = sp.plan;
    plan_ = fo.plan;
    layout_ = sp.layout;
    havePlan_ = true;
    safeFallback_ = false;
    handledFailures_ = 0;
    tracker_.clear();
    tracker_.setPersistent(fo.bits);
    ++planEpoch_;
    {
        std::lock_guard<CheckedMutex> lk(presetMutex_);
        planCopy_ = plan_;
        layoutCopy_ = layout_;
        bufferFramesCopy_ = bufferFrames_;
        deviceOutputsCopy_ = deviceOutputs_;
        planEpochCopy_ = planEpoch_;
    }

    RealtimeEngineConfig rc = cfg_.engine;
    rc.logger = log_;
    rc.audioSourceThreadSafe = cfg_.audioThreadSafe;
    MaskEngineConfig& mc = rc.engine;
    mc.seed = seed_;
    mc.corpus = cfg_.corpus;
    mc.audio = cfg_.audio;
    mc.lRefDbfs = plan_.level.lRefDbfs;
    mc.channels = sp.preset.outputs.channels;
    mc.zones = sp.preset.outputs.zones;
    mc.correctionEnabled = sp.preset.spectrum.correction.enabled.value_or(true);
    mc.correctionSpeed = speedFrom(sp.preset.spectrum.correction.speed);
    const std::string corpusVersion = cfg_.corpus ? cfg_.corpus->corpusVersion() : std::string();
    if (mc.correctionEnabled) mc.initialCorrection = store_.loadCorrection(basePlan_.planHash, corpusVersion, res.sampleRate);

    auto eng = std::make_unique<RealtimeEngine>(rc);
    std::string err;
    const bool ok = eng->prepare(res.sampleRate, sp.layout, res.numOutputs, res.bufferFrames, plan_, &err, &abort_);
    {
        std::lock_guard<CheckedMutex> lk(engineMutex_);
        engine_ = std::move(eng);
    }
    if (aborted()) return;
    if (!ok) {
        cfg_.backend->close();
        enterError("engine.prepareFailed", err);
        return;
    }
    if (auto st = store_.loadSelectorState(corpusVersion)) engine_->importSelectorState(*st);
    engine_->setXrunSource(cfg_.backend);
    if (!cfg_.backend->start(engine_.get())) {
        log_->log(LogLevel::Error, logcode::kDeviceOpenFailed, "device start failed", {{"device", cfg_.deviceId}});
        enterError("device.openFailed", "device start failed");
        return;
    }
    lastCallbacks_ = 0;
    lastCallbackAdvanceS_ = nowS();
    log_->log(LogLevel::Info, logcode::kSessionSeed, "session seed",
              {{"seed", seed_},
               {"corpusVersion", corpusVersion},
               {"dataSetHash", cfg_.dataSet->dataSetHash},
               {"appVersion", versionString()},
               {"sampleRate", res.sampleRate},
               {"strategy", toString(plan_.strategyId)},
               {"planHash", plan_.planHash}});
    if (!go(EngineState::Ready, "prepared")) return;
    if (abort_.load()) {  // stop() arrived after the last abort check
        beginStop(false, "stop() during prepare", true);
        return;
    }
    if (!cfg_.prepareOnly) doPlay();
}

void EngineController::doPlay() {
    if (!engine_ || !go(EngineState::Starting, "play()")) return;
    engine_->setOutputMode(OutputMode::FadeIn);
}

void EngineController::beginStop(bool rebuild, const std::string& reason, bool immediate) {
    if (!go(EngineState::Stopping, reason)) return;
    rebuildPending_ = rebuild;
    if (engine_) engine_->setOutputMode(OutputMode::FadeOut);
    const double fadeS = engine_ ? engine_->config().fadeOutMs * 1e-3 : 0.0;
    stopDeadlineS_ = nowS() + (immediate ? 0.0 : fadeS + 0.2);
}

void EngineController::releaseEngine() {
    if (cfg_.backend) {
        cfg_.backend->stop();
        if (cfg_.backend->isOpen()) {
            cfg_.backend->close();
            log_->log(LogLevel::Info, logcode::kDeviceClose, "device closed", {{"device", cfg_.deviceId}});
        }
    }
    std::unique_ptr<RealtimeEngine> old;
    {
        std::lock_guard<CheckedMutex> lk(engineMutex_);
        old = std::move(engine_);
    }
    if (old) old->release();
}

void EngineController::finishStop() {
    if (engine_) persist(true);
    releaseEngine();
    log_->log(LogLevel::Info, logcode::kEngineStop, "engine stopped", {{"rebuild", rebuildPending_}});
    if (rebuildPending_) {
        rebuildPending_ = false;
        if (go(EngineState::Preparing, "rebuild")) {
            abort_.store(false);
            post([this] { doPrepare(); });
        }
        return;
    }
    go(EngineState::Stopped, "stopped");
}

void EngineController::enterError(const std::string& code, const std::string& msg) {
    releaseEngine();
    log_->log(LogLevel::Error, code, msg);
    go(EngineState::Error, code, code);
    // Diagnostic snapshot on every ERROR (RELIABILITY §6.1).
    if (store_.enabled()) {
        std::string err;
        const nlohmann::json snap = diagnostics();
        std::error_code ec;
        std::filesystem::create_directories(store_.dir(), ec);
        writeFileAtomic(store_.dir() / "diagnostics_last_error.json", snap.dump(2), &err);
    }
}

void EngineController::deviceFailure(const std::string& reason) {
    if (engine_) engine_->setOutputMode(OutputMode::Silent);
    if (cfg_.backend) {
        cfg_.backend->stop();
        cfg_.backend->close();
    }
    // The graph is retained (RELIABILITY §1.1); no other device is tried.
    go(EngineState::DeviceLost, reason, reason);
}

// ------------------------------------------------------------------------------ periodic

void EngineController::periodic() {
    const EngineState s = sm_.state();
    const double now = nowS();
    if (engine_) enginePos_.store(engine_->position(), std::memory_order_relaxed);
    if (s == EngineState::Starting && engine_ && engine_->fadeComplete()) {
        if (tracker_.reasons(now) != 0)
            go(EngineState::Degraded, "fade-in complete (degraded)");
        else
            go(EngineState::Running, "fade-in complete");
        if (runningSinceS_ < 0.0) runningSinceS_ = now;
    }
    if (s == EngineState::Stopping && (!engine_ || engine_->fadeComplete() || now >= stopDeadlineS_)) finishStop();
    evaluateDegraded(now);
    persist(false);
}

void EngineController::evaluateDegraded(double now) {
    const EngineState s = sm_.state();
    const std::uint32_t r = masking(s) ? tracker_.reasons(now) : 0;
    const std::uint32_t added = r & ~lastReasons_;
    reasons_.store(r, std::memory_order_release);
    if (added) {
        log_->log(LogLevel::Warn, logcode::kEngineDegraded, "degraded", {{"reasons", degradedReasonCodes(r)},
                                                                          {"new", degradedReasonCodes(added)}});
    }
    if (r != 0 && s == EngineState::Running) {
        go(EngineState::Degraded, "degraded", degradedReasonCodes(added ? added : r).front());
    } else if (r != 0 && s == EngineState::Degraded && added) {
        pushStatus(s, s, degradedReasonCodes(added).front(), degradedReasonCodes(added));
    } else if (r == 0 && s == EngineState::Degraded) {
        log_->log(LogLevel::Info, logcode::kEngineRecovered, "all degraded conditions cleared");
        go(EngineState::Running, "recovered");
    }
    if (added && s != EngineState::Running && masking(s)) {
        // new codes also reach the UI when entering DEGRADED above
    }
    lastReasons_ = r;
}

void EngineController::watchdogCheck() {
    const double now = nowS();
    const EngineState s = sm_.state();
    // CPU usage of the process (advisory).
    const double cpu = static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
    if (lastCpuWallS_ > 0.0 && now > lastCpuWallS_)
        cpuPct_.store(100.0 * (cpu - lastCpuS_) / (now - lastCpuWallS_), std::memory_order_relaxed);
    lastCpuWallS_ = now;
    lastCpuS_ = cpu;

    if (log_->queueFill() > 0.9) {
        if (!loggerStallLogged_)
            log_->log(LogLevel::Error, std::string(logcode::kWatchdogStallPrefix) + "logger", "log queue above 90 %");
        loggerStallLogged_ = true;
    } else {
        loggerStallLogged_ = false;
    }
    if (!engine_ || !masking(s)) {
        lastCallbackAdvanceS_ = now;
        return;
    }
    const RtMetrics m = engine_->metrics();

    // RT: the callback counter must advance while masking; no callback for 2 s = device failure.
    if (m.callbacks != lastCallbacks_) {
        lastCallbacks_ = m.callbacks;
        lastCallbackAdvanceS_ = now;
    } else if (now - lastCallbackAdvanceS_ >= cfg_.callbackStallS) {
        log_->log(LogLevel::Error, std::string(logcode::kWatchdogStallPrefix) + "audio",
                  "no audio callback for " + std::to_string(now - lastCallbackAdvanceS_) + " s");
        log_->log(LogLevel::Error, logcode::kDeviceLost, "OUTPUT DEVICE LOST (callback stalled)", {{"device", cfg_.deviceId}});
        deviceFailure("rt.callbackStalled");
        return;
    }
    const std::int64_t nowUs = monotonicMicros();
    if (m.babble) {
        const bool plannerStall = m.lookaheadS < 1.0 && static_cast<double>(nowUs - m.plannerHeartbeatUs) > cfg_.plannerStallS * 1e6;
        tracker_.setCondition(kDegPlannerStalled, plannerStall, now);
        if (plannerStall && !plannerStallLogged_)
            log_->log(LogLevel::Error, std::string(logcode::kWatchdogStallPrefix) + "planner", "planner stalled",
                      {{"lookaheadS", m.lookaheadS}});
        plannerStallLogged_ = plannerStall;
        const bool preloadStall = m.oldestNeedAgeS > cfg_.preloadQueueAgeS;
        if (preloadStall && !preloadStallLogged_)
            log_->log(LogLevel::Error, std::string(logcode::kWatchdogStallPrefix) + "preload", "preload queue age above limit",
                      {{"oldestNeedAgeS", m.oldestNeedAgeS}});
        preloadStallLogged_ = preloadStall;
    }
    const bool analysisStall = static_cast<double>(nowUs - m.analysisHeartbeatUs) > cfg_.analysisStallS * 1e6;
    tracker_.setCondition(kDegAnalysisStalled, analysisStall, now);
    if (analysisStall && !analysisStallLogged_)
        log_->log(LogLevel::Error, std::string(logcode::kWatchdogStallPrefix) + "analysis", "analysis thread stalled");
    analysisStallLogged_ = analysisStall;

    // preload.behind: > 5 starvations per minute.
    starveHist_.emplace_back(now, m.starvations);
    while (!starveHist_.empty() && now - starveHist_.front().first > 60.0) starveHist_.pop_front();
    const std::uint64_t starvMin = m.starvations - starveHist_.front().second;
    tracker_.setCondition(kDegPreloadBehind, starvMin > 5, now);
    // audio.xruns: > 10 per minute for 2 minutes.
    xrunHist_.emplace_back(now, m.xruns);
    while (!xrunHist_.empty() && now - xrunHist_.front().first > 120.0) xrunHist_.pop_front();
    std::uint64_t lastMin = 0, prevMin = 0;
    {
        const double tMid = now - 60.0;
        std::uint64_t atMid = xrunHist_.front().second;
        for (const auto& [t, v] : xrunHist_)
            if (t <= tMid) atMid = v;
        lastMin = m.xruns - atMid;
        prevMin = atMid - xrunHist_.front().second;
    }
    const bool haveTwoMinutes = now - xrunHist_.front().first >= 119.0;
    tracker_.setCondition(kDegAudioXruns, haveTwoMinutes && lastMin > 10 && prevMin > 10, now);
    // Safety clip.
    const MaskStatistics st = engine_->statistics();
    tracker_.setCondition(kDegOutputClip, st.clipEvents > lastClip_, now);
    if (st.clipEvents > lastClip_)
        log_->log(LogLevel::Error, logcode::kOutputClip, "Output limiting - reduce Strength", {{"clipEvents", st.clipEvents}});
    lastClip_ = st.clipEvents;

    handleSourceFailures(m, now);
}

void EngineController::handleSourceFailures(const RtMetrics& m, double now) {
    (void)now;
    if (m.sourceFailures <= handledFailures_ || !plan_.babbleEnabled) return;
    handledFailures_ = m.sourceFailures;
    const auto strat = strategyFor(basePlan_);
    if (!strat) return;
    DegradeReason r;
    r.kind = DegradeReason::Kind::SourceFailures;
    r.availableSpeakers = availableSpeakers();
    r.failedPoolFraction = m.poolRecordings ? static_cast<double>(m.poolUnhealthyRecordings) /
                                                  static_cast<double>(m.poolRecordings)
                                            : 0.0;
    const MaskRenderPlan p = strat->degrade(basePlan_, r);
    const nlohmann::json d = {{"failedPoolFraction", r.failedPoolFraction},
                              {"unhealthyRecordings", m.unhealthyRecordings},
                              {"substitutions", m.substitutions},
                              {"policy", toString(basePlan_.fallback)}};
    if (p.status.error) {
        log_->log(LogLevel::Error, logcode::kCorpusDecodeFailed, "source failure with fallback policy Strict", d);
        enterError(p.status.errorCode.empty() ? "source.failure" : p.status.errorCode,
                   "talker source failure (fallback policy Strict)");
        return;
    }
    std::uint32_t bits = tracker_.persistent();
    for (const auto& c : p.status.degraded) bits |= degradedReasonFromCode(c);
    if (!p.babbleEnabled && !safeFallback_) {
        safeFallback_ = true;
        engine_->setBabbleFraction(0.0);  // same spectrum target and level, 2 s mixer ramp
        plan_ = p;
        std::lock_guard<CheckedMutex> lk(presetMutex_);
        planCopy_ = plan_;
        log_->log(LogLevel::Error, logcode::kCorpusReduced,
                  "Reduced Voice Library - switched to steady masking (fallback policy Safe)", d);
    } else if ((bits & kDegCorpusReduced) && !(tracker_.persistent() & kDegCorpusReduced)) {
        log_->log(LogLevel::Warn, logcode::kCorpusReduced, "Reduced Voice Library (> 10 % of the pool unhealthy)", d);
    }
    tracker_.setPersistent(bits);
}

void EngineController::persist(bool force) {
    if (!store_.enabled()) return;
    const double now = nowS();
    const EngineState s = sm_.state();
    std::string err;
    if (sessionDirty_ && (force || now - lastSessionSaveS_ >= cfg_.sessionSaveS)) {
        if (store_.saveSession(preset_, seed_, &err))
            sessionDirty_ = false;
        else
            log_->log(LogLevel::Error, logcode::kPersistWrite, "session.json write failed", {{"error", err}});
        lastSessionSaveS_ = now;
    }
    const bool activeMasking = s == EngineState::Running || s == EngineState::Degraded;
    if (activeMasking && !lkgPromoted_ && runningSinceS_ >= 0.0 && now - runningSinceS_ >= cfg_.lkgPromoteS) {
        if (store_.saveLastKnownGood(preset_, &err)) {
            lkgPromoted_ = true;
            log_->log(LogLevel::Info, logcode::kPresetLkgPromoted, "configuration promoted to last known good",
                      {{"runningS", now - runningSinceS_}});
        }
    }
    if (!engine_ || !engine_->prepared()) return;
    const std::string cv = cfg_.corpus ? cfg_.corpus->corpusVersion() : std::string();
    const bool babble = engine_->metrics().babble;
    if (babble && (force ? activeMasking || s == EngineState::Stopping : activeMasking && now - lastCorrectionSaveS_ >= cfg_.correctionSaveS)) {
        store_.saveCorrection(basePlan_.planHash, cv, fs_.load(), engine_->correction(), &err);
        lastCorrectionSaveS_ = now;
    }
    if (babble && (force || (activeMasking && now - lastSelectorSaveS_ >= cfg_.selectorSaveS))) {
        const nlohmann::json st = engine_->exportSelectorState();
        if (!st.is_null()) store_.saveSelectorState(st, cv, &err);
        lastSelectorSaveS_ = now;
    }
}

// ------------------------------------------------------------------------------ observation

nlohmann::json EngineController::currentPreset() const {
    std::lock_guard<CheckedMutex> lk(presetMutex_);
    return presetCopy_;
}

MaskRenderPlan EngineController::currentPlan() const {
    std::lock_guard<CheckedMutex> lk(presetMutex_);
    return planCopy_;
}

RtMetrics EngineController::metrics() const {
    std::lock_guard<CheckedMutex> lk(engineMutex_);
    return engine_ ? engine_->metrics() : RtMetrics{};
}

MaskStatistics EngineController::statistics() const {
    std::lock_guard<CheckedMutex> lk(engineMutex_);
    return engine_ ? engine_->statistics() : MaskStatistics{};
}

nlohmann::json EngineController::diagnostics() const {
    DiagnosticsInput in;
    in.appVersion = versionString();
    in.build = BF_VERSION_STRING;  // BuildInfo.h
    in.state = state();
    in.degradedReasons = degradedReasons();
    in.uptimeS = nowS();
    in.sessionSeed = seed_;
    in.corpusVersion = cfg_.corpus ? cfg_.corpus->corpusVersion() : std::string();
    in.dataSetHash = cfg_.dataSet ? cfg_.dataSet->dataSetHash : std::string();
    OutputLayout layout;
    {
        std::lock_guard<CheckedMutex> lk(presetMutex_);
        in.planEpoch = planEpochCopy_;
        in.bufferFrames = bufferFramesCopy_;
        in.outputs = deviceOutputsCopy_;
        layout = layoutCopy_;
    }
    in.driver = cfg_.backend ? cfg_.backend->typeName() : std::string();
    in.device = cfg_.deviceId;
    in.sampleRate = fs_.load();
    in.processCpuPct = cpuPct_.load();
    const MaskRenderPlan plan = currentPlan();
    in.strategy = std::string(toString(plan.strategyId));
    in.target = plan.target.id;
    in.meanActiveTarget = plan.babbleEnabled ? plan.talkers.targetMean() : 0.0;
    in.slots = plan.babbleEnabled ? static_cast<int>(plan.talkers.slots()) : 0;
    in.pool = plan.babbleEnabled ? static_cast<int>(plan.talkers.pool) : 0;
    in.limiterEnabled = plan.level.limiterEnabled;
    in.limiterCeilingDbtp = plan.level.limiterCeilingDbtp;
    if (cfg_.corpus) {
        in.corpusSpeakers = static_cast<int>(cfg_.corpus->numSpeakers());
        in.usableSpeechH = cfg_.corpus->totalSpeechSeconds() / 3600.0;
    }
    {
        std::lock_guard<CheckedMutex> lk(engineMutex_);
        if (engine_ && engine_->prepared()) {
            in.metrics = engine_->metrics();
            in.stats = engine_->statistics();
            in.haveStats = true;
            in.latencyMs = 1000.0 * (static_cast<double>(in.bufferFrames) + engine_->engine()->latencySamples()) /
                           std::max(in.sampleRate, 1.0);
        }
    }
    for (int c = 0; c < layout.size(); ++c) {
        OutputInfo o;
        o.index = c;
        o.label = layout.outputs[static_cast<std::size_t>(c)].label;
        o.zone = layout.outputs[static_cast<std::size_t>(c)].zone;
        in.outputsInfo.push_back(o);
    }
    in.logDropped = log_->dropped();
    in.xrunsPerMinute = in.uptimeS > 0.0 ? static_cast<double>(in.metrics.xruns) / std::max(in.uptimeS / 60.0, 1.0) : 0.0;
    const std::uint32_t r = in.degradedReasons;
    in.corpusHealth = (r & (kDegCorpusInsufficient | kDegCorpusNone)) ? CorpusHealth::Insufficient
                      : (r & kDegCorpusReduced)                      ? CorpusHealth::Reduced
                                                                     : CorpusHealth::Healthy;
    in.limiterHealth = (r & (kDegLimiterOverload | kDegOutputClip)) ? LimiterHealth::Sustained
                       : in.stats.limiterActiveFraction > 0.01      ? LimiterHealth::Active
                                                                    : LimiterHealth::Inactive;
    in.recentLog = log_->recent(LogLevel::Warn);
    return buildDiagnosticsSnapshot(in, cfg_.redactPathsInExports);
}

}  // namespace bf::rt
