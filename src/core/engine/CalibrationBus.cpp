#include "core/engine/CalibrationBus.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/engine/StationaryMaskEngine.h"
#include "core/spectrum/FirDesigner.h"

namespace bf {

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr std::size_t kChunk = 256;
constexpr std::size_t kMaxKernel = 16384;
constexpr double kToneRelDb = -12.0;  // coded tones relative to the burst level

double dbToLin(double db) noexcept { return std::pow(10.0, db / 20.0); }

// 0..1 raised-cosine ramp over `len` samples.
inline double raisedCos(std::int64_t k, std::int64_t len) noexcept {
    if (len <= 0 || k >= len) return 1.0;
    if (k <= 0) return 0.0;
    return 0.5 * (1.0 - std::cos(kPi * static_cast<double>(k) / static_cast<double>(len)));
}

ThirdOctArray defaultSpeechBands() {
    // Provisional universal LTASS (docs/SPECTRUM_ENGINE.md §2.1), 100 Hz..10 kHz, flat beyond.
    static const double t[21] = {-9, -5, -3, -1.5, 0, 0, 0, 0, -1.5, -3.5, -5, -6.5, -8, -9, -10, -11, -12, -13, -14, -15, -17};
    ThirdOctArray a{};
    for (std::size_t b = 0; b < kNumThirdOctBands; ++b) {
        if (b < kFirstOperatingBand) a[b] = t[0];
        else if (b > kLastOperatingBand) a[b] = t[20];
        else a[b] = t[b - kFirstOperatingBand];
    }
    return a;
}
}  // namespace

const char* toString(OutputSource s) noexcept {
    switch (s) {
        case OutputSource::Masker: return "MASKER";
        case OutputSource::Calibration: return "CALIBRATION";
        case OutputSource::Mute: return "MUTE";
    }
    return "?";
}

const char* toString(CalSignal s) noexcept {
    switch (s) {
        case CalSignal::Sine: return "sine";
        case CalSignal::PinkNoise: return "pink";
        case CalSignal::SpeechNoise: return "speech-shaped";
        case CalSignal::Sweep: return "sweep";
        case CalSignal::ChannelId: return "channel-id";
        case CalSignal::OctaveNoise: return "octave-band";
    }
    return "?";
}

CalibrationParams CalibrationParams::sine(double hz, double levelDbfs) {
    CalibrationParams p;
    p.signal = CalSignal::Sine;
    p.sineHz = hz;
    p.levelDbfs = levelDbfs;
    return p;
}

// ---- pure helpers ------------------------------------------------------------------------
double CalibrationBus::sweepInstantaneousHz(double f1, double f2, double T, double t) noexcept {
    const double L = T / std::log(f2 / f1);
    return f1 * std::exp(t / L);
}

double CalibrationBus::sweepPhaseRad(double f1, double f2, double T, double t) noexcept {
    const double L = T / std::log(f2 / f1);
    return 2.0 * kPi * f1 * L * (std::exp(t / L) - 1.0);
}

std::pair<double, double> CalibrationBus::channelIdTones(int output) noexcept {
    static const double a[4] = {400.0, 500.0, 630.0, 800.0};
    static const double b[8] = {1250.0, 1600.0, 2000.0, 2500.0, 3150.0, 4000.0, 5000.0, 6300.0};
    const int k = std::max(output, 0) % 32;
    return {a[k % 4], b[k / 4]};
}

std::vector<float> CalibrationBus::designPinkKernel(double fs) {
    ThirdOctArray flat{};
    flat.fill(0.0);
    FirDesignParams p;
    p.fs = fs;
    return designMinPhaseFir(flat, p).tapsFloat();
}

std::vector<float> CalibrationBus::designSpeechKernel(double fs, const std::optional<ThirdOctArray>& bandsDb) {
    FirDesignParams p;
    p.fs = fs;
    return designMinPhaseFir(bandsDb ? *bandsDb : defaultSpeechBands(), p).tapsFloat();
}

std::vector<float> CalibrationBus::designOctaveKernel(double fs, double fc, std::size_t taps) {
    const double lo = fc / std::sqrt(2.0);
    const double hi = std::min(fc * std::sqrt(2.0), 0.45 * fs);
    const double c = (static_cast<double>(taps) - 1.0) * 0.5;
    auto sinc = [](double z) { return std::abs(z) < 1e-12 ? 1.0 : std::sin(kPi * z) / (kPi * z); };
    std::vector<double> h(taps);
    double e = 0.0;
    for (std::size_t n = 0; n < taps; ++n) {
        const double x = static_cast<double>(n) - c;
        const double w = static_cast<double>(n) / static_cast<double>(taps - 1);
        const double win = 0.35875 - 0.48829 * std::cos(2 * kPi * w) + 0.14128 * std::cos(4 * kPi * w) -
                           0.01168 * std::cos(6 * kPi * w);
        h[n] = (2.0 * hi / fs * sinc(2.0 * hi * x / fs) - 2.0 * lo / fs * sinc(2.0 * lo * x / fs)) * win;
        e += h[n] * h[n];
    }
    const double g = 1.0 / std::sqrt(e);  // unit energy: white unit-variance in -> unit variance out
    std::vector<float> out(taps);
    for (std::size_t n = 0; n < taps; ++n) out[n] = static_cast<float>(h[n] * g);
    return out;
}

std::vector<float> makeSweepSignal(double fs, double f1, double f2, double T, double fadeS) {
    const auto N = static_cast<std::size_t>(std::llround(T * fs));
    const auto fadeN = static_cast<std::int64_t>(std::llround(fadeS * fs));
    std::vector<float> x(N);
    for (std::size_t n = 0; n < N; ++n) {
        const double t = static_cast<double>(n) / fs;
        const double env = raisedCos(static_cast<std::int64_t>(n), fadeN) *
                           raisedCos(static_cast<std::int64_t>(N - 1 - n), fadeN);
        x[n] = static_cast<float>(env * std::sin(CalibrationBus::sweepPhaseRad(f1, f2, T, t)));
    }
    return x;
}

std::vector<float> makeSweepInverseFilter(double fs, double f1, double f2, double T, double fadeS) {
    const std::vector<float> x = makeSweepSignal(fs, f1, f2, T, fadeS);
    const std::size_t N = x.size();
    const double L = T / std::log(f2 / f1);
    // |X||K| = fs^2 L / (4 f2) for the unit sweep with the exp(-t/L) weighting (stationary phase).
    const double scale = 4.0 * f2 / (fs * fs * L);
    std::vector<float> k(N);
    for (std::size_t n = 0; n < N; ++n)
        k[n] = static_cast<float>(static_cast<double>(x[N - 1 - n]) * std::exp(-static_cast<double>(n) / (fs * L)) * scale);
    return k;
}

// ---- job ---------------------------------------------------------------------------------
struct CalibrationBus::Job {
    CalibrationParams p;
    double fs = 48000.0;
    std::vector<int> outs;
    StationaryMaskEngine noise;
    std::vector<std::vector<float>> nbuf;
    std::vector<float*> nptr;
    double amp = 0.0;  // peak amplitude (sine / sweep), RMS linear (noise level handled in engine)
    std::int64_t t = 0, total = -1, maxN = 0, rampN = 0, stopN = 0, stopPos = 0;
    bool stopping = false, done = false;
    double phase = 0.0, inc = 0.0, ph1 = 0.0, ph2 = 0.0, inc1 = 0.0, inc2 = 0.0;
    std::int64_t burstN = 0, gapN = 0, slotN = 0, cycleN = 0, sweepN = 0, sweepFadeN = 0;
    double toneAmp = 0.0;

    void beginStop() noexcept {
        if (stopping) return;
        stopping = true;
        stopPos = 0;
    }

    void render(float* const* out, int n) noexcept {
        int off = 0;
        while (off < n && !done) {
            int m = std::min<int>(n - off, static_cast<int>(kChunk));
            if (!stopping) {
                if (t >= maxN) beginStop();
                else m = static_cast<int>(std::min<std::int64_t>(m, maxN - t));
            }
            renderChunk(out, off, m);
            off += m;
        }
    }

    void renderChunk(float* const* out, int off, int m) noexcept {
        switch (p.signal) {
            case CalSignal::Sine:
                for (int i = 0; i < m; ++i) {
                    const double s = amp * std::sin(phase) * raisedCos(t + i, rampN);
                    phase += inc;
                    if (phase >= 2.0 * kPi) phase -= 2.0 * kPi;
                    for (int o : outs) out[o][off + i] = static_cast<float>(s);
                }
                break;
            case CalSignal::Sweep:
                for (int i = 0; i < m; ++i) {
                    const std::int64_t k = t + i;
                    double s = 0.0;
                    if (k < sweepN) {
                        const double env = raisedCos(k, sweepFadeN) * raisedCos(sweepN - 1 - k, sweepFadeN);
                        s = amp * env * std::sin(sweepPhaseRad(p.sweepF1Hz, p.sweepF2Hz, p.sweepDurationS, static_cast<double>(k) / fs));
                    }
                    for (int o : outs) out[o][off + i] = static_cast<float>(s);
                }
                break;
            case CalSignal::PinkNoise:
            case CalSignal::SpeechNoise:
            case CalSignal::OctaveNoise:
                noise.process(nptr.data(), static_cast<std::size_t>(m));
                for (std::size_t k = 0; k < outs.size(); ++k)
                    for (int i = 0; i < m; ++i)
                        out[outs[k]][off + i] = nbuf[k][static_cast<std::size_t>(i)] * static_cast<float>(raisedCos(t + i, rampN));
                break;
            case CalSignal::ChannelId:
                noise.process(nptr.data(), static_cast<std::size_t>(m));
                for (int i = 0; i < m; ++i) {
                    std::int64_t k = t + i;
                    if (p.loop) k %= cycleN;
                    const auto slot = static_cast<std::size_t>(k / slotN);
                    const std::int64_t w = k % slotN;
                    if (slot >= outs.size() || w >= burstN) continue;
                    if (w == 0) {
                        ph1 = ph2 = 0.0;
                        const auto tones = channelIdTones(outs[slot]);
                        inc1 = 2.0 * kPi * tones.first / fs;
                        inc2 = 2.0 * kPi * tones.second / fs;
                    }
                    const double env = raisedCos(w, rampN) * raisedCos(burstN - 1 - w, rampN);
                    double s = static_cast<double>(nbuf[0][static_cast<std::size_t>(i)]);
                    if (p.codedTones) {
                        s += toneAmp * (std::sin(ph1) + std::sin(ph2));
                        ph1 += inc1;
                        ph2 += inc2;
                        if (ph1 >= 2.0 * kPi) ph1 -= 2.0 * kPi;
                        if (ph2 >= 2.0 * kPi) ph2 -= 2.0 * kPi;
                    }
                    out[outs[slot]][off + i] = static_cast<float>(s * env);
                }
                break;
        }
        if (stopping) {
            for (int i = 0; i < m; ++i) {
                const std::int64_t k = stopPos + i;
                const float g = k >= stopN ? 0.0f : static_cast<float>(0.5 * (1.0 + std::cos(kPi * static_cast<double>(k) / static_cast<double>(stopN))));
                for (int o : outs) out[o][off + i] *= g;
            }
            stopPos += m;
            if (stopPos >= stopN) done = true;
        }
        t += m;
        if (total >= 0 && t >= total) done = true;
    }
};

// ---- bus ---------------------------------------------------------------------------------
CalibrationBus::CalibrationBus() {
    for (auto& r : retired_) r.store(nullptr);
}

CalibrationBus::~CalibrationBus() {
    delete pending_.exchange(nullptr);
    delete job_;
    delete held_;
    for (auto& r : retired_) delete r.exchange(nullptr);
}

void CalibrationBus::prepare(double fs, int numOutputs) {
    delete pending_.exchange(nullptr);
    delete job_;
    job_ = nullptr;
    delete held_;
    held_ = nullptr;
    collectGarbage();
    fs_ = fs;
    nOut_ = std::max(numOutputs, 0);
    optr_.assign(static_cast<std::size_t>(nOut_), nullptr);
    active_.store(false);
    stopReq_.store(false);
    finished_.store(false);
    elapsed_.store(0);
}

void CalibrationBus::collectGarbage() noexcept {
    for (auto& r : retired_) delete r.exchange(nullptr, std::memory_order_acq_rel);
}

void CalibrationBus::retire(Job* j) noexcept {
    for (auto& r : retired_) {
        Job* expected = nullptr;
        if (r.compare_exchange_strong(expected, j, std::memory_order_acq_rel)) return;
    }
    held_ = j;  // ring full: keep until a slot frees up (never deleted on the RT thread)
}

bool CalibrationBus::start(const CalibrationParams& p, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    if (nOut_ <= 0 || fs_ <= 0.0) return fail("calibration bus not prepared");
    const bool sweep = p.signal == CalSignal::Sweep;
    const double level = sweep ? p.sweepPeakDbfs : p.levelDbfs;
    if (!std::isfinite(level)) return fail("level must be finite");
    if (level > kMaxLevelDbfs) return fail("level above -3 dBFS is not allowed");
    if (level < -100.0) return fail("level below -100 dBFS");
    if (level > kLoudThresholdDbfs && !p.confirmLoud) return fail("level above -12 dBFS requires confirmation");

    auto job = std::make_unique<Job>();
    job->p = p;
    job->fs = fs_;
    for (int o : p.outputs) {
        if (o < 0 || o >= nOut_) return fail("output index out of range");
        job->outs.push_back(o);
    }
    if (job->outs.empty())
        for (int o = 0; o < nOut_; ++o) job->outs.push_back(o);
    std::sort(job->outs.begin(), job->outs.end());
    job->outs.erase(std::unique(job->outs.begin(), job->outs.end()), job->outs.end());

    job->maxN = static_cast<std::int64_t>(std::llround(kMaxSeconds * fs_));
    job->stopN = std::max<std::int64_t>(1, std::llround(kStopFadeS * fs_));
    job->rampN = std::max<std::int64_t>(1, std::llround(kRampS * fs_));
    const double lin = dbToLin(level);
    int noiseCh = 0;
    std::vector<float> kernel;
    float noiseLevel = static_cast<float>(level);

    switch (p.signal) {
        case CalSignal::Sine:
            if (!(p.sineHz >= 20.0 && p.sineHz <= 20000.0 && p.sineHz < 0.45 * fs_)) return fail("sine frequency out of range");
            job->amp = lin * std::sqrt(2.0);
            job->inc = 2.0 * kPi * p.sineHz / fs_;
            break;
        case CalSignal::Sweep: {
            if (!(p.sweepF1Hz > 0.0 && p.sweepF2Hz > p.sweepF1Hz && p.sweepF2Hz <= 0.45 * fs_)) return fail("invalid sweep range");
            if (!(p.sweepDurationS >= 1.0 && p.sweepDurationS <= 30.0)) return fail("sweep duration must be 1..30 s");
            if (!(p.sweepFadeS >= 0.0 && p.sweepFadeS < 0.5 * p.sweepDurationS) || !(p.sweepTailS >= 0.0 && p.sweepTailS <= 10.0))
                return fail("invalid sweep fade/tail");
            job->amp = lin;
            job->sweepN = static_cast<std::int64_t>(std::llround(p.sweepDurationS * fs_));
            job->sweepFadeN = static_cast<std::int64_t>(std::llround(p.sweepFadeS * fs_));
            job->total = job->sweepN + static_cast<std::int64_t>(std::llround(p.sweepTailS * fs_));
            break;
        }
        case CalSignal::PinkNoise:
            noiseCh = static_cast<int>(job->outs.size());
            kernel = designPinkKernel(fs_);
            break;
        case CalSignal::SpeechNoise:
            noiseCh = static_cast<int>(job->outs.size());
            kernel = designSpeechKernel(fs_, p.speechBandsDb);
            break;
        case CalSignal::OctaveNoise: {
            const int b = p.octaveBandHz;
            if (!(b == 125 || b == 250 || b == 500 || b == 1000 || b == 2000 || b == 4000 || b == 8000))
                return fail("octave band must be 125..8000 Hz");
            noiseCh = static_cast<int>(job->outs.size());
            kernel = designOctaveKernel(fs_, b);
            break;
        }
        case CalSignal::ChannelId: {
            if (!(p.burstS >= 0.1 && p.burstS <= 10.0 && p.gapS >= 0.0 && p.gapS <= 10.0)) return fail("invalid burst/gap");
            noiseCh = 1;
            kernel = designPinkKernel(fs_);
            job->burstN = static_cast<std::int64_t>(std::llround(p.burstS * fs_));
            job->gapN = static_cast<std::int64_t>(std::llround(p.gapS * fs_));
            job->slotN = job->burstN + job->gapN;
            job->cycleN = job->slotN * static_cast<std::int64_t>(job->outs.size());
            if (!p.loop) job->total = job->cycleN;
            if (p.codedTones) {
                const double rel = dbToLin(kToneRelDb);
                // Pink burst + two tones keep the configured RMS level.
                noiseLevel = static_cast<float>(level + 10.0 * std::log10(1.0 - 2.0 * rel * rel));
                job->toneAmp = lin * rel * std::sqrt(2.0);
            }
            break;
        }
    }
    if (noiseCh > 0) {
        job->noise.prepare(fs_, static_cast<std::size_t>(noiseCh), p.seed, kMaxKernel, noiseLevel);
        if (!job->noise.setFilter(kernel, 0)) return fail("kernel design failed");
        job->nbuf.assign(static_cast<std::size_t>(noiseCh), std::vector<float>(kChunk, 0.0f));
        for (auto& b : job->nbuf) job->nptr.push_back(b.data());
        // Pre-roll the convolver latency so the first output sample is already signal.
        // (256 samples at <= 50 kHz, 512 above), in kChunk pieces (the size of nbuf).
        for (std::size_t left = PartitionedKernel::partitionForRate(fs_); left > 0;) {
            const std::size_t m = std::min(left, kChunk);
            job->noise.process(job->nptr.data(), m);
            left -= m;
        }
    }

    collectGarbage();
    finished_.store(false, std::memory_order_release);
    delete pending_.exchange(job.release(), std::memory_order_acq_rel);
    return true;
}

void CalibrationBus::stop() noexcept {
    delete pending_.exchange(nullptr, std::memory_order_acq_rel);
    stopReq_.store(true, std::memory_order_release);
}

void CalibrationBus::process(float* const* out, int n) noexcept {
    if (n <= 0) return;
    const auto un = static_cast<std::size_t>(n);
    for (int c = 0; c < nOut_; ++c) std::memset(out[c], 0, un * sizeof(float));

    if (held_) {
        Job* h = held_;
        held_ = nullptr;
        retire(h);
    }
    if (!held_) {
        if (Job* nj = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            if (job_) retire(job_);
            job_ = nj;
            active_.store(true, std::memory_order_release);
            stopReq_.store(false, std::memory_order_relaxed);
        }
    }
    if (!job_) {
        stopReq_.store(false, std::memory_order_relaxed);
        return;
    }
    if (stopReq_.exchange(false, std::memory_order_acq_rel)) job_->beginStop();
    job_->render(out, n);
    elapsed_.store(job_->t, std::memory_order_relaxed);
    if (job_->done) {
        retire(job_);
        job_ = nullptr;
        active_.store(false, std::memory_order_release);
        finished_.store(true, std::memory_order_release);
    }
}

// ---- selector ----------------------------------------------------------------------------
std::uint64_t SourceSelector::pack(std::uint32_t seq, std::uint32_t fade, OutputSource s) noexcept {
    return (static_cast<std::uint64_t>(seq & 0xFFFFFFu) << 40) | (static_cast<std::uint64_t>(fade) << 8) |
           static_cast<std::uint64_t>(s);
}

void SourceSelector::prepare(double fs) noexcept {
    fs_ = fs;
    req_.store(0);
    seq_.store(0);
    applied_ = 0;
    cur_ = OutputSource::Masker;
    gm_ = 1.0;
    gc_ = 0.0;
    m0_ = m1_ = 1.0;
    c0_ = c1_ = 0.0;
    pos_ = len_ = 0;
}

void SourceSelector::request(OutputSource s, double fadeSeconds) noexcept {
    const double f = std::clamp(fadeSeconds, 0.0, 10.0) * fs_;
    const auto fade = static_cast<std::uint32_t>(std::llround(f));
    const std::uint32_t seq = (seq_.fetch_add(1, std::memory_order_acq_rel) + 1) & 0xFFFFFFu;
    req_.store(pack(seq == 0 ? 1 : seq, fade, s), std::memory_order_release);
}

OutputSource SourceSelector::requested() const noexcept {
    const std::uint64_t v = req_.load(std::memory_order_acquire);
    return v == 0 ? OutputSource::Masker : static_cast<OutputSource>(v & 0xFF);
}

void SourceSelector::poll() noexcept {
    const std::uint64_t v = req_.load(std::memory_order_acquire);
    if (v == applied_) return;
    applied_ = v;
    applyNow(static_cast<OutputSource>(v & 0xFF), static_cast<double>((v >> 8) & 0xFFFFFFFFu) / fs_);
}

void SourceSelector::applyNow(OutputSource s, double fadeSeconds) noexcept {
    cur_ = s;
    m0_ = gm_;
    c0_ = gc_;
    m1_ = s == OutputSource::Masker ? 1.0 : 0.0;
    c1_ = s == OutputSource::Calibration ? 1.0 : 0.0;
    pos_ = 0;
    len_ = static_cast<std::int64_t>(std::llround(fadeSeconds * fs_));
    if (len_ <= 0) {
        len_ = 0;
        gm_ = m1_;
        gc_ = c1_;
    }
}

void SourceSelector::gains(int n, float* gm, float* gc) noexcept {
    for (int i = 0; i < n; ++i) {
        if (len_ > 0) {
            ++pos_;
            if (pos_ >= len_) {
                gm_ = m1_;
                gc_ = c1_;
                len_ = 0;
            } else {
                const double th = 0.5 * kPi * static_cast<double>(pos_) / static_cast<double>(len_);
                const double c = std::cos(th), s = std::sin(th);
                gm_ = m0_ * c + m1_ * s;
                gc_ = c0_ * c + c1_ * s;
            }
        }
        gm[i] = static_cast<float>(gm_);
        gc[i] = static_cast<float>(gc_);
    }
}

// ---- stage -------------------------------------------------------------------------------
void OutputSourceStage::prepare(double fs, int numOutputs, int maxFrames) {
    nOut_ = std::max(numOutputs, 0);
    cap_ = std::max(maxFrames, 1);
    bus_.prepare(fs, nOut_);
    sel_.prepare(fs);
    prev_.store(0);
    cal_.assign(static_cast<std::size_t>(nOut_), std::vector<float>(static_cast<std::size_t>(cap_), 0.0f));
    calPtr_.clear();
    for (auto& b : cal_) calPtr_.push_back(b.data());
    ioPtr_.assign(static_cast<std::size_t>(nOut_), nullptr);
    gm_.assign(static_cast<std::size_t>(cap_), 1.0f);
    gc_.assign(static_cast<std::size_t>(cap_), 0.0f);
}

bool OutputSourceStage::startTest(const CalibrationParams& p, std::string* error) {
    const OutputSource cur = sel_.requested();
    if (!bus_.start(p, error)) return false;
    if (cur != OutputSource::Calibration) prev_.store(static_cast<std::uint8_t>(cur));
    sel_.request(OutputSource::Calibration, SourceSelector::kCrossfadeS);
    return true;
}

void OutputSourceStage::stopTest() noexcept {
    bus_.stop();
    if (sel_.requested() == OutputSource::Calibration)
        sel_.request(static_cast<OutputSource>(prev_.load()), kStopCrossfadeS);
}

void OutputSourceStage::process(float* const* io, int n) noexcept {
    sel_.poll();
    if (sel_.steadyMasker() && !bus_.running()) return;  // bit-identical masker path
    for (int off = 0; off < n; off += cap_) {
        const int m = std::min(cap_, n - off);
        for (int c = 0; c < nOut_; ++c) ioPtr_[static_cast<std::size_t>(c)] = io[c] + off;
        bus_.process(calPtr_.data(), m);
        sel_.gains(m, gm_.data(), gc_.data());
        for (int c = 0; c < nOut_; ++c) {
            float* x = ioPtr_[static_cast<std::size_t>(c)];
            const float* y = cal_[static_cast<std::size_t>(c)].data();
            for (int i = 0; i < m; ++i) x[i] = gm_[static_cast<std::size_t>(i)] * x[i] + gc_[static_cast<std::size_t>(i)] * y[i];
        }
    }
    if (bus_.consumeFinished() && sel_.current() == OutputSource::Calibration)
        sel_.applyNow(static_cast<OutputSource>(prev_.load()), kStopCrossfadeS);
}

// ---- Test Speakers -----------------------------------------------------------------------
std::vector<int> enabledOutputs(const std::vector<OutputChannel>& channels, const std::vector<OutputZone>& zones,
                                int numOutputs) {
    std::vector<int> r;
    for (int o = 0; o < numOutputs; ++o) {
        bool ok = true;
        if (static_cast<std::size_t>(o) < channels.size()) {
            const OutputChannel& c = channels[static_cast<std::size_t>(o)];
            ok = c.enabled && !c.mute;
            if (ok && !zones.empty()) {
                ok = false;
                for (const OutputZone& z : zones)
                    if (z.id == c.zone) ok = z.enabled;
            }
        }
        if (ok) r.push_back(o);
    }
    return r;
}

bool startTestSpeakers(OutputSourceStage& stage, const std::vector<int>& outputs, const TestSpeakersOptions& opt,
                       std::string* error) {
    if (outputs.empty()) {
        if (error) *error = "no enabled outputs";
        return false;
    }
    CalibrationParams p;
    p.signal = CalSignal::ChannelId;
    p.levelDbfs = opt.levelDbfs;
    p.confirmLoud = opt.confirmLoud;
    p.codedTones = opt.codedTones;
    p.loop = opt.loop;
    p.seed = opt.seed;
    p.outputs = outputs;
    return stage.startTest(p, error);
}

}  // namespace bf
