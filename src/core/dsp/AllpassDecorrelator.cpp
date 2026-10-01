#include "core/dsp/AllpassDecorrelator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "core/math/Restrict.h"
#include "core/random/Random.h"

namespace bf {

namespace {

constexpr double kMinDelayMs = 0.4;
constexpr double kMaxRhoBetweenNeighbours = 0.10;
constexpr int kMaxAttempts = 24;

std::vector<int> primesInRange(int lo, int hi) {
  std::vector<int> out;
  for (int n = std::max(lo, 2); n <= hi; ++n) {
    bool prime = true;
    for (int d = 2; d * d <= n; ++d) {
      if (n % d == 0) {
        prime = false;
        break;
      }
    }
    if (prime) out.push_back(n);
  }
  return out;
}

// Impulse response of the cascade (double precision, design time only).
std::vector<double> cascadeIr(const std::vector<int>& delays, const std::vector<double>& g,
                              std::size_t len) {
  std::vector<double> x(len, 0.0);
  x[0] = 1.0;
  for (std::size_t s = 0; s < delays.size(); ++s) {
    const auto D = static_cast<std::size_t>(delays[s]);
    std::vector<double> w(len, 0.0), y(len, 0.0);
    for (std::size_t n = 0; n < len; ++n) {
      const double wd = n >= D ? w[n - D] : 0.0;
      w[n] = x[n] + g[s] * wd;
      y[n] = -g[s] * w[n] + wd;
    }
    x.swap(y);
  }
  return x;
}

double tailFraction(const std::vector<double>& ir, std::size_t from) {
  double tot = 0.0, tail = 0.0;
  for (std::size_t n = 0; n < ir.size(); ++n) {
    const double e = ir[n] * ir[n];
    tot += e;
    if (n >= from) tail += e;
  }
  return tot > 0.0 ? tail / tot : 0.0;
}

double innerProduct(const std::vector<double>& a, const std::vector<double>& b) {
  double s = 0.0, ea = 0.0, eb = 0.0;
  const auto n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    s += a[i] * b[i];
    ea += a[i] * a[i];
    eb += b[i] * b[i];
  }
  return (ea > 0.0 && eb > 0.0) ? s / std::sqrt(ea * eb) : 0.0;
}

}  // namespace

void AllpassDecorrelator::prepare(double fs, int numChannels, SpeakerVariation level,
                                  std::uint64_t masterSeed, std::uint64_t epoch) {
  fs_ = fs;
  channels_.clear();
  channels_.resize(idx(std::max(numChannels, 0)));
  stages_ = level == SpeakerVariation::Low ? 0 : (level == SpeakerVariation::Medium ? 3 : 5);
  const double maxMs = level == SpeakerVariation::High ? 4.5 : 3.0;
  dLo_ = static_cast<int>(std::lround(kMinDelayMs * 1e-3 * fs));
  dHi_ = static_cast<int>(std::floor(maxMs * 1e-3 * fs));
  if (stages_ == 0) return;

  const std::vector<int> allPrimes = primesInRange(dLo_, dHi_);
  const auto irLen = static_cast<std::size_t>(std::ceil(0.3 * fs));
  const auto tailFrom = static_cast<std::size_t>(std::ceil(kDecayLimitSeconds * fs));

  std::vector<double> prevIr;
  std::vector<int> prevSet;
  for (int c = 0; c < numChannels; ++c) {
    RngStream rng(masterSeed, "decorrelator.ch." + std::to_string(c), epoch);
    std::vector<int> pool;
    for (int p : allPrimes)
      if (std::find(prevSet.begin(), prevSet.end(), p) == prevSet.end()) pool.push_back(p);
    if (static_cast<int>(pool.size()) < stages_) pool = allPrimes;

    Channel best;
    std::vector<double> bestIr;
    double bestRho = 2.0;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
      std::vector<int> avail = pool;
      Channel cand;
      std::vector<int> delays;
      for (int s = 0; s < stages_; ++s) {
        Stage st;
        const auto k = static_cast<std::size_t>(rng.uniformInt(avail.size()));
        st.delay = avail[k];
        avail.erase(avail.begin() + static_cast<std::ptrdiff_t>(k));
        st.magnitude = static_cast<float>(0.45 + 0.15 * rng.uniform01());
        st.sign = ((c + s) % 2 == 0) ? 1.0f : -1.0f;
        delays.push_back(st.delay);
        cand.st.push_back(std::move(st));
      }
      // Decay check: scale |g| down until the EDC reaches -60 dB by 25 ms.
      double scale = 1.0;
      std::vector<double> ir;
      double tail = 0.0;
      for (int it = 0; it < 120; ++it) {
        std::vector<double> g;
        for (const auto& st : cand.st)
          g.push_back(static_cast<double>(st.sign) * static_cast<double>(st.magnitude) * scale);
        ir = cascadeIr(delays, g, irLen);
        tail = tailFraction(ir, tailFrom);
        if (tail <= kDecayLimitEnergy) break;
        scale *= 0.93;
      }
      const double rho = prevIr.empty() ? 0.0 : std::fabs(innerProduct(ir, prevIr));
      if (rho < bestRho) {
        bestRho = rho;
        cand.scale = scale;
        cand.tail = tail;
        best = std::move(cand);
        bestIr = std::move(ir);
      }
      if (bestRho <= kMaxRhoBetweenNeighbours) break;
    }
    for (auto& st : best.st) {
      st.g = static_cast<float>(static_cast<double>(st.sign) * static_cast<double>(st.magnitude) *
                                best.scale);
      st.buf.assign(idx(st.delay), 0.0f);
    }
    prevSet.clear();
    for (const auto& st : best.st) prevSet.push_back(st.delay);
    prevIr = std::move(bestIr);
    channels_[idx(c)] = std::move(best);
  }
}

void AllpassDecorrelator::reset() noexcept {
  for (auto& ch : channels_)
    for (auto& st : ch.st) {
      std::fill(st.buf.begin(), st.buf.end(), 0.0f);
      st.pos = 0;
    }
}

void AllpassDecorrelator::process(const float* const* in, float* const* out, int n) noexcept {
  for (std::size_t c = 0; c < channels_.size(); ++c) {
    auto& ch = channels_[c];
    if (ch.st.empty()) {
      if (in[c] != out[c] && n > 0) std::memmove(out[c], in[c], sizeof(float) * idx(n));
      continue;
    }
    // Stage-major: each all-pass stage filters the whole block in place before the next
    // (a cascade of causal filters, so this equals the sample-major order bit for bit). A
    // run of at most `delay` samples up to the ring wrap reads only values written before
    // it, so the run is element-wise independent and vectorises.
    if (in[c] != out[c] && n > 0) std::memmove(out[c], in[c], sizeof(float) * idx(n));
    for (auto& st : ch.st) {
      const float g = st.g, ng = -st.g;
      float* BF_RESTRICT y = out[c];
      int i = 0;
      while (i < n) {
        const int len = std::min(n - i, st.delay - st.pos);
        float* BF_RESTRICT b = st.buf.data() + idx(st.pos);
        for (int k = 0; k < len; ++k) {
          const float wd = b[k];
          const float w = y[i + k] + g * wd;
          y[i + k] = ng * w + wd;
          b[k] = w;
        }
        i += len;
        st.pos += len;
        if (st.pos == st.delay) st.pos = 0;
      }
    }
  }
}

}  // namespace bf
