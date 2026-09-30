#include "core/corpus/SyntheticCorpus.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "core/random/Distributions.h"
#include "core/random/Random.h"

namespace bf {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = static_cast<double>(kCorpusRate);
constexpr std::size_t kFirTaps = 32;
constexpr std::int64_t kEdge = 240;  // 5 ms region edge taper

inline float hashNoise(std::uint64_t key, std::int64_t n) noexcept {
    SplitMix64 sm(key ^ (static_cast<std::uint64_t>(n) * 0xD1B54A32D192ED03ULL));
    const std::uint64_t x = sm.next();
    return (static_cast<float>(x >> 40) * 0x1.0p-24f) * 2.0f - 1.0f;  // [-1, 1), var 1/3
}
}  // namespace

SyntheticCorpus::SyntheticCorpus(const SyntheticCorpusParams& p) : p_(p) {
    const std::int64_t recLen = static_cast<std::int64_t>(p.recordingSeconds * kFs);
    std::vector<SpeakerInput> spIn(p.numSpeakers);
    std::vector<RecordingInput> recIn;

    for (std::uint32_t s = 0; s < p.numSpeakers; ++s) {
        RngStream rng(p.seed, "synth.speaker." + std::to_string(s), 0);
        Speaker sp;
        sp.f0Hz = 85.0 * std::pow(2.0, 1.6 * rng.uniform01());
        sp.bandHz = 700.0 * std::pow(2.0, 1.5 * rng.uniform01());
        const double levelDb = -22.0 - 8.0 * rng.uniform01();
        sp.levelLin = std::pow(10.0, levelDb / 20.0);
        sp.modLo = 3.0 + 1.5 * rng.uniform01();
        sp.modHi = sp.modLo + 1.5;
        // Hann-windowed cosine FIR centred at bandHz, normalised to unit energy.
        sp.fir.resize(kFirTaps);
        double e = 0.0;
        for (std::size_t k = 0; k < kFirTaps; ++k) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (static_cast<double>(k) + 0.5) / kFirTaps);
            const double h = w * std::cos(2.0 * kPi * sp.bandHz * static_cast<double>(k) / kFs);
            sp.fir[k] = static_cast<float>(h);
            e += h * h;
        }
        for (auto& h : sp.fir) h = static_cast<float>(h / std::sqrt(e));

        auto& f = spIn[s].features;
        f[kFeatF0MedianSt] = static_cast<float>(12.0 * std::log2(sp.f0Hz / 100.0));
        f[kFeatF0RangeSt] = static_cast<float>(2.0 + 4.0 * rng.uniform01());
        f[kFeatSpeakingRate] = static_cast<float>(0.5 * (sp.modLo + sp.modHi));
        f[kFeatCentroidLog2] = static_cast<float>(std::log2(sp.bandHz));
        f[kFeatLtassPc1] = static_cast<float>(standardNormal(rng));
        f[kFeatLtassPc2] = static_cast<float>(standardNormal(rng));
        f[kFeatLtassPc3] = static_cast<float>(standardNormal(rng));
        f[kFeatLanguage] = 0.0f;
        speakers_.push_back(std::move(sp));
    }

    for (std::uint32_t s = 0; s < p.numSpeakers; ++s) {
        for (std::uint32_t m = 0; m < p.recordingsPerSpeaker; ++m) {
            const std::uint32_t idx = s * p.recordingsPerSpeaker + m;
            RngStream rng(p.seed, "synth.rec." + std::to_string(idx), 0);
            Recording rec;
            rec.speaker = s;
            rec.length = recLen;
            rec.noiseKey = deriveStreamSeed(p.seed, "synth.noise." + std::to_string(idx), 0);
            std::int64_t t = static_cast<std::int64_t>((0.2 + 0.3 * rng.uniform01()) * kFs);
            while (true) {
                const double spS = truncatedLogNormalMedian(rng, p.speechMedianS, p.speechSigmaLn, 0.25, 6.0);
                const std::int64_t e = t + static_cast<std::int64_t>(spS * kFs);
                if (e > recLen - static_cast<std::int64_t>(0.1 * kFs)) break;
                Region r;
                r.start = t;
                r.end = e;
                const auto& sp = speakers_[s];
                r.modHz = sp.modLo + (sp.modHi - sp.modLo) * rng.uniform01();
                r.modPhase = kPi * rng.uniform01();
                r.f0Hz = sp.f0Hz * std::pow(2.0, 0.3 * (rng.uniform01() - 0.5));
                rec.regions.push_back(r);
                const double pS = truncatedLogNormalMedian(rng, p.pauseMedianS, p.pauseSigmaLn, 0.08, 2.0);
                t = e + std::max<std::int64_t>(kMinPauseSamples, static_cast<std::int64_t>(pS * kFs));
            }
            recs_.push_back(std::move(rec));
        }
    }

    // Metadata (+ exact ASL).
    for (std::size_t r = 0; r < recs_.size(); ++r) {
        const Recording& rec = recs_[r];
        RecordingInput in;
        in.speaker = rec.speaker;
        in.length = rec.length;
        const double lvl = speakers_[rec.speaker].levelLin;
        in.aslDb = static_cast<float>(20.0 * std::log10(lvl));
        std::vector<float> buf;
        for (const auto& reg : rec.regions) {
            in.speech.push_back({reg.start, reg.end});
            if (p.measureAsl && p.signal == SyntheticCorpusParams::Signal::PseudoSpeech) {
                buf.assign(static_cast<std::size_t>(reg.end - reg.start), 0.0f);
                synthRegion(rec, reg, reg.start, reg.end, buf.data());
                double e = 0.0;
                for (float v : buf) e += static_cast<double>(v) * static_cast<double>(v);
                in.speechEnergy.push_back(e);
            } else if (p.signal == SyntheticCorpusParams::Signal::WhiteNoise) {
                in.speechEnergy.push_back(lvl * lvl * static_cast<double>(reg.end - reg.start));
            }
        }
        recIn.push_back(std::move(in));
    }
    snapshot_ = CorpusSnapshot::build("synthetic-" + std::to_string(p.seed) + "-" +
                                          std::to_string(p.numSpeakers) + "x" +
                                          std::to_string(p.recordingsPerSpeaker),
                                      std::move(spIn), std::move(recIn));
}

void SyntheticCorpus::synthRegion(const Recording& rec, const Region& reg, std::int64_t a,
                                  std::int64_t b, float* dst) {
    const Speaker& sp = speakers_[rec.speaker];
    const std::size_t n = static_cast<std::size_t>(b - a);
    if (p_.signal == SyntheticCorpusParams::Signal::WhiteNoise) {
        const float g = static_cast<float>(sp.levelLin * std::sqrt(3.0));
        for (std::size_t i = 0; i < n; ++i)
            dst[i] = g * hashNoise(rec.noiseKey, a + static_cast<std::int64_t>(i));
        return;
    }
    // Band noise: FIR over hash noise (random access: needs kFirTaps-1 history samples).
    noiseScratch_.resize(n + kFirTaps);
    for (std::size_t i = 0; i < n + kFirTaps - 1; ++i)
        noiseScratch_[i] = hashNoise(rec.noiseKey, a - static_cast<std::int64_t>(kFirTaps) + 1 +
                                                       static_cast<std::int64_t>(i));
    const double w0 = 2.0 * kPi * reg.f0Hz / kFs;
    const double amp = sp.levelLin * 3.0;
    const double wm = kPi * reg.modHz / kFs;
    for (std::size_t i = 0; i < n; ++i) {
        const std::int64_t pos = a + static_cast<std::int64_t>(i);
        float nz = 0.0f;
        const float* x = noiseScratch_.data() + i + kFirTaps - 1;
        for (std::size_t k = 0; k < kFirTaps; ++k) nz += sp.fir[k] * x[-static_cast<std::ptrdiff_t>(k)];
        // sin(h w) harmonics 1..4 with 1/h amplitudes (multiple-angle recurrence). The phase
        // is evaluated per sample, so every read chunking yields identical samples.
        const double ph = w0 * static_cast<double>(pos - reg.start);
        const double s1 = std::sin(ph), c2x = 2.0 * std::cos(ph);
        const double s2 = c2x * s1;
        const double s3 = c2x * s2 - s1;
        const double s4 = c2x * s3 - s2;
        const double tone = s1 + 0.5 * s2 + (1.0 / 3.0) * s3 + 0.25 * s4;
        const double sm = std::sin(wm * static_cast<double>(pos - reg.start) + reg.modPhase);
        double env = 0.15 + 0.85 * sm * sm;
        const std::int64_t d0 = pos - reg.start, d1 = reg.end - 1 - pos;
        const std::int64_t d = std::min(d0, d1);
        if (d < kEdge) env *= 0.5 - 0.5 * std::cos(kPi * static_cast<double>(d) / kEdge);
        dst[i] = static_cast<float>(amp * env * (0.35 * tone + 1.2 * static_cast<double>(nz)));
    }
}

bool SyntheticCorpus::read(RecordingId recId, std::uint64_t startSample48k, float* dst, std::size_t n) {
    ++reads_;
    if (recId >= recs_.size()) return false;
    const Recording& rec = recs_[recId];
    if (rec.failing) return false;
    std::memset(dst, 0, n * sizeof(float));
    const std::int64_t a = static_cast<std::int64_t>(startSample48k);
    const std::int64_t b = a + static_cast<std::int64_t>(n);
    auto it = std::lower_bound(rec.regions.begin(), rec.regions.end(), a,
                               [](const Region& r, std::int64_t v) { return r.end <= v; });
    for (; it != rec.regions.end() && it->start < b; ++it) {
        const std::int64_t s0 = std::max(a, it->start), s1 = std::min(b, it->end);
        if (s1 > s0) synthRegion(rec, *it, s0, s1, dst + (s0 - a));
    }
    return true;
}

void SyntheticCorpus::setFailing(RecordingId rec, bool failing) {
    if (rec < recs_.size()) recs_[rec].failing = failing;
}

}  // namespace bf
