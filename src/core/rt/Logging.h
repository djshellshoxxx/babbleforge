#pragma once
// Structured JSON Lines logging (docs/RELIABILITY.md §5).
//
//  - Non-RT threads: log() into a bounded MPSC queue (CheckedMutex; drop-oldest on overflow,
//    counted as logDropped).
//  - RT thread: logRt() pushes a POD event (code + engine sample + 4 numeric args) into an
//    SPSC FIFO; the logger thread formats it. Never blocks, never allocates.
//  - Logger thread ("bf.logger"): drains both queues every 100 ms (or on flush()) and writes
//    <dir>/<prefix>-YYYYMMDD.jsonl. At maxFileBytes (10 MB) the file is rotated to
//    <prefix>-YYYYMMDD-HHMMSS-<seq>.jsonl; at most maxFiles (10) files are kept, oldest deleted.
//  - Entry fields: ts (UTC ISO-8601 ms), mono (steady-clock µs), engineSample, level, code,
//    thread, msg, data.
//  - Rate limiting (codes listed in rateLimitedCodes): the first `rateLimitBurst` entries per
//    code and window pass, the rest are counted and summarised once per window
//    ("<n> similar entries suppressed").
//  - Path redaction (redactPathsInLogs): absolute paths in msg / data strings become
//    "<path>/…/<hash8>.<ext>". Exports use redactPaths() regardless (RELIABILITY.md §8).
//  - Disk errors: writing stops (advisory), the in-memory ring of recent WARN+ entries goes on.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/rt/RtCheck.h"
#include "core/rt/SpscFifo.h"

namespace bf::rt {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error, Fatal };
std::string_view toString(LogLevel l) noexcept;

// Required event codes (RELIABILITY.md §5.2) and host-specific ones.
namespace logcode {
inline constexpr const char* kEngineStart = "engine.start";
inline constexpr const char* kEngineStop = "engine.stop";
inline constexpr const char* kEngineState = "engine.state";
inline constexpr const char* kEngineDegraded = "engine.degraded";
inline constexpr const char* kEngineRecovered = "engine.recovered";
inline constexpr const char* kEngineIllegal = "engine.illegalTransition";
inline constexpr const char* kEngineRateUnsupported = "engine.rateUnsupported";
inline constexpr const char* kSessionSeed = "session.seed";
inline constexpr const char* kPresetLoad = "preset.load";
inline constexpr const char* kPresetChange = "preset.change";
inline constexpr const char* kPresetInvalid = "preset.invalid";
inline constexpr const char* kPresetLkgRestored = "preset.lkgRestored";
inline constexpr const char* kPresetLkgPromoted = "preset.lkgPromoted";
inline constexpr const char* kDeviceOpen = "device.open";
inline constexpr const char* kDeviceClose = "device.close";
inline constexpr const char* kDeviceLost = "device.lost";
inline constexpr const char* kDeviceReturned = "device.returned";
inline constexpr const char* kDeviceRateChange = "device.rateChange";
inline constexpr const char* kDeviceBufferChange = "device.bufferChange";
inline constexpr const char* kDeviceOpenFailed = "device.openFailed";
inline constexpr const char* kCorpusLoaded = "corpus.loaded";
inline constexpr const char* kCorpusFileMissing = "corpus.fileMissing";
inline constexpr const char* kCorpusDecodeFailed = "corpus.decodeFailed";
inline constexpr const char* kCorpusReduced = "corpus.reduced";
inline constexpr const char* kCorpusInsufficient = "corpus.insufficient";
inline constexpr const char* kAudioXrun = "audio.xrun";
inline constexpr const char* kAudioOverrun = "audio.overrun";
inline constexpr const char* kAudioCallbackGap = "audio.callbackGap";
inline constexpr const char* kPreloadLateStart = "preload.lateStart";
inline constexpr const char* kPreloadStarvation = "preload.starvation";
inline constexpr const char* kPreloadUnderflow = "preload.underflow";
inline constexpr const char* kPreloadLow = "preload.low";
inline constexpr const char* kPreloadSubstitution = "preload.substitution";
inline constexpr const char* kLimiterSustained = "limiter.sustained";
inline constexpr const char* kLimiterOverload = "limiter.overload";
inline constexpr const char* kOutputClip = "output.clip";
inline constexpr const char* kConfigInvalid = "config.invalid";
inline constexpr const char* kConfigClamped = "config.clamped";
inline constexpr const char* kSpectrumCorrectionLarge = "spectrum.correctionLarge";
inline constexpr const char* kSpectrumDesignDegraded = "spectrum.designDegraded";
inline constexpr const char* kSpectrumEqClamped = "spectrum.eqClamped";
inline constexpr const char* kSpectrumStationaryOffTarget = "spectrum.stationaryOffTarget";
inline constexpr const char* kWatchdogStallPrefix = "watchdog.stall.";  // + thread name
inline constexpr const char* kAnalysisTapOverflow = "analysis.tapOverflow";
inline constexpr const char* kPersistWrite = "persist.write";
inline constexpr const char* kDeterminismBroken = "session.determinismBroken";
inline constexpr const char* kLogSuppressed = "log.suppressed";
}  // namespace logcode

// RT -> logger POD event.
enum class RtLogCode : std::uint16_t { Xrun, Overrun, CallbackGap, LateStart, Starvation, Underflow, TapOverflow };
struct RtLogEvent {
    RtLogCode code = RtLogCode::Xrun;
    std::int64_t engineSample = 0;
    std::int64_t monoUs = 0;
    double args[4] = {0.0, 0.0, 0.0, 0.0};
};
static_assert(sizeof(RtLogEvent) <= 128);

struct LoggerConfig {
    std::filesystem::path dir;               // empty: no files, in-memory ring only
    std::string filePrefix = "babbleforge";
    std::uint64_t maxFileBytes = 10ull * 1024 * 1024;
    int maxFiles = 10;
    LogLevel minLevel = LogLevel::Info;
    bool redactPathsInLogs = false;
    std::size_t queueCapacity = 8192;
    int pollMs = 100;
    int rateLimitBurst = 10;
    double rateLimitWindowS = 60.0;
    std::set<std::string> rateLimitedCodes = {
        logcode::kAudioXrun,         logcode::kAudioOverrun,       logcode::kAudioCallbackGap,
        logcode::kPreloadLateStart,  logcode::kPreloadStarvation,  logcode::kPreloadUnderflow,
        logcode::kCorpusFileMissing, logcode::kCorpusDecodeFailed, logcode::kAnalysisTapOverflow,
        logcode::kPreloadSubstitution};
    std::size_t recentCapacity = 200;
};

// Names the calling thread for the log's "thread" field (static string).
void setCurrentThreadName(const char* name) noexcept;
const char* currentThreadName() noexcept;
std::int64_t monotonicMicros() noexcept;
std::string isoUtcNow();

class Logger {
public:
    explicit Logger(LoggerConfig cfg = {});
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void start();
    void stop();  // drains and joins

    // Any non-RT thread.
    void log(LogLevel level, std::string code, std::string msg, nlohmann::json data = nlohmann::json::object(),
             std::int64_t engineSample = -1);
    // RT thread (single producer at a time). False if the FIFO was full (counted).
    bool logRt(RtLogCode code, std::int64_t engineSample, double a0 = 0.0, double a1 = 0.0, double a2 = 0.0,
               double a3 = 0.0) noexcept;
    // Blocks until everything queued so far is written (non-RT).
    void flush();
    // Engine sample source for non-RT entries without an explicit sample.
    void setEngineSampleSource(std::function<std::int64_t()> src);
    void setMinLevel(LogLevel l) noexcept { minLevel_.store(l, std::memory_order_relaxed); }
    LogLevel minLevel() const noexcept { return minLevel_.load(std::memory_order_relaxed); }

    // Observation.
    std::vector<nlohmann::json> recent(LogLevel minLevel = LogLevel::Warn) const;  // WARN+ ring
    std::vector<std::filesystem::path> files() const;  // log files, oldest first
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t rtDropped() const noexcept { return rtDropped_.load(std::memory_order_relaxed); }
    std::uint64_t suppressed() const noexcept { return suppressed_.load(std::memory_order_relaxed); }
    std::uint64_t written() const noexcept { return written_.load(std::memory_order_relaxed); }
    std::uint64_t rotations() const noexcept { return rotations_.load(std::memory_order_relaxed); }
    bool diskError() const noexcept { return diskError_.load(std::memory_order_relaxed); }
    double queueFill() const;  // 0..1
    std::int64_t lastDrainMonoUs() const noexcept { return lastDrain_.load(std::memory_order_relaxed); }
    std::uint64_t countOf(const std::string& code) const;  // entries accepted per code (tests)
    const LoggerConfig& config() const noexcept { return cfg_; }

    static std::string redactPaths(std::string_view s);
    static void redactJson(nlohmann::json& j);

private:
    struct Entry {
        LogLevel level = LogLevel::Info;
        std::string code, msg, thread, ts;
        nlohmann::json data;
        std::int64_t engineSample = -1, mono = 0;
    };
    void run();
    void drain();
    void write(Entry& e);
    bool admit(const Entry& e, std::int64_t mono);
    void flushSuppressed(std::int64_t mono, bool force);
    void openFile();
    void rotate();
    void prune();
    Entry fromRt(const RtLogEvent& ev) const;

    LoggerConfig cfg_;
    std::atomic<LogLevel> minLevel_;
    mutable CheckedMutex qMutex_;
    std::deque<Entry> queue_;
    std::unique_ptr<SpscFifo<RtLogEvent, 1024>> rtFifo_;
    std::condition_variable_any cv_;
    std::thread thread_;
    std::atomic<bool> quit_{false}, running_{false};
    std::uint64_t enqueued_ = 0;       // under qMutex_
    std::uint64_t processed_ = 0;      // under qMutex_
    std::condition_variable_any doneCv_;
    std::atomic<bool> flushReq_{false};

    // Logger-thread state.
    std::ofstream file_;
    std::filesystem::path filePath_;
    std::string fileDay_;
    std::uint64_t fileBytes_ = 0;
    std::uint64_t rotSeq_ = 0;
    struct RateState {
        std::int64_t windowStart = 0;
        int count = 0;
        std::uint64_t suppressed = 0;
    };
    std::map<std::string, RateState> rate_;

    CheckedMutex drainMutex_;  // one drain at a time (logger thread or synchronous mode)
    mutable CheckedMutex recentMutex_;
    std::deque<nlohmann::json> recent_;
    std::map<std::string, std::uint64_t> perCode_;
    std::function<std::int64_t()> sampleSrc_;
    CheckedMutex srcMutex_;

    std::atomic<std::uint64_t> dropped_{0}, rtDropped_{0}, suppressed_{0}, written_{0}, rotations_{0};
    std::atomic<bool> diskError_{false};
    std::atomic<std::int64_t> lastDrain_{0};
};

}  // namespace bf::rt
