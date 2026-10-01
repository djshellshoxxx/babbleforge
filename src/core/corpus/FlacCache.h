#pragma once
// FLAC cache (docs/CORPUS.md §2): 48 kHz mono 24-bit, compression level 5, seek points every
// 48 000 samples. Writer plus a seekable IAudioSource over cache files (LRU of open decoders).
#include <cstdint>
#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/corpus/CorpusSnapshot.h"

namespace bf {

// Quantises float [-1, 1] to 24-bit (round to nearest, clamped) and writes the cache file
// (parent directories are created). Returns false and sets `error` on failure.
bool writeFlacCache(const std::filesystem::path& path, const float* x, std::size_t n, std::string* error = nullptr);

class FlacCacheAudioSource final : public IAudioSource {
public:
    static constexpr std::size_t kMaxOpenDecoders = 64;

    // paths[rec] is the cache file of RecordingId `rec` (empty = not available).
    explicit FlacCacheAudioSource(std::vector<std::filesystem::path> paths);
    ~FlacCacheAudioSource() override;
    FlacCacheAudioSource(const FlacCacheAudioSource&) = delete;
    FlacCacheAudioSource& operator=(const FlacCacheAudioSource&) = delete;

    bool read(RecordingId rec, std::uint64_t startSample48k, float* dst, std::size_t n) override;

    std::size_t openDecoders() const;
    std::uint64_t seeks() const noexcept { return seeks_; }

    // Cache generation lease (hot reload). The source reads from `cacheDir` (the generation
    // directory its paths live in); while it is alive the directory is "pinned" and
    // CorpusDb::purgeOldCaches() never removes it. After an import renamed `<root>/cache` to
    // `<root>/cache.old-<version>`, relocate() redirects future opens to the renamed
    // directory (open decoders keep their handle) and moves the pin with it.
    void setCacheDir(const std::filesystem::path& dir);
    void relocate(const std::filesystem::path& to);
    std::filesystem::path cacheDir() const;
    static bool isPinned(const std::filesystem::path& dir);
    // Renames a cache generation directory (`from` -> `to`, std::filesystem::rename semantics) while
    // live sources read from it: their open decoders are closed first (so the rename also works where
    // open files block it), and afterwards they are redirected to `to` (no read can fail in between).
    // This is how an import retires `<root>/cache` to `<root>/cache.old-<version>`.
    static void retireCacheDir(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec);

private:
    struct Dec;
    Dec* acquire(RecordingId rec);

    std::vector<std::filesystem::path> paths_;
    std::filesystem::path dir_;
    mutable std::mutex mu_;
    std::list<std::unique_ptr<Dec>> lru_;  // front = most recently used
    std::unordered_map<RecordingId, std::list<std::unique_ptr<Dec>>::iterator> open_;
    std::uint64_t seeks_ = 0;
};

}  // namespace bf
