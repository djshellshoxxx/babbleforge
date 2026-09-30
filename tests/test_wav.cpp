#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/io/WavWriter.h"
#include "core/random/Random.h"

using namespace bf;

TEST_CASE("WAV float32 round trip 1/2/8 channels", "[wav]") {
    for (int chi : {1, 2, 8}) {
        const auto ch = static_cast<std::uint16_t>(chi);
        const auto path = (std::filesystem::temp_directory_path() / ("bf_wav_test_" + std::to_string(ch) + ".wav")).string();
        const std::size_t frames = 10007;
        RngStream r(1, "wav", ch);
        std::vector<float> data(frames * ch);
        for (auto& v : data) v = r.uniformPM1f();
        {
            WavWriter w;
            REQUIRE(w.open(path, 48000, ch));
            const std::size_t split = 5000;
            REQUIRE(w.writeInterleaved(data.data(), split));
            std::vector<std::vector<float>> planes(ch, std::vector<float>(frames - split));
            std::vector<const float*> ptrs(ch);
            for (std::size_t c = 0; c < ch; ++c) {
                for (std::size_t i = 0; i < frames - split; ++i) planes[c][i] = data[(split + i) * ch + c];
                ptrs[c] = planes[c].data();
            }
            REQUIRE(w.writePlanar(ptrs.data(), frames - split));
            REQUIRE(w.close());
            CHECK(w.framesWritten() == frames);
        }
        WavData rd;
        std::string err;
        REQUIRE(readWav(path, rd, &err));
        CHECK(rd.channels == ch);
        CHECK(rd.sampleRate == 48000);
        CHECK(rd.isFloat);
        CHECK(rd.frames == frames);
        REQUIRE(rd.samples.size() == data.size());
        CHECK(std::memcmp(rd.samples.data(), data.data(), data.size() * 4) == 0);
        std::ifstream f(path, std::ios::binary);
        char hdr[24];
        f.read(hdr, 24);
        CHECK(std::memcmp(hdr, "RIFF", 4) == 0);
        std::uint16_t tag; std::memcpy(&tag, hdr + 20, 2);
        CHECK(tag == (ch > 2 ? 0xFFFE : 3));
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

TEST_CASE("WAV writer fails cleanly on bad open / oversize", "[wav]") {
    WavWriter w;
    CHECK_FALSE(w.open("/nonexistent_dir_bf/x.wav", 48000, 2));
    CHECK_FALSE(w.error().empty());
    CHECK_FALSE(w.open("x.wav", 0, 2));
    const auto path = (std::filesystem::temp_directory_path() / "bf_wav_big.wav").string();
    REQUIRE(w.open(path, 48000, 1));
    float one = 0.f;
    // 2^31 frames * 4 bytes = 8 GiB: rejected before any data is touched.
    CHECK_FALSE(w.writeInterleaved(&one, std::size_t(1) << 31));
    CHECK_FALSE(w.error().empty());
    w.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
