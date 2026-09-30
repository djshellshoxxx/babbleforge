#include "core/dsp/TruePeakDetector.h"

#include <algorithm>
#include <cmath>

#include "core/math/DetMath.h"

namespace bf {

namespace {
constexpr double kPi = 3.14159265358979323846;

double besselI0(double x) noexcept {
  double sum = 1.0, term = 1.0;
  const double q = x * x * 0.25;
  for (int k = 1; k < 200; ++k) {
    term *= q / (double(k) * double(k));
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}
}  // namespace

int TruePeakDetector::oversamplingForRate(double fs) noexcept {
  if (fs <= 50000.0) return 4;
  if (fs <= 100000.0) return 2;
  return 1;
}

void TruePeakDetector::prepare(double fs, int numChannels) {
  factor_ = oversamplingForRate(fs);
  numCh_ = std::max(0, numChannels);
  const int L = factor_;
  const int N = kTapsPerPhase * L;
  const int c = kTapsPerPhase / 2 * L;  // centre tap (integer multiple of L)
  const double beta = 8.0;
  const double i0b = besselI0(beta);
  std::vector<double> h(static_cast<size_t>(N));
  for (int m = 0; m < N; ++m) {
    const double d = double(m - c);
    const double arg = d / double(L);
    double s = 1.0;
    if (m != c) {
      const double px = kPi * arg;
      s = detsin(px) / px;
    }
    const double r = d / double(c);
    const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
    h[static_cast<size_t>(m)] = s * w;
  }
  coeffs_.assign(static_cast<size_t>(L * kTapsPerPhase), 0.0f);
  for (int j = 0; j < L; ++j)
    for (int i = 0; i < kTapsPerPhase; ++i)
      coeffs_[static_cast<size_t>(j * kTapsPerPhase + i)] =
          static_cast<float>(h[static_cast<size_t>((kTapsPerPhase - 1 - i) * L + j)]);
  hist_.assign(static_cast<size_t>(numCh_) * 2 * kTapsPerPhase, 0.0f);
  pos_.assign(static_cast<size_t>(numCh_), 0);
}

void TruePeakDetector::reset() noexcept {
  std::fill(hist_.begin(), hist_.end(), 0.0f);
  std::fill(pos_.begin(), pos_.end(), 0);
}

float TruePeakDetector::processSample(int ch, float x) noexcept {
  float* hist = hist_.data() + static_cast<size_t>(ch) * 2 * kTapsPerPhase;
  int& p = pos_[static_cast<size_t>(ch)];
  hist[p] = x;
  hist[p + kTapsPerPhase] = x;
  p = (p + 1 == kTapsPerPhase) ? 0 : p + 1;
  const float* w = hist + p;  // oldest .. newest
  float peak = 0.0f;
  for (int j = 0; j < factor_; ++j) {
    const float* cf = coeffs_.data() + static_cast<size_t>(j) * kTapsPerPhase;
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    for (int i = 0; i < kTapsPerPhase; i += 4) {
      a0 += cf[i] * w[i];
      a1 += cf[i + 1] * w[i + 1];
      a2 += cf[i + 2] * w[i + 2];
      a3 += cf[i + 3] * w[i + 3];
    }
    peak = std::max(peak, std::fabs((a0 + a1) + (a2 + a3)));
  }
  return peak;
}

void TruePeakDetector::process(int ch, const float* in, float* peakOut, int n) noexcept {
  for (int i = 0; i < n; ++i) peakOut[i] = processSample(ch, in[i]);
}

}  // namespace bf
