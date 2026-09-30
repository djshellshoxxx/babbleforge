#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "core/math/DetMath.h"
#include "core/random/Random.h"

using namespace bf;

namespace {

double ulpErr(double got, long double ref) {
    const double r = static_cast<double>(ref);
    if (r == 0.0) return got == 0.0 ? 0.0 : 1e30;
    const double u = std::nextafter(std::fabs(r), std::numeric_limits<double>::infinity()) - std::fabs(r);
    return static_cast<double>(std::fabs(static_cast<long double>(got) - ref)) / u;
}

template <class F, class R>
double maxUlp(F f, R ref, double lo, double hi, int n, bool logScale = false) {
    RngStream rng(99, "detmath", 0);
    double worst = 0;
    for (int i = 0; i < n; ++i) {
        const double u = rng.uniform01();
        const double x = logScale ? std::exp(std::log(lo) + u * (std::log(hi) - std::log(lo))) : lo + u * (hi - lo);
        worst = std::max(worst, ulpErr(f(x), ref(static_cast<long double>(x))));
    }
    return worst;
}

}  // namespace

TEST_CASE("detlog accuracy", "[detmath]") {
    auto ref = [](long double x) { return std::log(x); };
    CHECK(maxUlp(detlog, ref, 1e-300, 1e300, 300000, true) <= 2.0);
    CHECK(maxUlp(detlog, ref, 0.5, 2.0, 300000) <= 2.0);
    CHECK(maxUlp(detlog, ref, 0.999, 1.001, 100000) <= 2.0);
    CHECK(detlog(1.0) == 0.0);
    CHECK(detlog(5e-324) == Catch::Approx(std::log(5e-324)).epsilon(1e-14));
    CHECK(std::isinf(detlog(0.0)));
}

TEST_CASE("detexp accuracy", "[detmath]") {
    auto ref = [](long double x) { return std::exp(x); };
    CHECK(maxUlp(detexp, ref, -700, 700, 500000) <= 2.0);
    CHECK(maxUlp(detexp, ref, -2, 2, 300000) <= 2.0);
    CHECK(detexp(0.0) == 1.0);
}

TEST_CASE("detsin/detcos accuracy", "[detmath]") {
    auto rs = [](long double x) { return std::sin(x); };
    auto rc = [](long double x) { return std::cos(x); };
    CHECK(maxUlp(detsin, rs, -1e6, 1e6, 500000) <= 2.0);
    CHECK(maxUlp(detcos, rc, -1e6, 1e6, 500000) <= 2.0);
    CHECK(maxUlp(detsin, rs, -7, 7, 500000) <= 2.0);
    CHECK(maxUlp(detcos, rc, -7, 7, 500000) <= 2.0);
    CHECK(maxUlp(detsin, rs, -1e-3, 1e-3, 100000) <= 2.0);
    CHECK(detsin(0.0) == 0.0);
    CHECK(detcos(0.0) == 1.0);
}

// Regression vectors: exact bit patterns, generated once. They catch any cross-platform or
// cross-compiler divergence in the deterministic math.
namespace {
double vecInput(int kind, int i) {
    SplitMix64 sm(0xB0B0CAFEULL + static_cast<std::uint64_t>(kind));
    double u = 0;
    for (int k = 0; k <= i; ++k) u = static_cast<double>(sm.next() >> 11) * 0x1.0p-53;
    switch (kind) {
        case 0: return 1e-3 + u * 1000.0;   // log
        case 1: return -50.0 + u * 100.0;   // exp
        default: return -1000.0 + u * 2000.0;  // sin / cos
    }
}
std::uint64_t bits(double d) { std::uint64_t u; std::memcpy(&u, &d, 8); return u; }

const std::uint64_t kLogVec[64] = {
    0x4018b08fdba91de6ULL, 0x40106ac3abfe129fULL, 0x40199d380850f87eULL, 0x401b0f2026050293ULL,
    0x4014bb2732cac7a5ULL, 0x40146d2f8f4ff5e7ULL, 0x40182ae40e229669ULL, 0x4019d19474a8b0bdULL,
    0x4019ca5c5c2c3ce2ULL, 0x401b7ef20469e32dULL, 0x401604122abbe242ULL, 0x401a8ab5666a437eULL,
    0x4019127b0288e33dULL, 0x40164100d8fa8d86ULL, 0x40190134665b638fULL, 0x401aba2addc249f2ULL,
    0x401b1943588b8b50ULL, 0x401a32021b39e03bULL, 0x4018a542a29f4cc8ULL, 0x401a8439f3f03d50ULL,
    0x4016e0a963c0c846ULL, 0x4017a58f931332daULL, 0x401b996558455c01ULL, 0x40191c846e4bb2c8ULL,
    0x40197262e1cfe0f5ULL, 0x401999846c36833dULL, 0x401b2075f7d2e7fcULL, 0x4019acb4b80af20cULL,
    0x401961f6a64b9a6eULL, 0x400e7accf294a48bULL, 0x4011b27a174ce595ULL, 0x401ac2a0771c2548ULL,
    0x401a9d5307bbba46ULL, 0x401abe08a3182377ULL, 0x401397b53cd69dcdULL, 0x40185b3abacec6a5ULL,
    0x40196d6579617fdbULL, 0x40189b10bfb66d6cULL, 0x40195d47fd3c26a1ULL, 0x4017651b97f73487ULL,
    0x401b5a398131dd54ULL, 0x4011c29c243e3242ULL, 0x40189260a6e698fbULL, 0x4018ce6f45f12752ULL,
    0x401846afac8f67a2ULL, 0x4019a5b262449f21ULL, 0x400bd15ebc50e491ULL, 0x401724e3f6e13ddbULL,
    0x40181b60bd6297b5ULL, 0x40160a4c83fd0e0cULL, 0x4017fd1d3e39322eULL, 0x401890fcbf79cad0ULL,
    0x401ab117908aca92ULL, 0x4013e31d83c4172eULL, 0x40190568d6732295ULL, 0x401b8da5e5f3429aULL,
    0x40163f507f489197ULL, 0x40171c1ed46c9e0dULL, 0x401b6185a84ac08dULL, 0x4015557522680480ULL,
    0x401a502282553c2bULL, 0x401aefb02803b3f4ULL, 0x4018326ef20ebf3dULL, 0x401a760afed97093ULL,
};
const std::uint64_t kExpVec[64] = {
    0x43dbe4f5c60621ebULL, 0x3bf02c926a0e293aULL, 0x3bb9ef0fd52ecfb6ULL, 0x3b7bdef5e694cf8eULL,
    0x3dd4941c7aacd431ULL, 0x4302047eca663f70ULL, 0x3d82a46512911c67ULL, 0x43febaf3df8ac4c1ULL,
    0x447185442f3c0819ULL, 0x42ebf943f38fb288ULL, 0x3e93cf1c2411da62ULL, 0x434a928fb483fb77ULL,
    0x41d6b22b22fa8387ULL, 0x3bcf7a0394e16734ULL, 0x41edc7206bab23ccULL, 0x420dbd5307de8b93ULL,
    0x3cd1405c251c24ffULL, 0x3f489b3afb2e5d22ULL, 0x40515c0349cc4e4aULL, 0x3d960f130698fa4fULL,
    0x3f9473d7cda49e86ULL, 0x3e4b30b68a8504acULL, 0x4333a83c987563ebULL, 0x41b8b2e1765113fbULL,
    0x421c41b411d07566ULL, 0x3d02d6599670d9a7ULL, 0x3b9c4a2f60e14aacULL, 0x43bcdaf95d783735ULL,
    0x3e2759f7e5404712ULL, 0x3eaf723d3ea59892ULL, 0x4377dbbbba94268fULL, 0x3e13965974a0dec4ULL,
    0x41187e18d0829904ULL, 0x4396a76b4dbd6334ULL, 0x3ef0e757863c3b7fULL, 0x3e6de23a2c6e5d09ULL,
    0x43bd70d632332fccULL, 0x421108c8a67f87ffULL, 0x3ee82a643c26517bULL, 0x41ddcda2e103c07fULL,
    0x41987bf06284aa89ULL, 0x42b5e0562a24791cULL, 0x3bc48eaf4562bb72ULL, 0x4094087dcb6e9e44ULL,
    0x3c902d04961fd207ULL, 0x3c3686af8ef724d1ULL, 0x426bbb4b35913241ULL, 0x42ea2477ff0087f9ULL,
    0x4170b1ced846b928ULL, 0x3e5103d1a6ff6fa4ULL, 0x3f8099c55dc9655cULL, 0x3d7c1add3562fcb5ULL,
    0x3f50dc89439206faULL, 0x41c1955e6b6d86c9ULL, 0x3fff5b696c67a3bfULL, 0x408f501e5ee69b1aULL,
    0x3bf7cd63c8072b8cULL, 0x3eaca5b4cb5c9df6ULL, 0x3c8eebedcbbb4342ULL, 0x3f12e79a5be4eea2ULL,
    0x40535c8cd5f0ffd5ULL, 0x40b16136207260c5ULL, 0x3cd62b3c27dc8b00ULL, 0x3e34bbcaa42d4782ULL,
};
const std::uint64_t kSinVec[64] = {
    0xbfefffdb26ba41deULL, 0x3fe4c62152c9e789ULL, 0x3fe18f4318693b96ULL, 0x3fef381378264dd0ULL,
    0xbfe5f379a6a896d8ULL, 0x3fea93e32e17d100ULL, 0x3fee3c27fe26a74fULL, 0x3fd39638e99f407aULL,
    0x3fdfc998ca464256ULL, 0x3fec97cc4cf30760ULL, 0xbfdac6de8311d484ULL, 0xbfe95a9a15340c0dULL,
    0x3fe80ce715143400ULL, 0x3fee1ee5ff1f2433ULL, 0xbfe4312052afcbe8ULL, 0x3fd9a058dfa63d64ULL,
    0x3fd2de2e88913b3aULL, 0x3fe6ef4ec9a4fbe0ULL, 0x3fd3ea244761239aULL, 0xbfe377977a9731ccULL,
    0xbfd69464c9095236ULL, 0x3fd2515664da65fbULL, 0xbfd9661bf88e9c90ULL, 0xbfef5bca07b10997ULL,
    0x3fefb067c01bb195ULL, 0xbfdddcf5a379961cULL, 0x3fe9244c033ce2efULL, 0xbfd74d0b08f8a357ULL,
    0xbfd5fce479ff22c0ULL, 0xbfec6617dcb92ba7ULL, 0xbfecfde8eadce8fbULL, 0x3fd19f95c771a468ULL,
    0x3fd516b90d1e4f35ULL, 0x3fecc3a2dad145f5ULL, 0xbfe9269257681d3dULL, 0x3f9ac0cd9306c428ULL,
    0x3fef3d0ddffadcceULL, 0xbfefeb1ae9fc400eULL, 0x3fd7592715437fffULL, 0xbfcc5bb498470f28ULL,
    0x3feaaf35de7fe78eULL, 0xbfe36f161d111a74ULL, 0x3fc4c27a342cddf3ULL, 0xbfda38ade79078fdULL,
    0x3fddba0c05113440ULL, 0xbfefffefbc7a1e9bULL, 0xbfee8f6d3d0b9ceaULL, 0xbfe928d3c955d406ULL,
    0x3fed3c66049e11c3ULL, 0x3fe63f81e56f7686ULL, 0x3fd66930898af50bULL, 0xbfe37a0d56047ceeULL,
    0x3fd58f637de76b40ULL, 0xbfde77dfae0ef9bcULL, 0xbfeffa0a4903cb0cULL, 0x3fe96ecae4cbb93fULL,
    0xbfeaac4c643cbf33ULL, 0x3fd2b438cb023ef1ULL, 0xbfdfd9e99d3d7835ULL, 0x3fefe6acf6b6c0a3ULL,
    0xbfcf766d03e8b9baULL, 0x3fc632fa87cdae4eULL, 0x3fd881e5e3402414ULL, 0x3fefabbc89cf520fULL,
};
const std::uint64_t kCosVec[64] = {
    0x3f7847f995847ecaULL, 0xbfe857146de9268aULL, 0xbfeac077397d16acULL, 0x3fcc1b17929120e1ULL,
    0x3fe748bc2d0dc9d6ULL, 0x3fe1d2740a4514e4ULL, 0xbfd4f62207dd6ee2ULL, 0xbfee76ed3c187adfULL,
    0x3febc61d960e3cd4ULL, 0x3fdcbc78dcf8f80cULL, 0xbfed1088dfd36756ULL, 0xbfe3861841b68728ULL,
    0x3fe51bd54e82254bULL, 0x3fd59c0e34c30a22ULL, 0xbfe8d33bdfab25c2ULL, 0x3fed529e675ab776ULL,
    0xbfee93e95e9a5dcaULL, 0x3fe650d891d48b13ULL, 0xbfee69502e7aa69eULL, 0xbfe965be83cdae0aULL,
    0xbfedf138cbee5b71ULL, 0x3feea94ae04494f6ULL, 0xbfed5f46977afb15ULL, 0xbfc9800cd381b12dULL,
    0x3fc1ccbe6c7179f3ULL, 0x3fec4d844769c3cbULL, 0x3fe3cbd59dcaf7c6ULL, 0x3fedcdc5c5eea959ULL,
    0x3fee0d5ba1bf16b1ULL, 0xbfdd7f0e222fc5bbULL, 0xbfdb172980ee42c7ULL, 0x3feec34b66b62e98ULL,
    0x3fee367d0c50f460ULL, 0x3fdc0b56bfc56fdaULL, 0x3fe3c8f1b341d592ULL, 0xbfeffd3425c22dc4ULL,
    0xbfcbc20a57f7dd4dULL, 0x3fb245cf924b4710ULL, 0xbfedcb6726962999ULL, 0xbfef346c5b417ffcULL,
    0xbfe1a970e0fd6958ULL, 0x3fe96c413555ad23ULL, 0xbfef938a7d288a46ULL, 0x3fed30dfc87b9698ULL,
    0x3fec56b2fe2777eeULL, 0xbf70219d8fad50f0ULL, 0xbfd2fb28e81d9877ULL, 0xbfe3c613505a001bULL,
    0x3fda051fb950414dULL, 0xbfe70020f14c9ebeULL, 0x3fedf9550e959554ULL, 0xbfe963db8a268e97ULL,
    0x3fee212ada62fd76ULL, 0xbfec241eec29d5aeULL, 0xbfa386d258c915acULL, 0x3fe36bc3c3faf8cbULL,
    0xbfe1add6651dc6daULL, 0x3fee9a5a87d95b02ULL, 0xbfebc170e403934eULL, 0xbfb41d1f97cbc316ULL,
    0xbfef04abbaa90ec6ULL, 0xbfef83dbe67e6823ULL, 0xbfed8f96fb5ffefcULL, 0x3fc24fcf990382daULL,
};

}  // namespace

TEST_CASE("detmath regression vectors (bit exact)", "[detmath]") {
    for (int i = 0; i < 64; ++i) {
        CHECK(bits(detlog(vecInput(0, i))) == kLogVec[i]);
        CHECK(bits(detexp(vecInput(1, i))) == kExpVec[i]);
        CHECK(bits(detsin(vecInput(2, i))) == kSinVec[i]);
        CHECK(bits(detcos(vecInput(2, i))) == kCosVec[i]);
    }
}
