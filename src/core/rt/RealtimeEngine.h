#pragma once
// Real-time host of the MaskEngine (docs/REALTIME_ARCHITECTURE.md §2-§7, TALKER_ENGINE.md §2,
// §3.1, §8).
//
// Owns a MaskEngine in real-time mode (MaskEngineConfig::realtime; the offline path used by
// bfrender is untouched) and the threaded services that replace its synchronous parts:
//
//   thread          period  work
//   audio (RT)      device  audioCallback(): FTZ/DAZ, RT marker, sub-blocks <= 256 frames,
//                           fades, DSP-load histogram, xrun / overrun / callback-gap detection
//   bf.planner      20 ms   talker planner + selector (>= 4 s lookahead), spatial placement,
//                           events -> RT (SPSC), RT feedback (late start / starvation /
//                           underflow), substitution of failed sources, motion, balance
//   bf.preload.N    -       decode pool (2 threads): earliest-deadline-first block fills
//                           (upcoming: fadeIn + 2 s, active: 8 s ahead, low water 3 s)
//   bf.preload.u    5 ms    urgent lane: jobs whose data runs out within 1 s
//   bf.analysis     50 ms   tap rings (T1-T4, drop-on-full) -> trim, balance, spectral
//                           correction (stamped to the 256-frame grid), meters, statistics
//
// The logger and watchdog threads belong to the EngineController (they outlive rebuilds).
// Output is deterministic (identical event timeline) up to the first late start / starvation;
// after it `determinismBroken` is set (TALKER_ENGINE.md §8.3).
#include <array>
#include <atomic>
#include <mutex>
#include <set>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/engine/MaskEngine.h"
#include "core/rt/AudioBackend.h"
#include "core/rt/FaultInjection.h"
#include "core/rt/Logging.h"
#include "core/rt/RtCheck.h"
#include "core/rt/TapRing.h"

namespace bf::rt {

struct RealtimeEngineConfig {
    MaskEngineConfig engine;           // `realtime` is forced on
    Logger* logger = nullptr;
    bool audioSourceThreadSafe = false;  // false: decode-pool reads are serialised
    double lookaheadS = 4.5;           // planner horizon (>= 4 s)
    double pushWindowS = 1.0;          // events are handed to RT this long before their start
    int plannerPollMs = 20, analysisPollMs = 50, urgentPollMs = 5, decodeIdleMs = 10;
    int decodeThreads = 2;             // 1..4
    bool urgentLane = true;
    double targetAheadS = 8.0;         // active slots
    double upcomingBeyondFadeInS = 2.0;
    double lowWaterS = 3.0, warningS = 1.0;
    std::size_t blockPoolBytes = 0;    // 0: max(128 MiB, 1.5 x V x 10 s x fs x 4 B)
    double tapRingSeconds = 2.0;
    double preloadTimeoutS = 3.0;      // READY after the first second is preloaded or timeout
    double fadeInMs = 500.0, fadeOutMs = 300.0;
};

enum class OutputMode : std::uint8_t { Silent, FadeIn, On, FadeOut };

// Snapshot of the host counters (any thread).
struct RtMetrics {
    // audio
    std::uint64_t callbacks = 0, xruns = 0, overruns = 0, callbackGaps = 0;
    double dspLoadP50 = 0.0, dspLoadP99 = 0.0, dspLoadMax = 0.0;
    std::int64_t lastCallbackMonoUs = 0;
    std::int64_t position = 0;           // engine samples processed
    std::uint64_t tapOverflowFrames = 0;
    bool denormalsDisabled = false;
    // planner
    std::int64_t plannerHeartbeatUs = 0;
    double lookaheadS = 0.0;
    std::uint64_t eventsPlanned = 0, eventsPushed = 0, eventsDroppedNoChain = 0;
    // preload
    std::uint64_t lateStarts = 0, starvations = 0, underflows = 0, substitutions = 0, sourceFailures = 0;
    std::uint64_t unhealthyRecordings = 0, poolRecordings = 0, poolUnhealthyRecordings = 0;
    double p99ReadLatencyMs = 0.0, minBufferedS = 0.0, blockPoolUsedPct = 0.0, oldestNeedAgeS = 0.0;
    std::uint64_t poolExhausted = 0, preloadLow = 0;
    // analysis
    std::int64_t analysisHeartbeatUs = 0;
    std::uint64_t analysedChunks = 0;
    bool determinismBroken = false;
    bool segmentsLengthened = false;  // sustained starvation: planner load reduced (§8.3)
    bool babble = false;
};

class RealtimeEngine final : public IAudioCallback {
public:
    explicit RealtimeEngine(RealtimeEngineConfig cfg);
    ~RealtimeEngine() override;
    RealtimeEngine(const RealtimeEngine&) = delete;
    RealtimeEngine& operator=(const RealtimeEngine&) = delete;

    // Control thread. Builds the graph for (fs, layout, plan), starts the services and waits for
    // the initial preload (<= preloadTimeoutS; late events then start late). `abort` (optional)
    // cancels the wait (stop() during PREPARING). maxDeviceBlock: largest callback size.
    bool prepare(double fs, const OutputLayout& layout, int deviceOutputs, int maxDeviceBlock,
                 const MaskRenderPlan& plan, std::string* error = nullptr,
                 const std::atomic<bool>* abort = nullptr);
    // Control thread: stops the services and frees the graph (the device must be stopped).
    void release();
    bool prepared() const noexcept { return engine_ != nullptr; }

    // RT.
    void audioCallback(float* const* out, int numOutputs, int numFrames) noexcept override;

    // Control: output fades (Silent = callback running, silence, engine clock stopped).
    void setOutputMode(OutputMode m) noexcept;
    OutputMode outputMode() const noexcept { return static_cast<OutputMode>(mode_.load(std::memory_order_acquire)); }
    bool fadeComplete() const noexcept;  // FadeIn reached On / FadeOut reached silence
    void setXrunSource(const IAudioBackend* backend) noexcept { backend_.store(backend, std::memory_order_release); }

    // Hot reload (control thread): switches the babble engine to a new corpus without a restart.
    // The planner thread adopts the snapshot at its next step (a plan epoch: events starting within
    // 0.5 s keep playing on the old recordings, later ones are re-planned from the new pool, the
    // selector state migrates by `migration`). Running events keep reading their own generation's
    // audio source (kept alive by refcount, so its cache files stay readable) until they end;
    // `retirePrevious` (optional) is attached to the generation being replaced and released with it
    // (e.g. the owner of the old FlacCacheAudioSource). `audioThreadSafe`: the source serialises
    // its own reads. Returns false without a babble corpus. Logs corpus.loaded when adopted.
    bool adoptCorpus(std::shared_ptr<const CorpusSnapshot> snap, std::shared_ptr<IAudioSource> audio, bool audioThreadSafe,
                     CorpusMigration migration, std::shared_ptr<void> retirePrevious = {});
    std::uint64_t corpusAdoptions() const noexcept { return adoptions_.load(std::memory_order_acquire); }
    std::string corpusVersion() const;  // snapshot version the planner currently plans from
    // Speakers of the events planned from corpus generation `g` (0 = the library the engine was
    // prepared with, +1 per adoption); introspection for tests and diagnostics.
    std::set<SpeakerId> plannedSpeakers(std::uint64_t generation) const;

    // Live parameters (any thread).
    void setStrengthDb(double db) noexcept;
    void setBabbleFraction(double b) noexcept;
    void setLimiterCeilingDb(double db) noexcept;

    RtMetrics metrics() const;
    std::int64_t position() const noexcept { return engine_ ? engine_->rtPosition() : 0; }
    MaskStatistics statistics() const;
    MaskEngine* engine() noexcept { return engine_.get(); }
    const MaskEngine* engine() const noexcept { return engine_.get(); }
    double sampleRate() const noexcept { return fs_; }
    int deviceOutputs() const noexcept { return nOut_; }
    int maxDeviceBlock() const noexcept { return maxBlock_; }
    const RealtimeEngineConfig& config() const noexcept { return cfg_; }
    std::vector<SpeakerId> selectorPool() const;
    nlohmann::json exportSelectorState() const;
    bool importSelectorState(const nlohmann::json& j);
    OperatingBands correction() const;

private:
    struct Job;
    struct Generation;
    struct PendingAdoption;
    class TapSink;

    void plannerLoop();
    void decodeLoop(bool urgent, int index);
    void analysisLoop();
    void plannerStep();
    void adaptLoad(std::int64_t nowB, std::int64_t nowUs);
    void analysisStep();
    Job* pickJob(bool urgentOnly, std::int64_t nowB);
    void retireChain(BlockChain* chain);
    void releaseJobLocked(std::size_t idx);
    void substituteFailed(std::int64_t nowB);
    std::int64_t babbleNow() const noexcept;
    std::int64_t jobTarget(const Job& j, std::int64_t nowB) const noexcept;
    std::int64_t jobDeadline(const Job& j) const noexcept;
    void logEvent(LogLevel l, const char* code, std::string msg, nlohmann::json data = nlohmann::json::object());
    void stopServices();
    void applyAdoption(PendingAdoption&& a, std::int64_t nowB);  // planner thread, service + job locks held

    RealtimeEngineConfig cfg_;
    std::unique_ptr<LockedAudioSource> locked_;
    std::unique_ptr<MaskEngine> engine_;
    std::unique_ptr<TapSink> tapSink_;
    std::array<TapRing, kNumTaps> taps_;
    double fs_ = 48000.0;
    std::int64_t fsB_ = 48000;  // babble (planner) clock: engine samples
    int nCh_ = 0, nOut_ = 0, maxBlock_ = 0;
    std::int64_t babbleStart_ = 0;
    bool babble_ = false;

    // RT-owned.
    std::vector<std::vector<float>> scratch_;
    std::vector<float*> ptr_;
    std::vector<float> fadeBuf_;
    IAudioSource* audio_ = nullptr;
    double fadeGain_ = 0.0, fadeInStep_ = 0.0, fadeOutStep_ = 0.0;
    std::int64_t lastStartUs_ = 0;
    std::uint64_t lastBackendXruns_ = 0;
    std::uint64_t lastTapDrops_ = 0;
    double bufferUs_ = 0.0;

    // Shared atomics.
    std::atomic<std::uint8_t> mode_{0};
    std::atomic<bool> fadeDone_{true};
    std::atomic<const IAudioBackend*> backend_{nullptr};
    std::atomic<std::uint64_t> callbacks_{0}, xruns_{0}, overruns_{0}, gaps_{0};
    std::array<std::atomic<std::uint32_t>, 256> loadHist_{};
    std::atomic<float> loadMax_{0.0f};
    std::atomic<std::int64_t> lastCallbackUs_{0};
    std::atomic<bool> ftz_{false};

    // Services.
    std::atomic<bool> quit_{false};
    std::vector<std::thread> threads_;
    mutable CheckedMutex jobsMutex_;          // job table, free chains, health set
    mutable CheckedMutex poolMutex_;          // BlockPool free list (writers)
    std::vector<std::unique_ptr<Job>> jobs_;  // start order
    std::shared_ptr<Generation> gen_;         // current corpus generation (planner thread; jobs hold their own)
    mutable std::mutex adoptMutex_;
    // unique_ptr: PendingAdoption is incomplete here (MSVC's deque needs complete element types).
    std::deque<std::unique_ptr<PendingAdoption>> adoptQueue_;  // guarded by adoptMutex_
    std::string adoptedVersion_;              // guarded by adoptMutex_
    std::map<std::uint64_t, std::set<SpeakerId>> plannedSpeakers_;  // guarded by adoptMutex_
    std::atomic<bool> adoptPending_{false};
    std::atomic<std::uint64_t> adoptions_{0};
    std::vector<BlockChain*> freeChains_;
    std::vector<RecordingId> unhealthy_;
    std::vector<PlannedEvent> newEvents_;
    std::int64_t nextMotionB_ = 0, nextBalanceB_ = 0;
    std::atomic<std::int64_t> plannerBeatUs_{0}, analysisBeatUs_{0};
    std::atomic<double> lookaheadS_{0.0};
    std::atomic<std::uint64_t> planned_{0}, pushed_{0}, noChain_{0}, lateStarts_{0}, starvations_{0}, underflows_{0},
        substitutions_{0}, sourceFailures_{0}, poolExhausted_{0}, preloadLow_{0}, chunks_{0};
    std::atomic<bool> determinismBroken_{false};
    std::atomic<std::uint64_t> poolRecordings_{0}, poolUnhealthy_{0}, unhealthyCount_{0};
    // Read latency histogram (0.1 ms bins up to 1 s) and buffer state (published by the planner).
    std::array<std::atomic<std::uint32_t>, 10000> latHist_{};
    std::atomic<double> minBufferedS_{0.0}, oldestNeedAgeS_{0.0};
    std::uint64_t lastTapOverflowLogged_ = 0;  // analysis thread
    // Sustained starvation (> 5 / min): segments lengthened x1.5 until recovered (§8.3).
    std::deque<std::int64_t> starveTimesUs_;   // planner thread
    bool lengthened_ = false;
    TalkerPlanParams normalParams_;
    std::atomic<bool> segmentsLengthened_{false};
    std::vector<std::vector<float>> anaBuf_;   // analysis thread scratch
    std::vector<float*> anaPtr_;
};

}  // namespace bf::rt
