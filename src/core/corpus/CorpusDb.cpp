#include "core/corpus/CorpusDb.h"

#include <sqlite3.h>

#include <cstring>
#include <filesystem>

namespace bf {

namespace {

const char* kSchema = R"SQL(
CREATE TABLE corpus_info (key TEXT PRIMARY KEY, value TEXT);

CREATE TABLE speaker (
  speaker_id        INTEGER PRIMARY KEY,
  external_id       TEXT UNIQUE,
  language          TEXT,
  n_recordings      INTEGER,
  usable_speech_s   REAL,
  f0_median_hz      REAL, f0_mean_hz REAL, f0_p5_hz REAL, f0_p95_hz REAL,
  f0_range_st       REAL,
  speaking_rate_sps REAL,
  spectral_centroid_hz REAL,
  ltass_third_oct_db BLOB,
  ltass_pc1 REAL, ltass_pc2 REAL, ltass_pc3 REAL,
  asl_mean_dbfs     REAL,
  quality_mean      REAL,
  flags             INTEGER,
  enabled           INTEGER DEFAULT 1,
  user_labels       TEXT
);

CREATE TABLE recording (
  recording_id      INTEGER PRIMARY KEY,
  speaker_id        INTEGER REFERENCES speaker,
  source_path       TEXT,
  source_sha256     TEXT, pcm_sha256 TEXT,
  cache_file        TEXT,
  source_sample_rate INTEGER, source_channels INTEGER, source_format TEXT, lossy INTEGER,
  duration_s        REAL, speech_s REAL, speech_ratio REAL,
  peak_dbfs REAL, true_peak_dbtp REAL, rms_dbfs REAL, asl_dbfs REAL, activity_pct REAL,
  lufs_i REAL, noise_floor_dbfs REAL, snr_db REAL, bandwidth_hz REAL,
  clip_ratio REAL, dc_offset REAL, reverberant INTEGER,
  f0_median_hz REAL, f0_p5_hz REAL, f0_p95_hz REAL, f0_hist BLOB,
  speaking_rate_sps REAL, spectral_centroid_hz REAL,
  ltass_third_oct_db BLOB,
  pause_hist BLOB, pause_frac_gt100 REAL, pause_frac_gt250 REAL, pause_frac_gt600 REAL,
  quality_score REAL, quality_class INTEGER, reason_codes TEXT,
  fingerprint BLOB,
  vad_engine TEXT, analyzer_version TEXT, analyzed_utc TEXT
);

CREATE TABLE speech_region (
  recording_id INTEGER, start_sample INTEGER, end_sample INTEGER,
  PRIMARY KEY (recording_id, start_sample));

CREATE TABLE segment (
  segment_id        INTEGER PRIMARY KEY,
  recording_id      INTEGER REFERENCES recording,
  anchor_sample     INTEGER,
  max_end_sample    INTEGER,
  speech_frac_10s   REAL, longest_phrase_s REAL, asl_10s_dbfs REAL,
  solo_risk         INTEGER,
  excluded          INTEGER DEFAULT 0
);
CREATE INDEX segment_by_rec ON segment(recording_id);
)SQL";

std::string insertSql(const char* table, int n) {
    std::string q = std::string("INSERT INTO ") + table + " VALUES (";
    for (int i = 0; i < n; ++i) q += i ? ",?" : "?";
    return q + ")";
}

template <typename T>
void bindBlob(sqlite3_stmt* s, int i, const std::vector<T>& v) {
    if (v.empty()) sqlite3_bind_null(s, i);
    else sqlite3_bind_blob(s, i, v.data(), static_cast<int>(v.size() * sizeof(T)), SQLITE_TRANSIENT);
}
void bindText(sqlite3_stmt* s, int i, const std::string& t, bool nullIfEmpty = true) {
    if (t.empty() && nullIfEmpty) sqlite3_bind_null(s, i);
    else sqlite3_bind_text(s, i, t.c_str(), static_cast<int>(t.size()), SQLITE_TRANSIENT);
}
std::string colText(sqlite3_stmt* s, int i) {
    const auto* p = sqlite3_column_text(s, i);
    return p ? reinterpret_cast<const char*>(p) : std::string();
}
template <typename T>
std::vector<T> colBlob(sqlite3_stmt* s, int i) {
    const int bytes = sqlite3_column_bytes(s, i);
    std::vector<T> v(static_cast<std::size_t>(bytes) / sizeof(T));
    if (bytes > 0) std::memcpy(v.data(), sqlite3_column_blob(s, i), v.size() * sizeof(T));
    return v;
}

}  // namespace

CorpusDb::~CorpusDb() {
    for (auto* s : {insSpeaker_, insRecording_, insRegion_, insSegment_}) sqlite3_finalize(s);
    if (db_) sqlite3_close(db_);
}

bool CorpusDb::exec(const char* sql) {
    char* msg = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) {
        err_ = msg ? msg : "sqlite error";
        sqlite3_free(msg);
        return false;
    }
    return true;
}

sqlite3_stmt* CorpusDb::prepare(const char* sql) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &s, nullptr) != SQLITE_OK) err_ = sqlite3_errmsg(db_);
    return s;
}

std::unique_ptr<CorpusDb> CorpusDb::create(const std::string& path, std::string* error) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::unique_ptr<CorpusDb> d(new CorpusDb());
    if (sqlite3_open_v2(path.c_str(), &d->db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (error) *error = "cannot create " + path;
        return nullptr;
    }
    if (!d->exec("PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;") || !d->exec(kSchema)) {
        if (error) *error = d->err_;
        return nullptr;
    }
    d->insSpeaker_ = d->prepare(
        insertSql("speaker", 21).c_str());
    d->insRecording_ = d->prepare(
        insertSql("recording", 43).c_str());
    d->insRegion_ = d->prepare("INSERT INTO speech_region VALUES (?,?,?)");
    d->insSegment_ = d->prepare("INSERT INTO segment VALUES (?,?,?,?,?,?,?,?,?)");
    if (!d->insSpeaker_ || !d->insRecording_ || !d->insRegion_ || !d->insSegment_) {
        if (error) *error = d->err_;
        return nullptr;
    }
    return d;
}

std::unique_ptr<CorpusDb> CorpusDb::open(const std::string& path, bool readOnly, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (error) *error = "no such database: " + path;
        return nullptr;
    }
    std::unique_ptr<CorpusDb> d(new CorpusDb());
    if (sqlite3_open_v2(path.c_str(), &d->db_, readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (error) *error = "cannot open " + path;
        return nullptr;
    }
    return d;
}

bool CorpusDb::begin() { return exec("BEGIN"); }
bool CorpusDb::commit() { return exec("COMMIT"); }

bool CorpusDb::setInfo(const std::string& key, const std::string& value) {
    sqlite3_stmt* s = prepare("INSERT OR REPLACE INTO corpus_info VALUES (?,?)");
    if (!s) return false;
    bindText(s, 1, key, false);
    bindText(s, 2, value, false);
    const bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

bool CorpusDb::getInfo(const std::string& key, std::string& value) const {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM corpus_info WHERE key=?", -1, &s, nullptr) != SQLITE_OK) return false;
    bindText(s, 1, key, false);
    bool ok = false;
    if (sqlite3_step(s) == SQLITE_ROW) { value = colText(s, 0); ok = true; }
    sqlite3_finalize(s);
    return ok;
}

bool CorpusDb::insertSpeaker(const SpeakerRow& r) {
    sqlite3_stmt* s = insSpeaker_;
    sqlite3_reset(s);
    int i = 1;
    sqlite3_bind_int64(s, i++, r.speakerId);
    bindText(s, i++, r.externalId);
    bindText(s, i++, r.language);
    sqlite3_bind_int(s, i++, r.nRecordings);
    sqlite3_bind_double(s, i++, r.usableSpeechS);
    sqlite3_bind_double(s, i++, r.f0MedianHz);
    sqlite3_bind_double(s, i++, r.f0MeanHz);
    sqlite3_bind_double(s, i++, r.f0P5Hz);
    sqlite3_bind_double(s, i++, r.f0P95Hz);
    sqlite3_bind_double(s, i++, r.f0RangeSt);
    sqlite3_bind_double(s, i++, r.speakingRateSps);
    sqlite3_bind_double(s, i++, r.spectralCentroidHz);
    bindBlob(s, i++, r.ltassThirdOctDb);
    sqlite3_bind_double(s, i++, r.pc1);
    sqlite3_bind_double(s, i++, r.pc2);
    sqlite3_bind_double(s, i++, r.pc3);
    sqlite3_bind_double(s, i++, r.aslMeanDbfs);
    sqlite3_bind_double(s, i++, r.qualityMean);
    sqlite3_bind_int64(s, i++, r.flags);
    sqlite3_bind_int(s, i++, r.enabled ? 1 : 0);
    bindText(s, i++, r.userLabels);
    const bool ok = sqlite3_step(s) == SQLITE_DONE;
    if (!ok) err_ = sqlite3_errmsg(db_);
    return ok;
}

bool CorpusDb::insertRecording(const RecordingRow& r) {
    sqlite3_stmt* s = insRecording_;
    sqlite3_reset(s);
    int i = 1;
    auto D = [&](double v) { sqlite3_bind_double(s, i++, v); };
    auto I = [&](std::int64_t v) { sqlite3_bind_int64(s, i++, v); };
    I(r.recordingId);
    I(r.speakerId);
    bindText(s, i++, r.sourcePath);
    bindText(s, i++, r.sourceSha256);
    bindText(s, i++, r.pcmSha256);
    bindText(s, i++, r.cacheFile);
    I(r.sourceSampleRate);
    I(r.sourceChannels);
    bindText(s, i++, r.sourceFormat);
    I(r.lossy ? 1 : 0);
    D(r.durationS); D(r.speechS); D(r.speechRatio);
    D(r.peakDbfs); D(r.truePeakDbtp); D(r.rmsDbfs); D(r.aslDbfs); D(r.activityPct);
    D(r.lufsI); D(r.noiseFloorDbfs); D(r.snrDb); D(r.bandwidthHz);
    D(r.clipRatio); D(r.dcOffset);
    I(r.reverberant ? 1 : 0);
    D(r.f0MedianHz); D(r.f0P5Hz); D(r.f0P95Hz);
    bindBlob(s, i++, r.f0Hist);
    D(r.speakingRateSps); D(r.spectralCentroidHz);
    bindBlob(s, i++, r.ltassThirdOctDb);
    bindBlob(s, i++, r.pauseHist);
    D(r.pauseFracGt100); D(r.pauseFracGt250); D(r.pauseFracGt600);
    D(r.qualityScore);
    I(r.qualityClass);
    bindText(s, i++, r.reasonCodes);
    bindBlob(s, i++, r.fingerprint);
    bindText(s, i++, r.vadEngine);
    bindText(s, i++, r.analyzerVersion);
    bindText(s, i++, r.analyzedUtc);
    const bool ok = sqlite3_step(s) == SQLITE_DONE;
    if (!ok) err_ = sqlite3_errmsg(db_);
    return ok;
}

bool CorpusDb::insertRegion(const SpeechRegionRow& r) {
    sqlite3_reset(insRegion_);
    sqlite3_bind_int64(insRegion_, 1, r.recordingId);
    sqlite3_bind_int64(insRegion_, 2, r.startSample);
    sqlite3_bind_int64(insRegion_, 3, r.endSample);
    const bool ok = sqlite3_step(insRegion_) == SQLITE_DONE;
    if (!ok) err_ = sqlite3_errmsg(db_);
    return ok;
}

bool CorpusDb::insertSegment(const SegmentDbRow& r) {
    sqlite3_stmt* s = insSegment_;
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, r.segmentId);
    sqlite3_bind_int64(s, 2, r.recordingId);
    sqlite3_bind_int64(s, 3, r.anchorSample);
    sqlite3_bind_int64(s, 4, r.maxEndSample);
    sqlite3_bind_double(s, 5, r.speechFrac10s);
    sqlite3_bind_double(s, 6, r.longestPhraseS);
    sqlite3_bind_double(s, 7, r.asl10sDbfs);
    sqlite3_bind_int(s, 8, r.soloRisk ? 1 : 0);
    sqlite3_bind_int(s, 9, r.excluded ? 1 : 0);
    const bool ok = sqlite3_step(s) == SQLITE_DONE;
    if (!ok) err_ = sqlite3_errmsg(db_);
    return ok;
}

std::vector<SpeakerRow> CorpusDb::speakers() const {
    std::vector<SpeakerRow> out;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT * FROM speaker ORDER BY speaker_id", -1, &s, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        SpeakerRow r;
        int i = 0;
        r.speakerId = sqlite3_column_int64(s, i++);
        r.externalId = colText(s, i++);
        r.language = colText(s, i++);
        r.nRecordings = sqlite3_column_int(s, i++);
        r.usableSpeechS = sqlite3_column_double(s, i++);
        r.f0MedianHz = sqlite3_column_double(s, i++);
        r.f0MeanHz = sqlite3_column_double(s, i++);
        r.f0P5Hz = sqlite3_column_double(s, i++);
        r.f0P95Hz = sqlite3_column_double(s, i++);
        r.f0RangeSt = sqlite3_column_double(s, i++);
        r.speakingRateSps = sqlite3_column_double(s, i++);
        r.spectralCentroidHz = sqlite3_column_double(s, i++);
        r.ltassThirdOctDb = colBlob<float>(s, i++);
        r.pc1 = sqlite3_column_double(s, i++);
        r.pc2 = sqlite3_column_double(s, i++);
        r.pc3 = sqlite3_column_double(s, i++);
        r.aslMeanDbfs = sqlite3_column_double(s, i++);
        r.qualityMean = sqlite3_column_double(s, i++);
        r.flags = sqlite3_column_int64(s, i++);
        r.enabled = sqlite3_column_int(s, i++) != 0;
        r.userLabels = colText(s, i++);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(s);
    return out;
}

std::vector<RecordingRow> CorpusDb::recordings() const {
    std::vector<RecordingRow> out;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT * FROM recording ORDER BY recording_id", -1, &s, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        RecordingRow r;
        int i = 0;
        auto D = [&]() { return sqlite3_column_double(s, i++); };
        r.recordingId = sqlite3_column_int64(s, i++);
        r.speakerId = sqlite3_column_int64(s, i++);
        r.sourcePath = colText(s, i++);
        r.sourceSha256 = colText(s, i++);
        r.pcmSha256 = colText(s, i++);
        r.cacheFile = colText(s, i++);
        r.sourceSampleRate = sqlite3_column_int(s, i++);
        r.sourceChannels = sqlite3_column_int(s, i++);
        r.sourceFormat = colText(s, i++);
        r.lossy = sqlite3_column_int(s, i++) != 0;
        r.durationS = D(); r.speechS = D(); r.speechRatio = D();
        r.peakDbfs = D(); r.truePeakDbtp = D(); r.rmsDbfs = D(); r.aslDbfs = D(); r.activityPct = D();
        r.lufsI = D(); r.noiseFloorDbfs = D(); r.snrDb = D(); r.bandwidthHz = D();
        r.clipRatio = D(); r.dcOffset = D();
        r.reverberant = sqlite3_column_int(s, i++) != 0;
        r.f0MedianHz = D(); r.f0P5Hz = D(); r.f0P95Hz = D();
        r.f0Hist = colBlob<float>(s, i++);
        r.speakingRateSps = D(); r.spectralCentroidHz = D();
        r.ltassThirdOctDb = colBlob<float>(s, i++);
        r.pauseHist = colBlob<std::uint32_t>(s, i++);
        r.pauseFracGt100 = D(); r.pauseFracGt250 = D(); r.pauseFracGt600 = D();
        r.qualityScore = D();
        r.qualityClass = sqlite3_column_int(s, i++);
        r.reasonCodes = colText(s, i++);
        r.fingerprint = colBlob<std::uint32_t>(s, i++);
        r.vadEngine = colText(s, i++);
        r.analyzerVersion = colText(s, i++);
        r.analyzedUtc = colText(s, i++);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(s);
    return out;
}

std::vector<SpeechRegionRow> CorpusDb::regions() const {
    std::vector<SpeechRegionRow> out;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT recording_id,start_sample,end_sample FROM speech_region ORDER BY recording_id,start_sample", -1, &s, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(s) == SQLITE_ROW)
        out.push_back({sqlite3_column_int64(s, 0), sqlite3_column_int64(s, 1), sqlite3_column_int64(s, 2)});
    sqlite3_finalize(s);
    return out;
}

std::vector<SegmentDbRow> CorpusDb::segments() const {
    std::vector<SegmentDbRow> out;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT * FROM segment ORDER BY recording_id,anchor_sample", -1, &s, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        SegmentDbRow r;
        r.segmentId = sqlite3_column_int64(s, 0);
        r.recordingId = sqlite3_column_int64(s, 1);
        r.anchorSample = sqlite3_column_int64(s, 2);
        r.maxEndSample = sqlite3_column_int64(s, 3);
        r.speechFrac10s = sqlite3_column_double(s, 4);
        r.longestPhraseS = sqlite3_column_double(s, 5);
        r.asl10sDbfs = sqlite3_column_double(s, 6);
        r.soloRisk = sqlite3_column_int(s, 7) != 0;
        r.excluded = sqlite3_column_int(s, 8) != 0;
        out.push_back(r);
    }
    sqlite3_finalize(s);
    return out;
}

std::int64_t CorpusDb::countRows(const std::string& table) const {
    sqlite3_stmt* s = nullptr;
    const std::string q = "SELECT COUNT(*) FROM " + table;
    if (sqlite3_prepare_v2(db_, q.c_str(), -1, &s, nullptr) != SQLITE_OK) return -1;
    std::int64_t n = -1;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

}  // namespace bf
