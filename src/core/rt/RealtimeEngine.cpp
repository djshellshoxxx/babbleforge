#include "core/rt/RealtimeEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "core/rt/Denormals.h"
#include "core/rt/ThreadPriority.h"
#include "core/talker/SegmentSelector.h"

namespace bf::rt {

namespace {
constexpr std::size_t kLatBins = 10000;  // 0.1 ms bins, 0..1 s

// fsB: babble (planner) clock = engine rate.
std::int64_t readyNeed(const TalkerEvent& ev, std::int64_t len, std::int64_t fsB) {
    return std::min(len, std::max(ev.fadeInLen + fsB, static_cast<std::int64_t>(1.25 * static_cast<double>(fsB))));
}

// Serialises the reads of a source that is not thread-safe (new generations after a hot reload).
class SharedLockedSource final : public IAudioSource {
public:
    explicit SharedLockedSource(std::shared_ptr<IAudioSource> in) : in_(std::move(in)) {}
    bool read(RecordingId rec, std::uint64_t start, float* dst, std::size_t n) override {
        std::lock_guard<std::mutex> lk(m_);
        return in_->read(rec, start, dst, n);
    }

private:
    std::shared_ptr<IAudioSource> in_;
    std::mutex m_;
};

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(std::max(ms, 1))); }
}  // namespace

// One corpus snapshot with the audio source of its cache files. Jobs (running / queued events) hold the
// generation they were planned from: it stays alive (so does its cache lease) until the last of them is gone.
struct RealtimeEngine::Generation {
    std::shared_ptr<const CorpusSnapshot> snap;
    std::shared_ptr<IAudioSource> audio;  // serialised if the raw source is not thread-safe
    std::shared_ptr<void> owner;          // keeps the loaded corpus (FlacCacheAudioSource) alive
    std::shared_ptr<void> retired;        // released together with the generation (set when replaced)
};

struct RealtimeEngine::PendingAdoption {
    std::shared_ptr<const CorpusSnapshot> snap;
    std::shared_ptr<IAudioSource> audio;
    bool threadSafe = false;
    CorpusMigration migration;
    std::shared_ptr<void> retirePrevious;
};

struct RealtimeEngine::Job {
    std::shared_ptr<Generation> gen;
    PlannedEvent pe;
    BlockChain* chain = nullptr;
    std::int64_t chainOffset = 0;
    std::int64_t len = 0;
    bool pushed = false, busy = false, retired = false, failed = false, dead = false, started = false, done = false;
    int substitutions = 0;
    std::int64_t needSinceUs = 0;
};

class RealtimeEngine::TapSink final : public ITapSink {
public:
    explicit TapSink(std::array<TapRing, kNumTaps>& rings) : rings_(rings) {}
    void onTap(int tap, std::int64_t start, const float* const* ch, int nCh, int n) override {
        if (tap >= 0 && tap < kNumTaps) rings_[static_cast<std::size_t>(tap)].write(start, ch, nCh, n);
    }

private:
    std::array<TapRing, kNumTaps>& rings_;
};

RealtimeEngine::RealtimeEngine(RealtimeEngineConfig cfg) : cfg_(std::move(cfg)) {
    cfg_.decodeThreads = std::clamp(cfg_.decodeThreads, 1, 4);
    cfg_.lookaheadS = std::max(cfg_.lookaheadS, 4.0);
}

RealtimeEngine::~RealtimeEngine() { release(); }

void RealtimeEngine::logEvent(LogLevel l, const char* code, std::string msg, nlohmann::json data) {
    if (cfg_.logger) cfg_.logger->log(l, code, std::move(msg), std::move(data));
}

std::int64_t RealtimeEngine::babbleNow() const noexcept {
    return engine_ ? engine_->rtPosition() - babbleStart_ : 0;
}

bool RealtimeEngine::prepare(double fs, const OutputLayout& layout, int deviceOutputs, int maxDeviceBlock,
                             const MaskRenderPlan& plan, std::string* error, const std::atomic<bool>* abort) {
    release();
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        release();
        return false;
    };
    fs_ = fs;
    nCh_ = layout.size();
    nOut_ = std::max(deviceOutputs, 1);
    maxBlock_ = std::max(maxDeviceBlock, 1);

    MaskEngineConfig mc = cfg_.engine;
    mc.realtime = true;
    if (mc.audio && !cfg_.audioSourceThreadSafe) {
        locked_ = std::make_unique<LockedAudioSource>(*mc.audio);
        mc.audio = locked_.get();
    }
    fsB_ = static_cast<std::int64_t>(std::llround(fs));
    const bool wantBabble = plan.babbleEnabled && mc.corpus && mc.audio && isSupportedBabbleRate(fs);
    if (wantBabble) {
        const double V = static_cast<double>(std::max<std::uint32_t>(plan.talkers.slots(), 1));
        const double autoBytes = std::max(128.0 * 1024 * 1024, 1.5 * V * 10.0 * fs * 4.0);
        const double bytes = cfg_.blockPoolBytes ? static_cast<double>(cfg_.blockPoolBytes) : autoBytes;
        mc.blockPoolBlocks = static_cast<std::size_t>(bytes / (BlockPool::kBlockSize * sizeof(float)));
    }
    engine_ = std::make_unique<MaskEngine>(mc);
    std::string err;
    if (!engine_->prepare(fs, layout, MaskEngine::kCell, &err)) return fail(err);
    if (!engine_->setPlan(plan, 0, &err) || !engine_->rtApplyInitialPlan(&err)) return fail(err);
    babble_ = engine_->babbleMutable() != nullptr;
    babbleStart_ = engine_->babbleStartSample();

    const auto tapFrames = static_cast<std::size_t>(cfg_.tapRingSeconds * fs);
    for (auto& t : taps_) t.prepare(nCh_, tapFrames);
    lastTapDrops_ = 0;
    lastTapOverflowLogged_ = 0;
    tapSink_ = std::make_unique<TapSink>(taps_);
    engine_->setTapSink(tapSink_.get());

    scratch_.assign(static_cast<std::size_t>(std::max(nCh_, nOut_)), std::vector<float>(MaskEngine::kCell, 0.0f));
    ptr_.assign(static_cast<std::size_t>(nCh_), nullptr);
    fadeBuf_.assign(MaskEngine::kCell, 0.0f);
    audio_ = mc.audio;
    fadeGain_ = 0.0;
    fadeInStep_ = 1.0 / std::max(1.0, cfg_.fadeInMs * 1e-3 * fs);
    fadeOutStep_ = 1.0 / std::max(1.0, cfg_.fadeOutMs * 1e-3 * fs);
    mode_.store(static_cast<std::uint8_t>(OutputMode::Silent), std::memory_order_release);
    fadeDone_.store(true, std::memory_order_release);
    lastStartUs_ = 0;
    lastBackendXruns_ = 0;
    for (auto& b : loadHist_) b.store(0, std::memory_order_relaxed);
    loadMax_.store(0.0f, std::memory_order_relaxed);

    gen_ = std::make_shared<Generation>();
    gen_->snap = mc.corpus;
    if (mc.audio) gen_->audio = std::shared_ptr<IAudioSource>(std::shared_ptr<void>(), mc.audio);  // not owned
    {
        std::lock_guard<std::mutex> lk(adoptMutex_);
        adoptQueue_.clear();
        plannedSpeakers_.clear();
        adoptedVersion_ = mc.corpus ? mc.corpus->corpusVersion() : std::string();
    }
    adoptions_.store(0, std::memory_order_release);
    adoptPending_.store(false, std::memory_order_release);
    {
        std::lock_guard<CheckedMutex> lk(jobsMutex_);
        jobs_.clear();
        freeChains_.clear();
        if (babble_) {
            BabbleEngine* be = engine_->babbleMutable();
            for (std::size_t i = be->numChains(); i-- > 0;) freeChains_.push_back(be->chain(i));
        }
    }
    nextMotionB_ = engine_->motionInterval();
    nextBalanceB_ = fsB_;
    plannerBeatUs_.store(0);
    analysisBeatUs_.store(monotonicMicros());

    quit_.store(false, std::memory_order_release);
    threads_.emplace_back([this] { analysisLoop(); });
    if (babble_) {
        threads_.emplace_back([this] { plannerLoop(); });
        for (int i = 0; i < cfg_.decodeThreads; ++i) threads_.emplace_back([this, i] { decodeLoop(false, i); });
        if (cfg_.urgentLane) threads_.emplace_back([this] { decodeLoop(true, 0); });
        // READY once the events of the first second are preloaded (or after the timeout).
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(static_cast<int>(cfg_.preloadTimeoutS * 1000.0));
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (abort && abort->load(std::memory_order_acquire)) return fail("aborted");
            if (plannerBeatUs_.load() != 0) {
                std::lock_guard<CheckedMutex> lk(jobsMutex_);
                ready = true;
                for (const auto& j : jobs_) {
                    if (j->done || j->dead || j->pe.ev.startSample >= fsB_) continue;
                    if (!j->pushed ||
                        static_cast<std::int64_t>(j->chain->written()) < readyNeed(j->pe.ev, j->len, fsB_)) {
                        ready = false;
                        break;
                    }
                }
                if (ready) break;
            }
            sleepMs(5);
        }
        if (!ready)
            logEvent(LogLevel::Warn, logcode::kPreloadLateStart,
                     "initial preload not complete after the timeout; late events will start late",
                     {{"timeoutS", cfg_.preloadTimeoutS}});
    }
    return true;
}

void RealtimeEngine::stopServices() {
    quit_.store(true, std::memory_order_release);
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

void RealtimeEngine::release() {
    stopServices();
    if (engine_) engine_->setTapSink(nullptr);
    {
        std::lock_guard<CheckedMutex> lk(jobsMutex_);
        jobs_.clear();
        freeChains_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(adoptMutex_);
        adoptQueue_.clear();
    }
    gen_.reset();
    engine_.reset();
    tapSink_.reset();
    locked_.reset();
    babble_ = false;
}

// ------------------------------------------------------------------------------ RT callback

void RealtimeEngine::setOutputMode(OutputMode m) noexcept {
    fadeDone_.store(m == OutputMode::Silent || m == OutputMode::On, std::memory_order_release);
    mode_.store(static_cast<std::uint8_t>(m), std::memory_order_release);
}

bool RealtimeEngine::fadeComplete() const noexcept { return fadeDone_.load(std::memory_order_acquire); }

void RealtimeEngine::audioCallback(float* const* out, int numOutputs, int numFrames) noexcept {
    ScopedRealtimeThread rtMark;
    disableDenormals();
    const std::int64_t t0 = monotonicMicros();
    const int nOut = std::max(0, numOutputs);
    const double bufUs = static_cast<double>(numFrames) / fs_ * 1e6;
    if (lastStartUs_ != 0 && static_cast<double>(t0 - lastStartUs_) > 1.5 * bufUs) {
        gaps_.fetch_add(1, std::memory_order_relaxed);
        if (cfg_.logger)
            cfg_.logger->logRt(RtLogCode::CallbackGap, engine_ ? engine_->rtPosition() : 0,
                               static_cast<double>(t0 - lastStartUs_) * 1e-3, bufUs * 1e-3);
    }
    lastStartUs_ = t0;
    if (const IAudioBackend* be = backend_.load(std::memory_order_acquire)) {
        const std::uint64_t x = be->xrunCount();
        if (x > lastBackendXruns_) {
            xruns_.fetch_add(x - lastBackendXruns_, std::memory_order_relaxed);
            if (cfg_.logger)
                cfg_.logger->logRt(RtLogCode::Xrun, engine_ ? engine_->rtPosition() : 0,
                                   static_cast<double>(x - lastBackendXruns_));
        }
        lastBackendXruns_ = x;
    }

    const auto mode = static_cast<OutputMode>(mode_.load(std::memory_order_acquire));
    const bool silentDone = mode == OutputMode::Silent || (mode == OutputMode::FadeOut && fadeGain_ <= 0.0);
    if (!engine_ || silentDone || numFrames <= 0) {
        for (int c = 0; c < nOut; ++c) std::memset(out[c], 0, static_cast<std::size_t>(std::max(numFrames, 0)) * sizeof(float));
        if (mode == OutputMode::Silent) fadeGain_ = 0.0;
        if (mode == OutputMode::FadeOut) fadeDone_.store(true, std::memory_order_release);
    } else {
        const int N = nCh_;
        int off = 0;
        while (off < numFrames) {
            const int n = std::min(numFrames - off, MaskEngine::kCell);
            for (int c = 0; c < N; ++c)
                ptr_[static_cast<std::size_t>(c)] = c < nOut ? out[c] + off : scratch_[static_cast<std::size_t>(c)].data();
            engine_->process(ptr_.data(), n);
            // Fade gain (linear ramps; fade-in 500 ms, fade-out 300 ms).
            float* g = fadeBuf_.data();
            bool unity = true;
            for (int i = 0; i < n; ++i) {
                if (mode == OutputMode::FadeIn || mode == OutputMode::On) {
                    fadeGain_ = std::min(1.0, fadeGain_ + (mode == OutputMode::On ? 1.0 : fadeInStep_));
                } else if (mode == OutputMode::FadeOut) {
                    fadeGain_ = std::max(0.0, fadeGain_ - fadeOutStep_);
                }
                g[i] = static_cast<float>(fadeGain_);
                unity = unity && fadeGain_ >= 1.0;
            }
            const int nc = std::min(N, nOut);
            if (!unity)
                for (int c = 0; c < nc; ++c)
                    for (int i = 0; i < n; ++i) out[c][off + i] *= g[i];
            for (int c = N; c < nOut; ++c) std::memset(out[c] + off, 0, static_cast<std::size_t>(n) * sizeof(float));
            off += n;
        }
        if (mode == OutputMode::FadeIn && fadeGain_ >= 1.0) fadeDone_.store(true, std::memory_order_release);
        if (mode == OutputMode::FadeOut && fadeGain_ <= 0.0) fadeDone_.store(true, std::memory_order_release);
        const std::uint64_t drops = taps_[0].droppedFrames() + taps_[1].droppedFrames() + taps_[2].droppedFrames() +
                                    taps_[3].droppedFrames();
        if (drops > lastTapDrops_ && cfg_.logger)
            cfg_.logger->logRt(RtLogCode::TapOverflow, engine_->rtPosition(), static_cast<double>(drops - lastTapDrops_));
        lastTapDrops_ = drops;
    }

    const std::int64_t t1 = monotonicMicros();
    const double load = bufUs > 0.0 ? static_cast<double>(t1 - t0) / bufUs : 0.0;
    const auto bucket = static_cast<std::size_t>(std::clamp(load * 128.0, 0.0, 255.0));
    loadHist_[bucket].fetch_add(1, std::memory_order_relaxed);
    if (static_cast<float>(load) > loadMax_.load(std::memory_order_relaxed))
        loadMax_.store(static_cast<float>(load), std::memory_order_relaxed);
    if (load > 0.9) {
        overruns_.fetch_add(1, std::memory_order_relaxed);
        if (cfg_.logger) cfg_.logger->logRt(RtLogCode::Overrun, engine_ ? engine_->rtPosition() : 0, load);
    }
    ftz_.store(denormalsDisabled(), std::memory_order_relaxed);
    lastCallbackUs_.store(t1, std::memory_order_release);
    callbacks_.fetch_add(1, std::memory_order_release);
}

void RealtimeEngine::setStrengthDb(double db) noexcept {
    if (engine_) engine_->rtSetStrengthDb(db);
}
void RealtimeEngine::setBabbleFraction(double b) noexcept {
    if (engine_) engine_->rtSetBabbleFraction(std::clamp(b, 0.0, 1.0));
}
void RealtimeEngine::setLimiterCeilingDb(double db) noexcept {
    if (engine_) engine_->rtSetLimiterCeilingDb(db);
}

// ------------------------------------------------------------------------------ preload

std::int64_t RealtimeEngine::jobTarget(const Job& j, std::int64_t nowB) const noexcept {
    const TalkerEvent& ev = j.pe.ev;
    if (!j.started && nowB < ev.startSample)
        return std::min(j.len, ev.fadeInLen + static_cast<std::int64_t>(cfg_.upcomingBeyondFadeInS * static_cast<double>(fsB_)));
    const auto readPos = static_cast<std::int64_t>(j.chain->readPosition());
    return std::min(j.len, std::max(readPos + static_cast<std::int64_t>(cfg_.targetAheadS * static_cast<double>(fsB_)), readyNeed(ev, j.len, fsB_)));
}

std::int64_t RealtimeEngine::jobDeadline(const Job& j) const noexcept {
    return j.pe.ev.startSample + j.chainOffset + static_cast<std::int64_t>(j.chain->written());
}

RealtimeEngine::Job* RealtimeEngine::pickJob(bool urgentOnly, std::int64_t nowB) {
    Job* best = nullptr;
    std::int64_t bestDl = 0;
    for (auto& jp : jobs_) {
        Job& j = *jp;
        if (j.busy || j.retired || j.failed || j.dead || j.done) continue;
        if (static_cast<std::int64_t>(j.chain->written()) >= jobTarget(j, nowB)) continue;
        std::int64_t dl = jobDeadline(j);
        if (urgentOnly && dl - nowB >= fsB_) continue;
        // Low-water boost: an active voice with < 3 s buffered goes before upcoming events.
        if ((j.started || nowB >= j.pe.ev.startSample) && dl - nowB < static_cast<std::int64_t>(cfg_.lowWaterS * static_cast<double>(fsB_)))
            dl -= static_cast<std::int64_t>(cfg_.lowWaterS * static_cast<double>(fsB_));
        if (!best || dl < bestDl) {
            best = &j;
            bestDl = dl;
        }
    }
    if (best) best->busy = true;
    return best;
}

void RealtimeEngine::releaseJobLocked(std::size_t idx) {
    Job& j = *jobs_[idx];
    if (j.done) return;
    {
        std::lock_guard<CheckedMutex> pl(poolMutex_);
        j.chain->releaseAll(engine_->babbleMutable()->pool());
    }
    freeChains_.push_back(j.chain);
    j.done = true;
}

void RealtimeEngine::retireChain(BlockChain* chain) {
    for (std::size_t i = 0; i < jobs_.size(); ++i) {
        Job& j = *jobs_[i];
        if (j.chain != chain || j.done) continue;
        if (j.busy)
            j.retired = true;  // the decode worker releases it after its unit
        else
            releaseJobLocked(i);
        return;
    }
}

void RealtimeEngine::decodeLoop(bool urgent, int index) {
    static const char* const names[] = {"bf.preload.0", "bf.preload.1", "bf.preload.2", "bf.preload.3"};
    setCurrentThreadName(urgent ? "bf.preload.u" : names[std::clamp(index, 0, 3)]);
    // Below normal (urgent lane: normal) so preload bursts cannot preempt the audio callback.
    setCurrentThreadPriority(urgent ? ThreadPriority::Normal : ThreadPriority::BelowNormal);
    BlockPool& pool = engine_->babbleMutable()->pool();
    std::vector<float> buf(BlockPool::kBlockSize);
    while (!quit_.load(std::memory_order_acquire)) {
        const std::int64_t nowB = babbleNow();
        Job* j = nullptr;
        std::shared_ptr<const ProcessedLayout> layout;
        std::shared_ptr<Generation> gen;
        std::int64_t from = 0;
        std::size_t k = 0;
        {
            std::lock_guard<CheckedMutex> lk(jobsMutex_);
            j = pickJob(urgent, nowB);
            if (j) {
                gen = j->gen;
                layout = j->pe.layout;
                const auto w = static_cast<std::int64_t>(j->chain->written());
                from = j->chainOffset + w;
                k = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(buf.size()), jobTarget(*j, nowB) - w));
            }
        }
        if (!j) {
            sleepMs(urgent ? cfg_.urgentPollMs : cfg_.decodeIdleMs);
            continue;
        }
        const std::int64_t t0 = monotonicMicros();
        bool ok = true;
        if (layout) {
            // The event's own generation: its source reads the cache files of the snapshot it was planned from.
            IAudioSource* src = gen && gen->audio ? gen->audio.get() : audio_;
            SourcePreparer prep(*src);
            ok = prep.render(*layout, from, buf.data(), k);
        }
        else
            std::fill(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(k), 0.0f);
        const std::int64_t dt = monotonicMicros() - t0;
        latHist_[static_cast<std::size_t>(std::clamp<std::int64_t>(dt / 100, 0, static_cast<std::int64_t>(kLatBins) - 1))]
            .fetch_add(1, std::memory_order_relaxed);
        bool appended = true;
        if (ok) {
            std::lock_guard<CheckedMutex> pl(poolMutex_);
            j->chain->reclaim(pool);
            appended = j->chain->append(pool, buf.data(), k);
        }
        std::lock_guard<CheckedMutex> lk(jobsMutex_);
        j->busy = false;
        if (!ok) {
            j->failed = true;
            sourceFailures_.fetch_add(1, std::memory_order_relaxed);
            engine_->babbleMutable()->markSourceError();
        }
        if (!appended) poolExhausted_.fetch_add(1, std::memory_order_relaxed);
        if (j->retired) {
            for (std::size_t i = 0; i < jobs_.size(); ++i)
                if (jobs_[i].get() == j) releaseJobLocked(i);
        }
        if (!appended) sleepMs(cfg_.decodeIdleMs);
    }
}

// ------------------------------------------------------------------------------ planner

void RealtimeEngine::plannerLoop() {
    setCurrentThreadName("bf.planner");
    while (!quit_.load(std::memory_order_acquire)) {
        plannerStep();
        sleepMs(cfg_.plannerPollMs);
    }
}

void RealtimeEngine::substituteFailed(std::int64_t nowB) {
    BabbleEngine* be = engine_->babbleMutable();
    const CorpusSnapshot& snap = be->selector().snapshot();
    const MaskRenderPlan* plan = engine_->currentPlan();
    const double maxGapMs = plan ? plan->talkers.maxGapMs : 250.0;
    for (std::size_t i = 0; i < jobs_.size(); ++i) {
        Job& j = *jobs_[i];
        if (!j.failed || j.done || j.busy) continue;
        const RecordingId rec = j.pe.ev.recording;
        if (std::find(unhealthy_.begin(), unhealthy_.end(), rec) == unhealthy_.end()) {
            unhealthy_.push_back(rec);
            logEvent(LogLevel::Warn, logcode::kCorpusFileMissing, "source read failed; recording marked unhealthy",
                     {{"recording", rec}, {"speaker", j.pe.ev.speaker}});
        }
        if (j.pushed) {  // RT owns the voice: it will start late / starve and fade out
            j.failed = false;
            j.dead = true;
            continue;
        }
        if (j.gen != gen_) {  // planned from a replaced corpus generation: drop it, the new plan fills the gap
            releaseJobLocked(i);
            continue;
        }
        if (j.substitutions >= 4 || j.pe.ev.endSample <= nowB) {
            releaseJobLocked(i);
            continue;
        }
        // REPLACE (TALKER_ENGINE §3.1): a different recording of the same speaker, else another
        // speaker; the selector stream keeps offline renders deterministic, in real time the
        // substitution breaks determinism (logged).
        std::vector<SpeakerId> active;
        for (const auto& o : jobs_)
            if (!o->done && o.get() != &j && o->pe.ev.startSample < j.pe.ev.endSample &&
                o->pe.ev.endSample > j.pe.ev.startSample)
                active.push_back(o->pe.ev.speaker);
        SegmentSelector& sel = be->selectorMutable();
        std::optional<SegmentPick> pk;
        for (int attempt = 0; attempt < 8 && !pk; ++attempt) {
            pk = attempt < 3 ? sel.pickForSpeaker(j.pe.ev.speaker, j.pe.ev.startSample)
                             : sel.pick(j.pe.ev.startSample, active);
            if (pk && (pk->recording == rec ||
                       std::find(unhealthy_.begin(), unhealthy_.end(), pk->recording) != unhealthy_.end()))
                pk.reset();
        }
        if (!pk) {
            releaseJobLocked(i);
            continue;
        }
        const std::int64_t len = j.pe.ev.length();
        auto layout = std::make_shared<ProcessedLayout>(buildProcessedLayout(
            snap, pk->recording, pk->anchor, static_cast<std::int64_t>(std::llround(maxGapMs * 48.0)), engineToCorpus(len, fsB_) + 1, fsB_));
        sel.commit(*pk, layout->sourcePosAt(std::min(len, layout->length)), j.pe.ev.startSample, j.pe.ev.endSample);
        const double aslOld = snap.segment(j.pe.ev.segment).aslDb, aslNew = snap.segment(pk->segment).aslDb;
        j.pe.ev.segGainLin = static_cast<float>(j.pe.ev.segGainLin * std::pow(10.0, (aslOld - aslNew) / 20.0));
        j.pe.ev.speaker = pk->speaker;
        j.pe.ev.recording = pk->recording;
        j.pe.ev.segment = pk->segment;
        j.pe.ev.anchor = pk->anchor;
        j.pe.layout = std::move(layout);
        {
            std::lock_guard<CheckedMutex> pl(poolMutex_);
            j.chain->releaseAll(be->pool());
        }
        j.chain->reset(static_cast<std::uint64_t>(j.len));
        j.failed = false;
        ++j.substitutions;
        substitutions_.fetch_add(1, std::memory_order_relaxed);
        determinismBroken_.store(true, std::memory_order_relaxed);
        logEvent(LogLevel::Warn, logcode::kPreloadSubstitution, "talker source substituted",
                 {{"eventId", j.pe.ev.eventId}, {"failedRecording", rec}, {"recording", pk->recording},
                  {"speaker", pk->speaker}});
    }
}

void RealtimeEngine::adaptLoad(std::int64_t nowB, std::int64_t nowUs) {
    // TALKER_ENGINE §8.3: > 5 starvations per minute -> lengthen segments (median x1.5) until
    // recovered (no starvation for a minute). Caller holds the service and job locks.
    while (!starveTimesUs_.empty() && nowUs - starveTimesUs_.front() > 60'000'000) starveTimesUs_.pop_front();
    BabbleEngine* be = engine_->babbleMutable();
    const TalkerPlanParams cur = be->planner().params();
    TalkerPlanParams next;
    if (!lengthened_ && starveTimesUs_.size() > 5) {
        normalParams_ = cur;
        next = cur;
        next.medianS = std::min(cur.medianS * 1.5, cur.segMaxS);
    } else if (lengthened_ && starveTimesUs_.empty()) {
        next = normalParams_;
    } else {
        return;
    }
    const auto discarded = be->applyReplanNow(next, nowB);
    for (std::size_t i = 0; i < jobs_.size(); ++i) {
        Job& j = *jobs_[i];
        if (j.done || j.pushed) continue;  // pushed ones are dropped by the renderer (stale epoch)
        if (std::find(discarded.begin(), discarded.end(), j.pe.ev.eventId) == discarded.end()) continue;
        if (j.busy)
            j.retired = true;
        else
            releaseJobLocked(i);
    }
    lengthened_ = !lengthened_;
    segmentsLengthened_.store(lengthened_, std::memory_order_relaxed);
    logEvent(LogLevel::Warn, logcode::kPreloadStarvation,
             lengthened_ ? "sustained starvation: segments lengthened (median x1.5) to reduce preload load"
                         : "preload recovered: normal segment lengths restored",
             {{"medianS", next.medianS}, {"discardedEvents", discarded.size()}});
}

bool RealtimeEngine::adoptCorpus(std::shared_ptr<const CorpusSnapshot> snap, std::shared_ptr<IAudioSource> audio,
                                 bool audioThreadSafe, CorpusMigration migration, std::shared_ptr<void> retirePrevious) {
    if (!engine_ || !babble_ || !snap || !audio) return false;
    PendingAdoption a;
    a.snap = std::move(snap);
    a.audio = std::move(audio);
    a.threadSafe = audioThreadSafe;
    a.migration = std::move(migration);
    a.retirePrevious = std::move(retirePrevious);
    std::lock_guard<std::mutex> lk(adoptMutex_);
    adoptQueue_.push_back(std::make_unique<PendingAdoption>(std::move(a)));
    adoptPending_.store(true, std::memory_order_release);
    return true;
}

std::set<SpeakerId> RealtimeEngine::plannedSpeakers(std::uint64_t generation) const {
    std::lock_guard<std::mutex> lk(adoptMutex_);
    const auto it = plannedSpeakers_.find(generation);
    return it == plannedSpeakers_.end() ? std::set<SpeakerId>{} : it->second;
}

std::string RealtimeEngine::corpusVersion() const {
    std::lock_guard<std::mutex> lk(adoptMutex_);
    return adoptedVersion_;
}

void RealtimeEngine::applyAdoption(PendingAdoption&& a, std::int64_t nowB) {
    BabbleEngine* be = engine_->babbleMutable();
    auto gen = std::make_shared<Generation>();
    gen->snap = a.snap;
    gen->owner = a.audio;
    gen->audio = (a.threadSafe || cfg_.audioSourceThreadSafe) ? a.audio : std::make_shared<SharedLockedSource>(a.audio);
    const std::size_t oldSpeakers = be->snapshot().numSpeakers();
    const auto discarded = be->adoptCorpus(a.snap, a.migration, nowB);
    // Events that have not been handed to RT yet and were re-planned: free their chains (pushed ones
    // are dropped by the renderer: stale epoch). Everything else keeps its old generation.
    for (std::size_t i = 0; i < jobs_.size(); ++i) {
        Job& j = *jobs_[i];
        if (j.done || j.pushed) continue;
        if (std::find(discarded.begin(), discarded.end(), j.pe.ev.eventId) == discarded.end()) continue;
        if (j.busy)
            j.retired = true;
        else
            releaseJobLocked(i);
    }
    if (gen_) gen_->retired = std::move(a.retirePrevious);
    gen_ = std::move(gen);
    unhealthy_.clear();
    determinismBroken_.store(true, std::memory_order_relaxed);  // the timeline now depends on the reload moment
    const std::string version = a.snap->corpusVersion();
    {
        std::lock_guard<std::mutex> lk(adoptMutex_);
        adoptedVersion_ = version;
    }
    adoptions_.fetch_add(1, std::memory_order_release);
    logEvent(LogLevel::Info, logcode::kCorpusLoaded, "voice library reloaded without restart",
             {{"corpusVersion", version},
              {"speakers", a.snap->numSpeakers()},
              {"recordings", a.snap->numRecordings()},
              {"previousSpeakers", oldSpeakers},
              {"keptSpeakers", a.migration.keptSpeakers},
              {"keptRecordings", a.migration.keptRecordings},
              {"replannedEvents", discarded.size()},
              {"reload", true}});
}

void RealtimeEngine::plannerStep() {
    BabbleEngine* be = engine_->babbleMutable();
    if (!be) return;
    const std::int64_t nowB = babbleNow();
    std::lock_guard<std::mutex> svc(engine_->rtServiceMutex());
    std::lock_guard<CheckedMutex> lk(jobsMutex_);
    VoiceRenderer& r = be->renderer();

    BlockChain* ch = nullptr;
    while (r.popFinished(ch)) retireChain(ch);

    VoiceFeedback fb;
    while (r.popFeedback(fb)) {
        const nlohmann::json d = {{"eventId", fb.eventId}, {"slot", fb.slot}, {"recording", fb.recording}};
        switch (fb.kind) {
        case VoiceFeedback::Started:
            for (auto& j : jobs_)
                if (j->pe.ev.eventId == fb.eventId && !j->done) j->started = true;
            break;
        case VoiceFeedback::LateStart:
            lateStarts_.fetch_add(1, std::memory_order_relaxed);
            if (!determinismBroken_.exchange(true))
                logEvent(LogLevel::Warn, logcode::kDeterminismBroken, "late start: real-time output no longer bit-exact", d);
            logEvent(LogLevel::Warn, logcode::kPreloadLateStart, "talker event postponed (source not preloaded)", d);
            break;
        case VoiceFeedback::Starvation:
            starvations_.fetch_add(1, std::memory_order_relaxed);
            starveTimesUs_.push_back(monotonicMicros());
            determinismBroken_.store(true);
            logEvent(LogLevel::Warn, logcode::kPreloadStarvation, "talker event dropped (not preloaded after 1 s)", d);
            break;
        case VoiceFeedback::Underflow:
            underflows_.fetch_add(1, std::memory_order_relaxed);
            determinismBroken_.store(true);
            logEvent(LogLevel::Warn, logcode::kPreloadUnderflow, "active talker underflow: 20 ms fade-out", d);
            break;
        }
    }

    if (adoptPending_.load(std::memory_order_acquire)) {
        for (;;) {
            PendingAdoption a;
            {
                std::lock_guard<std::mutex> al(adoptMutex_);
                if (adoptQueue_.empty()) {
                    adoptPending_.store(false, std::memory_order_release);
                    break;
                }
                a = std::move(*adoptQueue_.front());
                adoptQueue_.pop_front();
            }
            applyAdoption(std::move(a), nowB);
        }
    }
    substituteFailed(nowB);
    adaptLoad(nowB, monotonicMicros());

    TalkerPlanner& pl = be->plannerMutable();
    pl.planUntil(nowB + static_cast<std::int64_t>(cfg_.lookaheadS * static_cast<double>(fsB_)));
    newEvents_.clear();
    pl.takeNew(newEvents_);
    const std::int64_t nowUs = monotonicMicros();
    for (auto& pe : newEvents_) {
        planned_.fetch_add(1, std::memory_order_relaxed);
        if (pe.ev.endSample <= nowB) continue;
        if (freeChains_.empty()) {
            noChain_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        auto job = std::make_unique<Job>();
        job->gen = gen_;
        job->chain = freeChains_.back();
        freeChains_.pop_back();
        job->chainOffset = std::max<std::int64_t>(0, nowB - pe.ev.startSample);
        job->len = pe.ev.length() - job->chainOffset;
        job->chain->reset(static_cast<std::uint64_t>(job->len));
        job->needSinceUs = nowUs;
        {
            std::lock_guard<std::mutex> al(adoptMutex_);
            plannedSpeakers_[adoptions_.load(std::memory_order_relaxed)].insert(pe.ev.speaker);
        }
        job->pe = std::move(pe);
        jobs_.push_back(std::move(job));
    }

    // Hand events to RT shortly before their start (placement = spatial gains at that time).
    const auto window = static_cast<std::int64_t>(cfg_.pushWindowS * static_cast<double>(fsB_));
    for (std::size_t i = 0; i < jobs_.size(); ++i) {
        Job& j = *jobs_[i];
        if (j.pushed || j.done || j.dead || j.failed || j.retired) continue;
        if (j.pe.ev.startSample - nowB >= window) continue;
        if (j.pe.ev.endSample <= nowB) {
            if (!j.busy) releaseJobLocked(i);
            continue;
        }
        VoiceEvent ve;
        ve.ev = j.pe.ev;
        ve.chain = j.chain;
        ve.chainOffset = j.chainOffset;
        ve.speech = j.pe.layout ? j.pe.layout->speech.data() : nullptr;
        ve.numSpeech = j.pe.layout ? static_cast<std::uint32_t>(j.pe.layout->speech.size()) : 0;
        ve.hasGains = engine_->rtPlaceEvent(ve.ev, ve.gains.data(), std::min<std::size_t>(ve.gains.size(), static_cast<std::size_t>(nCh_)));
        if (!r.pushEvent(ve)) break;  // FIFO full: next poll
        j.pushed = true;
        pushed_.fetch_add(1, std::memory_order_relaxed);
    }

    // Spatial motion (every motionInterval samples) and feed-forward balance (1 s).
    const std::int64_t mi = engine_->motionInterval();
    if (nowB - nextMotionB_ > 10 * mi) nextMotionB_ = nowB;
    while (nextMotionB_ <= nowB) {
        engine_->rtUpdateMotion(nextMotionB_);
        nextMotionB_ += mi;
    }
    if (nowB - nextBalanceB_ > 10 * fsB_) nextBalanceB_ = nowB;
    while (nextBalanceB_ <= nowB) {
        engine_->rtFeedForwardBalance();
        nextBalanceB_ += fsB_;
    }

    // Metrics: lookahead, buffered audio of active voices, queue age, pool use, corpus health.
    std::int64_t minBuf = -1, oldestNeed = 0;
    for (auto& jp : jobs_) {
        Job& j = *jp;
        if (j.done || j.dead) continue;
        const auto w = static_cast<std::int64_t>(j.chain->written());
        const bool complete = w >= j.len;
        if (j.started && !complete) {
            const std::int64_t buffered = w - static_cast<std::int64_t>(j.chain->readPosition());
            if (minBuf < 0 || buffered < minBuf) minBuf = buffered;
            if (buffered < static_cast<std::int64_t>(cfg_.warningS * static_cast<double>(fsB_))) preloadLow_.fetch_add(1, std::memory_order_relaxed);
        }
        if (!j.busy && !j.failed && w < jobTarget(j, nowB)) {
            oldestNeed = std::max(oldestNeed, nowUs - j.needSinceUs);
        } else {
            j.needSinceUs = nowUs;
        }
    }
    std::erase_if(jobs_, [](const std::unique_ptr<Job>& j) { return j->done; });
    minBufferedS_.store(minBuf < 0 ? cfg_.targetAheadS : static_cast<double>(minBuf) / static_cast<double>(fsB_), std::memory_order_relaxed);
    oldestNeedAgeS_.store(static_cast<double>(oldestNeed) * 1e-6, std::memory_order_relaxed);
    lookaheadS_.store(static_cast<double>(pl.frontier() - nowB) / static_cast<double>(fsB_), std::memory_order_relaxed);

    const CorpusSnapshot& snap = be->selector().snapshot();
    std::uint64_t poolRecs = 0, poolBad = 0;
    for (SpeakerId s : be->selector().pool()) {
        if (s >= snap.numSpeakers()) continue;
        const SpeakerRec& sr = snap.speaker(s);
        poolRecs += sr.nRecordings;
        for (RecordingId rr : unhealthy_)
            if (snap.recording(rr).speaker == s) ++poolBad;
    }
    poolRecordings_.store(poolRecs, std::memory_order_relaxed);
    poolUnhealthy_.store(poolBad, std::memory_order_relaxed);
    unhealthyCount_.store(unhealthy_.size(), std::memory_order_relaxed);
    plannerBeatUs_.store(monotonicMicros(), std::memory_order_release);
}

// ------------------------------------------------------------------------------ analysis

void RealtimeEngine::analysisLoop() {
    setCurrentThreadName("bf.analysis");
    setCurrentThreadPriority(ThreadPriority::BelowNormal);
    while (!quit_.load(std::memory_order_acquire)) {
        analysisStep();
        sleepMs(cfg_.analysisPollMs);
    }
}

void RealtimeEngine::analysisStep() {
    if (anaBuf_.size() != static_cast<std::size_t>(nCh_)) {
        anaBuf_.assign(static_cast<std::size_t>(nCh_), std::vector<float>(MaskEngine::kCell));
        anaPtr_.resize(static_cast<std::size_t>(nCh_));
        for (std::size_t c = 0; c < anaPtr_.size(); ++c) anaPtr_[c] = anaBuf_[c].data();
    }
    std::vector<float*>& p = anaPtr_;
    for (int tap = 0; tap < kNumTaps; ++tap) {
        TapRing& ring = taps_[static_cast<std::size_t>(tap)];
        bool more = true;
        while (more && !quit_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> svc(engine_->rtServiceMutex());
            for (int k = 0; k < 64; ++k) {
                TapRing::Chunk c;
                if (!ring.peek(c) || c.frames > MaskEngine::kCell || !ring.read(c, p.data())) {
                    more = false;
                    break;
                }
                engine_->rtAnalyzeTap(tap, c.start, p.data(), nCh_, static_cast<int>(c.frames));
                chunks_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    {
        std::lock_guard<std::mutex> svc(engine_->rtServiceMutex());
        engine_->rtCollectGarbage();
    }
    const std::uint64_t drops = taps_[0].droppedFrames() + taps_[1].droppedFrames() + taps_[2].droppedFrames() +
                                taps_[3].droppedFrames();
    if (drops > lastTapOverflowLogged_) {
        logEvent(LogLevel::Info, logcode::kAnalysisTapOverflow, "analysis late: tap audio dropped (window incomplete)",
                 {{"droppedFrames", drops}});
        lastTapOverflowLogged_ = drops;
    }
    analysisBeatUs_.store(monotonicMicros(), std::memory_order_release);
}

// ------------------------------------------------------------------------------ observation

RtMetrics RealtimeEngine::metrics() const {
    RtMetrics m;
    m.callbacks = callbacks_.load(std::memory_order_acquire);
    m.xruns = xruns_.load(std::memory_order_relaxed);
    m.overruns = overruns_.load(std::memory_order_relaxed);
    m.callbackGaps = gaps_.load(std::memory_order_relaxed);
    std::uint64_t total = 0;
    std::array<std::uint64_t, 256> h{};
    for (std::size_t i = 0; i < 256; ++i) total += h[i] = loadHist_[i].load(std::memory_order_relaxed);
    auto pct = [&](double q) {
        if (total == 0) return 0.0;
        std::uint64_t acc = 0;
        for (std::size_t i = 0; i < 256; ++i) {
            acc += h[i];
            if (static_cast<double>(acc) >= q * static_cast<double>(total)) return static_cast<double>(i + 1) / 128.0;
        }
        return 2.0;
    };
    m.dspLoadP50 = pct(0.5);
    m.dspLoadP99 = pct(0.99);
    m.dspLoadMax = loadMax_.load(std::memory_order_relaxed);
    m.lastCallbackMonoUs = lastCallbackUs_.load(std::memory_order_acquire);
    m.position = engine_ ? engine_->rtPosition() : 0;
    for (const auto& t : taps_) m.tapOverflowFrames += t.droppedFrames();
    m.denormalsDisabled = ftz_.load(std::memory_order_relaxed);
    m.plannerHeartbeatUs = plannerBeatUs_.load(std::memory_order_acquire);
    m.lookaheadS = lookaheadS_.load(std::memory_order_relaxed);
    m.eventsPlanned = planned_.load(std::memory_order_relaxed);
    m.eventsPushed = pushed_.load(std::memory_order_relaxed);
    m.eventsDroppedNoChain = noChain_.load(std::memory_order_relaxed);
    m.lateStarts = lateStarts_.load(std::memory_order_relaxed);
    m.starvations = starvations_.load(std::memory_order_relaxed);
    m.underflows = underflows_.load(std::memory_order_relaxed);
    m.substitutions = substitutions_.load(std::memory_order_relaxed);
    m.sourceFailures = sourceFailures_.load(std::memory_order_relaxed);
    m.unhealthyRecordings = unhealthyCount_.load(std::memory_order_relaxed);
    m.poolRecordings = poolRecordings_.load(std::memory_order_relaxed);
    m.poolUnhealthyRecordings = poolUnhealthy_.load(std::memory_order_relaxed);
    std::uint64_t ltot = 0;
    for (const auto& b : latHist_) ltot += b.load(std::memory_order_relaxed);
    if (ltot > 0) {
        std::uint64_t acc = 0;
        for (std::size_t i = 0; i < kLatBins; ++i) {
            acc += latHist_[i].load(std::memory_order_relaxed);
            if (static_cast<double>(acc) >= 0.99 * static_cast<double>(ltot)) {
                m.p99ReadLatencyMs = static_cast<double>(i + 1) * 0.1;
                break;
            }
        }
    }
    m.minBufferedS = minBufferedS_.load(std::memory_order_relaxed);
    m.oldestNeedAgeS = oldestNeedAgeS_.load(std::memory_order_relaxed);
    m.poolExhausted = poolExhausted_.load(std::memory_order_relaxed);
    m.preloadLow = preloadLow_.load(std::memory_order_relaxed);
    m.analysisHeartbeatUs = analysisBeatUs_.load(std::memory_order_acquire);
    m.analysedChunks = chunks_.load(std::memory_order_relaxed);
    m.determinismBroken = determinismBroken_.load(std::memory_order_relaxed);
    m.segmentsLengthened = segmentsLengthened_.load(std::memory_order_relaxed);
    m.babble = babble_;
    if (babble_ && engine_) {
        std::lock_guard<CheckedMutex> pl(poolMutex_);
        const BlockPool& pool = const_cast<MaskEngine*>(engine_.get())->babbleMutable()->pool();
        m.blockPoolUsedPct = 100.0 * (1.0 - static_cast<double>(pool.freeBlocks()) / static_cast<double>(std::max<std::size_t>(pool.capacity(), 1)));
    }
    return m;
}

MaskStatistics RealtimeEngine::statistics() const { return engine_ ? engine_->statistics() : MaskStatistics{}; }

std::vector<SpeakerId> RealtimeEngine::selectorPool() const {
    if (!engine_ || !babble_) return {};
    std::lock_guard<std::mutex> svc(engine_->rtServiceMutex());
    return engine_->babble()->selector().pool();
}

nlohmann::json RealtimeEngine::exportSelectorState() const {
    if (!engine_ || !babble_) return nlohmann::json();
    std::lock_guard<std::mutex> svc(engine_->rtServiceMutex());
    return engine_->babble()->selector().exportState(engine_->rtPosition() - babbleStart_);
}

bool RealtimeEngine::importSelectorState(const nlohmann::json& j) {
    if (!engine_ || !babble_) return false;
    std::lock_guard<std::mutex> svc(engine_->rtServiceMutex());
    return engine_->babbleMutable()->selectorMutable().importState(j, engine_->rtPosition() - babbleStart_);
}

OperatingBands RealtimeEngine::correction() const {
    if (!engine_) return OperatingBands{};
    std::lock_guard<std::mutex> svc(engine_->rtServiceMutex());
    return engine_->correction();
}

}  // namespace bf::rt
