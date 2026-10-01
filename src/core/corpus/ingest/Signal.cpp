#include "core/corpus/ingest/Signal.h"

extern "C" {
#include <fvad.h>
}

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>

#include "CDSPResampler.h"
#include "core/dsp/Fft.h"

namespace bf::ingest {

std::vector<float> resample(const float* x, std::size_t n, double srcRate, double dstRate) {
    if (srcRate == dstRate) return std::vector<float>(x, x + n);
    const std::size_t outLen = static_cast<std::size_t>(std::llround(static_cast<double>(n) * dstRate / srcRate));
    std::vector<float> out;
    out.reserve(outLen);
    constexpr int kBlock = 1 << 15;
    r8b::CDSPResampler24 rs(srcRate, dstRate, kBlock, 10.0);
    std::vector<double> in(kBlock);
    std::size_t pos = 0;
    std::size_t zeros = 0;
    while (out.size() < outLen) {
        int len;
        if (pos < n) {
            len = static_cast<int>(std::min<std::size_t>(kBlock, n - pos));
            for (int i = 0; i < len; ++i) in[static_cast<std::size_t>(i)] = x[pos + static_cast<std::size_t>(i)];
            pos += static_cast<std::size_t>(len);
        } else {
            len = kBlock;  // flush with zeros
            std::fill(in.begin(), in.end(), 0.0);
            zeros += static_cast<std::size_t>(len);
            if (zeros > static_cast<std::size_t>(kBlock) * 64) break;
        }
        double* op = nullptr;
        const int got = rs.process(in.data(), len, op);
        for (int i = 0; i < got && out.size() < outLen; ++i) out.push_back(static_cast<float>(op[i]));
    }
    out.resize(outLen, 0.0f);
    return out;
}

namespace {
struct Biquad {
    double b0, b1, b2, a1, a2;
    void run(std::vector<double>& v, bool reverse) const {
        const std::size_t n = v.size();
        if (n == 0) return;
        const double x0 = reverse ? v[n - 1] : v[0];
        // Steady-state initial conditions for a constant input x0 (high-pass: output 0).
        double z2 = b2 * x0;
        double z1 = b1 * x0 + z2;
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t i = reverse ? n - 1 - k : k;
            const double x = v[i];
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            v[i] = y;
        }
    }
};
}  // namespace

void removeDcZeroPhase(std::vector<float>& x, double fs, double fc) {
    // 2nd-order Butterworth high-pass via bilinear transform.
    const double pi = std::acos(-1.0);
    const double k = std::tan(pi * fc / fs);
    const double q = std::sqrt(0.5);
    const double norm = 1.0 / (1.0 + k / q + k * k);
    Biquad bq{norm, -2.0 * norm, norm, 2.0 * (k * k - 1.0) * norm, (1.0 - k / q + k * k) * norm};
    std::vector<double> v(x.begin(), x.end());
    bq.run(v, false);
    bq.run(v, true);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(v[i]);
}

std::vector<std::int16_t> toPcm16(const std::vector<float>& x) {
    std::vector<std::int16_t> o(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double v = std::nearbyint(static_cast<double>(x[i]) * 32768.0);
        o[i] = static_cast<std::int16_t>(std::clamp(v, -32768.0, 32767.0));
    }
    return o;
}

std::vector<SampleSpan> postProcessVadFlags(const std::vector<std::uint8_t>& flags) {
    const std::int64_t n = static_cast<std::int64_t>(flags.size());
    std::vector<SampleSpan> runs;
    for (std::int64_t i = 0; i < n;) {
        if (!flags[static_cast<std::size_t>(i)]) { ++i; continue; }
        std::int64_t j = i;
        while (j < n && flags[static_cast<std::size_t>(j)]) ++j;
        // 2. hangover: 150 ms after, 50 ms before.
        runs.push_back({std::max<std::int64_t>(0, i - 5), std::min(n, j + 15)});
        i = j;
    }
    // 3. merge gaps < 80 ms.
    std::vector<SampleSpan> merged;
    for (const auto& r : runs) {
        if (!merged.empty() && r.start - merged.back().end < 8) merged.back().end = std::max(merged.back().end, r.end);
        else merged.push_back(r);
    }
    // 4. drop short (< 200 ms) regions isolated by >= 500 ms pauses on both sides.
    std::vector<SampleSpan> out;
    for (std::size_t i = 0; i < merged.size(); ++i) {
        const auto& r = merged[i];
        if (r.length() < 20) {
            const std::int64_t before = i == 0 ? INT64_MAX : r.start - merged[i - 1].end;
            const std::int64_t after = i + 1 == merged.size() ? INT64_MAX : merged[i + 1].start - r.end;
            if (before >= 50 && after >= 50) continue;
        }
        out.push_back(r);
    }
    return out;
}

std::vector<std::uint8_t> vadFlagsWebRtc(const std::vector<std::int16_t>& pcm, int mode, bool* ok) {
    const std::size_t nFrames10 = (pcm.size() + 159) / 160;
    std::vector<std::uint8_t> flags(nFrames10, 0);
    std::unique_ptr<Fvad, decltype(&fvad_free)> v(fvad_new(), &fvad_free);
    if (!v || fvad_set_mode(v.get(), mode) != 0 || fvad_set_sample_rate(v.get(), 16000) != 0) {
        if (ok) *ok = false;
        return flags;
    }
    if (ok) *ok = true;
    std::vector<std::int16_t> frame(480);
    for (std::size_t f = 0; f * 480 < pcm.size(); ++f) {
        const std::size_t a = f * 480;
        const std::size_t len = std::min<std::size_t>(480, pcm.size() - a);
        std::fill(frame.begin(), frame.end(), std::int16_t{0});
        std::copy(pcm.begin() + static_cast<std::ptrdiff_t>(a), pcm.begin() + static_cast<std::ptrdiff_t>(a + len), frame.begin());
        const int r = fvad_process(v.get(), frame.data(), 480);
        if (r == 1)
            for (std::size_t k = 0; k < 3 && f * 3 + k < flags.size(); ++k) flags[f * 3 + k] = 1;
    }
    return flags;
}

std::vector<std::uint8_t> vadFlagsEnergy(const std::vector<float>& x) {
    constexpr std::size_t kHop = 480;  // 10 ms at 48 kHz
    const std::size_t nf = (x.size() + kHop - 1) / kHop;
    std::vector<double> eDb(nf, -120.0), flat(nf, 1.0);
    FftF fft(512);
    std::vector<float> buf(512);
    std::vector<std::complex<float>> spec(fft.numBins());
    for (std::size_t f = 0; f < nf; ++f) {
        const std::size_t a = f * kHop;
        const std::size_t len = std::min<std::size_t>(kHop, x.size() - a);
        double e = 0;
        std::fill(buf.begin(), buf.end(), 0.0f);
        for (std::size_t i = 0; i < len; ++i) {
            e += static_cast<double>(x[a + i]) * x[a + i];
            const double w = 0.5 - 0.5 * std::cos(2.0 * std::acos(-1.0) * (static_cast<double>(i) + 0.5) / static_cast<double>(kHop));
            buf[i] = static_cast<float>(x[a + i] * w);
        }
        eDb[f] = 10.0 * std::log10(std::max(e / static_cast<double>(kHop), 1e-12));
        fft.forward(buf.data(), spec.data());
        // Flatness over 200 Hz - 4 kHz (bin width 93.75 Hz).
        double lsum = 0, sum = 0;
        int cnt = 0;
        for (std::size_t k = 2; k <= 42; ++k) {
            const double p = std::norm(spec[k]) + 1e-20;
            lsum += std::log(p);
            sum += p;
            ++cnt;
        }
        flat[f] = std::exp(lsum / cnt) / (sum / cnt);
    }
    std::vector<double> sorted(eDb);
    std::sort(sorted.begin(), sorted.end());
    const double floorDb = sorted.empty() ? -120.0 : sorted[sorted.size() / 10];
    std::vector<std::uint8_t> flags(nf, 0);
    for (std::size_t f = 0; f < nf; ++f) flags[f] = (eDb[f] > floorDb + 10.0 && flat[f] < 0.5) ? 1 : 0;
    return flags;
}

std::vector<SampleSpan> detectSpeech(const std::vector<float>& audio48k, const std::vector<std::int16_t>& pcm16k,
                                     std::int64_t length, const AnalyzerConfig& cfg, std::string* engineName) {
    std::vector<std::uint8_t> flags;
    bool ok = false;
    if (cfg.vad == VadEngine::WebRtc) flags = vadFlagsWebRtc(pcm16k, cfg.vadMode, &ok);
    if (ok) {
        if (engineName) *engineName = "webrtc-fvad-m" + std::to_string(cfg.vadMode);
    } else {
        flags = vadFlagsEnergy(audio48k);
        if (engineName) *engineName = "energy-flatness";
    }
    const auto r10 = postProcessVadFlags(flags);
    std::vector<SampleSpan> out;
    for (const auto& r : r10) {
        SampleSpan s{r.start * 480, std::min<std::int64_t>(r.end * 480, length)};
        if (s.end > s.start) out.push_back(s);
    }
    return out;
}

}  // namespace bf::ingest
