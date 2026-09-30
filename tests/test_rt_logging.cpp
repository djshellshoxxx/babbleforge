// JSON Lines logging (RELIABILITY.md §5): entry fields, levels, rotation 10 MB x 10 (scaled
// down), RT POD FIFO, rate limiting, path redaction, required event codes.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/rt/Logging.h"

using namespace bf::rt;

namespace {
std::filesystem::path freshDir(const std::string& name) {
    const std::filesystem::path d = std::filesystem::path(BF_TEST_OUT_DIR) / name;
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d);
    return d;
}

std::vector<nlohmann::json> readLines(const std::filesystem::path& dir) {
    std::vector<nlohmann::json> r;
    for (const auto& de : std::filesystem::directory_iterator(dir)) {
        std::ifstream in(de.path());
        std::string line;
        while (std::getline(in, line))
            if (!line.empty()) r.push_back(nlohmann::json::parse(line));
    }
    return r;
}
}  // namespace

TEST_CASE("Logger: JSON Lines entries carry the required fields", "[rt][log]") {
    LoggerConfig lc;
    lc.dir = freshDir("log_fields");
    Logger log(lc);
    log.start();
    log.setEngineSampleSource([] { return std::int64_t{4800}; });
    setCurrentThreadName("bf.test");
    log.log(LogLevel::Info, logcode::kEngineStart, "hello", {{"k", 1}});
    log.log(LogLevel::Debug, "dbg.hidden", "below the INFO default level");
    log.logRt(RtLogCode::Xrun, 96000, 3.0);
    log.flush();
    const auto lines = readLines(lc.dir);
    REQUIRE(lines.size() == 2);
    for (const auto& j : lines)
        for (const char* k : {"ts", "mono", "engineSample", "level", "code", "thread", "msg", "data"}) CHECK(j.contains(k));
    bool sawStart = false, sawXrun = false;
    for (const auto& j : lines) {
        if (j["code"] == logcode::kEngineStart) {
            sawStart = true;
            CHECK(j["thread"] == "bf.test");
            CHECK(j["engineSample"] == 4800);
            CHECK(j["level"] == "INFO");
            CHECK(j["data"]["k"] == 1);
            const std::string ts = j["ts"];
            CHECK(ts.size() == 24);
            CHECK(ts.back() == 'Z');
        }
        if (j["code"] == logcode::kAudioXrun) {
            sawXrun = true;
            CHECK(j["engineSample"] == 96000);
            CHECK(j["level"] == "WARN");
            CHECK(j["data"]["args"][0] == 3.0);
        }
    }
    CHECK(sawStart);
    CHECK(sawXrun);
    const auto files = log.files();
    REQUIRE(files.size() == 1);
    CHECK(files[0].filename().string().rfind("babbleforge-", 0) == 0);
    CHECK(files[0].extension() == ".jsonl");
    log.stop();
}

TEST_CASE("Logger: rotation at maxFileBytes keeps at most maxFiles files", "[rt][log]") {
    LoggerConfig lc;
    lc.dir = freshDir("log_rotation");
    lc.maxFileBytes = 4096;
    lc.maxFiles = 4;
    Logger log(lc);
    log.start();
    const std::string pad(150, 'x');
    for (int i = 0; i < 400; ++i) log.log(LogLevel::Info, logcode::kEngineState, pad, {{"i", i}});
    log.flush();
    const auto files = log.files();
    CHECK(files.size() == 4);
    CHECK(log.rotations() >= 10);
    for (const auto& f : files) CHECK(std::filesystem::file_size(f) <= 4096);
    // The newest entries survive; the oldest were deleted with the oldest files.
    const auto lines = readLines(lc.dir);
    int maxI = -1, minI = 1 << 30;
    for (const auto& j : lines) {
        maxI = std::max(maxI, j["data"]["i"].get<int>());
        minI = std::min(minI, j["data"]["i"].get<int>());
    }
    CHECK(maxI == 399);
    CHECK(minI > 0);
    CHECK(log.written() == 400);
    log.stop();
}

TEST_CASE("Logger: rate limiting, RT FIFO overflow, redaction, disk-less mode", "[rt][log]") {
    LoggerConfig lc;
    lc.dir = freshDir("log_rate");
    lc.rateLimitWindowS = 3600.0;
    lc.redactPathsInLogs = true;
    Logger log(lc);
    log.start();
    for (int i = 0; i < 30; ++i) log.logRt(RtLogCode::Xrun, i);
    log.flush();
    CHECK(log.countOf(logcode::kAudioXrun) == 10);  // first 10 pass
    CHECK(log.suppressed() == 20);
    log.log(LogLevel::Warn, logcode::kCorpusFileMissing, "cannot open /home/alice/corpus/spk01/rec02.flac",
            {{"path", "C:\\Users\\alice\\voices\\a.flac"}});
    log.flush();
    const auto rec = log.recent(LogLevel::Warn);
    REQUIRE_FALSE(rec.empty());
    const auto& last = rec.back();
    const std::string msg = last["msg"];
    CHECK(msg.find("alice") == std::string::npos);
    CHECK(msg.find(".flac") != std::string::npos);
    CHECK(last["data"]["path"].get<std::string>().find("alice") == std::string::npos);
    CHECK(Logger::redactPaths("no paths here") == "no paths here");
    log.stop();

    // RT FIFO overflow is counted, never blocks.
    Logger idle(LoggerConfig{});
    int ok = 0;
    for (int i = 0; i < 1100; ++i) ok += idle.logRt(RtLogCode::Overrun, i) ? 1 : 0;
    CHECK(ok == 1024);
    CHECK(idle.rtDropped() == 76);

    // Required event codes (RELIABILITY §5.2) are defined.
    for (const char* c : {logcode::kEngineStart, logcode::kEngineStop, logcode::kEngineState, logcode::kEngineDegraded,
                          logcode::kEngineRecovered, logcode::kSessionSeed, logcode::kPresetLoad, logcode::kPresetChange,
                          logcode::kPresetInvalid, logcode::kPresetLkgRestored, logcode::kDeviceOpen, logcode::kDeviceClose,
                          logcode::kDeviceLost, logcode::kDeviceReturned, logcode::kDeviceRateChange,
                          logcode::kDeviceBufferChange, logcode::kDeviceOpenFailed, logcode::kCorpusLoaded,
                          logcode::kCorpusFileMissing, logcode::kCorpusDecodeFailed, logcode::kCorpusReduced,
                          logcode::kCorpusInsufficient, logcode::kAudioXrun, logcode::kAudioOverrun,
                          logcode::kPreloadLateStart, logcode::kPreloadStarvation, logcode::kLimiterSustained,
                          logcode::kLimiterOverload, logcode::kOutputClip, logcode::kConfigInvalid, logcode::kConfigClamped,
                          logcode::kSpectrumCorrectionLarge, logcode::kSpectrumDesignDegraded, logcode::kSpectrumEqClamped,
                          logcode::kSpectrumStationaryOffTarget, logcode::kWatchdogStallPrefix})
        CHECK(std::string(c).find('.') != std::string::npos);
}
