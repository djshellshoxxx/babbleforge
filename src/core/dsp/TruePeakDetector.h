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

  /// Threshold-gated block version for consumers that only act on peaks above `threshold`
  /// (the limiter). If a rigorous bound (max |x| over the filter window x the largest
  /// per-phase coefficient L1 norm, with a float rounding margin) shows that no output of
  /// this block can exceed `threshold`, the history is updated and peakOut[] is filled with
  /// 0 without running the polyphase filters; otherwise peakOut[i] = processSample(ch, in[i]).
  /// Either way every peakOut[i] > threshold is bit-identical to process(). Returns true if
  /// the filters ran.
  bool processAbove(int ch, const float* in, float* peakOut, int n, float threshold) noexcept;

 private:
  int factor_ = 4;
  int numCh_ = 0;
  std::vector<float> coeffs_;  // [phase][48], ordered oldest -> newest sample
  std::vector<float> hist_;    // [ch][2 * 48] double-length ring
  std::vector<int> pos_;       // [ch]
  double gainBound_ = 0.0;     // max over phases of sum |coeff| (x rounding margin)
};

}  // namespace bf
