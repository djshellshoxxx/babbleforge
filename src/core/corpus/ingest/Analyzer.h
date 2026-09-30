#pragma once
// Per-file analysis: Decode -> Resample -> Channel normalise -> DC removal -> VAD ->
// Segmentation -> Silence -> Levels -> LTASS -> F0 -> Rate -> Quality (docs/CORPUS.md §3).
// Duplicate detection across files and the DB/cache writes live in the Importer.
#include <string>

#include "core/corpus/ingest/IngestTypes.h"

namespace bf::ingest {

// Decodes and analyses `path`. Never throws; failures produce a Rejected result with reason
// codes. The processed 48 kHz mono audio stays in `audio48k` (for the cache writer).
RecordingAnalysis analyzeFile(const std::string& path, const AnalyzerConfig& cfg = {});

// The same pipeline from already decoded audio.
RecordingAnalysis analyzeAudio(const DecodedAudio& in, const AnalyzerConfig& cfg = {});

// Quality score and class from the metrics already filled in `a` (§1.3, §3.12); adds the
// warn.* / reject.* reason codes.
void scoreQuality(RecordingAnalysis& a);

}  // namespace bf::ingest
