#include "core/corpus/FlacCache.h"

#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace bf {

bool writeFlacCache(const std::filesystem::path& path, const float* x, std::size_t n, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
    if (!enc) return fail("flac encoder alloc");
    FLAC__stream_encoder_set_channels(enc, 1);
    FLAC__stream_encoder_set_bits_per_sample(enc, 24);
    FLAC__stream_encoder_set_sample_rate(enc, 48000);
    FLAC__stream_encoder_set_compression_level(enc, 5);
    FLAC__stream_encoder_set_total_samples_estimate(enc, n);
    FLAC__StreamMetadata* seek = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
    FLAC__StreamMetadata* meta[1] = {seek};
    bool ok = seek != nullptr;
    if (ok) {
        ok = FLAC__metadata_object_seektable_template_append_spaced_points_by_samples(seek, 48000, n) != 0;
        if (ok) ok = FLAC__metadata_object_seektable_template_sort(seek, true) != 0;
        if (ok) ok = FLAC__stream_encoder_set_metadata(enc, meta, 1) != 0;
    }
    if (ok) {
        const auto st = FLAC__stream_encoder_init_file(enc, path.string().c_str(), nullptr, nullptr);
        ok = st == FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    }
    if (ok) {
        std::vector<FLAC__int32> buf(4096);
        for (std::size_t s = 0; s < n && ok; s += buf.size()) {
            const std::size_t len = std::min(buf.size(), n - s);
            for (std::size_t i = 0; i < len; ++i) {
                const double v = std::nearbyint(static_cast<double>(x[s + i]) * 8388608.0);
                buf[i] = static_cast<FLAC__int32>(std::clamp(v, -8388608.0, 8388607.0));
            }
            ok = FLAC__stream_encoder_process_interleaved(enc, buf.data(), static_cast<unsigned>(len)) != 0;
        }
    }
    if (enc) {
        if (FLAC__stream_encoder_get_state(enc) != FLAC__STREAM_ENCODER_UNINITIALIZED) ok = FLAC__stream_encoder_finish(enc) != 0 && ok;
        FLAC__stream_encoder_delete(enc);
    }
    if (seek) FLAC__metadata_object_delete(seek);
    if (!ok) {
        std::filesystem::remove(path, ec);
        return fail("flac encode failed: " + path.string());
    }
    return true;
}

struct FlacCacheAudioSource::Dec {
    RecordingId rec = 0;
    FLAC__StreamDecoder* d = nullptr;
    std::uint64_t total = 0;
    std::uint32_t streamBlock = 4096;
    std::uint64_t blockStart = 0;
    std::vector<FLAC__int32> block;
    bool error = false;

    ~Dec() {
        if (d) {
            FLAC__stream_decoder_finish(d);
            FLAC__stream_decoder_delete(d);
        }
    }
    static FLAC__StreamDecoderWriteStatus write(const FLAC__StreamDecoder*, const FLAC__Frame* f,
                                                const FLAC__int32* const buf[], void* c) {
        auto* s = static_cast<Dec*>(c);
        s->blockStart = f->header.number_type == FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER
                            ? f->header.number.sample_number
                            : static_cast<std::uint64_t>(f->header.number.frame_number) * s->streamBlock;
        s->block.assign(buf[0], buf[0] + f->header.blocksize);
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    static void meta(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* m, void* c) {
        auto* s = static_cast<Dec*>(c);
        if (m->type == FLAC__METADATA_TYPE_STREAMINFO) {
            s->total = m->data.stream_info.total_samples;
            s->streamBlock = m->data.stream_info.min_blocksize;
        }
    }
    static void err(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* c) {
        static_cast<Dec*>(c)->error = true;
    }
};

namespace {
std::mutex& regMutex() { static std::mutex m; return m; }
std::vector<FlacCacheAudioSource*>& registry() { static std::vector<FlacCacheAudioSource*> v; return v; }
}  // namespace

FlacCacheAudioSource::FlacCacheAudioSource(std::vector<std::filesystem::path> paths) : paths_(std::move(paths)) {
    std::lock_guard<std::mutex> lk(regMutex());
    registry().push_back(this);
}

namespace {
std::mutex& pinMutex() { static std::mutex m; return m; }
std::map<std::string, int>& pinMap() { static std::map<std::string, int> m; return m; }
std::string pinKey(const std::filesystem::path& p) { return p.lexically_normal().generic_string(); }
void pinDir(const std::filesystem::path& p) {
    if (p.empty()) return;
    std::lock_guard<std::mutex> lk(pinMutex());
    ++pinMap()[pinKey(p)];
}
void unpinDir(const std::filesystem::path& p) {
    if (p.empty()) return;
    std::lock_guard<std::mutex> lk(pinMutex());
    const auto it = pinMap().find(pinKey(p));
    if (it != pinMap().end() && --it->second <= 0) pinMap().erase(it);
}
}  // namespace

FlacCacheAudioSource::~FlacCacheAudioSource() {
    {
        std::lock_guard<std::mutex> lk(regMutex());
        auto& r = registry();
        r.erase(std::remove(r.begin(), r.end(), this), r.end());
    }
    unpinDir(dir_);
}

void FlacCacheAudioSource::retireCacheDir(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
    std::lock_guard<std::mutex> rl(regMutex());
    std::vector<FlacCacheAudioSource*> hit;
    for (auto* s : registry())
        if (!s->dir_.empty() && pinKey(s->dir_) == pinKey(from)) hit.push_back(s);
    std::vector<std::unique_lock<std::mutex>> locks;
    for (auto* s : hit) {
        locks.emplace_back(s->mu_);
        s->open_.clear();
        s->lru_.clear();  // closes the decoders
    }
    std::filesystem::rename(from, to, ec);
    if (ec) return;
    for (auto* s : hit) {
        for (auto& p : s->paths_) {
            if (p.empty()) continue;
            const auto rel = p.lexically_relative(s->dir_);
            if (rel.empty() || *rel.begin() == "..") continue;
            p = to / rel;
        }
        unpinDir(s->dir_);
        s->dir_ = to;
        pinDir(s->dir_);
    }
}

bool FlacCacheAudioSource::isPinned(const std::filesystem::path& dir) {
    std::lock_guard<std::mutex> lk(pinMutex());
    return pinMap().count(pinKey(dir)) != 0;
}

void FlacCacheAudioSource::setCacheDir(const std::filesystem::path& dir) {
    std::lock_guard<std::mutex> lk(mu_);
    unpinDir(dir_);
    dir_ = dir;
    pinDir(dir_);
}

std::filesystem::path FlacCacheAudioSource::cacheDir() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dir_;
}

void FlacCacheAudioSource::relocate(const std::filesystem::path& to) {
    std::lock_guard<std::mutex> lk(mu_);
    if (dir_.empty() || to.empty()) return;
    for (auto& p : paths_) {
        if (p.empty()) continue;
        std::error_code ec;
        const auto rel = p.lexically_relative(dir_);
        if (rel.empty() || *rel.begin() == "..") continue;
        p = to / rel;
    }
    unpinDir(dir_);
    dir_ = to;
    pinDir(dir_);
}

std::size_t FlacCacheAudioSource::openDecoders() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lru_.size();
}

FlacCacheAudioSource::Dec* FlacCacheAudioSource::acquire(RecordingId rec) {
    const auto it = open_.find(rec);
    if (it != open_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);
        return lru_.front().get();
    }
    if (rec >= paths_.size() || paths_[rec].empty()) return nullptr;
    auto dec = std::make_unique<Dec>();
    dec->rec = rec;
    dec->d = FLAC__stream_decoder_new();
    if (!dec->d) return nullptr;
    const auto st = FLAC__stream_decoder_init_file(dec->d, paths_[rec].string().c_str(), &Dec::write, &Dec::meta,
                                                   &Dec::err, dec.get());
    if (st != FLAC__STREAM_DECODER_INIT_STATUS_OK) return nullptr;
    if (!FLAC__stream_decoder_process_until_end_of_metadata(dec->d) || dec->error) return nullptr;
    while (lru_.size() >= kMaxOpenDecoders) {
        open_.erase(lru_.back()->rec);
        lru_.pop_back();
    }
    lru_.push_front(std::move(dec));
    open_[rec] = lru_.begin();
    return lru_.front().get();
}

bool FlacCacheAudioSource::read(RecordingId rec, std::uint64_t start, float* dst, std::size_t n) {
    std::fill(dst, dst + n, 0.0f);
    std::lock_guard<std::mutex> lk(mu_);
    Dec* d = acquire(rec);
    if (!d) return false;
    constexpr float kScale = 1.0f / 8388608.0f;
    std::uint64_t pos = start;
    std::size_t done = 0;
    int tries = 0;
    while (done < n && pos < d->total) {
        const bool covered = !d->block.empty() && pos >= d->blockStart && pos < d->blockStart + d->block.size();
        if (!covered) {
            if (++tries > 8) return false;
            const bool next = !d->block.empty() && pos == d->blockStart + d->block.size();
            bool ok;
            if (next) {
                ok = FLAC__stream_decoder_process_single(d->d) != 0;
            } else {
                d->block.clear();
                ++seeks_;
                ok = FLAC__stream_decoder_seek_absolute(d->d, pos) != 0;
            }
            if (!ok || d->error) {
                open_.erase(rec);
                for (auto it = lru_.begin(); it != lru_.end(); ++it)
                    if ((*it)->rec == rec) { lru_.erase(it); break; }
                return false;
            }
            if (d->block.empty() || pos < d->blockStart || pos >= d->blockStart + d->block.size()) {
                if (FLAC__stream_decoder_get_state(d->d) == FLAC__STREAM_DECODER_END_OF_STREAM) break;
                continue;
            }
        }
        const std::size_t off = static_cast<std::size_t>(pos - d->blockStart);
        const std::size_t cnt = std::min(n - done, d->block.size() - off);
        for (std::size_t i = 0; i < cnt; ++i) dst[done + i] = static_cast<float>(d->block[off + i]) * kScale;
        done += cnt;
        pos += cnt;
        tries = 0;
    }
    return true;
}

}  // namespace bf
