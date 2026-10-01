#include "core/engine/OutputMatrix.h"

#include <algorithm>
#include <cmath>

#include "core/math/DetMath.h"

namespace bf {

namespace {
double dbToLin(double db) { return detexp(db * (detlog(10.0) / 20.0)); }
}  // namespace

void OutputMatrix::prepare(double fs, int maxBlock, const std::vector<int>& zoneOfOutput,
                           const std::vector<int>& deviceChannelOfOutput, int numDeviceChannels) {
  fs_ = fs;
  gainRamp_ = std::llround(kGainRampSeconds * fs);
  muteRamp_ = std::llround(kMuteRampSeconds * fs);
  fadeRamp_ = std::llround(kFadeSeconds * fs);
  zoneEnableRamp_ = std::llround(kZoneEnableRampSeconds * fs);
  maxDelay_ = static_cast<int>(std::ceil(kMaxDelaySeconds * fs));
  maxBlock_ = std::max(maxBlock, 1);
  numDev_ = std::max(numDeviceChannels, 0);
  out_.assign(zoneOfOutput.size(), Out{});
  for (std::size_t i = 0; i < out_.size(); ++i) {
    auto& o = out_[i];
    o.zone = std::clamp(zoneOfOutput[i], 0, kMaxZones - 1);
    o.device = i < deviceChannelOfOutput.size() ? deviceChannelOfOutput[i] : -1;
    if (o.device >= numDev_) o.device = -1;
    o.line.assign(idx(maxDelay_) + 1, 0.0f);
  }
  for (auto& z : zone_) z = Zone{};
  zoneF_.assign(idx(kMaxZones) * idx(maxBlock_), 1.0);
  scratch_.assign(idx(maxBlock_), 0.0f);
}

void OutputMatrix::reset() noexcept {
  for (auto& o : out_) {
    std::fill(o.line.begin(), o.line.end(), 0.0f);
    o.pos = 0;
  }
  applyImmediately();
}

void OutputMatrix::setZoneGainDb(int zone, double db) noexcept {
  zone_[std::clamp(zone, 0, kMaxZones - 1)].gain.set(dbToLin(std::clamp(db, -24.0, 6.0)), gainRamp_);
}
void OutputMatrix::setZoneEnabled(int zone, bool enabled) noexcept {
  zone_[std::clamp(zone, 0, kMaxZones - 1)].enable.set(enabled ? 1.0 : 0.0, zoneEnableRamp_);
}
void OutputMatrix::setOutputGainDb(int o, double db) noexcept {
  out_[idx(o)].gain.set(dbToLin(std::clamp(db, -40.0, 6.0)), gainRamp_);
}
void OutputMatrix::updateMute(Out& o) noexcept {
  o.mute.set((o.muted || !o.enabled) ? 0.0 : 1.0, muteRamp_);
}
void OutputMatrix::setMute(int o, bool mute) noexcept {
  out_[idx(o)].muted = mute;
  updateMute(out_[idx(o)]);
}
void OutputMatrix::setOutputEnabled(int o, bool enabled) noexcept {
  out_[idx(o)].enabled = enabled;
  updateMute(out_[idx(o)]);
}

void OutputMatrix::requestFade(Out& o) noexcept {
  if (o.pendPol == o.pol && o.pendDelay == o.delay && o.state == Fade::Idle) return;
  if (o.state != Fade::Out) {
    o.state = Fade::Out;
    o.fade.set(0.0, fadeRamp_);
  }
}
void OutputMatrix::setPolarityInvert(int o, bool invert) noexcept {
  auto& s = out_[idx(o)];
  s.pendPol = invert ? -1.0f : 1.0f;
  requestFade(s);
}
void OutputMatrix::setDelaySamples(int o, int samples) noexcept {
  auto& s = out_[idx(o)];
  s.pendDelay = std::clamp(samples, 0, maxDelay_);
  requestFade(s);
}
void OutputMatrix::setDelayMs(int o, double ms) noexcept {
  setDelaySamples(o, static_cast<int>(std::lround(std::clamp(ms, 0.0, 1000.0 * kMaxDelaySeconds) * 1e-3 * fs_)));
}
void OutputMatrix::setCalEq(int o, ICalEq* eq) noexcept { out_[idx(o)].eq = eq; }

void OutputMatrix::applyImmediately() noexcept {
  for (auto& z : zone_) {
    z.gain.snap();
    z.enable.snap();
  }
  for (auto& o : out_) {
    o.gain.snap();
    o.mute.snap();
    o.pol = o.pendPol;
    o.delay = o.pendDelay;
    o.state = Fade::Idle;
    o.fade.set(1.0, 0);
  }
}

void OutputMatrix::process(const float* const* in, float* const* deviceOut, int n) noexcept {
  for (int off = 0; off < n; off += maxBlock_) processChunk(in, deviceOut, off, std::min(maxBlock_, n - off));
}

void OutputMatrix::processChunk(const float* const* in, float* const* deviceOut, int off, int n) noexcept {
  for (int d = 0; d < numDev_; ++d)
    if (n > 0) std::fill(deviceOut[d] + off, deviceOut[d] + off + n, 0.0f);
  for (int zi = 0; zi < kMaxZones; ++zi) {
    double* f = &zoneF_[idx(zi) * idx(maxBlock_)];
    auto& z = zone_[zi];
    for (int i = 0; i < n; ++i) f[i] = z.gain.next() * z.enable.next();
  }
  const std::size_t line = idx(maxDelay_) + 1;
  for (auto& o : out_) {
    const double* zf = &zoneF_[idx(o.zone) * idx(maxBlock_)];
    const float* x = in[&o - out_.data()] + off;
    float* y = scratch_.data();
    for (int i = 0; i < n; ++i) {
      const double g = zf[i] * o.gain.next() * o.mute.next();
      o.line[o.pos] = static_cast<float>(static_cast<double>(x[i]) * g);
      std::size_t rd = o.pos + line - idx(o.delay);  // in [pos + 1, pos + line]: one wrap at most
      if (rd >= line) rd -= line;
      float v = o.line[rd];
      if (++o.pos == line) o.pos = 0;
      const double f = o.fade.next();
      v = static_cast<float>(static_cast<double>(v) * f) * o.pol;
      if (o.state == Fade::Out && o.fade.rem == 0) {  // fully faded out: jump, then fade in
        o.pol = o.pendPol;
        o.delay = o.pendDelay;
        o.state = Fade::In;
        o.fade.set(1.0, fadeRamp_);
      } else if (o.state == Fade::In && o.fade.rem == 0) {
        o.state = Fade::Idle;
      }
      y[i] = v;
    }
    if (o.eq) o.eq->process(y, n);
    if (o.device >= 0)
      for (int i = 0; i < n; ++i) deviceOut[o.device][off + i] += y[i];
  }
}

}  // namespace bf
