#include "core/corpus/ingest/Decode.h"

#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "core/io/WavWriter.h"

namespace bf::ingest {

namespace {

std::string lowerExt(const std::string& path) {
    std::string e = std::filesystem::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

bool readAll(const std::string& path, std::vector<unsigned char>& d) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    d.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::uint32_t be32(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
std::uint16_t be16(const unsigned char* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }

double ieeeExtended(const unsigned char* p) {
    const int expo = ((p[0] & 0x7F) << 8) | p[1];
    std::uint64_t mant = 0;
    for (int i = 0; i < 8; ++i) mant = (mant << 8) | p[2 + i];
    if (expo == 0 && mant == 0) return 0.0;
    const double v = std::ldexp(static_cast<double>(mant), expo - 16383 - 63);
    return (p[0] & 0x80) ? -v : v;
}

bool decodeAiff(const std::string& path, DecodedAudio& out, std::string& reason) {
    std::vector<unsigned char> d;
    if (!readAll(path, d)) { reason = "decode.open"; return false; }
    if (d.size() < 12 || std::memcmp(d.data(), "FORM", 4) != 0) { reason = "decode.header"; return false; }
    const bool aifc = std::memcmp(d.data() + 8, "AIFC", 4) == 0;
    if (!aifc && std::memcmp(d.data() + 8, "AIFF", 4) != 0) { reason = "decode.header"; return false; }
    unsigned channels = 0, bits = 0;
    std::uint64_t frames = 0;
    double rate = 0;
    bool littleEndian = false, isFloat = false;
    const unsigned char* ssnd = nullptr;
    std::size_t ssndLen = 0;
    std::size_t pos = 12;
    while (pos + 8 <= d.size()) {
        const std::uint32_t len = be32(&d[pos + 4]);
        const unsigned char* body = &d[pos + 8];
        const std::size_t avail = std::min<std::size_t>(len, d.size() - pos - 8);
        if (std::memcmp(&d[pos], "COMM", 4) == 0 && avail >= 18) {
            channels = be16(body);
            frames = be32(body + 2);
            bits = be16(body + 6);
            rate = ieeeExtended(body + 8);
            if (aifc && avail >= 22) {
                if (std::memcmp(body + 18, "sowt", 4) == 0) littleEndian = true;
                else if (std::memcmp(body + 18, "fl32", 4) == 0 || std::memcmp(body + 18, "FL32", 4) == 0) isFloat = true;
                else if (std::memcmp(body + 18, "NONE", 4) != 0 && std::memcmp(body + 18, "twos", 4) != 0) {
                    reason = "decode.unsupported";
                    return false;
                }
            }
        } else if (std::memcmp(&d[pos], "SSND", 4) == 0 && avail >= 8) {
            const std::uint32_t off = be32(body);
            if (8 + static_cast<std::size_t>(off) <= avail) {
                ssnd = body + 8 + off;
                ssndLen = avail - 8 - off;
            }
        }
        pos += 8 + static_cast<std::size_t>(len) + (len & 1u);
    }
    if (channels == 0 || !ssnd || rate <= 0 || bits == 0) { reason = "decode.header"; return false; }
    const std::size_t bps = (bits + 7) / 8;
    if (bps > 4 || (isFloat && bps != 4)) { reason = "decode.unsupported"; return false; }
    frames = std::min<std::uint64_t>(frames, ssndLen / (bps * channels));
    out.sampleRate = static_cast<std::uint32_t>(std::lround(rate));
    out.channels = static_cast<std::uint16_t>(channels);
    out.bitDepth = static_cast<int>(bits);
    out.format = "aiff";
    out.data.resize(static_cast<std::size_t>(frames * channels));
    const double scale = 1.0 / std::ldexp(1.0, static_cast<int>(bps * 8) - 1);
    for (std::size_t i = 0; i < out.data.size(); ++i) {
        const unsigned char* p = ssnd + i * bps;
        std::uint32_t u = 0;
        if (littleEndian) for (std::size_t k = bps; k-- > 0;) u = (u << 8) | p[k];
        else for (std::size_t k = 0; k < bps; ++k) u = (u << 8) | p[k];
        if (isFloat) {
            float f;
            std::memcpy(&f, &u, 4);
            out.data[i] = f;
        } else {
            std::int32_t s = static_cast<std::int32_t>(u << (32 - 8 * bps)) >> (32 - 8 * bps);
            if (bps == 1) s = static_cast<std::int8_t>(p[0]);
            out.data[i] = static_cast<float>(s * scale);
        }
    }
    return true;
}

struct FlacCtx {
    DecodedAudio* out;
    bool error = false;
    unsigned bits = 0;
};

FLAC__StreamDecoderWriteStatus flacWrite(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                                         const FLAC__int32* const buffer[], void* client) {
    auto* c = static_cast<FlacCtx*>(client);
    const unsigned ch = frame->header.channels;
    const unsigned n = frame->header.blocksize;
    if (c->out->channels == 0) {
        c->out->channels = static_cast<std::uint16_t>(ch);
        c->out->sampleRate = frame->header.sample_rate;
        c->out->bitDepth = static_cast<int>(frame->header.bits_per_sample);
    }
    const double scale = 1.0 / std::ldexp(1.0, static_cast<int>(frame->header.bits_per_sample) - 1);
    const std::size_t base = c->out->data.size();
    c->out->data.resize(base + static_cast<std::size_t>(n) * ch);
    for (unsigned i = 0; i < n; ++i)
        for (unsigned k = 0; k < ch; ++k)
            c->out->data[base + static_cast<std::size_t>(i) * ch + k] = static_cast<float>(buffer[k][i] * scale);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
void flacError(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* client) {
    static_cast<FlacCtx*>(client)->error = true;
}

bool decodeFlac(const std::string& path, DecodedAudio& out, std::string& reason) {
    FLAC__StreamDecoder* dec = FLAC__stream_decoder_new();
    if (!dec) { reason = "decode.alloc"; return false; }
    FlacCtx ctx{&out};
    out.format = "flac";
    const auto st = FLAC__stream_decoder_init_file(dec, path.c_str(), flacWrite, nullptr, flacError, &ctx);
    bool ok = st == FLAC__STREAM_DECODER_INIT_STATUS_OK;
    if (!ok) reason = st == FLAC__STREAM_DECODER_INIT_STATUS_ERROR_OPENING_FILE ? "decode.open" : "decode.flac";
    if (ok) {
        ok = FLAC__stream_decoder_process_until_end_of_stream(dec) != 0 && !ctx.error;
        if (!ok) reason = "decode.flac";
    }
    FLAC__stream_decoder_finish(dec);
    FLAC__stream_decoder_delete(dec);
    if (ok && out.channels == 0) { reason = "decode.empty"; return false; }
    return ok;
}

}  // namespace

bool decodeFile(const std::string& path, DecodedAudio& out, std::string& reason) {
    out = DecodedAudio{};
    reason.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) { reason = "decode.open"; return false; }
    const std::string ext = lowerExt(path);
    bool ok = false;
    if (ext == ".wav" || ext == ".bwf" || ext == ".wave") {
        WavData w;
        std::string err;
        if (!readWav(path, w, &err)) { reason = "decode.wav"; return false; }
        out.sampleRate = w.sampleRate;
        out.channels = w.channels;
        out.bitDepth = w.isFloat ? 32 : w.bitsPerSample;
        out.format = "wav";
        out.data = std::move(w.samples);
        ok = true;
    } else if (ext == ".aif" || ext == ".aiff" || ext == ".aifc") {
        ok = decodeAiff(path, out, reason);
    } else if (ext == ".flac") {
        ok = decodeFlac(path, out, reason);
    } else {
        reason = "decode.unsupported";
        return false;
    }
    if (!ok) {
        if (reason.empty()) reason = "decode.error";
        return false;
    }
    if (out.frames() == 0) { reason = "decode.empty"; return false; }
    return true;
}

}  // namespace bf::ingest
