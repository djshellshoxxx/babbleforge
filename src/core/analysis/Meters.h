#pragma once
// Output meters (docs/ENGINE.md section 5): RMS-fast (300 ms), RMS-Leq60, sample/true peak,
// BS.1770-5 loudness (M / S / gated I, all channel weights 1.0) and crest factor.
// process() is RT-safe (accumulates into preallocated plain members, single writer thread).
// snapshot() is non-RT and must be called from the same thread as process() (or with external
// synchronisation). Windows that are block based (Leq60, 10 s crest, LUFS) use 100 ms blocks and
// only completed blocks. Values in dB are floored at kFloorDb.
#include <cstdint>
#include <vector>

#include "core/dsp/TruePeakDetector.h"

namespace bf {

struct KWeightCoeffs {
  double b[3];
  double a[3];  // a[0] == 1
};
/// BS.1770 pre-filter (stage 1 high shelf) and RLB (stage 2 high pass) for any sample rate.
KWeightCoeffs kWeightStage1(double fs) noexcept;
KWeightCoeffs kWeightStage2(double fs) noexcept;

struct MeterSnapshot {
  static constexpr double kFloorDb = -200.0;
  std::vector<double> rmsFastDb;        // per channel, dBFS, last 300 ms
  std::vector<double> leq60Db;          // per channel, dBFS, last 60 s (completed blocks)
  std::vector<double> samplePeakDb;     // per channel, last 1 s
  std::vector<double> samplePeakMaxDb;  // per channel, session
  std::vector<double> truePeakDb;       // per channel, last 1 s (dBTP)
  std::vector<double> truePeakMaxDb;    // per channel, session
  double rmsFastAllDb = kFloorDb;       // energy mean over channels
  double leq60AllDb = kFloorDb;         // energy mean over channels
  double lufsM = kFloorDb;              // 400 ms
  double lufsS = kFloorDb;              // 3 s
  double lufsI = kFloorDb;              // gated, since reset
  double truePeak10sDb = kFloorDb;      // max over channels, last 10 s
  double rms10sDb = kFloorDb;           // energy mean over channels, last 10 s
  double crestDb = 0.0;                 // truePeak10sDb - rms10sDb (0 when silent)
  std::uint64_t frames = 0;
};

class Meters {
 public:
  static constexpr double kBlockSeconds = 0.1;

  void prepare(double fs, int numChannels);  // non-RT, allocates
  void reset() noexcept;
  void process(const float* const* in, int n) noexcept;  // RT
  MeterSnapshot snapshot() const;                        // non-RT

 private:
  struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double tick(double x) noexcept {
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };
  void closeBlock() noexcept;
  double lufsOfLastBlocks(int nBlocks) const noexcept;

  double fs_ = 48000.0;
  int nCh_ = 0;
  int blockLen_ = 4800;
  int posInBlock_ = 0;
  std::uint64_t frames_ = 0;
  std::uint64_t blocksDone_ = 0;

  TruePeakDetector tp_;
  std::vector<Biquad> k1_, k2_;

  // RMS-fast: sample-square ring per channel with running sum (recomputed each block).
  int fastLen_ = 0;
  std::vector<double> fastRing_;  // [ch][fastLen_]
  std::vector<double> fastSum_;
  int fastPos_ = 0;

  // Current block accumulators.
  std::vector<double> blkPow_;      // raw sum x^2 per channel
  double blkKPow_ = 0;              // sum over channels of K-weighted x^2
  std::vector<float> blkSamplePk_;  // per channel
  std::vector<float> blkTruePk_;

  // Block histories (ring, most recent at (blocksDone_-1) % size).
  static constexpr int kLeqBlocks = 600;   // 60 s
  static constexpr int kPeakBlocks = 100;  // 10 s
  static constexpr int kShortBlocks = 30;  // 3 s
  std::vector<double> powHist_;            // [kLeqBlocks][ch] raw mean square
  std::vector<double> kHist_;              // [kShortBlocks] sum over ch of K-weighted mean square
  std::vector<float> spHist_, tpHist_;     // [kPeakBlocks][ch]
  std::vector<float> spMax_, tpMax_;       // session max per channel

  // Gating histogram (0.05 LU bins from -70 LUFS).
  static constexpr int kBins = 2400;
  static constexpr double kBinLu = 0.05;
  std::vector<std::uint64_t> binCount_;
  std::vector<double> binEnergy_;
};

}  // namespace bf
