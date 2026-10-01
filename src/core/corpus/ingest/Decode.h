#pragma once
// Decode stage (docs/CORPUS.md §3.1): WAV/BWF, AIFF/AIFC and FLAC. Other containers return
// the reason code "decode.unsupported" (the JUCE-based decoders plug in here later).
#include <string>

#include "core/corpus/ingest/IngestTypes.h"

namespace bf::ingest {

// Returns true on success. On failure `reason` holds "decode.<error>".
bool decodeFile(const std::string& path, DecodedAudio& out, std::string& reason);

}  // namespace bf::ingest
