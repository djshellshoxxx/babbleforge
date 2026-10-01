#include "core/dsp/Limiter.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "core/math/DetMath.h"

namespace bf {

namespace {
constexpr double kLn10 = 2.302585092994046;
inline double dbToLin(double db) noexcept { return detexp(db * (kLn10 / 20.0)); }
inline double linToDb(double lin) noexcept { return 20.0 / kLn10 * detlog(lin); }
constexpr double kFastGrThreshold = 0.7943282347242815;  // 2 dB
constexpr double kHalfDbGain = 0.9440608762859234;        // 0.5 dB
}  // namespace

void Limiter::Group::clear() noexcept {
  head = count = 0;
  std::fill(ring1.begin(), ring1.end(), 0.0);
  std::fill(ring2.begin(), ring2.end(), 0.0);
  pos1 = pos2 = 0;
  sum1 = sum2 = 0;
  zero1 = zero2 = 0;
  env = 1.0;
  hold = 0;
  releasing = fastRelease = false;
  eventSamples = 0;
  eventMin = 1.0;
}

void Limiter::prepare(double fs, int numChannels, int maxBlock, const std::vector<int>& groupOfChannel) {
  fs_ = fs;
  nCh_ = std::max(1, numChannels);
  maxBlock_ = std::max(1, maxBlock);
  det_.prepare(fs, nCh_);
  delay_ = std::max(TruePeakDetector::kLatencySamples + 3,
                    static_cast<int>(std::lround(kLookaheadSeconds * fs)));
  const int smooth = delay_ - TruePeakDetector::kLatencySamples;  // triangular length S
  window_ = smooth + 2;
  box1_ = (smooth + 2) / 2;
  box2_ = smooth + 1 - box1_;
  holdSamples_ = static_cast<int>(std::lround(0.010 * fs));
  fastEventSamples_ = static_cast<int>(std::lround(0.050 * fs));
  relFast_ = 1.0 - detexp(-1.0 / (0.080 * fs));
  relSlow_ = 1.0 - detexp(-1.0 / (0.600 * fs));

  chGroup_.assign(static_cast<size_t>(nCh_), 0);
  std::map<int, int> dense;
  for (int c = 0; c < nCh_; ++c) {
    int id = (static_cast<size_t>(c) < groupOfChannel.size()) ? groupOfChannel[static_cast<size_t>(c)]
                                                              : -1 - c;
    if (groupOfChannel.empty()) id = -1 - c;
    auto it = dense.find(id);
    if (it == dense.end()) it = dense.emplace(id, static_cast<int>(dense.size())).first;
    chGroup_[static_cast<size_t>(c)] = it->second;
  }
  groups_.assign(dense.size(), Group{});
  for (auto& g : groups_) {
    g.dqVal.assign(static_cast<size_t>(window_ + 2), 1.0f);
    g.dqIdx.assign(static_cast<size_t>(window_ + 2), 0);
    g.ring1.assign(static_cast<size_t>(box1_), 0.0);
    g.ring2.assign(static_cast<size_t>(box2_), 0.0);
  }
  groupReq_.assign(groups_.size(), 1.0f);
  delayBuf_.assign(static_cast<size_t>(nCh_) * static_cast<size_t>(delay_), 0.0f);
  delayPos_.assign(static_cast<size_t>(nCh_), 0);
  peaks_.assign(static_cast<size_t>(nCh_) * static_cast<size_t>(maxBlock_), 0.0f);
  setCeilingDb(ceilingDb_);
  reset();
}

void Limiter::reset() noexcept {
  det_.reset();
  for (auto& g : groups_) g.clear();
  std::fill(delayBuf_.begin(), delayBuf_.end(), 0.0f);
  std::fill(delayPos_.begin(), delayPos_.end(), 0);
  counter_ = 0;
  stats_ = LimiterStats{};
}

void Limiter::setCeilingDb(double dbTP) noexcept {
  ceilingDb_ = std::min(-0.1, std::max(-6.0, dbTP));
  ceilingLin_ = static_cast<float>(dbToLin(ceilingDb_));
}

double Limiter::gainStep(Group& g, float required, std::int64_t idx) noexcept {
  // Monotonic deque: sliding minimum over the last window_ samples.
  const int cap = static_cast<int>(g.dqVal.size());
  while (g.count > 0) {
    const int back = (g.head + g.count - 1) % cap;
    if (g.dqVal[static_cast<size_t>(back)] >= required) --g.count;
    else break;
  }
  {
    const int slot = (g.head + g.count) % cap;
    g.dqVal[static_cast<size_t>(slot)] = required;
    g.dqIdx[static_cast<size_t>(slot)] = idx;
    ++g.count;
  }
  while (g.dqIdx[static_cast<size_t>(g.head)] <= idx - window_) {
    g.head = (g.head + 1) % cap;
    --g.count;
  }
  const double m = static_cast<double>(g.dqVal[static_cast<size_t>(g.head)]);

  // Envelope: instant attack (smoother ramps it), held then two-stage release.
  if (m <= g.env) {
    g.env = m;
    g.hold = holdSamples_;
    g.releasing = false;
  } else if (g.hold > 0) {
    --g.hold;
  } else {
    if (!g.releasing) {
      g.releasing = true;
      g.fastRelease = (g.eventMin > kFastGrThreshold) && (g.eventSamples < fastEventSamples_);
    }
    g.env += (m - g.env) * (g.fastRelease ? relFast_ : relSlow_);
    if (m - g.env < 1e-9) g.env = m;
  }
  if (g.env < 1.0) {
    if (!g.releasing) ++g.eventSamples;
    g.eventMin = std::min(g.eventMin, g.env);
  } else {
    g.eventSamples = 0;
    g.eventMin = 1.0;
    g.releasing = false;
  }

  // Cascaded box filters on the deficit (1 - env); exact zero once the windows are empty.
  const double d = 1.0 - g.env;
  g.sum1 += d - g.ring1[static_cast<size_t>(g.pos1)];
  g.ring1[static_cast<size_t>(g.pos1)] = d;
  g.pos1 = (g.pos1 + 1 == box1_) ? 0 : g.pos1 + 1;
  g.zero1 = (d == 0.0) ? g.zero1 + 1 : 0;
  if (g.zero1 >= box1_) g.sum1 = 0.0;
  const double s1 = g.sum1 / double(box1_);
  g.sum2 += s1 - g.ring2[static_cast<size_t>(g.pos2)];
  g.ring2[static_cast<size_t>(g.pos2)] = s1;
  g.pos2 = (g.pos2 + 1 == box2_) ? 0 : g.pos2 + 1;
  g.zero2 = (s1 == 0.0) ? g.zero2 + 1 : 0;
  if (g.zero2 >= box2_) g.sum2 = 0.0;
  const double s2 = std::max(0.0, g.sum2 / double(box2_));
  return 1.0 - s2;
}

void Limiter::process(const float* const* in, float* const* out, int n) noexcept {
  int done = 0;
  const size_t nG = groups_.size();
  while (done < n) {
    const int len = std::min(n - done, maxBlock_);
    for (int c = 0; c < nCh_; ++c)
      det_.processAbove(c, in[c] + done, peaks_.data() + static_cast<size_t>(c) * static_cast<size_t>(maxBlock_), len,
                        ceilingLin_);  // only peaks above the ceiling are used below
    double blockMinGain = 1.0, lastGain = 1.0;
    std::uint64_t above = 0;
    for (int i = 0; i < len; ++i) {
      std::fill(groupReq_.begin(), groupReq_.end(), 1.0f);
      for (int c = 0; c < nCh_; ++c) {
        const float pk = peaks_[static_cast<size_t>(c) * static_cast<size_t>(maxBlock_) + static_cast<size_t>(i)];
        if (pk > ceilingLin_) {
          const float r = ceilingLin_ / pk;
          float& gr = groupReq_[static_cast<size_t>(chGroup_[static_cast<size_t>(c)])];
          gr = std::min(gr, r);
        }
      }
      // Per-group gain; single value per group for this sample.
      double minG = 1.0;
      for (size_t g = 0; g < nG; ++g) {
        const double gain = gainStep(groups_[g], groupReq_[g], counter_);
        groupReq_[g] = static_cast<float>(gain);  // reuse as storage of the gain (float below)
        minG = std::min(minG, gain);
      }
      ++counter_;
      // groupReq_ now holds float gains; apply in float for every channel.
      for (int c = 0; c < nCh_; ++c) {
        float* buf = delayBuf_.data() + static_cast<size_t>(c) * static_cast<size_t>(delay_);
        int& p = delayPos_[static_cast<size_t>(c)];
        const float delayed = buf[p];
        buf[p] = in[c][done + i];
        p = (p + 1 == delay_) ? 0 : p + 1;
        const float gn = bypass_ ? 1.0f : groupReq_[static_cast<size_t>(chGroup_[static_cast<size_t>(c)])];
        float y = delayed * gn;
        if (y > 1.0f) { y = 1.0f; ++stats_.clipEvents; }
        else if (y < -1.0f) { y = -1.0f; ++stats_.clipEvents; }
        out[c][done + i] = y;
      }
      if (bypass_) minG = 1.0;
      blockMinGain = std::min(blockMinGain, minG);
      lastGain = minG;
      if (minG < kHalfDbGain) ++above;
    }
    stats_.samples += static_cast<std::uint64_t>(len);
    stats_.samplesAbove05Db += above;
    stats_.grDbBlockMax = std::max(0.0f, static_cast<float>(-linToDb(std::max(blockMinGain, 1e-9))));
    stats_.grDbCurrent = std::max(0.0f, static_cast<float>(-linToDb(std::max(lastGain, 1e-9))));
    stats_.grDbMax = std::max(stats_.grDbMax, stats_.grDbBlockMax);
    done += len;
  }
}

}  // namespace bf
