#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

#include "core/dsp/PartitionedConvolver.h"
#include "core/random/Random.h"

using namespace bf;

namespace {
std::vector<float> runConvolver(const std::vector<float>& h, const std::vector<float>& x, std::size_t block) {
    PartitionedConvolver conv;
    conv.prepare(48000.0, 4096);
    REQUIRE(conv.postKernel(PartitionedKernel::create(h.data(), h.size())));
    std::vector<float> y(x.size());
    for (std::size_t off = 0; off < x.size(); off += block) {
        const std::size_t n = std::min(block, x.size() - off);
        conv.process(x.data() + off, y.data() + off, n);
    }
    return y;
}
}  // namespace

TEST_CASE("Partitioned convolver equals direct convolution for any block size", "[convolver]") {
    RngStream rng(7);
    std::vector<float> h(1500), x(24000);
    for (std::size_t i = 0; i < h.size(); ++i)
        h[i] = static_cast<float>((rng.uniform01() - 0.5) * std::exp(-static_cast<double>(i) / 400.0));
    for (auto& v : x) v = static_cast<float>(rng.uniform01() * 2.0 - 1.0);
    // Direct convolution in double, delayed by the fixed latency.
    const std::size_t lat = PartitionedKernel::kPartition;  // latency at 48 kHz
    std::vector<double> ref(x.size(), 0.0);
    for (std::size_t t = lat; t < x.size(); ++t) {
        const std::size_t tt = t - lat;
        double s = 0.0;
        for (std::size_t k = 0; k < h.size() && k <= tt; ++k) s += static_cast<double>(h[k]) * x[tt - k];
        ref[t] = s;
    }
    std::vector<float> first;
    for (std::size_t block : {1u, 17u, 64u, 256u, 480u, 1024u}) {
        const auto y = runConvolver(h, x, block);
        double num = 0.0, den = 0.0;
        for (std::size_t t = 0; t < x.size(); ++t) {
            num += (y[t] - ref[t]) * (y[t] - ref[t]);
            den += ref[t] * ref[t];
        }
        const double rel = std::sqrt(num / den);
        INFO("block " << block << " rel err " << rel);
        CHECK(rel < 1e-5);
        if (first.empty()) first = y;
        else CHECK(std::memcmp(first.data(), y.data(), y.size() * sizeof(float)) == 0);
    }
}

TEST_CASE("Kernel swap crossfades without discontinuity and retires the old kernel", "[convolver]") {
    const double fs = 48000.0;
    PartitionedConvolver conv;
    conv.prepare(fs, 1024);
    CHECK(conv.crossfadeLength() == 4800);
    std::vector<float> hA(300, 0.0f), hB(300, 0.0f);
    hA[0] = 1.0f;
    hB[0] = -1.0f;  // polarity inversion: an instant switch would jump by 2x amplitude
    REQUIRE(conv.postKernel(PartitionedKernel::create(hA.data(), hA.size())));
    CHECK_FALSE(conv.postKernel(PartitionedKernel::create(std::vector<float>(2000, 0.f).data(), 2000)));

    const std::size_t total = 48000;
    std::vector<float> x(total), y(total);
    for (std::size_t i = 0; i < total; ++i) x[i] = static_cast<float>(std::sin(2.0 * 3.141592653589793 * 440.0 * static_cast<double>(i) / fs));
    const std::size_t swapAt = 12000;
    std::size_t off = 0;
    const std::size_t block = 97;
    bool posted = false;
    while (off < total) {
        const std::size_t n = std::min(block, total - off);
        if (!posted && off >= swapAt) {
            auto k = PartitionedKernel::create(hB.data(), hB.size());
            k->effectiveSample = static_cast<std::int64_t>(swapAt + 1000);
            // A superseded pending kernel is replaced (and deleted on this thread).
            REQUIRE(conv.postKernel(PartitionedKernel::create(hA.data(), hA.size())));
            REQUIRE(conv.postKernel(std::move(k)));
            posted = true;
        }
        conv.process(x.data() + off, y.data() + off, n);
        off += n;
    }
    CHECK_FALSE(conv.isCrossfading());
    CHECK(conv.collectGarbage() == 1);

    auto maxJump = [&](std::size_t a, std::size_t b) {
        double m = 0.0;
        for (std::size_t i = a + 1; i < b; ++i) m = std::max(m, std::fabs(static_cast<double>(y[i] - y[i - 1])));
        return m;
    };
    const double steadyA = maxJump(2000, swapAt);
    const double steadyB = maxJump(swapAt + 1000 + 256 + 4800 + 1024, total);
    const double during = maxJump(swapAt, swapAt + 1000 + 256 + 4800 + 1024);
    INFO("steady " << steadyA << " / " << steadyB << " during " << during);
    CHECK(during < 1.5 * std::max(steadyA, steadyB));
    // Before the swap the output is +x, after it -x (delayed by the latency).
    const std::size_t lat = PartitionedKernel::kPartition;  // latency at 48 kHz
    CHECK(std::fabs(y[swapAt] - x[swapAt - lat]) < 1e-5f);
    CHECK(std::fabs(y[total - 1] + x[total - 1 - lat]) < 1e-5f);
    // The crossfade starts at the first 256-grid boundary at/after the effective sample.
    const std::size_t start = ((swapAt + 1000 + 255) / 256) * 256 + lat;
    CHECK(std::fabs(y[start - 1] - x[start - 1 - lat]) < 1e-5f);
    CHECK(std::fabs(y[start + 4800] + x[start + 4800 - lat]) < 1e-5f);
}

TEST_CASE("Convolver at 96 kHz uses 512-sample partitions and equals direct convolution", "[convolver]") {
    PartitionedConvolver conv;
    conv.prepare(96000.0, 4096);
    REQUIRE(conv.partitionSize() == 512);
    REQUIRE(conv.latency() == 512);
    CHECK(PartitionedKernel::partitionForRate(44100.0) == 256);
    CHECK(PartitionedKernel::partitionForRate(88200.0) == 512);
    RngStream rng(11);
    std::vector<float> h(3000), x(30000);
    for (std::size_t i = 0; i < h.size(); ++i)
        h[i] = static_cast<float>((rng.uniform01() - 0.5) * std::exp(-static_cast<double>(i) / 800.0));
    for (auto& v : x) v = static_cast<float>(rng.uniform01() * 2.0 - 1.0);
    // A 256-partition kernel does not fit a 512-partition convolver.
    CHECK_FALSE(conv.postKernel(PartitionedKernel::create(h.data(), h.size(), 256)));
    // Shared spectra: the clone is what the convolver runs.
    auto proto = PartitionedKernel::create(h.data(), h.size(), conv.partitionSize());
    auto k = proto->clone();
    CHECK(k->partition(0) == proto->partition(0));
    CHECK(k->numPartitions() == 6);
    proto.reset();  // the clone keeps the spectra alive
    REQUIRE(conv.postKernel(std::move(k)));
    std::vector<float> y(x.size());
    for (std::size_t off = 0; off < x.size(); off += 333) {
        const std::size_t n = std::min<std::size_t>(333, x.size() - off);
        conv.process(x.data() + off, y.data() + off, n);
    }
    double num = 0.0, den = 0.0;
    for (std::size_t t = 512; t < x.size(); ++t) {
        const std::size_t tt = t - 512;
        double s = 0.0;
        for (std::size_t j = 0; j < h.size() && j <= tt; ++j) s += static_cast<double>(h[j]) * x[tt - j];
        num += (y[t] - s) * (y[t] - s);
        den += s * s;
    }
    CHECK(std::sqrt(num / den) < 1e-5);
}
