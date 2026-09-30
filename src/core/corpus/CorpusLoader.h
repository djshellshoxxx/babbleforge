#pragma once
// Loads an on-disk corpus (CorpusDb + FLAC cache) into the runtime CorpusSnapshot plus an
// IAudioSource over the cache files (docs/CORPUS.md §6).
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/corpus/CorpusSnapshot.h"
#include "core/corpus/FlacCache.h"

namespace bf {

struct LoadedCorpus {
    std::shared_ptr<const CorpusSnapshot> snapshot;
    std::shared_ptr<FlacCacheAudioSource> audio;
    std::vector<std::int64_t> speakerDbId;    // SpeakerId   -> speaker.speaker_id
    std::vector<std::int64_t> recordingDbId;  // RecordingId -> recording.recording_id
    std::vector<std::string> languages;       // language code i (1-based) = languages[i-1]; 0 = unknown
};

// Only usable (Good/Usable) recordings of enabled speakers with a cache file are loaded.
bool loadCorpus(const std::filesystem::path& corpusRoot, LoadedCorpus& out, std::string* error = nullptr);

}  // namespace bf
