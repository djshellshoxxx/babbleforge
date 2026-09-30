#include "core/corpus/ingest/Features.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <map>

#include "core/corpus/ingest/Signal.h"
#include "core/dsp/Fft.h"
#include "core/spectrum/SpectrumTarget.h"

namespace bf::ingest {

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1);
    const std::size_t i = static_cast<std::size_t>(pos);
    const double f = pos - static_cast<double>(i);
    return i + 1 < v.size() ? v[i] * (1.0 - f) + v[i + 1] * f : v[i];
}

// ---------------------------------------------------------------- P.56 method B
P56Result p56MethodB(const float* x, std::size_t n, double fs) {
    P56Result r;
    if (n == 0) return r;
    constexpr double kTau = 0.03, kHang = 0.2, kMargin = 15.9;
    const double g = std::exp(-1.0 / (kTau * fs));
    const long hangN = static_cast<long>(std::ceil(kHang * fs));
    double c[16];
    for (int j = 0; j < 16; ++j) c[j] = std::ldexp(1.0, j - 15);
    double a[16] = {};
    long h[16];
    for (auto& v : h) v = hangN;
    double sq = 0.0, p = 0.0, q = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = x[i];
        sq += v * v;
        p = g * p + (1.0 - g) * std::fabs(v);
        q = g * q + (1.0 - g) * p;
        for (int j = 0; j < 16; ++j) {
            if (q >= c[j]) {
                a[j] += 1.0;
                h[j] = 0;
            } else if (h[j] < hangN) {
                a[j] += 1.0;
                ++h[j];
            }
        }
    }
    if (sq <= 0.0) return r;
    const double meanSq = sq / static_cast<double>(n);
    r.rmsDb = 10.0 * std::log10(meanSq);
    double A[16], C[16], D[16];
    int last = -1;
    for (int j = 0; j < 16; ++j) {
        C[j] = 20.0 * std::log10(c[j]);
        if (a[j] > 0.0) {
            A[j] = 10.0 * std::log10(sq / a[j]);
            D[j] = A[j] - C[j];
            last = j;
        } else {
            A[j] = 0.0;
            D[j] = -1e9;
        }
    }
    if (last < 0) return r;
    int k = -1;
    for (int j = 0; j <= last; ++j)
        if (D[j] <= kMargin) { k = j; break; }
    double asl;
    if (k <= 0) {
        asl = A[k < 0 ? last : 0];
    } else {
        const double t = (D[k - 1] - kMargin) / (D[k - 1] - D[k]);
        asl = A[k - 1] + t * (A[k] - A[k - 1]);
    }
    r.aslDb = asl;
    r.activityPct = std::min(100.0, 100.0 * meanSq / std::pow(10.0, asl / 10.0));
    r.valid = true;
    return r;
}

// ---------------------------------------------------------------- clipping
ClipResult detectClipping(const float* x, std::size_t n, std::size_t stride, double fs) {
    ClipResult res;
    if (n == 0) return res;
    double peak = 0.0;
    for (std::size_t i = 0; i < n; ++i) peak = std::max(peak, static_cast<double>(std::fabs(x[i * stride])));
    if (peak <= 0.0) return res;
    const double thr = peak * std::pow(10.0, -0.01 / 20.0);
    std::vector<std::uint8_t> mark(n, 0);
    std::size_t run = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double a = std::fabs(x[i * stride]);
        if (a >= 0.999) mark[i] = 1;
        if (a >= thr) {
            // Flat top: consecutive samples must also agree to ~1 LSB at 16 bit (2^-17, see Features.h), so a smooth
            // low-frequency sine near its peak is not flagged.
            const bool flat = run > 0 && std::fabs(static_cast<double>(x[i * stride]) - static_cast<double>(x[(i - 1) * stride])) <= 0x1p-17;
            run = (run == 0 || flat) ? run + 1 : 1;
            if (run == 3) mark[i - 2] = mark[i - 1] = 1;
            if (run >= 3) mark[i] = 1;
        } else {
            run = 0;
        }
    }
    std::size_t count = 0;
    const std::size_t mergeGap = static_cast<std::size_t>(fs * 0.001);
    std::size_t runStart = 0, runEnd = 0;
    bool open = false;
    double longest = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!mark[i]) continue;
        ++count;
        if (open && i - runEnd < mergeGap) {
            runEnd = i;
        } else {
            if (open) longest = std::max(longest, static_cast<double>(runEnd - runStart + 1));
            runStart = runEnd = i;
            open = true;
        }
    }
    if (open) longest = std::max(longest, static_cast<double>(runEnd - runStart + 1));
    res.ratio = static_cast<double>(count) / static_cast<double>(n);
    res.longestRunMs = 1000.0 * longest / fs;
    return res;
}

// ---------------------------------------------------------------- LTASS
LtassResult analyzeLtass(const std::vector<float>& x, const std::vector<SampleSpan>& regions) {
    constexpr std::size_t N = 4096, H = 2048;
    constexpr double fs = 48000.0;
    LtassResult res;
    if (x.size() < N || regions.empty()) return res;
    static const std::vector<double> win = [] {
        std::vector<double> w(N);
        for (std::size_t i = 0; i < N; ++i) w[i] = 0.5 - 0.5 * std::cos(2.0 * std::acos(-1.0) * static_cast<double>(i) / static_cast<double>(N));
        return w;
    }();
    FftF fft(N);
    std::vector<float> buf(N);
    std::vector<std::complex<float>> spec(fft.numBins());
    std::vector<double> psd(fft.numBins(), 0.0);
    std::size_t ri = 0;
    for (std::size_t t = 0; t + N <= x.size(); t += H) {
        const auto centre = static_cast<std::int64_t>(t + H);
        while (ri < regions.size() && regions[ri].end <= centre) ++ri;
        if (ri >= regions.size()) break;
        if (centre < regions[ri].start) continue;
        for (std::size_t i = 0; i < N; ++i) buf[i] = static_cast<float>(x[t + i] * win[i]);
        fft.forward(buf.data(), spec.data());
        for (std::size_t k = 0; k < psd.size(); ++k) psd[k] += static_cast<double>(std::norm(spec[k]));
        ++res.frames;
    }
    if (res.frames == 0) return res;
    const double df = fs / static_cast<double>(N);
    // Third-octave band powers with fractional bin overlap.
    double total = 0.0;
    for (std::size_t b = 0; b < kNumLtassBands; ++b) {
        const double lo = thirdOctLowerEdgeHz(b), hi = thirdOctUpperEdgeHz(b);
        double p = 0.0;
        const auto k0 = static_cast<std::size_t>(std::max(0.0, std::floor(lo / df - 0.5)));
        for (std::size_t k = k0; k < psd.size(); ++k) {
            const double kl = (static_cast<double>(k) - 0.5) * df, kh = (static_cast<double>(k) + 0.5) * df;
            if (kl >= hi) break;
            const double ov = std::min(hi, kh) - std::max(lo, kl);
            if (ov > 0.0) p += psd[k] * ov / df;
        }
        res.bandPower[b] = p;
        total += p;
    }
    if (total <= 0.0) return res;
    for (std::size_t b = 0; b < kNumLtassBands; ++b) {
        res.bandPower[b] /= total;
        res.bandDb[b] = static_cast<float>(10.0 * std::log10(std::max(res.bandPower[b], 1e-12)));
    }
    // Centroid (>= 50 Hz).
    double num = 0.0, den = 0.0;
    for (std::size_t k = 1; k < psd.size(); ++k) {
        const double f = static_cast<double>(k) * df;
        if (f < 50.0) continue;
        num += f * psd[k];
        den += psd[k];
    }
    res.centroidHz = den > 0.0 ? num / den : 0.0;
    // Effective bandwidth: highest frequency at which the 1/3-octave band level (PSD smoothed over
    // a 1/3-octave window x f, i.e. band power rather than density) is within 30 dB of the peak
    // band level of the LTASS.
    std::vector<double> pre(psd.size() + 1, 0.0);
    for (std::size_t k = 0; k < psd.size(); ++k) pre[k + 1] = pre[k] + psd[k];
    std::vector<double> lev(psd.size(), 0.0);
    const double w = std::pow(2.0, 1.0 / 6.0);
    double peak = 0.0;
    for (std::size_t k = 1; k < psd.size(); ++k) {
        const double f = static_cast<double>(k) * df;
        const auto a = static_cast<std::size_t>(std::max(1.0, std::floor(f / w / df)));
        const auto bIdx = std::min(psd.size() - 1, static_cast<std::size_t>(std::ceil(f * w / df)));
        lev[k] = (pre[bIdx + 1] - pre[a]) / static_cast<double>(bIdx - a + 1) * f;
        if (f >= 100.0) peak = std::max(peak, lev[k]);
    }
    const double thr = peak * 1e-3;
    double bw = 0.0;
    for (std::size_t k = psd.size() - 1; k >= 1; --k) {
        if (lev[k] >= thr && static_cast<double>(k) * df >= 100.0) { bw = static_cast<double>(k) * df; break; }
    }
    res.bandwidthHz = bw;
    res.valid = true;
    return res;
}

// ---------------------------------------------------------------- YIN
namespace {
double medianOf(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}
}  // namespace

F0Stats yinF0(const std::vector<float>& x, const std::vector<std::uint8_t>& speech10) {
    constexpr std::size_t W = 640, TAUMAX = 272, NFFT = 1024, HOP = 160;
    constexpr double fs = 16000.0, kThresh = 0.15;
    F0Stats st;
    const std::size_t nFrames = speech10.size();
    st.track.assign(nFrames, 0.0f);
    FftD fft(NFFT);
    std::vector<double> a(NFFT), b(NFFT), d(TAUMAX + 1), cm(TAUMAX + 1), cumSq(W + TAUMAX + 1);
    std::vector<std::complex<double>> A(fft.numBins()), B(fft.numBins());
    const auto sampleAt = [&](std::int64_t i) -> double {
        return (i >= 0 && i < static_cast<std::int64_t>(x.size())) ? static_cast<double>(x[static_cast<std::size_t>(i)]) : 0.0;
    };
    for (std::size_t t = 0; t < nFrames; ++t) {
        if (!speech10[t]) continue;
        const std::int64_t start = static_cast<std::int64_t>(t * HOP) - static_cast<std::int64_t>(W / 2);
        std::fill(a.begin(), a.end(), 0.0);
        std::fill(b.begin(), b.end(), 0.0);
        double ea = 0.0;
        cumSq[0] = 0.0;
        for (std::size_t i = 0; i < W + TAUMAX; ++i) {
            const double v = sampleAt(start + static_cast<std::int64_t>(i));
            b[i] = v;
            if (i < W) { a[i] = v; ea += v * v; }
            cumSq[i + 1] = cumSq[i] + v * v;
        }
        if (ea < 1e-9) continue;
        fft.forward(a.data(), A.data());
        fft.forward(b.data(), B.data());
        for (std::size_t k = 0; k < A.size(); ++k) A[k] = std::conj(A[k]) * B[k];
        fft.inverse(A.data(), a.data());  // a[tau] = sum_j a_j b_{j+tau}
        d[0] = 0.0;
        for (std::size_t tau = 1; tau <= TAUMAX; ++tau)
            d[tau] = std::max(0.0, ea + (cumSq[tau + W] - cumSq[tau]) - 2.0 * a[tau]);
        cm[0] = 1.0;
        double run = 0.0;
        for (std::size_t tau = 1; tau <= TAUMAX; ++tau) {
            run += d[tau];
            cm[tau] = run > 0.0 ? d[tau] * static_cast<double>(tau) / run : 1.0;
        }
        const std::size_t tauMin = static_cast<std::size_t>(fs / 400.0), tauHi = static_cast<std::size_t>(fs / 60.0);
        std::size_t best = 0;
        for (std::size_t tau = tauMin; tau <= tauHi; ++tau) {
            if (cm[tau] < kThresh) {
                while (tau + 1 <= tauHi && cm[tau + 1] < cm[tau]) ++tau;
                best = tau;
                break;
            }
        }
        if (best == 0) continue;  // aperiodic -> unvoiced
        double tauF = static_cast<double>(best);
        if (best > 1 && best + 1 <= TAUMAX) {
            const double s0 = cm[best - 1], s1 = cm[best], s2 = cm[best + 1];
            const double den = s0 - 2.0 * s1 + s2;
            if (std::fabs(den) > 1e-12) tauF += 0.5 * (s0 - s2) / den;
        }
        st.track[t] = static_cast<float>(fs / tauF);
    }
    // Octave-error cleanup: 5-frame median over voiced neighbours, then 1 s running median.
    std::vector<float> med(st.track);
    std::vector<double> tmp;
    for (std::size_t t = 0; t < nFrames; ++t) {
        if (st.track[t] <= 0.0f) continue;
        tmp.clear();
        for (std::size_t k = (t >= 2 ? t - 2 : 0); k <= std::min(nFrames - 1, t + 2); ++k)
            if (st.track[k] > 0.0f) tmp.push_back(st.track[k]);
        med[t] = static_cast<float>(medianOf(tmp));
    }
    std::vector<float> clean(med);
    for (std::size_t t = 0; t < nFrames; ++t) {
        if (med[t] <= 0.0f) continue;
        tmp.clear();
        for (std::size_t k = (t >= 50 ? t - 50 : 0); k <= std::min(nFrames - 1, t + 50); ++k)
            if (med[k] > 0.0f) tmp.push_back(med[k]);
        const double m = medianOf(tmp);
        if (med[t] > 2.0 * m || med[t] < 0.5 * m) clean[t] = 0.0f;
    }
    st.track = clean;
    std::vector<double> v;
    std::size_t nSpeech = 0;
    for (std::size_t t = 0; t < nFrames; ++t) {
        if (speech10[t]) ++nSpeech;
        if (st.track[t] > 0.0f) v.push_back(st.track[t]);
    }
    st.voicedRatio = nSpeech ? static_cast<double>(v.size()) / static_cast<double>(nSpeech) : 0.0;
    if (v.empty()) return st;
    double sum = 0.0;
    for (double f : v) sum += f;
    st.meanHz = sum / static_cast<double>(v.size());
    st.medianHz = percentile(v, 0.5);
    st.p5Hz = percentile(v, 0.05);
    st.p95Hz = percentile(v, 0.95);
    st.rangeSt = st.p5Hz > 0 ? 12.0 * std::log2(st.p95Hz / st.p5Hz) : 0.0;
    std::array<double, kF0HistBins> h{};
    for (double f : v) {
        const double s = 12.0 * std::log2(f / 100.0);
        const int bin = std::clamp(static_cast<int>(std::floor((s + 8.0) / 2.0)), 0, kF0HistBins - 1);
        h[static_cast<std::size_t>(bin)] += 1.0;
    }
    for (std::size_t i = 0; i < h.size(); ++i) st.hist[i] = static_cast<float>(h[i] / static_cast<double>(v.size()));
    return st;
}

// ---------------------------------------------------------------- speaking rate
double speakingRate(const std::vector<float>& x, const std::vector<std::uint8_t>& speech10,
                    const std::vector<float>& f0) {
    const std::size_t nFrames = speech10.size();
    if (nFrames < 3) return 0.0;
    std::vector<double> inten(nFrames, -120.0);
    constexpr std::int64_t half = 256;
    for (std::size_t t = 0; t < nFrames; ++t) {
        const std::int64_t c = static_cast<std::int64_t>(t) * 160;
        double e = 0.0;
        for (std::int64_t i = -half; i < half; ++i) {
            const std::int64_t k = c + i;
            if (k < 0 || k >= static_cast<std::int64_t>(x.size())) continue;
            const double w = 0.5 + 0.5 * std::cos(std::acos(-1.0) * static_cast<double>(i) / static_cast<double>(half));
            e += w * w * static_cast<double>(x[static_cast<std::size_t>(k)]) * x[static_cast<std::size_t>(k)];
        }
        inten[t] = 10.0 * std::log10(e / static_cast<double>(2 * half) + 1e-12);
    }
    std::vector<double> sp;
    std::size_t nSpeech = 0;
    for (std::size_t t = 0; t < nFrames; ++t)
        if (speech10[t]) { sp.push_back(inten[t]); ++nSpeech; }
    if (nSpeech < 10) return 0.0;
    const double thr = percentile(sp, 0.99) - 25.0;
    long peak = -1;
    std::size_t count = 0;
    for (std::size_t t = 1; t + 1 < nFrames; ++t) {
        if (!(inten[t] > inten[t - 1] && inten[t] >= inten[t + 1])) continue;
        if (!speech10[t] || t >= f0.size() || f0[t] <= 0.0f || inten[t] < thr) continue;
        if (peak < 0) { peak = static_cast<long>(t); ++count; continue; }
        const auto p = static_cast<std::size_t>(peak);
        double dip = inten[t];
        for (std::size_t k = p; k <= t; ++k) dip = std::min(dip, inten[k]);
        if (t - p >= 10) {
            if (inten[t] - dip >= 2.0) { peak = static_cast<long>(t); ++count; }
        } else if (inten[t] > inten[p]) {
            peak = static_cast<long>(t);  // same nucleus, move to the higher peak
        }
    }
    return static_cast<double>(count) / (static_cast<double>(nSpeech) / 100.0);
}

// ---------------------------------------------------------------- fingerprint
std::vector<std::uint32_t> computeFingerprint(const std::vector<float>& x16k) {
    constexpr double fsFp = 5512.5;
    constexpr std::size_t N = 2048, HOP = 64, NB = 33;
    const auto y = resample(x16k.data(), x16k.size(), 16000.0, fsFp);
    std::vector<std::uint32_t> out;
    if (y.size() < N + HOP) return out;
    static const std::vector<float> win = [] {
        std::vector<float> w(N);
        for (std::size_t i = 0; i < N; ++i) w[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::acos(-1.0) * static_cast<double>(i) / static_cast<double>(N)));
        return w;
    }();
    std::array<std::size_t, NB + 1> edge{};
    const double binHz = fsFp / static_cast<double>(N);
    for (std::size_t b = 0; b <= NB; ++b)
        edge[b] = static_cast<std::size_t>(std::lround(300.0 * std::pow(2000.0 / 300.0, static_cast<double>(b) / static_cast<double>(NB)) / binHz));
    for (std::size_t b = 1; b <= NB; ++b) edge[b] = std::max(edge[b], edge[b - 1] + 1);
    FftF fft(N);
    std::vector<float> buf(N);
    std::vector<std::complex<float>> spec(fft.numBins());
    std::array<double, NB> prev{}, cur{};
    bool havePrev = false;
    for (std::size_t s = 0; s + N <= y.size(); s += HOP) {
        for (std::size_t i = 0; i < N; ++i) buf[i] = y[s + i] * win[i];
        fft.forward(buf.data(), spec.data());
        double tot = 0.0;
        for (std::size_t b = 0; b < NB; ++b) {
            double e = 0.0;
            for (std::size_t k = edge[b]; k < edge[b + 1]; ++k) e += static_cast<double>(std::norm(spec[k]));
            cur[b] = e;
            tot += e;
        }
        if (havePrev) {
            std::uint32_t v = 0;
            if (tot > 0.0)
                for (std::size_t b = 0; b < 32; ++b)
                    if ((cur[b] - cur[b + 1]) - (prev[b] - prev[b + 1]) > 0.0) v |= 1u << b;
            out.push_back(v);
        }
        prev = cur;
        havePrev = true;
    }
    return out;
}

void FingerprintIndex::add(std::uint32_t recIdx, const std::vector<std::uint32_t>& fp) {
    const auto slot = static_cast<std::uint32_t>(fps_.size());
    fps_.push_back(fp);
    ids_.push_back(recIdx);
    for (std::size_t i = 0; i < fp.size(); i += 4)
        if (fp[i] != 0) index_.emplace(fp[i], std::make_pair(slot, static_cast<std::uint32_t>(i)));
}

FingerprintMatch FingerprintIndex::query(const std::vector<std::uint32_t>& q, std::uint32_t* recIdx) const {
    FingerprintMatch best;
    std::map<std::pair<std::uint32_t, std::int64_t>, int> votes;
    for (std::size_t i = 0; i < q.size(); ++i) {
        if (q[i] == 0) continue;
        const auto range = index_.equal_range(q[i]);
        for (auto it = range.first; it != range.second; ++it)
            ++votes[{it->second.first, static_cast<std::int64_t>(it->second.second) - static_cast<std::int64_t>(i)}];
    }
    std::vector<std::pair<int, std::pair<std::uint32_t, std::int64_t>>> cand;
    for (const auto& [k, v] : votes)
        if (v >= 2) cand.push_back({v, k});
    std::sort(cand.begin(), cand.end(), [](const auto& l, const auto& r) { return l.first > r.first; });
    if (cand.size() > 8) cand.resize(8);
    for (const auto& [v, key] : cand) {
        const auto& c = fps_[key.first];
        const std::int64_t off = key.second;
        const std::int64_t i0 = std::max<std::int64_t>(0, -off);
        const std::int64_t i1 = std::min<std::int64_t>(static_cast<std::int64_t>(q.size()), static_cast<std::int64_t>(c.size()) - off);
        std::int64_t cnt = 0, bits = 0;
        for (std::int64_t i = i0; i < i1; ++i) {
            const std::uint32_t a = q[static_cast<std::size_t>(i)], b = c[static_cast<std::size_t>(i + off)];
            if (a == 0 || b == 0) continue;
            ++cnt;
            bits += std::popcount(a ^ b);
        }
        if (cnt < kNearDupMinFrames) continue;
        const double ber = static_cast<double>(bits) / (32.0 * static_cast<double>(cnt));
        if (ber < kNearDupBer && (!best.found || ber < best.ber)) {
            best.found = true;
            best.ber = ber;
            best.offset = off;
            best.overlap = cnt;
            best.frames = i1 - i0;
            if (recIdx) *recIdx = ids_[key.first];
        }
    }
    return best;
}

}  // namespace bf::ingest
