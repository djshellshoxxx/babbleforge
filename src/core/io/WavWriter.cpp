#include "core/io/WavWriter.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace bf {
namespace {

void put16(std::vector<unsigned char>& b, std::uint32_t v) {
    b.push_back(static_cast<unsigned char>(v & 0xFF));
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
}
void put32(std::vector<unsigned char>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
}
void putTag(std::vector<unsigned char>& b, const char* t) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<unsigned char>(t[i]));
}
std::uint32_t channelMask(unsigned ch) {
    switch (ch) {
        case 3: return 0x7;
        case 4: return 0x33;
        case 5: return 0x37;
        case 6: return 0x3F;
        case 7: return 0x13F;
        case 8: return 0x63F;
        default: return 0;
    }
}
inline std::uint32_t rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }
inline std::uint32_t rd32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

WavWriter::~WavWriter() { close(); }

bool WavWriter::fail(const std::string& msg) {
    failed_ = true;
    if (error_.empty()) error_ = msg;
    return false;
}

bool WavWriter::writeHeader() {
    std::vector<unsigned char> h;
    const bool ext = ch_ > 2;
    const std::uint32_t fmtSize = ext ? 40 : 16;
    const std::uint32_t data = static_cast<std::uint32_t>(dataBytes_);
    // RIFF size = 4 (WAVE) + (8 + fmt) + (8 + 4 fact) + (8 + data) + pad
    const std::uint64_t riff = 4 + 8 + fmtSize + 12 + 8 + dataBytes_ + (dataBytes_ & 1);
    putTag(h, "RIFF");
    put32(h, static_cast<std::uint32_t>(riff));
    putTag(h, "WAVE");
    putTag(h, "fmt ");
    put32(h, fmtSize);
    put16(h, ext ? 0xFFFE : 3);
    put16(h, ch_);
    put32(h, rate_);
    put32(h, rate_ * ch_ * 4u);
    put16(h, ch_ * 4u);
    put16(h, 32);
    if (ext) {
        put16(h, 22);
        put16(h, 32);
        put32(h, channelMask(ch_));
        put32(h, 0x00000003); put16(h, 0x0000); put16(h, 0x0010);  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
        const unsigned char tail[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        h.insert(h.end(), tail, tail + 8);
    }
    putTag(h, "fact");
    put32(h, 4);
    put32(h, static_cast<std::uint32_t>(frames_));
    putTag(h, "data");
    put32(h, data);
    f_.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
    return static_cast<bool>(f_);
}

bool WavWriter::open(const std::string& path, std::uint32_t sampleRate, std::uint16_t channels) {
    if (open_) close();
    failed_ = false;
    error_.clear();
    frames_ = dataBytes_ = 0;
    if (channels == 0 || sampleRate == 0) return fail("invalid channel count or sample rate");
    if (static_cast<std::uint64_t>(sampleRate) * channels * 4 > 0xFFFFFFFFULL)
        return fail("byte rate overflow");
    rate_ = sampleRate;
    ch_ = channels;
    f_.open(path, std::ios::binary | std::ios::trunc | std::ios::out);
    if (!f_) return fail("cannot open " + path);
    open_ = true;
    if (!writeHeader()) { open_ = false; f_.close(); return fail("header write failed"); }
    return true;
}

bool WavWriter::writeInterleaved(const float* data, std::size_t frames) {
    if (!open_ || failed_) return false;
    const std::uint64_t bytes = static_cast<std::uint64_t>(frames) * ch_ * 4;
    if (dataBytes_ + bytes > kMaxDataBytes) return fail("WAV data would exceed 4 GiB limit");
    const std::size_t total = frames * ch_;
    constexpr std::size_t kChunk = 1 << 14;
    for (std::size_t off = 0; off < total; off += kChunk) {
        const std::size_t n = std::min(kChunk, total - off);
        buf_.resize(n * 4);
        for (std::size_t i = 0; i < n; ++i) {
            std::uint32_t u;
            std::memcpy(&u, &data[off + i], 4);
            buf_[i * 4 + 0] = static_cast<unsigned char>(u & 0xFF);
            buf_[i * 4 + 1] = static_cast<unsigned char>((u >> 8) & 0xFF);
            buf_[i * 4 + 2] = static_cast<unsigned char>((u >> 16) & 0xFF);
            buf_[i * 4 + 3] = static_cast<unsigned char>((u >> 24) & 0xFF);
        }
        f_.write(reinterpret_cast<const char*>(buf_.data()), static_cast<std::streamsize>(n * 4));
    }
    if (!f_) return fail("write failed");
    dataBytes_ += bytes;
    frames_ += frames;
    return true;
}

bool WavWriter::writePlanar(const float* const* channels, std::size_t frames) {
    if (!open_ || failed_) return false;
    std::vector<float> tmp;
    constexpr std::size_t kFrames = 4096;
    for (std::size_t off = 0; off < frames; off += kFrames) {
        const std::size_t n = std::min(kFrames, frames - off);
        tmp.resize(n * ch_);
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t c = 0; c < ch_; ++c) tmp[i * ch_ + c] = channels[c][off + i];
        if (!writeInterleaved(tmp.data(), n)) return false;
    }
    return true;
}

bool WavWriter::close() {
    if (!open_) return !failed_;
    open_ = false;
    bool ok = !failed_;
    if (dataBytes_ & 1) { const char z = 0; f_.write(&z, 1); }  // never for float32, defensive
    f_.seekp(0);
    ok = writeHeader() && ok;
    f_.flush();
    ok = static_cast<bool>(f_) && ok;
    f_.close();
    if (!ok) fail("finalise failed");
    return ok;
}

bool readWav(const std::string& path, WavData& out, std::string* error) {
    auto err = [&](const char* m) { if (error) *error = m; return false; };
    std::ifstream f(path, std::ios::binary);
    if (!f) return err("cannot open");
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4))
        return err("not RIFF/WAVE");
    std::size_t pos = 12;
    bool haveFmt = false;
    std::uint32_t fmtTag = 0;
    out = WavData{};
    while (pos + 8 <= d.size()) {
        const unsigned char* c = d.data() + pos;
        const std::uint64_t sz = rd32(c + 4);
        const std::size_t body = pos + 8;
        if (!std::memcmp(c, "fmt ", 4)) {
            if (sz < 16 || body + sz > d.size()) return err("bad fmt chunk");
            fmtTag = rd16(d.data() + body);
            out.channels = static_cast<std::uint16_t>(rd16(d.data() + body + 2));
            out.sampleRate = rd32(d.data() + body + 4);
            out.bitsPerSample = static_cast<std::uint16_t>(rd16(d.data() + body + 14));
            if (fmtTag == 0xFFFE) {
                if (sz < 40) return err("bad extensible fmt");
                fmtTag = rd16(d.data() + body + 24);
            }
            haveFmt = true;
        } else if (!std::memcmp(c, "data", 4)) {
            if (!haveFmt) return err("data before fmt");
            std::uint64_t n = sz;
            if (body + n > d.size()) n = d.size() - body;
            const unsigned bytes = out.bitsPerSample / 8u;
            if (out.channels == 0 || bytes == 0) return err("bad format");
            out.isFloat = (fmtTag == 3);
            if (!((fmtTag == 3 && bytes == 4) ||
                  (fmtTag == 1 && (bytes == 2 || bytes == 3 || bytes == 4))))
                return err("unsupported format");
            const std::uint64_t count = n / bytes;
            out.frames = count / out.channels;
            out.samples.resize(out.frames * out.channels);
            const unsigned char* p = d.data() + body;
            for (std::size_t i = 0; i < out.samples.size(); ++i, p += bytes) {
                if (fmtTag == 3) {
                    const std::uint32_t u = rd32(p);
                    std::memcpy(&out.samples[i], &u, 4);
                } else if (bytes == 2) {
                    out.samples[i] = static_cast<float>(static_cast<std::int16_t>(rd16(p))) / 32768.0f;
                } else if (bytes == 3) {
                    std::int32_t v = static_cast<std::int32_t>(
                        (static_cast<std::uint32_t>(p[0]) << 8) | (static_cast<std::uint32_t>(p[1]) << 16) |
                        (static_cast<std::uint32_t>(p[2]) << 24));
                    out.samples[i] = static_cast<float>(v >> 8) / 8388608.0f;
                } else {
                    out.samples[i] = static_cast<float>(static_cast<std::int32_t>(rd32(p)) / 2147483648.0);
                }
            }
            return true;
        }
        pos = body + sz + (sz & 1);
    }
    return err("no data chunk");
}

}  // namespace bf
