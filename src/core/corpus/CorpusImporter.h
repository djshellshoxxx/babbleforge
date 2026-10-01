#pragma once
// Corpus import job: scan -> analyse (thread pool) -> duplicate detection -> speaker
// aggregation + LTASS PCA -> staging DB/cache -> atomic commit (docs/CORPUS.md §3.12, §3.13, §4).
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/corpus/ingest/IngestTypes.h"

namespace bf {

struct ImportOptions {
    std::filesystem::path inputDir, corpusRoot, speakersCsv;
    std::string speakerRegex = R"(^([^_]+)_.*)";
    int threads = 0;                  // 0 = hardware threads - 2 (min 1)
    bool storeSourcePaths = true;     // recording.source_path + source_links.json
    ingest::AnalyzerConfig analyzer;
    std::function<void(std::size_t done, std::size_t total, const std::string& file)> progress;
    const std::atomic<bool>* cancel = nullptr;  // set from another thread: stops with error "cancelled" (nothing is installed)
    // Incremental mode: the new recordings are appended to the existing corpus at `baseRoot`
    // (default: corpusRoot): existing recording / speaker / segment ids and cache files are kept,
    // new files get the next ids, duplicates are detected against the existing recordings too,
    // the speaker aggregates, the LTASS PCA basis and the corpusVersion are recomputed. The merged
    // corpus is written (staged, then installed atomically) to corpusRoot. When baseRoot is
    // missing or empty this is a plain import.
    bool addToExisting = false;
    std::filesystem::path baseRoot;
};

struct ImportFileReport {
    std::string relPath, speaker;
    ingest::QualityClass cls = ingest::QualityClass::Rejected;
    std::vector<std::string> reasons;
    double durationS = 0, speechS = 0, snrDb = 0, aslDb = 0, bandwidthHz = 0, qualityScore = 0;
};

struct ImportResult {
    bool ok = false;
    std::string error;
    std::string corpusVersion;  // 16 hex chars
    std::vector<ImportFileReport> files;
    // nGood / nUsable / nRejected / totalS / usableS / usableSpeechS describe the imported files
    // only; nSpeakers / nUsableSpeakers describe the whole resulting corpus.
    std::size_t nGood = 0, nUsable = 0, nRejected = 0, nSpeakers = 0, nUsableSpeakers = 0;
    double totalS = 0, usableS = 0, usableSpeechS = 0;
    // Incremental mode.
    bool added = false;                 // an existing corpus was extended
    std::string previousVersion;        // corpusVersion before the merge
    std::size_t nNewSpeakers = 0;       // speakers that did not exist before
    std::size_t nTotalFiles = 0;        // recordings in the resulting corpus
    // Cache generation that importCorpus() renamed away when it installed over an existing corpus
    // (`<corpusRoot>/cache.old-<previous version>`), empty if none.
    std::filesystem::path retiredCacheDir;
};

ImportResult importCorpus(const ImportOptions& opt);

struct RemoveOptions {
    std::filesystem::path corpusRoot;
    std::vector<std::string> speakers;          // external speaker ids (or numeric speaker_id)
    std::vector<std::int64_t> recordings;       // recording_id
};

struct RemoveResult {
    bool ok = false;
    std::string error;
    std::string previousVersion, corpusVersion;
    std::size_t speakersRemoved = 0, recordingsRemoved = 0;
    std::size_t nSpeakers = 0, nUsableSpeakers = 0;
};

// Marks speakers / recordings as removed (speaker disabled; recordings quality class Rejected with
// reason `user.removed`, cache file reference dropped), recomputes the speaker aggregates, the PCA
// basis and the corpusVersion, and rewrites the database and manifest atomically. The cache files
// stay on disk (running engines may still read them); the next incremental import / rebuild does
// not carry them into the new cache generation.
RemoveResult removeFromCorpus(const RemoveOptions& opt);

}  // namespace bf
