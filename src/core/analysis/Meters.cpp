#include "core/analysis/Meters.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/math/DetMath.h"

namespace bf {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kLn10 = 2.302585092994046;
constexpr double kFloor = MeterSnapshot::kFloorDb;
constexpr double kLoudnessOffset = -0.691;

inline double db10(double power) noexcept {
  if (!(power > 1e-20)) return kFloor;
  return std::max(kFloor, 10.0 / kLn10 * detlog(power));
}
inline double db20(double amp) noexcept {
  if (!(amp > 1e-10)) return kFloor;
  return std::max(kFloor, 20.0 / kLn10 * detlog(amp));
}
inline double tanD(double x) noexcept {
  double s, c;
  detsincos(x, s, c);
  return s / c;
}
inline double pow10d(double x) noexcept { return detexp(x * kLn10); }
}  // namespace

KWeightCoeffs kWeightStage1(double fs) noexcept {
  const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
  const double K = tanD(kPi * f0 / fs);
  const double Vh = pow10d(G / 20.0);
  const double Vb = detexp(0.4996667741545416 * detlog(Vh));
  const double a0 = 1.0 + K / Q + K * K;
  KWeightCoeffs c;
  c.b[0] = (Vh + Vb * K / Q + K * K) / a0;
  c.b[1] = 2.0 * (K * K - Vh) / a0;
  c.b[2] = (Vh - Vb * K / Q + K * K) / a0;
  c.a[0] = 1.0;
  c.a[1] = 2.0 * (K * K - 1.0) / a0;
  c.a[2] = (1.0 - K / Q + K * K) / a0;
  return c;
}

KWeightCoeffs kWeightStage2(double fs) noexcept {
  const double f0 = 38.13547087602444, Q = 0.5003270373238773;
  const double K = tanD(kPi * f0 / fs);
  const double a0 = 1.0 + K / Q + K * K;
  KWeightCoeffs c;
  c.b[0] = 1.0;
  c.b[1] = -2.0;
  c.b[2] = 1.0;
  c.a[0] = 1.0;
  c.a[1] = 2.0 * (K * K - 1.0) / a0;
  c.a[2] = (1.0 - K / Q + K * K) / a0;
  return c;
}

void Meters::prepare(double fs, int numChannels) {
  fs_ = fs;
  nCh_ = std::max(1, numChannels);
  blockLen_ = std::max(1, static_cast<int>(std::lround(kBlockSeconds * fs)));
  fastLen_ = 3 * blockLen_;
  tp_.prepare(fs, nCh_);
  const size_t nc = static_cast<size_t>(nCh_);
  k1_.assign(nc, Biquad{});
  k2_.assign(nc, Biquad{});
  const KWeightCoeffs s1 = kWeightStage1(fs), s2 = kWeightStage2(fs);
  for (size_t c = 0; c < nc; ++c) {
    k1_[c].b0 = s1.b[0]; k1_[c].b1 = s1.b[1]; k1_[c].b2 = s1.b[2]; k1_[c].a1 = s1.a[1]; k1_[c].a2 = s1.a[2];
    k2_[c].b0 = s2.b[0]; k2_[c].b1 = s2.b[1]; k2_[c].b2 = s2.b[2]; k2_[c].a1 = s2.a[1]; k2_[c].a2 = s2.a[2];
  }
  fastRing_.assign(nc * static_cast<size_t>(fastLen_), 0.0);
  fastSum_.assign(nc, 0.0);
  blkPow_.assign(nc, 0.0);
  blkSamplePk_.assign(nc, 0.0f);
  blkTruePk_.assign(nc, 0.0f);
  powHist_.assign(static_cast<size_t>(kLeqBlocks) * nc, 0.0);
  kHist_.assign(kShortBlocks, 0.0);
  spHist_.assign(static_cast<size_t>(kPeakBlocks) * nc, 0.0f);
  tpHist_.assign(static_cast<size_t>(kPeakBlocks) * nc, 0.0f);
  spMax_.assign(nc, 0.0f);
  tpMax_.assign(nc, 0.0f);
  binCount_.assign(kBins, 0);
  binEnergy_.assign(kBins, 0.0);
  reset();
}

void Meters::reset() noexcept {
  tp_.reset();
  for (auto& b : k1_) b.z1 = b.z2 = 0;
  for (auto& b : k2_) b.z1 = b.z2 = 0;
  std::fill(fastRing_.begin(), fastRing_.end(), 0.0);
  std::fill(fastSum_.begin(), fastSum_.end(), 0.0);
  fastPos_ = 0;
  std::fill(blkPow_.begin(), blkPow_.end(), 0.0);
  blkKPow_ = 0;
  std::fill(blkSamplePk_.begin(), blkSamplePk_.end(), 0.0f);
  std::fill(blkTruePk_.begin(), blkTruePk_.end(), 0.0f);
  std::fill(powHist_.begin(), powHist_.end(), 0.0);
  std::fill(kHist_.begin(), kHist_.end(), 0.0);
  std::fill(spHist_.begin(), spHist_.end(), 0.0f);
  std::fill(tpHist_.begin(), tpHist_.end(), 0.0f);
  std::fill(spMax_.begin(), spMax_.end(), 0.0f);
  std::fill(tpMax_.begin(), tpMax_.end(), 0.0f);
  std::fill(binCount_.begin(), binCount_.end(), 0);
  std::fill(binEnergy_.begin(), binEnergy_.end(), 0.0);
  posInBlock_ = 0;
  frames_ = 0;
  blocksDone_ = 0;
}

void Meters::process(const float* const* in, int n) noexcept {
  for (int i = 0; i < n; ++i) {
    for (int c = 0; c < nCh_; ++c) {
      const size_t ci = static_cast<size_t>(c);
      const float x = in[c][i];
      const double xd = x;
      const double sq = xd * xd;
      double& slot = fastRing_[ci * static_cast<size_t>(fastLen_) + static_cast<size_t>(fastPos_)];
      fastSum_[ci] += sq - slot;
      slot = sq;
      blkPow_[ci] += sq;
      const double k = k2_[ci].tick(k1_[ci].tick(xd));
      blkKPow_ += k * k;
      const float ax = std::fabs(x);
      blkSamplePk_[ci] = std::max(blkSamplePk_[ci], ax);
      blkTruePk_[ci] = std::max(blkTruePk_[ci], std::max(ax, tp_.processSample(c, x)));
    }
    fastPos_ = (fastPos_ + 1 == fastLen_) ? 0 : fastPos_ + 1;
    if (++posInBlock_ == blockLen_) closeBlock();
  }
  frames_ += static_cast<std::uint64_t>(n);
}

void Meters::closeBlock() noexcept {
  const size_t nc = static_cast<size_t>(nCh_);
  const double inv = 1.0 / double(blockLen_);
  const size_t slot = static_cast<size_t>(blocksDone_);
  for (size_t c = 0; c < nc; ++c) {
    powHist_[(slot % kLeqBlocks) * nc + c] = blkPow_[c] * inv;
    spHist_[(slot % kPeakBlocks) * nc + c] = blkSamplePk_[c];
    tpHist_[(slot % kPeakBlocks) * nc + c] = blkTruePk_[c];
    spMax_[c] = std::max(spMax_[c], blkSamplePk_[c]);
    tpMax_[c] = std::max(tpMax_[c], blkTruePk_[c]);
    blkPow_[c] = 0.0;
    blkSamplePk_[c] = 0.0f;
    blkTruePk_[c] = 0.0f;
    // Bound running-sum drift: recompute exactly once per block.
    double s = 0.0;
    const double* r = fastRing_.data() + c * static_cast<size_t>(fastLen_);
    for (int i = 0; i < fastLen_; ++i) s += r[i];
    fastSum_[c] = s;
  }
  kHist_[slot % kShortBlocks] = blkKPow_ * inv;
  blkKPow_ = 0.0;
  ++blocksDone_;
  posInBlock_ = 0;

  // Gating block (400 ms, 75 % overlap): one per completed 100 ms block once 4 exist.
  if (blocksDone_ >= 4) {
    double e = 0.0;
    for (int i = 1; i <= 4; ++i) e += kHist_[static_cast<size_t>(blocksDone_ - static_cast<std::uint64_t>(i)) % kShortBlocks];
    e *= 0.25;
    const double l = kLoudnessOffset + db10(e);
    if (l > -70.0) {
      int bin = static_cast<int>((l + 70.0) / kBinLu);
      bin = std::min(bin, kBins - 1);
      ++binCount_[static_cast<size_t>(bin)];
      binEnergy_[static_cast<size_t>(bin)] += e;
    }
  }
}

double Meters::lufsOfLastBlocks(int nBlocks) const noexcept {
  const int avail = static_cast<int>(std::min<std::uint64_t>(blocksDone_, static_cast<std::uint64_t>(nBlocks)));
  if (avail < nBlocks) return kFloor;
  double e = 0.0;
  for (int i = 1; i <= nBlocks; ++i)
    e += kHist_[static_cast<size_t>(blocksDone_ - static_cast<std::uint64_t>(i)) % kShortBlocks];
  return kLoudnessOffset + db10(e / double(nBlocks));
}

MeterSnapshot Meters::snapshot() const {
  MeterSnapshot s;
  const size_t nc = static_cast<size_t>(nCh_);
  s.frames = frames_;
  s.rmsFastDb.resize(nc);
  s.leq60Db.resize(nc);
  s.samplePeakDb.resize(nc);
  s.samplePeakMaxDb.resize(nc);
  s.truePeakDb.resize(nc);
  s.truePeakMaxDb.resize(nc);

  const std::uint64_t filled = std::min<std::uint64_t>(frames_, static_cast<std::uint64_t>(fastLen_));
  double fastAll = 0.0;
  for (size_t c = 0; c < nc; ++c) {
    const double ms = filled ? std::max(0.0, fastSum_[c]) / double(fastLen_) : 0.0;
    s.rmsFastDb[c] = db10(ms);
    fastAll += ms;
  }
  s.rmsFastAllDb = db10(fastAll / double(nCh_));

  const int nLeq = static_cast<int>(std::min<std::uint64_t>(blocksDone_, kLeqBlocks));
  const int nTen = static_cast<int>(std::min<std::uint64_t>(blocksDone_, kPeakBlocks));
  double leqAll = 0.0, tenAll = 0.0;
  float tp10 = 0.0f;
  for (size_t c = 0; c < nc; ++c) {
    double sum60 = 0.0, sum10 = 0.0;
    for (int i = 1; i <= nLeq; ++i) {
      const double v = powHist_[(static_cast<size_t>(blocksDone_ - static_cast<std::uint64_t>(i)) % kLeqBlocks) * nc + c];
      sum60 += v;
      if (i <= nTen) sum10 += v;
    }
    const double ms60 = nLeq ? sum60 / double(nLeq) : 0.0;
    s.leq60Db[c] = db10(ms60);
    leqAll += ms60;
    tenAll += nTen ? sum10 / double(nTen) : 0.0;

    // Peaks: last 1 s = current partial block + 9 most recent complete blocks.
    float sp1 = blkSamplePk_[c], tp1 = blkTruePk_[c];
    float tpMax10 = blkTruePk_[c];
    for (int i = 1; i <= std::min(nTen, kPeakBlocks - 1); ++i) {
      const size_t idx = (static_cast<size_t>(blocksDone_ - static_cast<std::uint64_t>(i)) % kPeakBlocks) * nc + c;
      if (i <= 9) {
        sp1 = std::max(sp1, spHist_[idx]);
        tp1 = std::max(tp1, tpHist_[idx]);
      }
      tpMax10 = std::max(tpMax10, tpHist_[idx]);
    }
    tp10 = std::max(tp10, tpMax10);
    s.samplePeakDb[c] = db20(sp1);
    s.truePeakDb[c] = db20(tp1);
    s.samplePeakMaxDb[c] = db20(std::max(spMax_[c], blkSamplePk_[c]));
    s.truePeakMaxDb[c] = db20(std::max(tpMax_[c], blkTruePk_[c]));
  }
  s.leq60AllDb = db10(leqAll / double(nCh_));
  s.rms10sDb = db10(tenAll / double(nCh_));
  s.truePeak10sDb = db20(tp10);
  s.crestDb = (s.truePeak10sDb > kFloor && s.rms10sDb > kFloor) ? s.truePeak10sDb - s.rms10sDb : 0.0;

  s.lufsM = lufsOfLastBlocks(4);
  s.lufsS = lufsOfLastBlocks(kShortBlocks);

  // Gated integrated loudness from the histogram.
  double eAbs = 0.0;
  std::uint64_t nAbs = 0;
  for (int b = 0; b < kBins; ++b) {
    eAbs += binEnergy_[static_cast<size_t>(b)];
    nAbs += binCount_[static_cast<size_t>(b)];
  }
  if (nAbs > 0) {
    const double gammaR = kLoudnessOffset + db10(eAbs / double(nAbs)) - 10.0;
    int startBin = static_cast<int>(std::floor((gammaR + 70.0) / kBinLu)) + 1;
    startBin = std::max(0, startBin);
    double eRel = 0.0;
    std::uint64_t nRel = 0;
    for (int b = startBin; b < kBins; ++b) {
      eRel += binEnergy_[static_cast<size_t>(b)];
      nRel += binCount_[static_cast<size_t>(b)];
    }
    if (nRel > 0) s.lufsI = kLoudnessOffset + db10(eRel / double(nRel));
  }
  return s;
}

}  // namespace bf
