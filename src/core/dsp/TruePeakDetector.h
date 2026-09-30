#pragma once
// BS.1770-5 Annex 2 style true-peak detector: polyphase oversampling (4x at fs <= 50 kHz,
// 2x at <= 100 kHz, else 1x), 48 taps per phase, Kaiser (beta = 8) windowed-sinc lowpass.
// Per-channel state. prepare() allocates; process*() are RT-safe (no allocation, no locks).
#include <cstddef>
#include <vector>

namespace bf {

class TruePeakDetector {
 public:
  static constexpr int kTapsPerPhase = 48;
  /// Delay (base-rate samples) between the input and the phase-0 output.
  static constexpr int kLatencySamples = kTapsPerPhase / 2;

  static int oversamplingForRate(double fs) noexcept;

  void prepare(double fs, int numChannels);  // non-RT
  void reset() noexcept;                     // clears all channel histories

  int oversampling() const noexcept { return factor_; }
  int numChannels() const noexcept { return numCh_; }

  /// Pushes one sample; returns the max |value| over the oversampled points that follow
  /// the input sample delayed by kLatencySamples (linear amplitude).
  float processSample(int ch, float x) noexcept;

  /// Block version: peakOut[i] = processSample(ch, in[i]).
  void process(int ch, const float* in, float* peakOut, int n) noexcept;

 private:
  int factor_ = 4;
  int numCh_ = 0;
  std::vector<float> coeffs_;  // [phase][48], ordered oldest -> newest sample
  std::vector<float> hist_;    // [ch][2 * 48] double-length ring
  std::vector<int> pos_;       // [ch]
};

}  // namespace bf
