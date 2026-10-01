#pragma once
// Deterministic distributions (docs/REALTIME_ARCHITECTURE.md section 8.2). DetMath only.
#include <cstddef>
#include <span>

#include "core/random/Random.h"

namespace bf {

double exponential(RngStream& rng, double mean) noexcept;

// Box-Muller (non-polar). One-shot form: uses one uniform pair, discards the sine value.
double standardNormal(RngStream& rng) noexcept;

// Box-Muller with the second value cached (two draws of the stream per two normals).
struct NormalSampler {
    double next(RngStream& rng) noexcept;
private:
    bool has_ = false;
    double cached_ = 0.0;
};

double logNormal(RngStream& rng, double mu, double sigma) noexcept;  // exp(mu + sigma Z)

// sd * Z with |Z| <= limitSd; rejection max 8 tries, then clamp.
double truncatedNormal(RngStream& rng, double sd, double limitSd) noexcept;

// median * exp(sigmaLn * Z) constrained to [lo, hi]; resample max 8 then clamp.
double truncatedLogNormalMedian(RngStream& rng, double median, double sigmaLn, double lo,
                                double hi) noexcept;

// Index chosen with probability proportional to weights (order matters: caller supplies a
// stable order). Always consumes exactly one uniform. Degenerate (sum <= 0) returns 0.
std::size_t weightedChoice(RngStream& rng, std::span<const double> weights) noexcept;

}  // namespace bf
