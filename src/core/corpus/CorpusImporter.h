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
    std::size_t nGood = 0, nUsable = 0, nRejected = 0, nSpeakers = 0, nUsableSpeakers = 0;
    double totalS = 0, usableS = 0, usableSpeechS = 0;
};

ImportResult importCorpus(const ImportOptions& opt);

}  // namespace bf
