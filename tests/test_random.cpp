#include <catch2/catch_test_macros.hpp>

#include <array>

#include "core/random/Random.h"

using namespace bf;

TEST_CASE("SplitMix64 golden vector", "[random]") {
    SplitMix64 sm(1234567);
    CHECK(sm.next() == 6457827717110365317ULL);
    CHECK(sm.next() == 3203168211198807973ULL);
    CHECK(sm.next() == 9817491932198370423ULL);
}

TEST_CASE("xoshiro256** golden vector", "[random]") {
    Xoshiro256StarStar x(1, 2, 3, 4);
    CHECK(x.next() == 11520ULL);
    CHECK(x.next() == 0ULL);
    CHECK(x.next() == 1509978240ULL);
    CHECK(x.next() == 1215971899390074240ULL);
}

TEST_CASE("FNV1a64 known values", "[random]") {
    CHECK(fnv1a64("") == 0xCBF29CE484222325ULL);
    CHECK(fnv1a64("a") == 0xAF63DC4C8601EC8CULL);
    CHECK(fnv1a64("foobar") == 0x85944171F73967E8ULL);
}

TEST_CASE("Stream derivation is deterministic and distinct", "[random]") {
    const auto a = deriveStreamSeed(42, "planner.global", 0);
    CHECK(a == deriveStreamSeed(42, "planner.global", 0));
    CHECK(a == SplitMix64(42 ^ fnv1a64("planner.global")).next());
    CHECK(a != deriveStreamSeed(42, "planner.global", 1));
    CHECK(a != deriveStreamSeed(42, "selector", 0));
    CHECK(a != deriveStreamSeed(43, "planner.global", 0));
    RngStream r1(42, "gainvar", 3), r2(42, "gainvar", 3), r3(42, "gainvar", 4);
    bool diff = false;
    for (int i = 0; i < 16; ++i) {
        const auto v = r1.nextU64();
        CHECK(v == r2.nextU64());
        diff |= v != r3.nextU64();
    }
    CHECK(diff);
}

TEST_CASE("RngStream uniform ranges", "[random]") {
    RngStream r(1, "t", 0);
    for (int i = 0; i < 100000; ++i) {
        const double d = r.uniform01();
        REQUIRE((d >= 0.0 && d < 1.0));
        const float f = r.uniformFloat01();
        REQUIRE((f >= 0.0f && f < 1.0f));
        const float p = r.uniformPM1f();
        REQUIRE((p >= -1.0f && p < 1.0f));
    }
    CHECK(r.uniformInt(0) == 0);
    CHECK(r.uniformInt(1) == 0);
}

TEST_CASE("Lemire uniformInt is unbiased (chi-square)", "[random]") {
    RngStream r(2024, "chi", 0);
    constexpr int n = 7;
    constexpr int N = 1000000;
    std::array<double, n> cnt{};
    for (int i = 0; i < N; ++i) cnt[r.uniformInt(n)] += 1.0;
    double chi = 0;
    const double e = double(N) / n;
    for (double c : cnt) chi += (c - e) * (c - e) / e;
    CHECK(chi < 30.0);  // df=6; p=1e-4 threshold is ~27
}
