#pragma once
// SQLite corpus metadata database (docs/CORPUS.md §5; schema is reproduced verbatim).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/corpus/CorpusSnapshot.h"

struct sqlite3;
struct sqlite3_stmt;

namespace bf {

struct SpeakerRow {
    std::int64_t speakerId = 0;
    std::string externalId, language;  // language empty = NULL
    int nRecordings = 0;
    double usableSpeechS = 0;
    double f0MedianHz = 0, f0MeanHz = 0, f0P5Hz = 0, f0P95Hz = 0, f0RangeSt = 0;
    double speakingRateSps = 0, spectralCentroidHz = 0;
    std::vector<float> ltassThirdOctDb;  // 26 values
    double pc1 = 0, pc2 = 0, pc3 = 0;
    double aslMeanDbfs = 0, qualityMean = 0;
    std::int64_t flags = 0;
    bool enabled = true;
    std::string userLabels;  // JSON
};

struct RecordingRow {
    std::int64_t recordingId = 0, speakerId = 0;
    std::string sourcePath, sourceSha256, pcmSha256, cacheFile;  // cacheFile empty = NULL (rejected)
    int sourceSampleRate = 0, sourceChannels = 0;
    std::string sourceFormat;
    bool lossy = false;
    double durationS = 0, speechS = 0, speechRatio = 0;
    double peakDbfs = 0, truePeakDbtp = 0, rmsDbfs = 0, aslDbfs = 0, activityPct = 0;
    double lufsI = 0, noiseFloorDbfs = 0, snrDb = 0, bandwidthHz = 0;
    double clipRatio = 0, dcOffset = 0;
    bool reverberant = false;
    double f0MedianHz = 0, f0P5Hz = 0, f0P95Hz = 0;
    std::vector<float> f0Hist;
    double speakingRateSps = 0, spectralCentroidHz = 0;
    std::vector<float> ltassThirdOctDb;
    std::vector<std::uint32_t> pauseHist;
    double pauseFracGt100 = 0, pauseFracGt250 = 0, pauseFracGt600 = 0;
    double qualityScore = 0;
    int qualityClass = 0;  // 0 rejected, 1 usable, 2 good
    std::string reasonCodes;  // comma separated
    std::vector<std::uint32_t> fingerprint;
    std::string vadEngine, analyzerVersion, analyzedUtc;
};

struct SegmentDbRow {
    std::int64_t segmentId = 0, recordingId = 0, anchorSample = 0, maxEndSample = 0;
    double speechFrac10s = 0, longestPhraseS = 0, asl10sDbfs = 0;
    bool soloRisk = false, excluded = false;
};

struct SpeechRegionRow {
    std::int64_t recordingId = 0, startSample = 0, endSample = 0;
};

class CorpusDb {
public:
    ~CorpusDb();
    CorpusDb(const CorpusDb&) = delete;
    CorpusDb& operator=(const CorpusDb&) = delete;

    // Creates a new database file (an existing file is replaced) with the §5 schema.
    static std::unique_ptr<CorpusDb> create(const std::string& path, std::string* error = nullptr);
    static std::unique_ptr<CorpusDb> open(const std::string& path, bool readOnly, std::string* error = nullptr);

    bool begin();
    bool commit();
    bool setInfo(const std::string& key, const std::string& value);
    bool getInfo(const std::string& key, std::string& value) const;

    bool insertSpeaker(const SpeakerRow& r);
    bool insertRecording(const RecordingRow& r);
    bool insertRegion(const SpeechRegionRow& r);
    bool insertSegment(const SegmentDbRow& r);

    std::vector<SpeakerRow> speakers() const;
    std::vector<RecordingRow> recordings() const;            // all recordings incl. rejected
    std::vector<SpeechRegionRow> regions() const;            // ordered by recording, start
    std::vector<SegmentDbRow> segments() const;              // ordered by recording, anchor

    // Simple counts for `bfcorpus info`.
    std::int64_t countRows(const std::string& table) const;

    const std::string& lastError() const noexcept { return err_; }

private:
    CorpusDb() = default;
    bool exec(const char* sql);
    sqlite3_stmt* prepare(const char* sql);
    sqlite3* db_ = nullptr;
    mutable std::string err_;
    sqlite3_stmt *insSpeaker_ = nullptr, *insRecording_ = nullptr, *insRegion_ = nullptr, *insSegment_ = nullptr;
};

}  // namespace bf
