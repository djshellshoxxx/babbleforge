#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "core/random/Distributions.h"

using namespace bf;

namespace {
constexpr int N = 1000000;
}

TEST_CASE("exponential moments", "[distributions]") {
    RngStream r(7, "exp", 0);
    const double mean = 3.0;
    double s = 0, s2 = 0;
    for (int i = 0; i < N; ++i) { const double x = exponential(r, mean); REQUIRE(x >= 0); s += x; s2 += x * x; }
    const double m = s / N, var = s2 / N - m * m;
    CHECK(std::abs(m - mean) < 3 * mean / std::sqrt(double(N)));
    CHECK(std::abs(var - mean * mean) < 0.05 * mean * mean);
}

TEST_CASE("standard normal moments (both samplers)", "[distributions]") {
    for (int mode = 0; mode < 2; ++mode) {
        RngStream r(8, "norm", static_cast<std::uint64_t>(mode));
        NormalSampler ns;
        double s = 0, s2 = 0, s4 = 0;
        for (int i = 0; i < N; ++i) {
            const double z = mode ? ns.next(r) : standardNormal(r);
            s += z; s2 += z * z; s4 += z * z * z * z;
        }
        const double se = 1.0 / std::sqrt(double(N));
        CHECK(std::abs(s / N) < 3 * se);
        CHECK(std::abs(s2 / N - 1.0) < 3 * std::sqrt(2.0 / N));
        CHECK(std::abs(s4 / N - 3.0) < 3 * std::sqrt(96.0 / N));
    }
}

TEST_CASE("logNormal moments", "[distributions]") {
    RngStream r(9, "ln", 0);
    const double mu = 0.5, sg = 0.4;
    double s = 0;
    for (int i = 0; i < N; ++i) s += logNormal(r, mu, sg);
    const double mean = std::exp(mu + sg * sg / 2);
    const double sd = mean * std::sqrt(std::exp(sg * sg) - 1);
    CHECK(std::abs(s / N - mean) < 3 * sd / std::sqrt(double(N)));
}

TEST_CASE("truncatedNormal respects bounds", "[distributions]") {
    RngStream r(10, "tn", 0);
    double s = 0;
    for (int i = 0; i < N; ++i) {
        const double x = truncatedNormal(r, 2.0, 1.5);
        REQUIRE(std::abs(x) <= 3.0 + 1e-12);
        s += x;
    }
    CHECK(std::abs(s / N) < 3 * 2.0 / std::sqrt(double(N)));
    for (int i = 0; i < 100; ++i) REQUIRE(std::abs(truncatedNormal(r, 1.0, 0.0)) == 0.0);
}

TEST_CASE("truncatedLogNormalMedian bounds and median", "[distributions]") {
    RngStream r(11, "tln", 0);
    int below = 0;
    for (int i = 0; i < N; ++i) {
        const double x = truncatedLogNormalMedian(r, 100.0, 0.5, 20.0, 500.0);
        REQUIRE((x >= 20.0 && x <= 500.0));
        below += x < 100.0;
    }
    CHECK(std::abs(below / double(N) - 0.5) < 3 * 0.5 / std::sqrt(double(N)) + 0.002);
    for (int i = 0; i < 100; ++i) {
        const double x = truncatedLogNormalMedian(r, 1.0, 0.1, 50.0, 60.0);  // unreachable -> clamp
        REQUIRE(x == 50.0);
    }
}

TEST_CASE("weightedChoice frequencies", "[distributions]") {
    RngStream r(12, "wc", 0);
    const std::vector<double> w{1.0, 0.0, 3.0, 6.0};
    std::vector<double> cnt(w.size());
    for (int i = 0; i < N; ++i) cnt[weightedChoice(r, w)] += 1;
    CHECK(cnt[1] == 0);
    const double p[] = {0.1, 0.0, 0.3, 0.6};
    for (int i = 0; i < 4; ++i)
        CHECK(std::abs(cnt[static_cast<std::size_t>(i)] / N - p[i]) <= 3 * std::sqrt(p[i] * (1 - p[i]) / N) + 1e-12);
    CHECK(weightedChoice(r, std::vector<double>{}) == 0);
}

TEST_CASE("distributions are reproducible", "[distributions]") {
    RngStream a(5, "x", 0), b(5, "x", 0);
    for (int i = 0; i < 100; ++i) CHECK(standardNormal(a) == standardNormal(b));
}
