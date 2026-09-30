#pragma once
// Deterministic, libm-free math (fdlibm-derived algorithms; identical results on every
// IEEE-754 platform when compiled without FP contraction). Domains:
//   detlog: x > 0 (x <= 0 gives -inf / NaN), detexp: |x| < 700,
//   detsin/detcos: |x| < ~1.6e6.
#include <cmath>

namespace bf {

double detlog(double x) noexcept;
double detexp(double x) noexcept;
double detsin(double x) noexcept;
double detcos(double x) noexcept;
void detsincos(double x, double& s, double& c) noexcept;
inline double detsqrt(double x) noexcept { return std::sqrt(x); }  // IEEE correctly rounded

}  // namespace bf
