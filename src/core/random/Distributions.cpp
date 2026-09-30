#include "core/random/Distributions.h"

#include "core/math/DetMath.h"

namespace bf {

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
struct Pair { double z0, z1; };
Pair boxMuller(RngStream& rng) noexcept {
    const double u1 = 1.0 - rng.uniform01();  // (0, 1]
    const double u2 = rng.uniform01();
    const double r = detsqrt(-2.0 * detlog(u1));
    double s, c;
    detsincos(kTwoPi * u2, s, c);
    return {r * c, r * s};
}
}  // namespace

double exponential(RngStream& rng, double mean) noexcept {
    return -mean * detlog(1.0 - rng.uniform01());
}

double standardNormal(RngStream& rng) noexcept { return boxMuller(rng).z0; }

double NormalSampler::next(RngStream& rng) noexcept {
    if (has_) { has_ = false; return cached_; }
    const Pair p = boxMuller(rng);
    cached_ = p.z1;
    has_ = true;
    return p.z0;
}

double logNormal(RngStream& rng, double mu, double sigma) noexcept {
    return detexp(mu + sigma * standardNormal(rng));
}

double truncatedNormal(RngStream& rng, double sd, double limitSd) noexcept {
    double z = 0.0;
    for (int i = 0; i < 8; ++i) {
        z = standardNormal(rng);
        if (z >= -limitSd && z <= limitSd) return sd * z;
    }
    z = z < -limitSd ? -limitSd : (z > limitSd ? limitSd : z);
    return sd * z;
}

double truncatedLogNormalMedian(RngStream& rng, double median, double sigmaLn, double lo,
                                double hi) noexcept {
    double v = median;
    for (int i = 0; i < 8; ++i) {
        v = median * detexp(sigmaLn * standardNormal(rng));
        if (v >= lo && v <= hi) return v;
    }
    return v < lo ? lo : (v > hi ? hi : v);
}

std::size_t weightedChoice(RngStream& rng, std::span<const double> weights) noexcept {
    const double u = rng.uniform01();
    double total = 0.0;
    for (double w : weights) total += (w > 0.0 ? w : 0.0);
    if (!(total > 0.0)) return 0;
    const double target = u * total;
    double cum = 0.0;
    std::size_t lastPositive = 0;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        if (!(weights[i] > 0.0)) continue;
        cum += weights[i];
        lastPositive = i;
        if (target < cum) return i;
    }
    return lastPositive;
}

}  // namespace bf
