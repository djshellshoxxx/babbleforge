#pragma once
// True-peak look-ahead safety limiter (docs/ENGINE.md section 6).
//
// Signal path per sample: true-peak detector (on the input) -> required gain per link group
// -> sliding-window minimum (monotonic deque) -> two-stage release with 10 ms peak hold ->
// cascaded box smoother (triangular, S samples) -> gain applied to the delayed signal ->
// safety clip at +-1.0. Latency equals lookahead (5 ms) including the detector delay of 24
// samples, so the smoother is (lookahead - 24) samples long. Output is independent of the
// block partitioning (bit-exact). All memory is allocated in prepare().
#include <cstdint>
#include <vector>

#include "core/dsp/TruePeakDetector.h"

namespace bf {

struct LimiterStats {
  float grDbCurrent = 0.0f;   // gain reduction (dB, >= 0) at the end of the last block
  float grDbBlockMax = 0.0f;  // max gain reduction within the last block
  float grDbMax = 0.0f;       // max since reset
  std::uint64_t samples = 0;             // frames processed since reset
  std::uint64_t samplesAbove05Db = 0;    // frames with GR > 0.5 dB
  std::uint64_t clipEvents = 0;          // safety-clipped samples
};

class Limiter {
 public:
  static constexpr double kDefaultCeilingDb = -1.0;
  static constexpr double kLookaheadSeconds = 0.005;

  /// groupOfChannel: link group id per channel (equal ids share one gain). Empty = unlinked.
  void prepare(double fs, int numChannels, int maxBlock, const std::vector<int>& groupOfChannel = {});
  void reset() noexcept;

  void setCeilingDb(double dbTP) noexcept;  // clamped to [-6, -0.1]
  double ceilingDb() const noexcept { return ceilingDb_; }
  void setBypass(bool b) noexcept { bypass_ = b; }
  bool bypass() const noexcept { return bypass_; }

  int latencySamples() const noexcept { return delay_; }

  /// in/out: [ch][n]; in may alias out. Any n >= 0.
  void process(const float* const* in, float* const* out, int n) noexcept;

  const LimiterStats& stats() const noexcept { return stats_; }

 private:
  struct Group {
    std::vector<float> dqVal;
    std::vector<std::int64_t> dqIdx;
    int head = 0, count = 0;
    std::vector<double> ring1, ring2;
    int pos1 = 0, pos2 = 0;
    double sum1 = 0, sum2 = 0;
    int zero1 = 0, zero2 = 0;
    double env = 1.0;
    int hold = 0;
    bool releasing = false, fastRelease = false;
    std::int64_t eventSamples = 0;
    double eventMin = 1.0;
    void clear() noexcept;
  };

  double gainStep(Group& g, float required, std::int64_t idx) noexcept;

  double fs_ = 48000.0;
  int nCh_ = 0, maxBlock_ = 0;
  int delay_ = 0, window_ = 0, box1_ = 1, box2_ = 1, holdSamples_ = 0, fastEventSamples_ = 0;
  double relFast_ = 0, relSlow_ = 0;
  double ceilingDb_ = kDefaultCeilingDb;
  float ceilingLin_ = 0.891f;
  bool bypass_ = false;
  std::int64_t counter_ = 0;

  TruePeakDetector det_;
  std::vector<int> chGroup_;
  std::vector<Group> groups_;
  std::vector<float> groupReq_;
  std::vector<float> delayBuf_;  // [ch][delay_]
  std::vector<int> delayPos_;
  std::vector<float> peaks_;     // [ch][maxBlock]
  LimiterStats stats_;
};

}  // namespace bf
