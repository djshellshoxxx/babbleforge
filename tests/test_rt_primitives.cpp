// Real-time host primitives (REALTIME_ARCHITECTURE.md §5): SPSC FIFO, RCU slot, seqlock, tap
// ring, FTZ/DAZ, RT marker. Stress tests run a producer and a consumer thread concurrently.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "core/rt/Denormals.h"
#include "core/rt/RcuSlot.h"
#include "core/rt/RtCheck.h"
#include "core/rt/SeqLock.h"
#include "core/rt/SpscFifo.h"
#include "core/rt/TapRing.h"

using namespace bf::rt;

namespace {
struct Msg {
    std::uint64_t seq;
    std::uint64_t check;
};

struct Payload {
    static std::atomic<int> alive;
    explicit Payload(std::uint64_t v) : value(v), inv(~v) { alive.fetch_add(1); }
    ~Payload() { alive.fetch_sub(1); }
    std::uint64_t value, inv;
};
std::atomic<int> Payload::alive{0};

struct Snap {
    std::uint64_t a = 0, b = 0, c = 0;
    double d = 0.0;
    std::uint32_t e = 0;
};
}  // namespace

TEST_CASE("SpscFifo: two-thread stress keeps order and content", "[rt][spsc]") {
    auto fifo = std::make_unique<SpscFifo<Msg, 1024>>();
    constexpr std::uint64_t kN = 2'000'000;
    std::atomic<bool> bad{false};
    std::thread consumer([&] {
        std::uint64_t expect = 0;
        Msg m{};
        while (expect < kN) {
            if (!fifo->pop(m)) continue;
            if (m.seq != expect || m.check != m.seq * 2654435761ull) bad.store(true);
            ++expect;
        }
    });
    std::uint64_t fullHits = 0;
    for (std::uint64_t i = 0; i < kN; ++i) {
        const Msg m{i, i * 2654435761ull};
        while (!fifo->push(m)) ++fullHits;
    }
    consumer.join();
    CHECK_FALSE(bad.load());
    CHECK(fifo->empty());
    INFO("producer saw a full FIFO " << fullHits << " times");
    Msg m{};
    CHECK_FALSE(fifo->pop(m));
}

TEST_CASE("SpscFifo: capacity is exact and push fails when full", "[rt][spsc]") {
    SpscFifo<int, 8> f;
    for (int i = 0; i < 8; ++i) REQUIRE(f.push(i));
    CHECK_FALSE(f.push(99));
    int v = -1;
    REQUIRE(f.peek(v));
    CHECK(v == 0);
    for (int i = 0; i < 8; ++i) {
        REQUIRE(f.pop(v));
        CHECK(v == i);
    }
    CHECK_FALSE(f.pop(v));
}

TEST_CASE("RcuSlot: control publishes while RT acquires; retire queue and GC reclaim everything", "[rt][rcu]") {
    Payload::alive.store(0);
    {
        RcuSlot<Payload> slot;
        std::atomic<bool> stop{false}, bad{false};
        std::atomic<std::uint64_t> seen{0}, observed{0};
        std::thread rt([&] {
            ScopedRealtimeThread mark;
            std::uint64_t last = 0;
            while (!stop.load(std::memory_order_acquire)) {
                if (Payload* p = slot.acquire()) {
                    if (p->inv != ~p->value || p->value < last) bad.store(true);
                    if (p->value != last) seen.fetch_add(1, std::memory_order_relaxed);
                    last = p->value;
                    observed.store(last, std::memory_order_release);
                }
            }
        });
        std::size_t freed = 0;
        for (std::uint64_t i = 1; i <= 20000; ++i) {
            slot.publish(std::make_unique<Payload>(i));
            if (i % 16 == 0) freed += slot.collectGarbage();
            // Handshake for the first publications: the RT side must see each one before the next is
            // published (deterministic, independent of scheduling); the rest race freely.
            if (i <= 64)
                while (observed.load(std::memory_order_acquire) != i) std::this_thread::yield();
        }
        // Let RT pick up the last one.
        while (slot.hasPending()) std::this_thread::yield();
        while (observed.load(std::memory_order_acquire) != 20000) std::this_thread::yield();
        stop.store(true, std::memory_order_release);
        rt.join();
        freed += slot.collectGarbage();
        CHECK_FALSE(bad.load());
        CHECK(seen.load() >= 64);
        CHECK(freed == 19999);            // every object but the current one
        CHECK(Payload::alive.load() == 1);  // the current object, owned by the slot
    }
    CHECK(Payload::alive.load() == 0);
}

TEST_CASE("SeqLock: readers never observe a torn snapshot", "[rt][seqlock]") {
    SeqLock<Snap> sl;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        std::uint64_t i = 0;
        while (!stop.load(std::memory_order_acquire)) {
            ++i;
            sl.store(Snap{i, i * 3, i * 7, static_cast<double>(i) * 0.5, static_cast<std::uint32_t>(i & 0xFFFFu)});
        }
    });
    std::uint64_t reads = 0, torn = 0, last = 0, backwards = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < end) {
        const Snap s = sl.load();
        ++reads;
        if (s.b != s.a * 3 || s.c != s.a * 7 || s.d != static_cast<double>(s.a) * 0.5 ||
            s.e != static_cast<std::uint32_t>(s.a & 0xFFFFu))
            ++torn;
        if (s.a < last) ++backwards;
        last = s.a;
    }
    stop.store(true);
    writer.join();
    CHECK(reads > 1000);
    CHECK(torn == 0);
    CHECK(backwards == 0);
}

TEST_CASE("TapRing: stream order, start samples and drop-on-full counter", "[rt][tap]") {
    TapRing ring;
    ring.prepare(2, 1024);
    std::vector<float> a(256), b(256);
    const float* ch[2] = {a.data(), b.data()};
    std::int64_t start = 0;
    int accepted = 0;
    for (int k = 0; k < 6; ++k) {  // 6 x 256 > 1024: the last two are dropped
        for (int i = 0; i < 256; ++i) {
            a[static_cast<std::size_t>(i)] = static_cast<float>(start + i);
            b[static_cast<std::size_t>(i)] = -static_cast<float>(start + i);
        }
        accepted += ring.write(start, ch, 2, 256) ? 1 : 0;
        start += 256;
    }
    CHECK(accepted == 4);
    CHECK(ring.droppedFrames() == 512);
    CHECK(ring.droppedChunks() == 2);
    std::vector<float> x(256), y(256);
    float* dst[2] = {x.data(), y.data()};
    TapRing::Chunk c;
    std::int64_t expect = 0;
    while (ring.read(c, dst)) {
        CHECK(c.start == expect);
        CHECK(c.frames == 256u);
        CHECK(x[10] == static_cast<float>(expect + 10));
        CHECK(y[255] == -static_cast<float>(expect + 255));
        expect += 256;
    }
    CHECK(expect == 1024);

    // Concurrent producer / consumer.
    ring.prepare(1, 4096);
    std::atomic<bool> done{false};
    std::atomic<bool> bad{false};
    std::thread cons([&] {
        std::vector<float> buf(256);
        float* d[1] = {buf.data()};
        TapRing::Chunk ck;
        std::int64_t next = 0;
        while (!done.load() || ring.bufferedFrames() > 0) {
            if (!ring.read(ck, d)) continue;
            if (ck.start < next) bad.store(true);
            for (std::uint32_t i = 0; i < ck.frames; ++i)
                if (buf[i] != static_cast<float>((ck.start + i) % 100000)) bad.store(true);
            next = ck.start + ck.frames;
        }
    });
    std::vector<float> src(200);
    const float* s1[1] = {src.data()};
    for (std::int64_t t = 0; t < 2'000'000; t += 200) {
        for (int i = 0; i < 200; ++i) src[static_cast<std::size_t>(i)] = static_cast<float>((t + i) % 100000);
        ring.write(t, s1, 1, 200);
    }
    done.store(true);
    cons.join();
    CHECK_FALSE(bad.load());
}

TEST_CASE("FTZ/DAZ and the RT thread marker", "[rt][denormals]") {
    CHECK_FALSE(isRealtimeThread());
    {
        ScopedRealtimeThread mark;
        CHECK(isRealtimeThread());
    }
    CHECK_FALSE(isRealtimeThread());
    const std::uint64_t locks0 = rtLockCount();
    CheckedMutex m;
    {
        ScopedRealtimeThread mark;
        m.lock();  // counted (not aborting by default)
        m.unlock();
    }
    CHECK(rtLockCount() == locks0 + 1);
#if defined(__SSE__) || defined(_M_X64) || defined(_M_AMD64)
    std::thread t([] {
        const std::uint32_t prev = disableDenormals();
        CHECK(denormalsDisabled());
        volatile float tiny = 1e-38f;
        volatile float r = tiny * 1e-3f;  // denormal result flushed to zero
        CHECK(r == 0.0f);
        restoreFloatingPointState(prev);
    });
    t.join();
#endif
}
