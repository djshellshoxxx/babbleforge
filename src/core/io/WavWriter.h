#pragma once
// Minimal RIFF/WAVE float32 writer (WAVE_FORMAT_EXTENSIBLE when channels > 2) and a
// simple reader (float32 / PCM16 / PCM24 / PCM32). Explicit little-endian.
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace bf {

class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    bool open(const std::string& path, std::uint32_t sampleRate, std::uint16_t channels);
    // Interleaved: frames * channels floats.
    bool writeInterleaved(const float* data, std::size_t frames);
    // Planar: `channels` pointers, each with `frames` floats.
    bool writePlanar(const float* const* channels, std::size_t frames);
    // Finalises the header. Idempotent. Returns false if any error occurred.
    bool close();

    bool isOpen() const noexcept { return open_; }
    std::uint64_t framesWritten() const noexcept { return frames_; }
    // True once the data chunk has passed 2 GiB (some readers treat sizes as signed).
    bool exceeds2GiB() const noexcept { return dataBytes_ > 0x7FFFFFFFULL; }
    const std::string& error() const noexcept { return error_; }

    static constexpr std::uint64_t kMaxDataBytes = 0xFFFFFFFFULL - 100;

private:
    bool writeHeader();
    bool fail(const std::string& msg);
    std::ofstream f_;
    bool open_ = false;
    bool failed_ = false;
    std::uint32_t rate_ = 0;
    std::uint16_t ch_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t dataBytes_ = 0;
    std::string error_;
    std::vector<unsigned char> buf_;
};

struct WavData {
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bitsPerSample = 0;
    bool isFloat = false;
    std::uint64_t frames = 0;
    std::vector<float> samples;  // interleaved, PCM scaled to [-1, 1)
};

bool readWav(const std::string& path, WavData& out, std::string* error = nullptr);

}  // namespace bf
