// EngineController on the NullBackend (RELIABILITY.md §1-§6, PRESETS.md §9): device loss and
// return (never another device), callback stall, device-initiated rate change, fallback
// policies for failing sources, slow sources (late starts), live preset changes, persistence
// (LKG restore / promotion) and the diagnostic snapshot. A 10-minute soak is tagged [.long].
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "core/config/AtomicFile.h"
#include "core/rt/EngineController.h"
#include "rt_fixtures.h"

using namespace bf;
using namespace bf::rt;
using S = EngineState;

namespace {

std::filesystem::path freshDir(const std::string& name) {
    const std::filesystem::path d = std::filesystem::path(BF_TEST_OUT_DIR) / name;
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d);
    return d;
}

EngineControllerConfig baseConfig(NullBackend& backend, const nlohmann::json& preset, std::uint64_t seed = 5) {
    EngineControllerConfig cc;
    cc.dataSet = &bftest::dataSet();
    cc.backend = &backend;
    cc.deviceId = backend.devices().front().id;
    cc.bufferFrames = 512;
    cc.seed = seed;
    cc.preset = preset;
    cc.engine.blockPoolBytes = 32u << 20;
    cc.degradedHysteresisS = 2.0;
    return cc;
}

void withCorpus(EngineControllerConfig& cc, IAudioSource& audio, bool threadSafe) {
    cc.corpus = bftest::shapedCorpus().snapshot();
    cc.audio = &audio;
    cc.audioThreadSafe = threadSafe;
}

bool historyHas(const EngineController& ctl, S state) {
    for (const auto& e : ctl.statusHistory())
        if (e.state == state) return true;
    return false;
}

}  // namespace

TEST_CASE("Device lost -> DEVICE_LOST within 2 s; only the same device is ever reopened", "[rt][controller][device]") {
    NullBackend backend;
    backend.addDevice(AudioDeviceInfo{"Null:Other Output:8", "Other Output", "Null", 8});
    auto cc = baseConfig(backend, bftest::presetDoc("office", "speech_noise"));
    const std::string dev = cc.deviceId;
    EngineController ctl(cc);
    REQUIRE(ctl.start() == CommandResult::Ok);
    REQUIRE(ctl.waitForState(S::Running, 5.0));

    const auto t0 = std::chrono::steady_clock::now();
    backend.injectDeviceLost();
    REQUIRE(ctl.waitForState(S::DeviceLost, 2.0));
    const double lostAfter = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(lostAfter < 2.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(ctl.state() == S::DeviceLost);  // no automatic switch to "Other Output"
    CHECK(ctl.statusHistory().back().headline == "OUTPUT DEVICE LOST");

    // The same device returns: autoReconnectSameDevice -> PREPARING -> ... -> RUNNING.
    backend.injectDeviceReturn();
    REQUIRE(ctl.waitForState(S::Running, 5.0));

    // Driver stall: no callback for 2 s while RUNNING -> treated as device failure.
    backend.injectStall(4000);
    REQUIRE(ctl.waitForState(S::DeviceLost, 4.5));
    CHECK(ctl.stop() == CommandResult::Ok);
    REQUIRE(ctl.waitForState(S::Stopped, 2.0));

    for (const auto& id : backend.openHistory()) CHECK(id == dev);
    CHECK(backend.openHistory().size() == 2);
    ctl.logger().flush();
    CHECK(ctl.logger().countOf(logcode::kDeviceLost) >= 2);
    CHECK(ctl.logger().countOf(logcode::kDeviceReturned) == 1);
    INFO("device lost detected after " << lostAfter << " s");
}

TEST_CASE("Sample-rate change 48 kHz -> 44.1 kHz rebuilds and is RUNNING within 3 s (stationary)", "[rt][controller][device]") {
    NullBackend backend;
    auto cc = baseConfig(backend, bftest::presetDoc("office", "speech_noise"));
    EngineController ctl(cc);
    REQUIRE(ctl.start() == CommandResult::Ok);
    REQUIRE(ctl.waitForState(S::Running, 5.0));
    CHECK(ctl.sampleRate() == 48000.0);
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t h0 = ctl.statusHistory().size();
    backend.injectRateChange(44100.0);
    REQUIRE(bftest::waitUntil([&] { return ctl.statusHistory().size() > h0 + 3 && ctl.state() == S::Running; }, 3.0));
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(t < 3.0);
    CHECK(ctl.sampleRate() == 44100.0);
    CHECK(ctl.degradedReasons() == 0u);
    // STOPPING -> PREPARING -> READY -> STARTING -> RUNNING
    const auto h = ctl.statusHistory();
    std::vector<S> seq;
    for (std::size_t i = h0; i < h.size(); ++i) seq.push_back(h[i].state);
    CHECK(seq == std::vector<S>{S::Stopping, S::Preparing, S::Ready, S::Starting, S::Running});
    const std::uint64_t c0 = ctl.metrics().callbacks;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(ctl.metrics().callbacks > c0);
    INFO("rebuild at 44.1 kHz took " << t << " s");
}

TEST_CASE("Babble plan at 44.1 kHz: reported as engine.rateUnsupported (stationary fallback / Strict ERROR)", "[rt][controller][device]") {
    auto& src = bftest::shapedCorpus();
    {
        NullBackend backend;
        auto cc = baseConfig(backend, bftest::presetDoc("office", "balanced"));
        cc.sampleRate = 44100.0;
        withCorpus(cc, src, false);
        EngineController ctl(cc);
        REQUIRE(ctl.start() == CommandResult::Ok);
        REQUIRE(ctl.waitForState(S::Degraded, 5.0));
        CHECK((ctl.degradedReasons() & kDegRateUnsupported) != 0u);
        CHECK((ctl.degradedReasons() & kDegFallbackStationary) != 0u);
        CHECK_FALSE(ctl.currentPlan().babbleEnabled);
        CHECK_FALSE(ctl.metrics().babble);
        ctl.logger().flush();
        CHECK(ctl.logger().countOf(logcode::kEngineRateUnsupported) >= 1);
    }
    {
        NullBackend backend;
        auto cc = baseConfig(backend, bftest::presetDoc("office", "balanced", "stereo", "strict"));
        cc.sampleRate = 44100.0;
        withCorpus(cc, src, false);
        EngineController ctl(cc);
        REQUIRE(ctl.start() == CommandResult::Ok);
        REQUIRE(ctl.waitForState(S::Error, 5.0));
        CHECK(ctl.statusHistory().back().detailCode == "engine.rateUnsupported");
        CHECK(ctl.reset() == CommandResult::Ok);
        CHECK(ctl.state() == S::Stopped);
    }
}

TEST_CASE("Missing / corrupt sources: substitutions and DEGRADED per fallback policy", "[rt][controller][sources]") {
    auto& src = bftest::shapedCorpus();
    const auto snap = src.snapshot();
    FaultInjectingSource faulty(src);
    // Half of the speakers: missing files; one more: corrupt after a few reads.
    for (SpeakerId s = 0; s < snap->numSpeakers(); s += 2) {
        const SpeakerRec& sr = snap->speaker(s);
        for (std::uint32_t r = 0; r < sr.nRecordings; ++r)
            faulty.setFault(sr.firstRecording + r, FaultInjectingSource::Fault::Missing);
    }
    faulty.setFault(snap->speaker(1).firstRecording, FaultInjectingSource::Fault::Corrupt, 3);

    SECTION("Continuous: substitute and continue, DEGRADED corpus.reduced") {
        NullBackend backend;
        auto cc = baseConfig(backend, bftest::presetDoc("office", "balanced"));
        withCorpus(cc, faulty, true);
        EngineController ctl(cc);
        REQUIRE(ctl.start() == CommandResult::Ok);
        REQUIRE(ctl.waitForState(S::Degraded, 8.0));
        CHECK((ctl.degradedReasons() & kDegCorpusReduced) != 0u);
        const RtMetrics m = ctl.metrics();
        CHECK(m.substitutions > 0);
        CHECK(m.sourceFailures > 0);
        CHECK(m.unhealthyRecordings > 0);
        CHECK(m.determinismBroken);
        CHECK(ctl.currentPlan().babbleEnabled);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const MaskStatistics st = ctl.statistics();
        CHECK(st.babbleRmsDb > -40.0);  // babble keeps running on the substitutes
        ctl.logger().flush();
        CHECK(ctl.logger().countOf(logcode::kCorpusFileMissing) > 0);
        CHECK(ctl.logger().countOf(logcode::kPreloadSubstitution) > 0);
        INFO("substitutions " << m.substitutions << " failures " << m.sourceFailures << " unhealthy "
                              << m.unhealthyRecordings << "/" << m.poolRecordings);
    }
    SECTION("Safe: switch to stationary masking") {
        NullBackend backend;
        auto cc = baseConfig(backend, bftest::presetDoc("office", "balanced", "stereo", "safe"));
        withCorpus(cc, faulty, true);
        EngineController ctl(cc);
        REQUIRE(ctl.start() == CommandResult::Ok);
        REQUIRE(bftest::waitUntil([&] { return (ctl.degradedReasons() & kDegFallbackStationary) != 0u; }, 8.0));
        CHECK(ctl.state() == S::Degraded);
        CHECK_FALSE(ctl.currentPlan().babbleEnabled);
        CHECK(ctl.statistics().configuredBabbleFraction == 0.0);
    }
    SECTION("Strict: ERROR") {
        NullBackend backend;
        auto cc = baseConfig(backend, bftest::presetDoc("office", "balanced", "stereo", "strict"));
        withCorpus(cc, faulty, true);
        EngineController ctl(cc);
        REQUIRE(ctl.start() == CommandResult::Ok);
        REQUIRE(ctl.waitForState(S::Error, 8.0));
        CHECK_FALSE(backend.isOpen());
    }
}

TEST_CASE("Slow voice library: late starts / starvations are counted, masking continues", "[rt][controller][sources]") {
    auto& src = bftest::shapedCorpus();
    FaultInjectingSource slow(src);
    NullBackend backend;
    auto cc = baseConfig(backend, bftest::presetDoc("office", "dense"));
    withCorpus(cc, slow, true);
    EngineController ctl(cc);
    REQUIRE(ctl.start() == CommandResult::Ok);
    REQUIRE(ctl.waitForState(S::Running, 8.0));
    slow.setReadDelayMs(600);  // a stalled network drive
    REQUIRE(bftest::waitUntil([&] {
        const RtMetrics m = ctl.metrics();
        return m.lateStarts + m.starvations + m.underflows > 0;
    }, 12.0));
    const RtMetrics m = ctl.metrics();
    CHECK(m.determinismBroken);
    const std::uint64_t c0 = m.callbacks;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(ctl.metrics().callbacks > c0);  // audio never stops
    CHECK((ctl.state() == S::Running || ctl.state() == S::Degraded));
    slow.setReadDelayMs(0);
    CHECK(ctl.stop() == CommandResult::Ok);
    REQUIRE(ctl.waitForState(S::Stopped, 5.0));
    INFO("lateStarts " << m.lateStarts << " starvations " << m.starvations << " underflows " << m.underflows);
}

TEST_CASE("Live preset changes: Strength is hot, a strategy change rebuilds, invalid presets are rejected", "[rt][controller]") {
    NullBackend backend;
    auto cc = baseConfig(backend, bftest::presetDoc("office", "speech_noise"));
    EngineController ctl(cc);
    REQUIRE(ctl.start() == CommandResult::Ok);
    REQUIRE(ctl.waitForState(S::Running, 5.0));
    const std::size_t h0 = ctl.statusHistory().size();
    CHECK(ctl.setStrength(3.0) == CommandResult::Ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(ctl.state() == S::Running);
    CHECK(ctl.statusHistory().size() == h0);  // no state change
    CHECK(ctl.currentPlan().level.strengthDb == 3.0);

    nlohmann::json bad = ctl.currentPreset();
    bad["area"] = "no_such_area";
    CHECK(ctl.setPreset(bad) == CommandResult::InvalidArgument);
    CHECK(ctl.currentPreset()["area"] == "office");

    nlohmann::json other = ctl.currentPreset();
    other["outputs"]["layout"] = "ring4";
    CHECK(ctl.setPreset(other) == CommandResult::Ok);
    REQUIRE(bftest::waitUntil([&] { return historyHas(ctl, S::Stopping) && ctl.state() == S::Running; }, 5.0));
    CHECK(ctl.metrics().callbacks > 0);
    ctl.logger().flush();
    CHECK(ctl.logger().countOf(logcode::kPresetChange) == 2);
    CHECK(ctl.logger().countOf(logcode::kPresetInvalid) == 1);
}

TEST_CASE("Persistence: LKG restore on invalid session config, session save and LKG promotion", "[rt][controller][persist]") {
    const auto dir = freshDir("rt_persist");
    SessionStore store(dir);
    nlohmann::json good = bftest::presetDoc("conference", "speech_noise");
    good["name"] = "known good";
    REQUIRE(store.saveLastKnownGood(good));
    nlohmann::json broken = bftest::presetDoc("office", "speech_noise");
    broken["area"] = "no_such_area";
    REQUIRE(store.saveSession(broken, 1));
    {
        NullBackend backend;
        auto cc = baseConfig(backend, nlohmann::json());
        cc.preset.reset();
        cc.stateDir = dir;
        EngineController ctl(cc);
        CHECK(ctl.startupConfig().source == "lkg");
        CHECK(ctl.startupConfig().lkgRestored);
        CHECK(ctl.currentPreset()["name"] == "known good");
        ctl.logger().flush();
        CHECK(ctl.logger().countOf(logcode::kPresetLkgRestored) == 1);
        CHECK(ctl.logger().countOf(logcode::kConfigInvalid) == 1);
        const auto st = ctl.pollStatus();
        REQUIRE(st.has_value());
        CHECK(st->detailCode == logcode::kPresetLkgRestored);
    }
    {  // corrupt session file and no LKG -> factory default
        const auto d2 = freshDir("rt_persist_factory");
        std::ofstream(d2 / "session.json") << "{ not json";
        NullBackend backend;
        auto cc = baseConfig(backend, nlohmann::json());
        cc.preset.reset();
        cc.stateDir = d2;
        EngineController ctl(cc);
        CHECK(ctl.startupConfig().source == "factory");
        CHECK(ctl.currentPreset()["area"] == "office");
    }
    {  // session.json written; LKG promoted after lkgPromoteS of RUNNING without ERROR
        const auto d3 = freshDir("rt_persist_promote");
        NullBackend backend;
        auto cc = baseConfig(backend, bftest::presetDoc("small_room", "speech_noise"));
        cc.stateDir = d3;
        cc.lkgPromoteS = 1.0;
        cc.sessionSaveS = 0.2;
        EngineController ctl(cc);
        REQUIRE(ctl.start() == CommandResult::Ok);
        REQUIRE(ctl.waitForState(S::Running, 5.0));
        REQUIRE(bftest::waitUntil([&] { return std::filesystem::exists(d3 / "last_known_good.json"); }, 3.0));
        CHECK(std::filesystem::exists(d3 / "session.json"));
        SessionStore s3(d3);
        const auto lkg = s3.loadLastKnownGood();
        REQUIRE(lkg.has_value());
        CHECK((*lkg)["area"] == "small_room");
        CHECK(s3.loadSessionPreset().has_value());
    }
}

TEST_CASE("Diagnostics snapshot carries the RELIABILITY sec. 6.1 fields and the health model", "[rt][controller][diag]") {
    NullBackend backend;
    auto cc = baseConfig(backend, bftest::presetDoc("office", "speech_noise", "ring4"));
    EngineController ctl(cc);
    REQUIRE(ctl.start() == CommandResult::Ok);
    REQUIRE(ctl.waitForState(S::Running, 5.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const nlohmann::json d = ctl.diagnostics();
    for (const auto& p : diagnosticsRequiredPaths()) {
        INFO(p);
        CHECK(d.contains(nlohmann::json::json_pointer(p)));
    }
    CHECK(d["schema"] == "babbleforge.diagnostics/1");
    CHECK(d["engine"]["state"] == "RUNNING");
    CHECK(d["engine"]["sessionSeed"] == 5);
    CHECK(d["audio"]["sampleRate"] == 48000.0);
    CHECK(d["audio"]["driver"] == "Null");
    CHECK(d["outputs"].size() == 4);
    CHECK(d["health"]["outputHealthy"] == true);
    CHECK(d["audio"]["dspLoad"]["p99"].get<double>() < 0.5);

    // Health model thresholds (§6.2).
    CHECK(evaluateHealth(0.2, 5.0, 0.0, LimiterHealth::Inactive, CorpusHealth::Healthy, false).outputHealthy());
    const HealthReport w = evaluateHealth(0.6, 2.0, 3.0, LimiterHealth::Active, CorpusHealth::Reduced, false);
    CHECK(w.dspLoad == Health::Warning);
    CHECK(w.preload == Health::Warning);
    CHECK(w.xruns == Health::Warning);
    CHECK(w.outputHealthy());
    const HealthReport b = evaluateHealth(0.9, 0.5, 11.0, LimiterHealth::Sustained, CorpusHealth::Insufficient, false);
    CHECK(b.dspLoad == Health::Bad);
    CHECK(b.preload == Health::Bad);
    CHECK(b.xruns == Health::Bad);
    CHECK(b.limiter == Health::Bad);
    CHECK(b.corpus == Health::Bad);
    CHECK_FALSE(b.outputHealthy());
    CHECK_FALSE(evaluateHealth(0.1, 5.0, 0.0, LimiterHealth::Inactive, CorpusHealth::Healthy, true).outputHealthy());
}

TEST_CASE("Soak: 10 minutes of babble on the NullBackend", "[.long][rt][soak]") {
    if (!std::getenv("BF_LONG_TESTS")) SKIP("set BF_LONG_TESTS=1");
    auto& src = bftest::shapedCorpus();
    NullBackend backend;
    auto cc = baseConfig(backend, bftest::presetDoc("open_office", "balanced", "ring4"));
    withCorpus(cc, src, false);
    cc.stateDir = freshDir("rt_soak");
    cc.log.dir = cc.stateDir / "logs";
    EngineController ctl(cc);
    REQUIRE(ctl.start() == CommandResult::Ok);
    REQUIRE(ctl.waitForState(S::Running, 8.0));
    const auto end = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        REQUIRE(ctl.state() == S::Running);
    }
    const RtMetrics m = ctl.metrics();
    const MaskStatistics st = ctl.statistics();
    CHECK(m.xruns == 0);
    CHECK(m.starvations == 0);
    CHECK(m.lateStarts == 0);
    CHECK(m.eventsDroppedNoChain == 0);
    CHECK(m.poolExhausted == 0);
    CHECK(std::fabs(st.outputRmsDb - (-26.0)) < 1.5);
    CHECK(std::filesystem::exists(cc.stateDir / "last_known_good.json"));
    INFO("events " << m.eventsPushed << " load p99 " << m.dspLoadP99 << " minBuf " << m.minBufferedS);
    CHECK(ctl.stop() == CommandResult::Ok);
    REQUIRE(ctl.waitForState(S::Stopped, 5.0));
}
