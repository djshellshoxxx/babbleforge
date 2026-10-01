// Incremental corpus import ("add to existing") and speaker / recording removal
// (src/core/corpus/CorpusImporter.cpp), plus the cache-generation lease of FlacCacheAudioSource.
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "core/rt/EngineController.h"
#include "core/rt/NullBackend.h"
#include "ingest_fixtures.h"
#include "rt_fixtures.h"

#ifdef BF_WITH_CORPUS_DB
#include "core/corpus/CorpusDb.h"
#include "core/corpus/CorpusImporter.h"
#include "core/corpus/CorpusLoader.h"
#include "core/corpus/FlacCache.h"

using namespace bf;
using namespace bftest;
namespace fs = std::filesystem;

namespace {

void put(const fs::path& in, const char* rel, const std::vector<float>& x) {
    fs::create_directories((in / rel).parent_path());
    REQUIRE(writeWav(in / rel, x));
}

ImportResult run(const fs::path& in, const fs::path& root, bool add) {
    ImportOptions o;
    o.inputDir = in;
    o.corpusRoot = root;
    o.threads = 2;
    o.addToExisting = add;
    return importCorpus(o);
}

std::map<std::int64_t, std::vector<std::pair<std::int64_t, std::int64_t>>> anchors(const fs::path& root) {
    std::string err;
    auto db = CorpusDb::open((root / "corpus.sqlite").string(), true, &err);
    REQUIRE(db);
    std::map<std::int64_t, std::vector<std::pair<std::int64_t, std::int64_t>>> m;
    for (const auto& g : db->segments()) m[g.recordingId].emplace_back(g.anchorSample, g.maxEndSample);
    return m;
}

std::map<std::string, SpeakerRow> speakersOf(const fs::path& root) {
    std::string err;
    auto db = CorpusDb::open((root / "corpus.sqlite").string(), true, &err);
    REQUIRE(db);
    std::map<std::string, SpeakerRow> m;
    for (const auto& s : db->speakers()) m[s.externalId] = s;
    return m;
}

bool anyReason(const ImportResult& r, const std::string& rel, const std::string& code) {
    for (const auto& f : r.files)
        if (f.relPath == rel)
            for (const auto& c : f.reasons)
                if (c == code) return true;
    return false;
}

}  // namespace

TEST_CASE("corpus: incremental import, duplicates across old and new, removal, cache lease", "[ingest][corpus][incremental]") {
    const double sec = 20.0;
    const auto in1 = fixtureDir("inc_in1"), in2 = fixtureDir("inc_in2"), in3 = fixtureDir("inc_in3");
    const auto root = fixtureDir("inc_root");
    const auto b1 = speechLike(sec, 31);
    put(in1, "alice/a1.wav", speechLike(sec, 30));
    put(in1, "bob/b1.wav", b1);
    put(in1, "carol/c1.wav", speechLike(sec, 32));
    put(in2, "dave/d1.wav", speechLike(sec, 33));
    put(in2, "alice/a2.wav", speechLike(sec, 34));
    put(in2, "bobcopy/b1_copy.wav", b1);  // exact duplicate of an EXISTING recording

    const auto r1 = run(in1, root, false);
    REQUIRE(r1.ok);
    REQUIRE(r1.nUsableSpeakers == 3);
    const auto anchors1 = anchors(root);
    const auto spk1 = speakersOf(root);
    REQUIRE(spk1.size() == 3);
    LoadedCorpus c1;
    REQUIRE(loadCorpus(root, c1));
    CHECK(c1.snapshot->numSpeakers() == 3);
    CHECK(c1.snapshot->corpusVersion() == r1.corpusVersion);
    REQUIRE(c1.audio->cacheDir() == root / "cache");

    // ---- add ----
    const auto r2 = run(in2, root, true);
    REQUIRE(r2.ok);
    INFO(r2.error);
    CHECK(r2.added);
    CHECK(r2.previousVersion == r1.corpusVersion);
    CHECK(r2.corpusVersion != r1.corpusVersion);
    CHECK(r2.nNewSpeakers == 2);  // dave and bobcopy
    CHECK(r2.nTotalFiles == 6);
    CHECK(anyReason(r2, "bobcopy/b1_copy.wav", "duplicate.exact"));  // duplicate against the existing corpus
    CHECK_FALSE(r2.retiredCacheDir.empty());
    CHECK(fs::exists(r2.retiredCacheDir));

    // Old ids and anchors are stable; the new recordings continue the numbering.
    const auto anchors2 = anchors(root);
    for (std::int64_t id = 1; id <= 3; ++id) {
        REQUIRE(anchors1.count(id));
        CHECK(anchors2.at(id) == anchors1.at(id));
    }
    const auto spk2 = speakersOf(root);
    for (const auto& [name, s] : spk1) CHECK(spk2.at(name).speakerId == s.speakerId);
    CHECK(spk2.at("dave").speakerId > 3);
    CHECK(spk2.at("alice").nRecordings == 2);
    CHECK(spk2.at("bob").pc1 != spk1.at("bob").pc1);  // PCA recomputed over the larger speaker set
    CHECK_FALSE(spk2.at("bobcopy").enabled);          // only a duplicate: no usable recording

    LoadedCorpus c2;
    REQUIRE(loadCorpus(root, c2));
    CHECK(c2.snapshot->numSpeakers() == 4);  // alice, bob, carol, dave
    CHECK(c2.snapshot->corpusVersion() == r2.corpusVersion);
    CHECK(c2.snapshot->numRecordings() == 5);
    // Every cache file (old and new) is readable from the merged cache.
    std::vector<float> buf(4800);
    for (RecordingId r = 0; r < c2.snapshot->numRecordings(); ++r) CHECK(c2.audio->read(r, 48000, buf.data(), buf.size()));

    // The old source follows the renamed generation and keeps working; the merged one is separate.
    c1.audio->relocate(r2.retiredCacheDir);
    CHECK(c1.audio->cacheDir() == r2.retiredCacheDir);
    CHECK(FlacCacheAudioSource::isPinned(r2.retiredCacheDir));
    for (RecordingId r = 0; r < c1.snapshot->numRecordings(); ++r) CHECK(c1.audio->read(r, 96000, buf.data(), buf.size()));

    // Adding only duplicates (the same folder again) changes nothing: same version, same rows.
    const auto r3 = run(in2, root, true);
    REQUIRE(r3.ok);
    CHECK(r3.corpusVersion == r2.corpusVersion);
    CHECK(anchors(root) == anchors2);
    CHECK(anyReason(r3, "dave/d1.wav", "duplicate.exact"));
    CHECK(anyReason(r3, "alice/a2.wav", "duplicate.exact"));

    // ---- cache generation purge only after release ----
    {
        std::error_code ec;
        // Generations now: cache.old-<v1> (pinned by c1.audio), cache.old-<v2> (most recent).
        const fs::path gen1 = r2.retiredCacheDir;
        REQUIRE(fs::exists(gen1, ec));
        const auto r4 = run(in3, root, true);  // in3 is empty -> error, nothing touched
        CHECK_FALSE(r4.ok);
        put(in3, "erin/e1.wav", speechLike(sec, 35));
        const auto r5 = run(in3, root, true);
        REQUIRE(r5.ok);
        CHECK(fs::exists(gen1, ec));  // still read by c1.audio
        const int removedPinned = CorpusDb::purgeOldCaches(root.string());
        CHECK(fs::exists(gen1, ec));
        (void)removedPinned;
        c1.audio.reset();
        c1.snapshot.reset();
        CHECK_FALSE(FlacCacheAudioSource::isPinned(gen1));
        CorpusDb::purgeOldCaches(root.string());
        CHECK_FALSE(fs::exists(gen1, ec));  // released: purged (a newer generation remains)
    }

    // ---- controller: a stopped engine takes the library from disk (reloadCorpus loads on the control thread) ----
    {
        rt::NullBackend backend;
        rt::EngineControllerConfig cc;
        cc.dataSet = &bftest::dataSet();
        cc.backend = &backend;
        cc.deviceId = backend.devices().front().id;
        cc.seed = 3;
        rt::EngineController ctl(cc);
        CHECK(ctl.corpusVersion().empty());
        CHECK(ctl.reloadCorpus(root / "nonexistent") == rt::CommandResult::Failed);
        REQUIRE(ctl.reloadCorpus(root) == rt::CommandResult::Ok);
        LoadedCorpus now;
        REQUIRE(loadCorpus(root, now));
        CHECK(ctl.corpusVersion() == now.snapshot->corpusVersion());
        ctl.logger().flush();
        CHECK(ctl.logger().countOf(rt::logcode::kCorpusLoaded) >= 1);
        CHECK(ctl.logger().countOf(rt::logcode::kCorpusReloadFailed) == 1);
    }

    // ---- removal ----
    {
        const auto before = speakersOf(root);
        REQUIRE(before.at("dave").enabled);
        RemoveOptions ro;
        ro.corpusRoot = root;
        ro.speakers = {"dave"};
        const auto rr = removeFromCorpus(ro);
        REQUIRE(rr.ok);
        INFO(rr.error);
        CHECK(rr.speakersRemoved == 1);
        CHECK(rr.recordingsRemoved == 1);
        CHECK(rr.corpusVersion != rr.previousVersion);
        const auto after = speakersOf(root);
        CHECK_FALSE(after.at("dave").enabled);
        LoadedCorpus c;
        REQUIRE(loadCorpus(root, c));
        CHECK(c.snapshot->numSpeakers() == before.size() - 1 - 1);  // bobcopy was never usable
        CHECK(c.snapshot->corpusVersion() == rr.corpusVersion);
        // The remaining speakers keep their ids and anchors.
        CHECK(after.at("alice").speakerId == before.at("alice").speakerId);

        // Removing a single recording.
        RemoveOptions ro2;
        ro2.corpusRoot = root;
        ro2.recordings = {2};  // bob/b1.wav
        const auto rr2 = removeFromCorpus(ro2);
        REQUIRE(rr2.ok);
        CHECK(rr2.recordingsRemoved == 1);
        CHECK_FALSE(speakersOf(root).at("bob").enabled);  // bob had one recording only
        ro2.recordings = {9999};
        CHECK_FALSE(removeFromCorpus(ro2).ok);
        ro2.recordings.clear();
        ro2.speakers = {"nobody"};
        CHECK_FALSE(removeFromCorpus(ro2).ok);

        // A removed recording can be re-added (it is no longer an active duplicate).
        const auto r6 = run(in2, root, true);
        REQUIRE(r6.ok);
        CHECK_FALSE(anyReason(r6, "dave/d1.wav", "duplicate.exact"));
        CHECK(speakersOf(root).at("dave").enabled);
    }
}
#endif
