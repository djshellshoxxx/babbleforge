#pragma once
// Deterministic PRNG per docs/REALTIME_ARCHITECTURE.md section 8.2.
#include <cstdint>
#include <string_view>

namespace bf {

class SplitMix64 {
public:
    explicit constexpr SplitMix64(std::uint64_t seed = 0) noexcept : state_(seed) {}
    constexpr std::uint64_t next() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
private:
    std::uint64_t state_;
};

class Xoshiro256StarStar {
public:
    constexpr Xoshiro256StarStar() noexcept : s_{1, 2, 3, 4} {}
    constexpr Xoshiro256StarStar(std::uint64_t s0, std::uint64_t s1, std::uint64_t s2,
                                 std::uint64_t s3) noexcept : s_{s0, s1, s2, s3} {}
    // Fill state with 4 SplitMix64 outputs.
    explicit constexpr Xoshiro256StarStar(std::uint64_t seed) noexcept : s_{} {
        SplitMix64 sm(seed);
        for (auto& v : s_) v = sm.next();
    }
    constexpr std::uint64_t next() noexcept {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }
private:
    static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept {
        return (x << k) | (x >> (64 - k));
    }
    std::uint64_t s_[4];
};

constexpr std::uint64_t fnv1a64(std::string_view s) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 0x100000001B3ULL;
    }
    return h;
}

// streamSeed = first SplitMix64 output seeded with (master ^ FNV1a64(name) ^ epoch*golden).
constexpr std::uint64_t deriveStreamSeed(std::uint64_t master, std::string_view name,
                                         std::uint64_t epoch) noexcept {
    SplitMix64 sm(master ^ fnv1a64(name) ^ (epoch * 0x9E3779B97F4A7C15ULL));
    return sm.next();
}

class RngStream {
public:
    // Seed directly from an already-derived stream seed.
    explicit constexpr RngStream(std::uint64_t streamSeed) noexcept : gen_(streamSeed) {}
    constexpr RngStream(std::uint64_t master, std::string_view name, std::uint64_t epoch) noexcept
        : gen_(deriveStreamSeed(master, name, epoch)) {}

    constexpr std::uint64_t nextU64() noexcept { return gen_.next(); }
    // [0,1)
    constexpr double uniform01() noexcept {
        return static_cast<double>(gen_.next() >> 11) * 0x1.0p-53;
    }
    constexpr float uniformFloat01() noexcept {
        return static_cast<float>(gen_.next() >> 40) * 0x1.0p-24f;
    }
    // Uniform integer in [0, n) (Lemire, unbiased). n == 0 returns 0.
    std::uint64_t uniformInt(std::uint64_t n) noexcept;
    // Noise in [-1, 1).
    constexpr float uniformPM1f() noexcept { return uniformFloat01() * 2.0f - 1.0f; }

private:
    Xoshiro256StarStar gen_;
};

}  // namespace bf
