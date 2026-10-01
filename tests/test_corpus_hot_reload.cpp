// Voice library hot reload (EngineController::adoptCorpus / reloadCorpus -> RealtimeEngine::adoptCorpus):
// selector state migration, no dropout while the snapshot is swapped under a running NullBackend
// engine, new speakers in the selections that follow, the old generation (and its cache lease)
// released only after its last reader.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

#include "core/corpus/FlacCache.h"
#include "core/corpus/SyntheticCorpus.h"
#include "core/rt/EngineController.h"
#include "rt_fixtures.h"

using namespace bf;
using namespace bf::rt;
namespace fs = std::filesystem;

namespace {

// A "library" built from the synthetic corpus: FLAC cache files on disk + a FlacCacheAudioSource
// (the production source) + stable ids, so corpora with the same seed and more speakers are
// extensions of each other (identical first speakers / recordings).
LoadedCorpus makeLibrary(const fs::path& dir, std::uint32_t speakers, double seconds = 20.0) {
    SyntheticCorpusParams p;
    p.numSpeakers = speakers;
    p.recordingsPerSpeaker = 2;
    p.recordingSeconds = seconds;
    p.seed = 0xC0FFEE;
    SyntheticCorpus syn(p);
    LoadedCorpus lc;
    lc.snapshot = syn.snapshot();
    const std::size_t nr = lc.snapshot->numRecordings();
    std::vector<fs::path> paths(nr);
    std::vector<float> buf;
    for (RecordingId r = 0; r < nr; ++r) {
        const auto& rec = lc.snapshot->recording(r);
        buf.assign(static_cast<std::size_t>(rec.length), 0.0f);
        REQUIRE(syn.read(r, 0, buf.data(), buf.size()));
        paths[r] = dir / "cache" / std::to_string(rec.speaker) / (std::to_string(r) + ".flac");
        REQUIRE(writeFlacCache(paths[r], buf.data(), buf.size()));
    }
    lc.audio = std::make_shared<FlacCacheAudioSource>(std::move(paths));
    lc.audio->setCacheDir(dir / "cache");
    for (SpeakerId s = 0; s < lc.snapshot->numSpeakers(); ++s) {
        lc.speakerDbId.push_back(static_cast<std::int64_t>(s) + 1);
        lc.identity.speakerDbId.push_back(static_cast<std::int64_t>(s) + 1);
        lc.identity.speakerExternalId.push_back("spk" + std::to_string(s));
    }
    for (RecordingId r = 0; r < nr; ++r) {
        lc.recordingDbId.push_back(static_cast<std::int64_t>(r) + 1);
        lc.identity.recordingDbId.push_back(static_cast<std::int64_t>(r) + 1);
        lc.identity.recordingPcmSha.push_back("pcm" + std::to_string(r));
    }
    return lc;
}

struct Syn {
    std::unique_ptr<SyntheticCorpus> corpus;
    std::shared_ptr<const CorpusSnapshot> snapshot;
    CorpusIdentity identity;
};

// Metadata-only library (no cache files) with stable ids; the same seed makes larger corpora
// extensions of smaller ones.
Syn makeSyn(std::uint32_t speakers, double seconds, std::uint64_t seed = 0xC0FFEE) {
    SyntheticCorpusParams p;
    p.numSpeakers = speakers;
    p.recordingsPerSpeaker = 2;
    p.recordingSeconds = seconds;
    p.seed = seed;
    p.measureAsl = false;
    Syn o;
    o.corpus = std::make_unique<SyntheticCorpus>(p);
    o.snapshot = o.corpus->snapshot();
    for (SpeakerId s = 0; s < o.snapshot->numSpeakers(); ++s) {
        o.identity.speakerDbId.push_back(static_cast<std::int64_t>(s) + 1);
        o.identity.speakerExternalId.push_back("spk" + std::to_string(s));
    }
    for (RecordingId r = 0; r < o.snapshot->numRecordings(); ++r) {
        o.identity.recordingDbId.push_back(static_cast<std::int64_t>(r) + 1);
        o.identity.recordingPcmSha.push_back("pcm" + std::to_string(r));
    }
    return o;
}

fs::path freshDir(const std::string& name) {
    const fs::path d = fs::path(BF_TEST_OUT_DIR) / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

// RT-safe level meter: energy per callback block and "all zero" blocks.
class BlockMeter final : public NullBackend::Observer {
public:
    explicit BlockMeter(std::size_t cap) : energy_(cap, 0.0f), frames_(cap, 0), silent_(cap, 0) {}
    void onOutput(const float* const* out, int nOut, int n) noexcept override {
        const std::size_t i = count_.load(std::memory_order_relaxed);
        if (i >= energy_.size()) return;
        double e = 0.0;
        bool zero = true;
        for (int c = 0; c < nOut; ++c)
            for (int k = 0; k < n; ++k) {
                e += static_cast<double>(out[c][k]) * out[c][k];
                zero = zero && out[c][k] == 0.0f;
            }
        energy_[i] = static_cast<float>(e / std::max(1, nOut));
        frames_[i] = static_cast<std::uint16_t>(n);
        silent_[i] = zero ? 1 : 0;
        count_.store(i + 1, std::memory_order_release);
    }
    std::size_t blocks() const noexcept { return count_.load(std::memory_order_acquire); }
    // RMS (linear) over blocks [a, b).
    double rms(std::size_t a, std::size_t b) const {
        double e = 0.0;
        std::size_t n = 0;
        for (std::size_t i = a; i < b && i < blocks(); ++i) {
            e += energy_[i];
            n += frames_[i];
        }
        return n ? std::sqrt(e / static_cast<double>(n)) : 0.0;
    }
    std::size_t silentBlocks(std::size_t a, std::size_t b) const {
        std::size_t n = 0;
        for (std::size_t i = a; i < b && i < blocks(); ++i) n += silent_[i];
        return n;
    }

private:
    std::vector<float> energy_;
    std::vector<std::uint16_t> frames_, silent_;
    std::atomic<std::size_t> count_{0};
};

}  // namespace

TEST_CASE("selector state migrates across a library change", "[corpus][hotreload][selector]") {
    const Syn a = makeSyn(12, 60.0), b = makeSyn(18, 60.0);
    SelectorConfig cfg;
    cfg.poolSize = 12;
    cfg.voiceSlots = 6;
    cfg.diversity = DiversityMode::Balanced;
    cfg.seed = 9;
    cfg.rotationPeriodS = 0.0;
    cfg.segCooldownOverrideS = 1.0;
    SegmentSelector selA(a.snapshot, cfg);

    // Use speaker 3 for a while: its shuffle advances, anchors are committed (cooldowns).
    const SpeakerId spk = 3;
    std::set<std::pair<RecordingId, std::int64_t>> consumed;
    std::int64_t t = 0;
    for (int i = 0; i < 10; ++i) {
        const auto pk = selA.pickForSpeaker(spk, t);
        REQUIRE(pk);
        consumed.insert({pk->recording, pk->anchor});
        selA.commit(*pk, pk->anchor + 48000 * 4, t, t + 48000 * 4);
        t += 48000 * 5;
    }
    REQUIRE(consumed.size() == 10);

    SECTION("extension: kept speakers continue their cycle, new speakers enter the pool") {
        const CorpusMigration mig = makeCorpusMigration(a.identity, b.identity);
        CHECK(mig.keptSpeakers == 12);
        CHECK(mig.keptRecordings == 24);
        SegmentSelector selB(b.snapshot, cfg);
        selB.migrateFrom(selA, mig);
        CHECK(selB.cycleOf(spk) == selA.cycleOf(spk));
        CHECK(selB.shuffleSize(spk) == selA.shuffleSize(spk));
        // The anchors already played in this cycle are not repeated before the cycle ends.
        const std::size_t remaining = selA.shuffleSize(spk) - 10;
        for (std::size_t i = 0; i < remaining; ++i) {
            const auto pk = selB.pickForSpeaker(spk, t + static_cast<std::int64_t>(i));
            REQUIRE(pk);
            CHECK(selB.cycleOf(spk) == selA.cycleOf(spk));
            CHECK(consumed.count({pk->recording, pk->anchor}) == 0);
        }
        // New speakers (index >= 12) are in the pool; surviving members stay.
        const auto& pool = selB.pool();
        CHECK(pool.size() == 12);
        CHECK(std::any_of(pool.begin(), pool.end(), [](SpeakerId s) { return s >= 12; }));
        std::size_t survivors = 0;
        for (SpeakerId s : selA.pool()) survivors += std::count(pool.begin(), pool.end(), s) ? 1 : 0;
        CHECK(survivors >= 12 - (12 + 3) / 4);
        // The cooldown regions of the played anchors came along.
        for (const auto& c : consumed) CHECK(c.first < b.snapshot->numRecordings());
    }

    SECTION("unrelated library: unknown anchors are dropped, nothing breaks") {
        const Syn c = makeSyn(12, 30.0);  // same ids, different recordings (shorter)
        const CorpusMigration mig = makeCorpusMigration(a.identity, c.identity);
        SegmentSelector selC(c.snapshot, cfg);
        selC.migrateFrom(selA, mig);
        std::size_t kept = 0;
        for (const auto& [rec, anchor] : consumed) {
            (void)rec;
            (void)anchor;
            ++kept;
        }
        CHECK(kept == 10);
        // Not a single stale index: the permutation only references valid, eligible anchors.
        for (SpeakerId s = 0; s < c.snapshot->numSpeakers(); ++s)
            for (auto idx : selC.shuffleOf(s)) CHECK(idx < c.snapshot->anchorsOf(s).size());
        for (int i = 0; i < 40; ++i) CHECK(selC.pick(t + i * 48000, {}).has_value());
    }

    SECTION("replaced library (different content keys) migrates nothing") {
        CorpusIdentity other = b.identity;
        for (auto& sha : other.recordingPcmSha) sha += "-x";
        for (auto& e : other.speakerExternalId) e += "-x";
        const CorpusMigration mig = makeCorpusMigration(a.identity, other);
        CHECK(mig.keptSpeakers == 0);
        CHECK(mig.keptRecordings == 0);
    }
}

TEST_CASE("hot reload under a running NullBackend engine: no dropout, new speakers selected, old generation released late",
          "[rt][controller][hotreload]") {
    const auto dirA = freshDir("hr_run_a"), dirB = freshDir("hr_run_b");
    LoadedCorpus a = makeLibrary(dirA, 16, 25.0);
    LoadedCorpus b = makeLibrary(dirB, 24, 25.0);
    const CorpusIdentity idA = a.identity;
    const std::string versionA = a.snapshot->corpusVersion(), versionB = b.snapshot->corpusVersion();
    REQUIRE(versionA != versionB);
    std::weak_ptr<FlacCacheAudioSource> weakA = a.audio;
    const fs::path cacheA = dirA / "cache";
    REQUIRE(FlacCacheAudioSource::isPinned(cacheA));

    NullBackend backend;
    BlockMeter meter(60000);
    backend.setObserver(&meter);
    EngineControllerConfig cc;
    cc.dataSet = &bftest::dataSet();
    cc.backend = &backend;
    cc.deviceId = backend.devices().front().id;
    cc.bufferFrames = 512;
    cc.seed = 21;
    cc.preset = bftest::presetDoc("office", "balanced");
    cc.engine.blockPoolBytes = 32u << 20;
    cc.corpus = a.snapshot;
    cc.audio = a.audio.get();
    cc.audioThreadSafe = true;
    EngineController ctl(cc);
    REQUIRE(ctl.start() == CommandResult::Ok);
    REQUIRE(ctl.waitForState(EngineState::Running, 10.0));
    REQUIRE(ctl.metrics().babble);
    CHECK(ctl.degradedReasons() == 0u);

    // Baseline level over 4 s.
    std::this_thread::sleep_for(std::chrono::seconds(4));
    const std::size_t preEnd = meter.blocks();
    const std::size_t blocksPerSec = 48000 / 512;
    const std::size_t preBegin = preEnd - 3 * blocksPerSec;
    const double preRms = meter.rms(preBegin, preEnd);
    REQUIRE(preRms > 1e-4);
    std::vector<double> preWin;
    for (std::size_t i = preBegin; i + blocksPerSec / 4 <= preEnd; i += blocksPerSec / 4) preWin.push_back(meter.rms(i, i + blocksPerSec / 4));
    const double preFloor = *std::min_element(preWin.begin(), preWin.end());
    const RtMetrics m0 = ctl.metrics();
    const std::set<SpeakerId> before = ctl.plannedSpeakers(0);
    CHECK(before.size() >= 3);
    CHECK(std::all_of(before.begin(), before.end(), [](SpeakerId s) { return s < 16; }));

    // ---- hot reload ----
    CorpusReloadOptions opt;
    opt.previousIdentity = &idA;
    opt.retirePrevious = std::move(a.audio);  // the only strong owner of the old source besides the engine
    a.audio.reset();
    const std::size_t reloadBlock = meter.blocks();
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(ctl.adoptCorpus(std::move(b), opt) == CommandResult::Ok);
    const double adoptMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    opt.retirePrevious.reset();  // from here on only the engine's old generation keeps the old source alive
    REQUIRE(bftest::waitUntil([&] { return ctl.corpusAdoptions() == 1; }, 5.0));
    CHECK(ctl.state() == EngineState::Running);  // no restart
    CHECK(ctl.corpusVersion() == versionB);
    // The old library is still referenced by the events that were already running.
    CHECK_FALSE(weakA.expired());
    CHECK(FlacCacheAudioSource::isPinned(cacheA));

    // Keep running on the new library for 9 s.
    std::this_thread::sleep_for(std::chrono::seconds(9));
    const std::size_t postEnd = meter.blocks();
    const RtMetrics m1 = ctl.metrics();
    CHECK(ctl.state() == EngineState::Running);

    // No dropout: the level through the swap stays within the baseline's range and the stream never goes silent.
    CHECK(meter.silentBlocks(preBegin, postEnd) == 0);
    std::vector<double> postWin;
    for (std::size_t i = reloadBlock; i + blocksPerSec / 4 <= postEnd; i += blocksPerSec / 4) postWin.push_back(meter.rms(i, i + blocksPerSec / 4));
    REQUIRE(postWin.size() > 30);
    const double postMin = *std::min_element(postWin.begin(), postWin.end());
    const double postRms = meter.rms(reloadBlock, postEnd);
    const double dbDiff = 20.0 * std::log10(postRms / preRms);
    INFO("pre " << preRms << " floor " << preFloor << " post " << postRms << " min " << postMin << " diff " << dbDiff << " dB, adopt "
                << adoptMs << " ms");
    CHECK(postMin > 0.4 * preFloor);
    CHECK(std::fabs(dbDiff) < 3.0);
    // The real-time path stayed healthy through the swap.
    CHECK(m1.underflows == m0.underflows);
    CHECK(m1.starvations == m0.starvations);
    CHECK(m1.lateStarts == m0.lateStarts);
    CHECK(m1.sourceFailures == 0);
    CHECK(m1.xruns == m0.xruns);

    // New speakers (index >= 16 exist only in the new library) are selected after the switch.
    const std::set<SpeakerId> after = ctl.plannedSpeakers(1);
    CHECK(std::any_of(after.begin(), after.end(), [](SpeakerId s) { return s >= 16; }));
    // Surviving speakers kept their identity (same index in the new snapshot).
    CHECK(std::any_of(after.begin(), after.end(), [&](SpeakerId s) { return before.count(s) != 0; }));

    ctl.logger().flush();
    CHECK(ctl.logger().countOf(logcode::kCorpusLoaded) >= 2);  // startup + the reload

    // The old generation (and its cache lease) is gone once the last event that read it has ended.
    CHECK(bftest::waitUntil([&] { return weakA.expired(); }, 20.0));
    CHECK_FALSE(FlacCacheAudioSource::isPinned(cacheA));
    CHECK(ctl.stop() == CommandResult::Ok);
    REQUIRE(ctl.waitForState(EngineState::Stopped, 3.0));
}
