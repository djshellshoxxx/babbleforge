#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "core/random/Random.h"
#include "core/talker/SourcePreparer.h"
#include "core/talker/VoiceRenderer.h"

using namespace bf;

namespace {
constexpr double kHalfPi = 1.57079632679489661923;

struct Rig {
    BlockPool pool{64};
    std::vector<std::unique_ptr<BlockChain>> chains;
    std::vector<std::vector<SampleSpan>> masks;
    VoiceRenderer r;
    explicit Rig(std::size_t nCh) { r.prepare(nCh, 16, &pool); r.setCountNorm(1.0, 0, true); }

    // Event whose audio is `fill(i)` for event-relative sample i.
    template <class F>
    void add(TalkerEvent ev, F fill, std::vector<SampleSpan> mask = {}) {
        auto ch = std::make_unique<BlockChain>();
        ch->reset(static_cast<std::uint64_t>(ev.length()));
        std::vector<float> buf(static_cast<std::size_t>(ev.length()));
        for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = fill(i);
        REQUIRE(ch->append(pool, buf.data(), buf.size()));
        masks.push_back(std::move(mask));
        VoiceEvent ve;
        ve.ev = ev;
        ve.chain = ch.get();
        ve.speech = masks.back().data();
        ve.numSpeech = static_cast<std::uint32_t>(masks.back().size());
        REQUIRE(r.pushEvent(ve));
        chains.push_back(std::move(ch));
    }
    std::vector<std::vector<float>> render(std::size_t nCh, std::size_t frames, const std::vector<std::size_t>& blocks) {
        std::vector<std::vector<float>> out(nCh, std::vector<float>(frames));
        std::vector<float*> p(nCh);
        std::size_t off = 0, bi = 0;
        while (off < frames) {
            const std::size_t n = std::min(blocks[bi++ % blocks.size()], frames - off);
            for (std::size_t c = 0; c < nCh; ++c) p[c] = out[c].data() + off;
            r.process(p.data(), n);
            off += n;
        }
        return out;
    }
};

TalkerEvent mkEvent(std::uint64_t id, std::uint32_t slot, std::int64_t start, std::int64_t fi,
                    std::int64_t fos, std::int64_t end, float gain, float pan = 0.0f) {
    TalkerEvent e;
    e.eventId = id;
    e.slot = slot;
    e.startSample = start;
    e.fadeInLen = fi;
    e.fadeOutStart = fos;
    e.endSample = end;
    e.segGainLin = gain;
    e.pan = pan;
    return e;
}
}  // namespace

TEST_CASE("VoiceRenderer: sample-accurate start and equal-power fades", "[voice]") {
    Rig rig(1);
    rig.add(mkEvent(0, 0, 1000, 480, 5000, 5960, 0.5f), [](std::size_t) { return 1.0f; });
    const auto out = rig.render(1, 8000, {100});
    const auto& y = out[0];
    for (std::size_t n = 0; n < 1000; ++n) REQUIRE(y[n] == 0.0f);
    for (std::size_t i = 0; i < 480; ++i)
        REQUIRE(std::fabs(y[1000 + i] - 0.5 * std::sin(kHalfPi * static_cast<double>(i) / 480.0)) < 1e-6);
    for (std::size_t n = 1480; n < 5000; ++n) REQUIRE(std::fabs(y[n] - 0.5f) < 1e-6f);
    for (std::size_t i = 0; i < 960; ++i)
        REQUIRE(std::fabs(y[5000 + i] - 0.5 * std::cos(kHalfPi * static_cast<double>(i) / 960.0)) < 1e-6);
    for (std::size_t n = 5960; n < 8000; ++n) REQUIRE(y[n] == 0.0f);
    // Equal power: sin^2 + cos^2 = 1 at the crossfade midpoint.
    const double a = std::sin(kHalfPi * 0.5), b = std::cos(kHalfPi * 0.5);
    CHECK(std::fabs(a * a + b * b - 1.0) < 1e-12);
    BlockChain* done = nullptr;
    CHECK(rig.r.popFinished(done));
    CHECK(done == rig.chains[0].get());
    CHECK(done->finished());
}

TEST_CASE("VoiceRenderer: output is bit-identical for any block size", "[voice][determinism]") {
    auto run = [](const std::vector<std::size_t>& blocks) {
        Rig rig(2);
        RngStream rng(77);
        for (std::uint64_t k = 0; k < 10; ++k) {
            const std::int64_t start = static_cast<std::int64_t>(rng.uniformInt(90000));
            const std::int64_t len = 20000 + static_cast<std::int64_t>(rng.uniformInt(60000));
            const std::uint64_t seed = rng.nextU64();
            rig.add(mkEvent(k, static_cast<std::uint32_t>(k % 6), start, 2000 + static_cast<std::int64_t>(k) * 97,
                            start + len - 3000, start + len, 0.2f + 0.05f * static_cast<float>(k),
                            rng.uniformPM1f()),
                    [seed](std::size_t i) {
                        SplitMix64 sm(seed + i);
                        return static_cast<float>(sm.next() >> 40) * 0x1.0p-24f - 0.5f;
                    },
                    {{0, 5000}, {9000, 15000}});
        }
        rig.r.setCountNorm(0.7, 30000);
        rig.r.setTrimDb(-1.5, 70001);
        return rig.render(2, 144000, blocks);
    };
    const auto ref = run({256});
    CHECK(run({64}) == ref);
    CHECK(run({480}) == ref);
    CHECK(run({1024}) == ref);
    CHECK(run({1, 7, 333, 4096, 2}) == ref);
    double e = 0.0;
    for (float v : ref[0]) e += static_cast<double>(v) * v;
    CHECK(e > 0.0);
}

TEST_CASE("VoiceRenderer: count normalisation ramps over 2 s from its stamped boundary", "[voice]") {
    Rig rig(1);
    rig.add(mkEvent(0, 0, 0, 1, 400000, 400001, 1.0f), [](std::size_t) { return 1.0f; });
    rig.r.setCountNorm(0.5, 10000);
    const auto y = rig.render(1, 200000, {777})[0];
    CHECK(y[10239] == 1.0f);                                    // applied at grid 10240
    CHECK(std::fabs(y[10240] - (1.0 - 0.5 / 96000.0)) < 1e-6);  // first ramp sample
    CHECK(std::fabs(y[10240 + 47999] - 0.75) < 1e-6);           // halfway
    CHECK(std::fabs(y[10240 + 96000] - 0.5) < 1e-7);            // done
    CHECK(std::fabs(y[199999] - 0.5) < 1e-7);
}

TEST_CASE("VoiceRenderer: publishes k_a / k_s per sub-block", "[voice]") {
    Rig rig(1);
    rig.add(mkEvent(0, 0, 0, 10, 9000, 9100, 1.0f), [](std::size_t) { return 0.1f; }, {{0, 2000}, {5000, 6000}});
    rig.add(mkEvent(1, 1, 1000, 10, 7000, 7100, 1.0f), [](std::size_t) { return 0.1f; }, {{1000, 3000}});
    rig.render(1, 10240, {100});
    std::vector<OccupancyFrame> fr;
    OccupancyFrame f;
    while (rig.r.popOccupancy(f)) fr.push_back(f);
    REQUIRE(fr.size() == 40);
    for (const auto& x : fr) {
        const std::int64_t t = x.sample;
        const int ka = (t < 9100) + (t >= 1000 && t < 7100);
        const int ks = ((t < 2000) || (t >= 5000 && t < 6000)) + (t >= 2000 && t < 4000);
        CHECK(x.activeTalkers == ka);
        CHECK(x.speakingTalkers == ks);
    }
}

TEST_CASE("VoiceRenderer: stale-epoch events are dropped; per-slot gains", "[voice]") {
    Rig rig(2);
    TalkerEvent a = mkEvent(0, 0, 0, 10, 40000, 40010, 1.0f, -1.0f);  // hard left
    TalkerEvent b = mkEvent(1, 1, 20000, 10, 40000, 40010, 1.0f, 0.0f);
    b.epoch = 0;
    rig.add(a, [](std::size_t) { return 1.0f; });
    rig.add(b, [](std::size_t) { return 1.0f; });
    rig.r.dropStale(1, 10000);  // b starts after the freeze point and is stale
    const float g[2] = {0.0f, 1.0f};
    rig.r.setGains(0, g, 2);  // move slot 0 to the right channel
    const auto out = rig.render(2, 40000, {512});
    // Right channel rises to 1 (50 ms smoothing), left decays to 0; b never sounds.
    CHECK(std::fabs(out[1][30000] - 1.0f) < 1e-4f);
    CHECK(std::fabs(out[0][30000]) < 1e-4f);
    CHECK(rig.r.droppedEvents() == 0);
    BlockChain* done = nullptr;
    int finished = 0;
    while (rig.r.popFinished(done)) ++finished;
    CHECK(finished == 1);  // b's chain returned immediately
}

TEST_CASE("BlockChain: single-writer/single-reader semantics and block recycling", "[voice]") {
    BlockPool pool(4);
    BlockChain ch;
    ch.reset(40000);
    std::vector<float> src(40000);
    for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<float>(i);
    REQUIRE(ch.append(pool, src.data(), 20000));
    CHECK(pool.freeBlocks() == 2);
    std::vector<float> dst(1000);
    CHECK_FALSE(ch.read(pool, 19500, dst.data(), 1000));  // not yet written
    REQUIRE(ch.read(pool, 16000, dst.data(), 1000));       // crosses no block edge
    CHECK(dst[0] == 16000.0f);
    REQUIRE(ch.read(pool, 16000 - 500, dst.data(), 1000)); // crosses a block edge
    CHECK(dst[499] == 15999.0f);
    CHECK(dst[500] == 16000.0f);
    ch.setReadIndex(17000);
    ch.reclaim(pool);
    CHECK(pool.freeBlocks() == 3);
    REQUIRE(ch.append(pool, src.data() + 20000, 20000));
    CHECK(ch.written() == 40000);
    ch.releaseAll(pool);
    CHECK(pool.freeBlocks() == 4);
}
