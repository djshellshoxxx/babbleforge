// RT-check binary (bfcore_rtcheck_tests, linked with the operator new/delete hooks,
// REALTIME_ARCHITECTURE.md §1.3): 20 s of babble through the EngineController on the
// NullBackend. The audio callback must not allocate, free or lock; no xruns; the output level
// and spectrum must match the offline render of the same preset and seed.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

#include "core/analysis/SpectrumAnalyzer.h"
#include "core/rt/EngineController.h"
#include "core/rt/RtCheck.h"
#include "rt_fixtures.h"

using namespace bf;
using namespace bf::rt;

TEST_CASE("RT check: 20 s NullBackend run without RT allocations, locks or xruns; output matches offline", "[rt][rtcheck]") {
    REQUIRE(rtCheckHooksActive());
    setAbortOnRtViolation(false);
    resetRtViolationCounts();
    {  // the hooks see allocations on RT-flagged threads only (explicit calls: never elided)
        void* off = ::operator new(64);
        ScopedRealtimeThread mark;
        void* on = ::operator new(64);
        ::operator delete(on);
        ::operator delete(off);
    }
    REQUIRE(rtAllocationCount() == 1);
    REQUIRE(rtDeallocationCount() == 2);
    resetRtViolationCounts();

    constexpr double kSeconds = 20.0;
    constexpr std::uint64_t kSeed = 20260930;
    auto& src = bftest::shapedCorpus();
    const nlohmann::json preset = bftest::presetDoc("office", "balanced");

    NullBackend backend;
    backend.setDevicePeriods(3);  // tolerate host scheduling hiccups of the (non-RT) timer thread
    bftest::OutputCapture cap(2, static_cast<std::size_t>(kSeconds * 48000.0));
    backend.setObserver(&cap);
    EngineControllerConfig cc;
    cc.dataSet = &bftest::dataSet();
    cc.backend = &backend;
    cc.deviceId = backend.devices().front().id;
    cc.bufferFrames = 512;
    cc.seed = kSeed;
    cc.preset = preset;
    cc.corpus = src.snapshot();
    cc.audio = &src;
    cc.engine.blockPoolBytes = 32u << 20;
    RtMetrics m;
    MaskStatistics rtStats;
    {
        EngineController ctl(cc);
        REQUIRE(ctl.start() == CommandResult::Ok);
        REQUIRE(ctl.waitForState(EngineState::Running, 8.0));
        cap.arm();
        REQUIRE(bftest::waitUntil([&] { return cap.frames() >= static_cast<std::size_t>(kSeconds * 48000.0); }, kSeconds + 5.0));
        m = ctl.metrics();
        rtStats = ctl.statistics();
        CHECK(ctl.state() == EngineState::Running);
        CHECK(ctl.stop() == CommandResult::Ok);
        REQUIRE(ctl.waitForState(EngineState::Stopped, 3.0));
    }
    const std::uint64_t allocs = rtAllocationCount(), frees = rtDeallocationCount(), locks = rtLockCount();
    INFO("RT allocations " << allocs << " frees " << frees << " locks " << locks << " | callbacks " << m.callbacks
                           << " xruns " << m.xruns << " overruns " << m.overruns << " gaps " << m.callbackGaps
                           << " | load p50 " << m.dspLoadP50 << " p99 " << m.dspLoadP99 << " max " << m.dspLoadMax
                           << " | late " << m.lateStarts << " starv " << m.starvations << " under " << m.underflows
                           << " | p99 read " << m.p99ReadLatencyMs << " ms minBuf " << m.minBufferedS
                           << " s | tap drops " << m.tapOverflowFrames);
    CHECK(allocs == 0);
    CHECK(frees == 0);
    CHECK(locks == 0);
    CHECK(m.xruns == 0);
    CHECK(m.starvations == 0);
    CHECK(m.lateStarts == 0);
    CHECK(m.tapOverflowFrames == 0);
    CHECK(m.denormalsDisabled == true);
    CHECK(m.dspLoadP99 < 0.5);
    CHECK_FALSE(m.determinismBroken);

    // Offline render of the same preset and seed.
    nlohmann::json doc = bftest::scenarioDoc("office", "balanced", "stereo", kSeconds, kSeed);
    doc["preset"] = preset;
    const RenderResult off = bftest::render(doc, bftest::handle(src));
    REQUIRE(off.audio.size() == 2);

    const auto& rt = cap.data();
    const std::size_t a = 48000, b = static_cast<std::size_t>(kSeconds * 48000.0);  // skip the 500 ms fade-in
    bool finite = true;
    float peak = 0.0f;
    for (const auto& ch : rt)
        for (std::size_t i = a; i < b; ++i) {
            finite = finite && std::isfinite(ch[i]);
            peak = std::max(peak, std::fabs(ch[i]));
        }
    CHECK(finite);
    CHECK(peak < 0.95f);
    const double rtDb = bftest::rmsDb(rt, a, b), offDb = bftest::rmsDb(off.audio, a, b);
    const float* rp[2] = {rt[0].data() + a, rt[1].data() + a};
    const float* op[2] = {off.audio[0].data() + a, off.audio[1].data() + a};
    const SpectrumAnalysis rs = analyzeSpectrum(rp, 2, b - a, 48000.0);
    const SpectrumAnalysis os = analyzeSpectrum(op, 2, b - a, 48000.0);
    double meanDev = 0.0;
    std::size_t nb = 0;
    for (std::size_t k = kFirstOperatingBand; k <= kLastOperatingBand; ++k, ++nb) meanDev += rs.overallDb[k] - os.overallDb[k];
    meanDev /= static_cast<double>(nb);
    double maxShape = 0.0;
    for (std::size_t k = kFirstOperatingBand; k <= kLastOperatingBand; ++k)
        maxShape = std::max(maxShape, std::fabs(rs.overallDb[k] - os.overallDb[k] - meanDev));
    INFO("RT output " << rtDb << " dBFS, offline " << offDb << " dBFS, spectrum shape max dev " << maxShape
                      << " dB | RT meanActive " << rtStats.meanActive << " offline " << off.stats.meanActive);
    CHECK(std::fabs(rtDb - offDb) < 1.5);
    CHECK(std::fabs(rtDb - (-26.0)) < 2.0);
    CHECK(maxShape < 3.0);
    CHECK(std::fabs(rtStats.meanActive - off.stats.meanActive) < 1.5);
    WARN("RT allocs " << allocs << ", locks " << locks << ", xruns " << m.xruns << ", gaps " << m.callbackGaps
                      << ", load p99 " << m.dspLoadP99 << ", max " << m.dspLoadMax << ", RT " << rtDb << " dBFS vs offline "
                      << offDb << " dBFS, shape dev " << maxShape << " dB, meanActive " << rtStats.meanActive << " vs "
                      << off.stats.meanActive);
}
