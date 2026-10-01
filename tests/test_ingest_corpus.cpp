// End-to-end tests of the on-disk corpus: import -> SQLite/FLAC cache -> snapshot + audio source.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <set>

#include "core/corpus/ingest/SpeakerIdentity.h"
#include "ingest_fixtures.h"

#ifdef BF_WITH_CORPUS_DB
#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include "core/corpus/CorpusDb.h"
#include "core/corpus/CorpusImporter.h"
#include "core/corpus/CorpusLoader.h"
#include "core/corpus/FlacCache.h"
#include "core/corpus/ingest/Analyzer.h"
#endif

using namespace bf;
using namespace bf::ingest;
using namespace bftest;
using Catch::Approx;

TEST_CASE("speaker identity sources and priority", "[ingest][speakers]") {
    const auto dir = fixtureDir("speakers");
    {
        std::ofstream f(dir / "speakers.csv");
        f << "file,speaker_id,language,labels\n"
             "csv/one.wav,csvSpeaker,en-GB,\"{\"\"gender\"\":\"\"f\"\"}\"\n"
             "\"quoted,name.wav\",qs,de-DE\n";
    }
    SpeakerResolver r;
    std::string err;
    REQUIRE(r.init(dir / "speakers.csv", R"(^([A-Za-z0-9]+)_.*)", &err));
    SECTION("csv wins over folder and regex") {
        const auto a = r.resolve("csv/one.wav");
        CHECK(a.externalId == "csvSpeaker");
        CHECK(a.language == "en-GB");
        CHECK(a.labelsJson.find("gender") != std::string::npos);
        CHECK_FALSE(a.unknown);
        CHECK(r.resolve("quoted,name.wav").externalId == "qs");
        CHECK(r.resolve("elsewhere/quoted,name.wav").externalId == "qs");  // basename match
    }
    SECTION("folder convention") {
        CHECK(r.resolve("spk7/file_a.wav").externalId == "spk7");
    }
    SECTION("filename regex") {
        const auto a = r.resolve("bob_take3.wav");
        CHECK(a.externalId == "bob");
        CHECK_FALSE(a.unknown);
    }
    SECTION("otherwise one speaker per file, flagged unknown") {
        const auto a = r.resolve("noseparator.wav");
        CHECK(a.unknown);
        CHECK(a.externalId == "noseparator.wav");
    }
    SECTION("csv line splitting") {
        CHECK(splitCsvLine("a,\"b,c\",\"d\"\"e\",") == std::vector<std::string>{"a", "b,c", "d\"e", ""});
    }
}

#ifdef BF_WITH_CORPUS_DB

namespace {

std::vector<float> reversed(std::vector<float> x) {
    std::reverse(x.begin(), x.end());
    return x;
}

std::vector<std::string> columns(sqlite3* db, const char* table) {
    std::vector<std::string> c;
    sqlite3_stmt* s = nullptr;
    const std::string q = std::string("PRAGMA table_info(") + table + ")";
    sqlite3_prepare_v2(db, q.c_str(), -1, &s, nullptr);
    while (sqlite3_step(s) == SQLITE_ROW) c.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(s, 1)));
    sqlite3_finalize(s);
    return c;
}

std::map<std::string, ImportFileReport> byPath(const ImportResult& r) {
    std::map<std::string, ImportFileReport> m;
    for (const auto& f : r.files) m[f.relPath] = f;
    return m;
}

struct Built {
    std::filesystem::path in;
    ImportResult res;
};

// Shared fixture corpus (25 s files) imported once for all sections.
const Built& built() {
    static const Built b = [] {
        Built out;
        out.in = fixtureDir("corpus_in");
        const auto root = fixtureDir("corpus_out");
        const double sec = 25.0;
        const auto a1 = speechLike(sec, 21), a2 = speechLike(sec, 22), b1 = speechLike(sec, 23), b2 = speechLike(sec, 24),
                   c1 = speechLike(sec, 25);
        auto put = [&](const char* rel, const std::vector<float>& x) {
            std::filesystem::create_directories((out.in / rel).parent_path());
            REQUIRE(writeWav(out.in / rel, x));
        };
        put("alice/a1.wav", a1);
        put("gina/a2.wav", a2);
        put("bob/b1.wav", b1);
        put("bob/b2.wav", b2);
        put("carol/c1.wav", c1);
        put("alice2/a1_rev.wav", reversed(a1));         // same voice features, different content
        std::filesystem::copy_file(out.in / "carol/c1.wav", out.in / "carol/c1_copy.wav");   // exact duplicate
        std::vector<float> re = lowpass(b1, 12000.0);                                      // re-encoded duplicate
        for (auto& v : re) v = std::round(v * 0.5f * 32768.0f) / 32768.0f;
        put("bob/b1_reenc.wav", re);
        put("dave/clip.wav", clipped(a2, 25.0f));
        put("eve/noisy.wav", addWhiteNoise(b2, -41.0));
        std::ofstream(out.in / "unknown.mp3", std::ios::binary) << "ID3 not decodable yet";
        std::ofstream(out.in / "speakers.csv") << "file,speaker_id,language\nalice/a1.wav,alice,en-GB\n";

        ImportOptions opt;
        opt.inputDir = out.in;
        opt.corpusRoot = root;
        opt.speakersCsv = out.in / "speakers.csv";
        opt.threads = 2;
        out.res = importCorpus(opt);
        return out;
    }();
    return b;
}

}  // namespace

// One test case (one ctest process): the fixture corpus is imported once and shared by all sections.
TEST_CASE("corpus: import, database, loader, cache and versioning", "[ingest][corpus]") {
SECTION("import: classification, duplicates, reason codes") {
    const auto& b = built();
    REQUIRE(b.res.ok);
    INFO(b.res.error);
    const auto f = byPath(b.res);
    REQUIRE(f.size() == 11);  // speakers.csv is not audio
    for (const char* good : {"alice/a1.wav", "gina/a2.wav", "bob/b1.wav", "bob/b2.wav", "carol/c1.wav", "alice2/a1_rev.wav"}) {
        std::string rs;
        for (const auto& c : f.at(good).reasons) rs += c + " ";
        INFO(good << " reasons: " << rs << " snr " << f.at(good).snrDb << " bw " << f.at(good).bandwidthHz);
        CHECK(f.at(good).cls != QualityClass::Rejected);
    }
    CHECK(f.at("carol/c1_copy.wav").cls == QualityClass::Rejected);
    CHECK(std::count(f.at("carol/c1_copy.wav").reasons.begin(), f.at("carol/c1_copy.wav").reasons.end(), "duplicate.exact") == 1);
    CHECK(f.at("bob/b1_reenc.wav").cls == QualityClass::Rejected);
    CHECK(std::count(f.at("bob/b1_reenc.wav").reasons.begin(), f.at("bob/b1_reenc.wav").reasons.end(), "duplicate.near") == 1);
    CHECK(f.at("dave/clip.wav").cls == QualityClass::Rejected);
    CHECK(std::count(f.at("dave/clip.wav").reasons.begin(), f.at("dave/clip.wav").reasons.end(), "reject.clipping") == 1);
    CHECK(std::count(f.at("eve/noisy.wav").reasons.begin(), f.at("eve/noisy.wav").reasons.end(), "reject.snr") == 1);
    CHECK(f.at("unknown.mp3").reasons == std::vector<std::string>{"decode.unsupported"});
    CHECK(b.res.nGood + b.res.nUsable == 6);
    CHECK(b.res.nRejected == 5);
    CHECK(b.res.nSpeakers == 8);
    CHECK(b.res.nUsableSpeakers == 5);
    CHECK(b.res.corpusVersion.size() == 16);
}

SECTION("import: DB schema, manifest, cache layout") {
    const auto& b = built();
    REQUIRE(b.res.ok);
    const auto root = bftest::fixturePath("corpus_out");
    // Schema columns exactly as CORPUS.md §5.
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open_v2((root / "corpus.sqlite").string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    CHECK(columns(db, "corpus_info") == std::vector<std::string>{"key", "value"});
    CHECK(columns(db, "speaker") == std::vector<std::string>{
        "speaker_id", "external_id", "language", "n_recordings", "usable_speech_s", "f0_median_hz", "f0_mean_hz", "f0_p5_hz",
        "f0_p95_hz", "f0_range_st", "speaking_rate_sps", "spectral_centroid_hz", "ltass_third_oct_db", "ltass_pc1", "ltass_pc2",
        "ltass_pc3", "asl_mean_dbfs", "quality_mean", "flags", "enabled", "user_labels"});
    CHECK(columns(db, "recording") == std::vector<std::string>{
        "recording_id", "speaker_id", "source_path", "source_sha256", "pcm_sha256", "cache_file", "source_sample_rate",
        "source_channels", "source_format", "lossy", "duration_s", "speech_s", "speech_ratio", "peak_dbfs", "true_peak_dbtp",
        "rms_dbfs", "asl_dbfs", "activity_pct", "lufs_i", "noise_floor_dbfs", "snr_db", "bandwidth_hz", "clip_ratio", "dc_offset",
        "reverberant", "f0_median_hz", "f0_p5_hz", "f0_p95_hz", "f0_hist", "speaking_rate_sps", "spectral_centroid_hz",
        "ltass_third_oct_db", "pause_hist", "pause_frac_gt100", "pause_frac_gt250", "pause_frac_gt600", "quality_score",
        "quality_class", "reason_codes", "fingerprint", "vad_engine", "analyzer_version", "analyzed_utc"});
    CHECK(columns(db, "speech_region") == std::vector<std::string>{"recording_id", "start_sample", "end_sample"});
    CHECK(columns(db, "segment") == std::vector<std::string>{
        "segment_id", "recording_id", "anchor_sample", "max_end_sample", "speech_frac_10s", "longest_phrase_s", "asl_10s_dbfs",
        "solo_risk", "excluded"});
    sqlite3_close(db);

    auto cdb = CorpusDb::open((root / "corpus.sqlite").string(), true);
    REQUIRE(cdb);
    std::string v, an, pca;
    CHECK(cdb->getInfo("corpusVersion", v));
    CHECK(v == b.res.corpusVersion);
    CHECK(cdb->getInfo("analyzerVersion", an));
    CHECK(an == kAnalyzerVersion);
    REQUIRE(cdb->getInfo("pcaBasis", pca));
    const auto pj = nlohmann::json::parse(pca);
    REQUIRE(pj["components"].size() == 3);
    REQUIRE(pj["components"][0].size() == 21);
    double nrm = 0;
    for (double c : pj["components"][0]) nrm += c * c;
    CHECK(nrm == Approx(1.0).margin(1e-6));

    const auto recs = cdb->recordings();
    CHECK(recs.size() == 11);
    std::size_t withCache = 0;
    for (const auto& r : recs) {
        const bool usable = r.qualityClass > 0;
        CHECK(r.cacheFile.empty() == !usable);
        CHECK(r.analyzerVersion == kAnalyzerVersion);
        if (usable) {
            ++withCache;
            CHECK(std::filesystem::exists(root / r.cacheFile));
            CHECK(r.durationS == Approx(25.0));
            CHECK(r.sourceSha256.size() == 64);
            CHECK(r.vadEngine == "webrtc-fvad-m2");
            CHECK(r.ltassThirdOctDb.size() == 26);
            CHECK(r.f0Hist.size() == 12);
            CHECK(r.pauseHist.size() == 6);
            CHECK(r.fingerprint.size() > 1000);
        }
    }
    CHECK(withCache == 6);
    std::size_t nFlac = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root / "cache"))
        if (e.path().extension() == ".flac") ++nFlac;
    CHECK(nFlac == 6);  // rejected files leave no cache file
    CHECK_FALSE(std::filesystem::exists(root / ".staging"));

    const auto man = nlohmann::json::parse(std::ifstream(root / "manifest.json"));
    CHECK(man["corpusVersion"] == b.res.corpusVersion);
    CHECK(man["analyzerVersion"] == kAnalyzerVersion);
    CHECK(man["counts"]["rejected"] == 5);
    CHECK(man["pcaBasis"]["components"].size() == 3);
    CHECK(man["corpusHash"].get<std::string>().rfind(b.res.corpusVersion, 0) == 0);

    // Speakers: language from CSV, unknown identity flag, PCA scores mean-removed, possible duplicate.
    const auto spk = cdb->speakers();
    std::map<std::string, SpeakerRow> sp;
    for (const auto& s : spk) sp[s.externalId] = s;
    CHECK(sp.at("alice").language == "en-GB");
    CHECK((sp.at("unknown.mp3").flags & kSpkUnknownIdentity) != 0);
    CHECK(sp.at("unknown.mp3").enabled == false);
    CHECK(sp.at("bob").nRecordings == 2);
    CHECK(sp.at("bob").usableSpeechS > 10.0);
    double s1 = 0, s2 = 0, s3 = 0;
    std::size_t nu = 0;
    for (const auto& s : spk) {
        if (!s.enabled) continue;
        ++nu;
        s1 += s.pc1; s2 += s.pc2; s3 += s.pc3;
        CHECK(s.ltassThirdOctDb.size() == 26);
        CHECK(s.f0MedianHz > 60.0);
    }
    CHECK(nu == 5);
    CHECK(std::fabs(s1) < 1e-6);
    CHECK(std::fabs(s2) < 1e-6);
    CHECK(std::fabs(s3) < 1e-6);
    // Same voice under different ids (time-reversed copy of the same speech): flagged, both ways.
    CHECK((sp.at("alice").flags & kSpkPossibleDuplicate) != 0);
    CHECK((sp.at("alice2").flags & kSpkPossibleDuplicate) != 0);
    CHECK((sp.at("bob").flags & kSpkPossibleDuplicate) == 0);
}

SECTION("loader: snapshot fields and anchors match the database") {
    const auto& b = built();
    REQUIRE(b.res.ok);
    const auto root = bftest::fixturePath("corpus_out");
    LoadedCorpus lc;
    std::string err;
    REQUIRE(loadCorpus(root, lc, &err));
    const auto snap = lc.snapshot;
    REQUIRE(snap);
    CHECK(snap->corpusVersion() == b.res.corpusVersion);
    CHECK(snap->numSpeakers() == 5);
    CHECK(snap->numRecordings() == 6);
    CHECK(lc.languages == std::vector<std::string>{"en-GB"});
    CHECK(snap->totalSpeechSeconds() > 40.0);

    auto cdb = CorpusDb::open((root / "corpus.sqlite").string(), true);
    const auto recs = cdb->recordings();
    const auto segs = cdb->segments();
    const auto regs = cdb->regions();
    std::map<std::int64_t, RecordingRow> recById;
    for (const auto& r : recs) recById[r.recordingId] = r;

    std::size_t nSegSnap = 0;
    for (std::size_t s = 0; s < snap->numSpeakers(); ++s) {
        const auto& sr = snap->speaker(static_cast<SpeakerId>(s));
        CHECK(sr.nRecordings >= 1);
        CHECK(sr.nAnchors >= 1);
        CHECK(sr.weight > 0.5f);
        CHECK(sr.weight <= 1.0f);
        CHECK(sr.usableSpeechS > 5.0);
        CHECK(sr.features[kFeatCentroidLog2] > 6.0f);
        CHECK(sr.features[kFeatLanguage] == (lc.speakerDbId[s] == 0 ? 0.0f : sr.features[kFeatLanguage]));
        for (const auto& a : snap->anchorsOf(static_cast<SpeakerId>(s))) {
            ++nSegSnap;
            const auto& rec = snap->recording(a.recording);
            const auto dbId = lc.recordingDbId[a.recording];
            CHECK(rec.speaker == s);
            CHECK(a.anchor >= 0);
            CHECK(a.anchor + a.maxLen == rec.length);
            CHECK(a.maxLen >= 2 * kCorpusRate);
            // Anchor is the start of a speech region and matches a stored segment row.
            bool inRegion = false;
            for (const auto& r : regs) if (r.recordingId == dbId && r.startSample == a.anchor) inRegion = true;
            CHECK(inRegion);
            const auto it = std::find_if(segs.begin(), segs.end(), [&](const SegmentDbRow& g) { return g.recordingId == dbId && g.anchorSample == a.anchor; });
            REQUIRE(it != segs.end());
            CHECK(it->maxEndSample == rec.length);
            CHECK(a.aslDb == Approx(it->asl10sDbfs).margin(1e-3));
            CHECK(a.speechFrac == Approx(it->speechFrac10s).margin(1e-4));
            CHECK(a.longestPhraseS == Approx(it->longestPhraseS).margin(1e-4));
            CHECK(((a.flags & kSegSoloRisk) != 0) == it->soloRisk);
        }
    }
    std::size_t nSegDb = 0;
    for (const auto& g : segs) if (!g.excluded && recById.at(g.recordingId).qualityClass > 0) ++nSegDb;
    CHECK(nSegSnap == nSegDb);
    CHECK(snap->numSegments() == nSegDb);
    // Regions and lengths.
    for (std::size_t r = 0; r < snap->numRecordings(); ++r) {
        const auto dbId = lc.recordingDbId[r];
        std::size_t n = 0;
        for (const auto& g : regs) if (g.recordingId == dbId) ++n;
        CHECK(snap->regionsOf(static_cast<RecordingId>(r)).size() == n);
        CHECK(snap->recording(static_cast<RecordingId>(r)).length == 25 * kCorpusRate);
        CHECK(snap->recording(static_cast<RecordingId>(r)).aslDb == Approx(recById.at(dbId).aslDbfs).margin(1e-3));
        CHECK(snap->recording(static_cast<RecordingId>(r)).quality == Approx(recById.at(dbId).qualityScore / 100.0).margin(1e-4));
    }
}

SECTION("loader: cache audio equals the processed audio within 24-bit quantisation") {
    const auto& b = built();
    REQUIRE(b.res.ok);
    const auto root = bftest::fixturePath("corpus_out");
    LoadedCorpus lc;
    REQUIRE(loadCorpus(root, lc));
    auto cdb = CorpusDb::open((root / "corpus.sqlite").string(), true);
    std::map<std::int64_t, RecordingRow> recById;
    for (const auto& r : cdb->recordings()) recById[r.recordingId] = r;

    // Check two recordings in full against a fresh analysis (the pipeline is deterministic).
    for (RecordingId rid : {RecordingId{0}, RecordingId{4}}) {
        const auto& row = recById.at(lc.recordingDbId[rid]);
        const auto ref = analyzeFile((b.in / row.sourcePath).string());
        REQUIRE(ref.audio48k.size() == 25u * 48000u);
        std::vector<float> got(ref.audio48k.size());
        REQUIRE(lc.audio->read(rid, 0, got.data(), got.size()));
        double err = 0;
        for (std::size_t i = 0; i < got.size(); ++i) err = std::max(err, std::fabs(static_cast<double>(got[i]) - static_cast<double>(ref.audio48k[i])));
        CHECK(err <= 0.5 / 8388608.0 + 1e-8);
        CHECK(row.pcmSha256 == ref.pcmSha256);

        // Random access (seeks) returns the same samples as the sequential read.
        std::vector<float> chunk(5000);
        for (std::uint64_t start : {std::uint64_t{123457}, std::uint64_t{4096}, std::uint64_t{900001}, std::uint64_t{0}, std::uint64_t{1199000}}) {
            REQUIRE(lc.audio->read(rid, start, chunk.data(), chunk.size()));
            for (std::size_t i = 0; i < chunk.size(); ++i) {
                if (start + i >= got.size()) { REQUIRE(chunk[i] == 0.0f); continue; }
                REQUIRE(chunk[i] == got[start + i]);
            }
        }
        // Past the end: zeros, no error.
        REQUIRE(lc.audio->read(rid, got.size() + 100, chunk.data(), chunk.size()));
        CHECK(chunk[0] == 0.0f);
    }
    // Unknown recording id fails.
    float z[4];
    CHECK_FALSE(lc.audio->read(9999, 0, z, 4));
}

SECTION("FlacCacheAudioSource keeps at most 64 decoders open") {
    const auto& b = built();
    REQUIRE(b.res.ok);
    const auto root = bftest::fixturePath("corpus_out");
    LoadedCorpus lc;
    REQUIRE(loadCorpus(root, lc));
    auto cdb = CorpusDb::open((root / "corpus.sqlite").string(), true);
    std::filesystem::path any;
    for (const auto& r : cdb->recordings()) if (!r.cacheFile.empty()) { any = root / r.cacheFile; break; }
    std::vector<std::filesystem::path> paths(80, any);
    FlacCacheAudioSource src(paths);
    std::vector<float> ref(2000), x(2000);
    REQUIRE(src.read(0, 50000, ref.data(), ref.size()));
    for (RecordingId r = 0; r < 80; ++r) {
        REQUIRE(src.read(r, 50000, x.data(), x.size()));
        CHECK(x == ref);
        CHECK(src.openDecoders() <= FlacCacheAudioSource::kMaxOpenDecoders);
    }
    CHECK(src.openDecoders() == FlacCacheAudioSource::kMaxOpenDecoders);
    // Recording 0 was evicted; it can be reopened transparently.
    REQUIRE(src.read(0, 50000, x.data(), x.size()));
    CHECK(x == ref);
}

SECTION("corpusVersion is stable across re-imports (thread count, root)") {
    const auto& b = built();
    REQUIRE(b.res.ok);
    ImportOptions opt;
    opt.inputDir = b.in;
    opt.speakersCsv = b.in / "speakers.csv";
    opt.threads = 1;  // different thread count, different root
    opt.corpusRoot = fixtureDir("corpus_out2");
    const auto r2 = importCorpus(opt);
    REQUIRE(r2.ok);
    CHECK(r2.corpusVersion == b.res.corpusVersion);
    CHECK(r2.nGood + r2.nUsable == b.res.nGood + b.res.nUsable);
    // Re-import over the existing corpus replaces it atomically and yields the same version.
    const auto r3 = importCorpus(opt);
    REQUIRE(r3.ok);
    CHECK(r3.corpusVersion == b.res.corpusVersion);
    CHECK_FALSE(std::filesystem::exists(opt.corpusRoot / ".staging"));
    CHECK_FALSE(std::filesystem::exists(opt.corpusRoot / ".cache.old"));
    // The previous cache generation is retained as cache.old-<version>, not deleted at once.
    CHECK(std::filesystem::is_directory(opt.corpusRoot / ("cache.old-" + b.res.corpusVersion)));
    {
        namespace fs = std::filesystem;
        fs::create_directories(opt.corpusRoot / "cache.old-aaaa");
        fs::create_directories(opt.corpusRoot / "cache.old-zzzz");
        fs::last_write_time(opt.corpusRoot / "cache.old-aaaa", fs::file_time_type::clock::now() - std::chrono::hours(48));
        fs::last_write_time(opt.corpusRoot / "cache.old-zzzz", fs::file_time_type::clock::now() + std::chrono::hours(48));
        CHECK(CorpusDb::purgeOldCaches(opt.corpusRoot.string()) == 2);  // keeps only the newest (zzzz)
        CHECK(fs::exists(opt.corpusRoot / "cache.old-zzzz"));
        CHECK_FALSE(fs::exists(opt.corpusRoot / "cache.old-aaaa"));
        CHECK_FALSE(fs::exists(opt.corpusRoot / ("cache.old-" + b.res.corpusVersion)));
        fs::create_directories(opt.corpusRoot / "cache.old-aaaa");
        REQUIRE(importCorpus(opt).ok);  // next import purges superseded generations first
        CHECK_FALSE(fs::exists(opt.corpusRoot / "cache.old-aaaa"));
    }
    LoadedCorpus lc;
    REQUIRE(loadCorpus(opt.corpusRoot, lc));
    CHECK(lc.snapshot->corpusVersion() == b.res.corpusVersion);
}

SECTION("import of an unusable input directory fails cleanly") {
    ImportOptions opt;
    opt.inputDir = fixtureDir("empty_in");
    opt.corpusRoot = fixtureDir("empty_out");
    CHECK_FALSE(importCorpus(opt).ok);
    opt.inputDir = "/nonexistent/dir/for/bfcorpus";
    CHECK_FALSE(importCorpus(opt).ok);
}
}

TEST_CASE("corpus: overlapping content excludes anchors in the later recording", "[ingest][corpus][overlap]") {
    const auto in = fixtureDir("overlap_in");
    const auto root = fixtureDir("overlap_out");
    const auto first = speechLike(25.0, 31);
    const auto other = speechLike(30.0, 32);
    // Later file: 30 s of unrelated speech followed by the first 12 s of the earlier file.
    std::vector<float> second = other;
    second.insert(second.end(), first.begin(), first.begin() + 48000 * 12);
    std::filesystem::create_directories(in / "a");
    std::filesystem::create_directories(in / "b");
    REQUIRE(writeWav(in / "a/first.wav", first));
    REQUIRE(writeWav(in / "b/second.wav", second));

    ImportOptions opt;
    opt.inputDir = in;
    opt.corpusRoot = root;
    opt.threads = 1;
    const auto res = importCorpus(opt);
    REQUIRE(res.ok);
    const auto f = byPath(res);
    const auto& rep = f.at("b/second.wav");
    CHECK(rep.cls != QualityClass::Rejected);
    CHECK(std::count(rep.reasons.begin(), rep.reasons.end(), "duplicate.overlap") == 1);
    CHECK(std::count(f.at("a/first.wav").reasons.begin(), f.at("a/first.wav").reasons.end(), "duplicate.overlap") == 0);

    auto db = CorpusDb::open((root / "corpus.sqlite").string(), true);
    REQUIRE(db);
    std::map<std::int64_t, std::string> paths;
    for (const auto& r : db->recordings()) paths[r.recordingId] = r.sourcePath;
    std::size_t inside = 0, insideExcluded = 0, outside = 0, outsideExcluded = 0, firstExcluded = 0;
    for (const auto& g : db->segments()) {
        const bool isSecond = paths[g.recordingId].find("second") != std::string::npos;
        if (!isSecond) { firstExcluded += g.excluded ? 1 : 0; continue; }
        // Stay clear of the region edges (alignment granularity ~12 ms plus anchor snapping).
        if (g.anchorSample > 48000 * 31 && g.anchorSample < 48000 * 41) { ++inside; insideExcluded += g.excluded ? 1 : 0; }
        if (g.anchorSample < 48000 * 29) { ++outside; outsideExcluded += g.excluded ? 1 : 0; }
    }
    CHECK(firstExcluded == 0);
    CHECK(inside > 0);
    CHECK(insideExcluded == inside);
    CHECK(outside > 0);
    CHECK(outsideExcluded == 0);
}

#endif  // BF_WITH_CORPUS_DB
