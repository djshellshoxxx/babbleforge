// Engine state machine (RELIABILITY.md §1): exhaustive legal / illegal transition table,
// rejection of illegal transitions, DEGRADED hysteresis, controller command legality.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>
#include <utility>

#include "core/rt/EngineController.h"
#include "core/rt/EngineState.h"
#include "rt_fixtures.h"

using namespace bf::rt;
using S = EngineState;

namespace {
// RELIABILITY.md §1.3, written out independently of the implementation.
std::set<std::pair<S, S>> expectedLegal() {
    std::set<std::pair<S, S>> t = {
        {S::Stopped, S::Preparing},  {S::Preparing, S::Ready},     {S::Preparing, S::Stopped},
        {S::Ready, S::Starting},     {S::Ready, S::Stopping},      {S::Ready, S::DeviceLost},
        {S::Starting, S::Running},   {S::Starting, S::Degraded},   {S::Starting, S::DeviceLost},
        {S::Starting, S::Stopping},  {S::Running, S::Degraded},    {S::Running, S::DeviceLost},
        {S::Running, S::Stopping},   {S::Degraded, S::Running},    {S::Degraded, S::DeviceLost},
        {S::Degraded, S::Stopping},  {S::DeviceLost, S::Preparing}, {S::DeviceLost, S::Stopped},
        {S::Stopping, S::Stopped},   {S::Stopping, S::Preparing},  {S::Error, S::Stopped},
    };
    for (S s : kAllEngineStates)
        if (s != S::Error) t.insert({s, S::Error});  // any -> ERROR (fatal)
    return t;
}
}  // namespace

TEST_CASE("State machine: exhaustive legal/illegal transition table", "[rt][state]") {
    const auto legal = expectedLegal();
    int nLegal = 0, nIllegal = 0;
    for (S from : kAllEngineStates) {
        for (S to : kAllEngineStates) {
            const bool want = legal.count({from, to}) > 0;
            INFO(toString(from) << " -> " << toString(to));
            CHECK(isLegalTransition(from, to) == want);
            // Drive a fresh machine into `from` via a legal path, then try the transition.
            EngineStateMachine sm;
            std::vector<S> path;
            switch (from) {
            case S::Stopped: break;
            case S::Preparing: path = {S::Preparing}; break;
            case S::Ready: path = {S::Preparing, S::Ready}; break;
            case S::Starting: path = {S::Preparing, S::Ready, S::Starting}; break;
            case S::Running: path = {S::Preparing, S::Ready, S::Starting, S::Running}; break;
            case S::Degraded: path = {S::Preparing, S::Ready, S::Starting, S::Degraded}; break;
            case S::DeviceLost: path = {S::Preparing, S::Ready, S::DeviceLost}; break;
            case S::Stopping: path = {S::Preparing, S::Ready, S::Stopping}; break;
            case S::Error: path = {S::Error}; break;
            }
            for (S p : path) REQUIRE(sm.transition(p, "setup"));
            REQUIRE(sm.state() == from);
            int transitions = 0, illegal = 0;
            sm.onTransition = [&](S, S, std::string_view) { ++transitions; };
            sm.onIllegal = [&](S f, S t, std::string_view) {
                ++illegal;
                CHECK(f == from);
                CHECK(t == to);
            };
            const bool ok = sm.transition(to, "test");
            CHECK(ok == want);
            CHECK(sm.state() == (want ? to : from));  // illegal: state unchanged
            CHECK(transitions == (want ? 1 : 0));
            CHECK(illegal == (want ? 0 : 1));
            (want ? nLegal : nIllegal) += 1;
        }
    }
    CHECK(nLegal == 29);
    CHECK(nLegal + nIllegal == 81);
}

TEST_CASE("DEGRADED reasons: codes and 30 s hysteresis", "[rt][state]") {
    CHECK(degradedReasonCodes(kDegCorpusReduced | kDegAudioXruns) == std::vector<std::string>{"corpus.reduced", "audio.xruns"});
    for (int i = 0; i < kNumDegradedReasons; ++i)
        CHECK(degradedReasonFromCode(degradedReasonCode(1u << i)) == (1u << i));
    DegradedTracker t(30.0);
    CHECK(t.reasons(0.0) == 0);
    t.setCondition(kDegPreloadBehind, true, 10.0);
    CHECK(t.reasons(10.0) == kDegPreloadBehind);
    t.setCondition(kDegPreloadBehind, false, 12.0);  // cleared at 12 s
    CHECK(t.reasons(41.9) == kDegPreloadBehind);     // < 30 s after clearing
    CHECK(t.reasons(42.1) == 0u);
    t.setCondition(kDegPreloadBehind, true, 50.0);   // re-asserted
    t.setCondition(kDegPreloadBehind, false, 51.0);
    t.setPersistent(kDegFallbackStationary);
    CHECK(t.reasons(200.0) == kDegFallbackStationary);
    t.clear();
    CHECK(t.reasons(200.0) == 0u);
}

TEST_CASE("EngineController: illegal commands are rejected, logged at WARN and change nothing", "[rt][state][controller]") {
    NullBackend backend;
    EngineControllerConfig cc;
    cc.dataSet = &bftest::dataSet();
    cc.backend = &backend;
    cc.deviceId = backend.devices().front().id;
    cc.seed = 7;
    cc.preset = bftest::presetDoc("office", "speech_noise");
    EngineController ctl(cc);
    REQUIRE(ctl.state() == S::Stopped);
    CHECK(ctl.stop() == CommandResult::IllegalTransition);
    CHECK(ctl.play() == CommandResult::IllegalTransition);
    CHECK(ctl.reconnect() == CommandResult::IllegalTransition);
    CHECK(ctl.reset() == CommandResult::IllegalTransition);
    CHECK(ctl.state() == S::Stopped);
    ctl.logger().flush();
    CHECK(ctl.logger().countOf(logcode::kEngineIllegal) == 4);
    for (const auto& e : ctl.logger().recent(LogLevel::Warn))
        if (e.value("code", "") == logcode::kEngineIllegal) CHECK(e.value("level", "") == "WARN");

    // One-click start: STOPPED -> PREPARING -> READY -> STARTING -> RUNNING.
    REQUIRE(ctl.start() == CommandResult::Ok);
    CHECK(ctl.start() == CommandResult::IllegalTransition);
    REQUIRE(ctl.waitForState(S::Running, 5.0));
    std::vector<S> seen;
    for (const auto& ev : ctl.statusHistory()) seen.push_back(ev.state);
    CHECK(seen == std::vector<S>{S::Preparing, S::Ready, S::Starting, S::Running});
    const auto st = ctl.pollStatus();  // coalesced: the latest state only
    REQUIRE(st.has_value());
    CHECK(st->state == S::Running);
    CHECK(st->headline == "MASKING ACTIVE");
    CHECK_FALSE(ctl.pollStatus().has_value());
    CHECK(ctl.play() == CommandResult::IllegalTransition);
    REQUIRE(ctl.stop() == CommandResult::Ok);
    REQUIRE(ctl.waitForState(S::Stopped, 3.0));
    CHECK(ctl.statusHistory().back().previous == S::Stopping);
}

TEST_CASE("EngineController: stop() during PREPARING aborts cleanly", "[rt][state][controller]") {
    auto& src = bftest::shapedCorpus();
    bf::rt::FaultInjectingSource slow(src);
    slow.setReadDelayMs(20);  // slow voice library: PREPARING takes a while
    NullBackend backend;
    EngineControllerConfig cc;
    cc.dataSet = &bftest::dataSet();
    cc.backend = &backend;
    cc.deviceId = backend.devices().front().id;
    cc.corpus = src.snapshot();
    cc.audio = &slow;
    cc.audioThreadSafe = true;
    cc.seed = 11;
    cc.preset = bftest::presetDoc("office", "balanced");
    cc.engine.blockPoolBytes = 16u << 20;
    EngineController ctl(cc);
    REQUIRE(ctl.start() == CommandResult::Ok);
    REQUIRE(ctl.state() == S::Preparing);
    CHECK(ctl.stop() == CommandResult::Ok);  // asynchronous abort
    REQUIRE(ctl.waitForState(S::Stopped, 10.0));
    const auto h = ctl.statusHistory();
    REQUIRE(h.size() >= 2);
    CHECK(h.back().previous == S::Preparing);
    CHECK_FALSE(backend.isOpen());
}
