#include "core/corpus/ingest/Analyzer.h"

#include <algorithm>
#include <cmath>

#include "core/analysis/Meters.h"
#include "core/config/Sha256.h"
#include "core/corpus/ingest/Decode.h"
#include "core/corpus/ingest/Features.h"
#include "core/corpus/ingest/Signal.h"

namespace bf::ingest {

namespace {

constexpr double kRate = 48000.0;

double toDb(double lin) { return 20.0 * std::log10(std::max(lin, 1e-12)); }

std::vector<std::uint8_t> speechMask10(const std::vector<SampleSpan>& regions, std::size_t n10) {
    std::vector<std::uint8_t> m(n10, 0);
    for (const auto& r : regions) {
        const std::int64_t f0 = (r.start + 479) / 480, f1 = r.end / 480;
        for (std::int64_t f = f0; f < f1 && f < static_cast<std::int64_t>(n10); ++f) m[static_cast<std::size_t>(f)] = 1;
    }
    return m;
}

// 30 ms frame RMS statistics: returns speech-frame RMS dB (energy mean) and noise floor (10th
// percentile over non-speech frames).
struct FrameLevels {
    double speechDb = -120.0, noiseDb = -120.0;
    bool haveNoise = false;
};
FrameLevels frameLevels(const std::vector<float>& x, const std::vector<SampleSpan>& regions) {
    constexpr std::size_t F = 1440;
    FrameLevels out;
    std::vector<double> noise, all;
    double speechE = 0.0, speechN = 0.0;
    std::size_t ri = 0;
    for (std::size_t s = 0; s + F <= x.size(); s += F) {
        double e = 0.0;
        for (std::size_t i = 0; i < F; ++i) e += static_cast<double>(x[s + i]) * x[s + i];
        e /= static_cast<double>(F);
        const auto a = static_cast<std::int64_t>(s), b = static_cast<std::int64_t>(s + F);
        while (ri < regions.size() && regions[ri].end <= a) ++ri;
        const bool overlaps = ri < regions.size() && regions[ri].start < b;
        const double db = 10.0 * std::log10(std::max(e, 1e-12));
        all.push_back(db);
        if (overlaps) {
            const bool inside = regions[ri].start <= a && regions[ri].end >= b;
            if (inside) { speechE += e; speechN += 1.0; }
        } else {
            noise.push_back(db);
        }
    }
    if (speechN > 0.0) out.speechDb = 10.0 * std::log10(std::max(speechE / speechN, 1e-12));
    if (!noise.empty()) { out.noiseDb = percentile(noise, 0.10); out.haveNoise = true; }
    else if (!all.empty()) out.noiseDb = percentile(all, 0.10);
    return out;
}

std::vector<double> perChannelSnr(const std::vector<std::vector<float>>& ch, const std::vector<SampleSpan>& regions) {
    std::vector<double> snr;
    for (const auto& c : ch) {
        const auto fl = frameLevels(c, regions);
        snr.push_back(fl.speechDb - std::max(fl.noiseDb, -120.0));
    }
    return snr;
}

double correlation(const std::vector<float>& a, const std::vector<float>& b, const std::vector<SampleSpan>& regions) {
    double ma = 0, mb = 0, n = 0;
    for (const auto& r : regions)
        for (std::int64_t i = r.start; i < r.end && i < static_cast<std::int64_t>(a.size()); ++i) {
            ma += a[static_cast<std::size_t>(i)]; mb += b[static_cast<std::size_t>(i)]; n += 1;
        }
    if (n < 1) return 0.0;
    ma /= n; mb /= n;
    double saa = 0, sbb = 0, sab = 0;
    for (const auto& r : regions)
        for (std::int64_t i = r.start; i < r.end && i < static_cast<std::int64_t>(a.size()); ++i) {
            const double x = a[static_cast<std::size_t>(i)] - ma, y = b[static_cast<std::size_t>(i)] - mb;
            saa += x * x; sbb += y * y; sab += x * y;
        }
    return (saa > 0 && sbb > 0) ? sab / std::sqrt(saa * sbb) : 0.0;
}

// Heuristic reverberation estimate: level decay from just before a speech offset (VAD region
// ends include the 150 ms hangover) to 0-40 ms after the region end.
bool estimateReverberant(const std::vector<float>& x, const std::vector<SampleSpan>& regions, double floorDb) {
    std::vector<double> decays;
    const auto pw = [&](std::int64_t a, std::int64_t b) {
        a = std::max<std::int64_t>(a, 0);
        b = std::min<std::int64_t>(b, static_cast<std::int64_t>(x.size()));
        if (b <= a) return -120.0;
        double e = 0;
        for (auto i = a; i < b; ++i) e += static_cast<double>(x[static_cast<std::size_t>(i)]) * x[static_cast<std::size_t>(i)];
        return 10.0 * std::log10(std::max(e / static_cast<double>(b - a), 1e-12));
    };
    for (std::size_t i = 0; i + 1 < regions.size(); ++i) {
        const std::int64_t e = regions[i].end;
        if (regions[i + 1].start - e < 14400) continue;  // pause >= 300 ms
        const double la = pw(e - 9120, e - 7200);          // 190..150 ms before the end
        if (la < floorDb + 20.0) continue;
        const double lb = pw(e, e + 1920);
        decays.push_back(la - lb);
    }
    if (decays.size() < 5) return false;
    return percentile(decays, 0.5) < 15.0;
}

}  // namespace

void scoreQuality(RecordingAnalysis& a) {
    auto lin = [](double v, double zeroAt, double fullAt, double maxPen) {
        if (zeroAt == fullAt) return 0.0;
        const double t = (v - zeroAt) / (fullAt - zeroAt);
        return maxPen * std::clamp(t, 0.0, 1.0);
    };
    bool reject = false;
    auto hard = [&](const char* c) { a.addReason(c); reject = true; };
    if (a.sourceRate < 32000) hard("reject.sample_rate");
    else if (a.sourceRate < 44100) a.addReason("warn.sample_rate");
    if (a.bandwidthHz < 7500.0) hard("reject.bandwidth");
    else if (a.bandwidthHz < 11000.0) a.addReason("warn.bandwidth");
    if (a.clipRatio > 1e-4 || a.clipRunMs > 3.0) hard("reject.clipping");
    else if (a.clipRatio >= 1e-5) a.addReason("warn.clipping");
    if (a.snrDb < 25.0) hard("reject.snr");
    else if (a.snrDb < 35.0) a.addReason("warn.snr");
    if (a.speechRatio < 0.30) hard("reject.speech_ratio");
    else if (a.speechRatio < 0.60) a.addReason("warn.speech_ratio");
    if (a.durationS < 10.0) hard("reject.duration");
    else if (a.durationS < 30.0) a.addReason("warn.duration");
    if (std::fabs(a.dcOffset) > 0.01) a.addReason("warn.dc_offset");
    if (a.lossy) a.addReason("lossy");
    if (a.nonSpeechFrac > 0.10) a.addReason("review.nonSpeech");  // review only, never rejected
    if (a.reverberant) a.addReason("reverberant");
    if (a.multichannelSelected) a.addReason("multichannelSelected");

    double pen = 0.0;
    pen += lin(a.snrDb, 35.0, 25.0, 40.0);
    pen += lin(a.bandwidthHz, 11000.0, 7500.0, 30.0);
    pen += a.clipRatio < 1e-5 ? 0.0 : lin(a.clipRatio, 1e-5, 1e-4, 20.0);
    pen += lin(a.speechRatio, 0.60, 0.30, 20.0);
    if (a.lossy) pen += 10.0;
    if (a.reverberant) pen += 10.0;
    if (a.multichannelSelected) pen += 5.0;
    a.qualityScore = std::clamp(100.0 - pen, 0.0, 100.0);
    if (reject || a.qualityScore < 50.0) {
        a.qualityClass = QualityClass::Rejected;
        if (!reject) a.addReason("reject.quality");
    } else {
        a.qualityClass = a.qualityScore >= 80.0 ? QualityClass::Good : QualityClass::Usable;
    }
}

RecordingAnalysis analyzeFile(const std::string& path, const AnalyzerConfig& cfg) {
    DecodedAudio in;
    std::string reason;
    if (!decodeFile(path, in, reason)) {
        RecordingAnalysis a;
        a.sourcePath = path;
        a.decodeFailed = true;
        a.qualityClass = QualityClass::Rejected;
        a.addReason(reason);
        return a;
    }
    auto a = analyzeAudio(in, cfg);
    a.sourcePath = path;
    return a;
}

RecordingAnalysis analyzeAudio(const DecodedAudio& in, const AnalyzerConfig& cfg) {
    RecordingAnalysis a;
    a.sourceRate = in.sampleRate;
    a.sourceChannels = in.channels;
    a.sourceFormat = in.format;
    a.lossy = in.lossy;
    const std::size_t nSrc = static_cast<std::size_t>(in.frames());
    const std::size_t nch = in.channels;

    // --- clipping on the source samples (worst channel) ---
    for (std::size_t c = 0; c < nch; ++c) {
        const auto cl = detectClipping(in.data.data() + c, nSrc, nch, in.sampleRate, in.bitDepth == 32);
        a.clipRatio = std::max(a.clipRatio, cl.ratio);
        a.clipRunMs = std::max(a.clipRunMs, cl.longestRunMs);
    }

    // --- resample every channel to 48 kHz ---
    std::vector<std::vector<float>> ch(nch);
    {
        std::vector<float> tmp(nSrc);
        for (std::size_t c = 0; c < nch; ++c) {
            for (std::size_t i = 0; i < nSrc; ++i) tmp[i] = in.data[i * nch + c];
            ch[c] = resample(tmp.data(), nSrc, in.sampleRate, kRate);
        }
    }
    const std::size_t n = ch[0].size();
    a.length = static_cast<std::int64_t>(n);
    a.durationS = static_cast<double>(n) / kRate;

    // --- channel normalisation (§3.3) ---
    std::vector<float> mono;
    if (nch == 1) {
        mono = std::move(ch[0]);
    } else {
        std::vector<float> mix(n, 0.0f);
        for (std::size_t c = 0; c < nch; ++c)
            for (std::size_t i = 0; i < n; ++i) mix[i] += ch[c][i];
        const float inv = 1.0f / static_cast<float>(nch);
        for (auto& v : mix) v *= inv;
        auto mix16 = resample(mix.data(), n, kRate, 16000.0);
        const auto regs = detectSpeech(mix, toPcm16(mix16), a.length, cfg, nullptr);
        double rho = 0.0;
        int pairs = 0;
        for (std::size_t i = 0; i < nch; ++i)
            for (std::size_t j = i + 1; j < nch; ++j) { rho += correlation(ch[i], ch[j], regs); ++pairs; }
        rho /= std::max(pairs, 1);
        if (rho >= 0.9) {
            mono = std::move(mix);
        } else {
            const auto snr = perChannelSnr(ch, regs);
            const auto best = static_cast<std::size_t>(std::max_element(snr.begin(), snr.end()) - snr.begin());
            mono = std::move(ch[best]);
            a.multichannelSelected = true;
        }
    }
    ch.clear();

    // --- DC removal ---
    {
        double m = 0.0;
        for (float v : mono) m += v;
        a.dcOffset = n ? m / static_cast<double>(n) : 0.0;
    }
    removeDcZeroPhase(mono, kRate);

    // --- VAD ---
    const auto x16 = resample(mono.data(), n, kRate, 16000.0);
    const auto pcm16 = toPcm16(x16);
    a.regions = detectSpeech(mono, pcm16, a.length, cfg, &a.vadEngine);
    {
        Sha256 h;
        std::vector<unsigned char> bytes(pcm16.size() * 2);
        for (std::size_t i = 0; i < pcm16.size(); ++i) {
            const auto u = static_cast<std::uint16_t>(pcm16[i]);
            bytes[2 * i] = static_cast<unsigned char>(u & 0xFF);
            bytes[2 * i + 1] = static_cast<unsigned char>(u >> 8);
        }
        h.update(bytes.data(), bytes.size());
        a.pcmSha256 = h.finishHex();
    }

    // --- segmentation (§3.6): the runtime snapshot builder derives pauses, anchors, stats ---
    {
        RecordingInput ri;
        ri.speaker = 0;
        ri.length = a.length;
        ri.speech = a.regions;
        for (const auto& r : a.regions) {
            double e = 0.0;
            for (auto i = r.start; i < r.end; ++i) e += static_cast<double>(mono[static_cast<std::size_t>(i)]) * mono[static_cast<std::size_t>(i)];
            ri.speechEnergy.push_back(e);
        }
        SpeakerInput si;
        const auto snap = CorpusSnapshot::build("", {si}, {ri});
        const auto regs = snap->regionsOf(0);
        a.regions.assign(regs.begin(), regs.end());
        const auto ps = snap->pausesOf(0);
        a.pauses.assign(ps.begin(), ps.end());
        for (std::size_t g = 0; g < snap->numSegments(); ++g) {
            const auto& s = snap->segment(static_cast<SegmentId>(g));
            SegmentRow row;
            row.anchor = s.anchor;
            row.maxEnd = a.length;
            row.speechFrac = s.speechFrac;
            row.longestPhraseS = s.longestPhraseS;
            row.asl10sDb = s.aslDb;
            row.soloRisk = (s.flags & kSegSoloRisk) != 0;
            const std::int64_t e = std::min<std::int64_t>(a.length, s.anchor + 10 * kCorpusRate);
            const auto p = p56MethodB(mono.data() + s.anchor, static_cast<std::size_t>(e - s.anchor), kRate);
            if (p.valid) row.asl10sDb = static_cast<float>(p.aslDb);
            a.segments.push_back(row);
        }
    }
    double speechSamples = 0.0;
    for (const auto& r : a.regions) speechSamples += static_cast<double>(r.length());
    a.speechS = speechSamples / kRate;
    a.speechRatio = a.durationS > 0 ? a.speechS / a.durationS : 0.0;

    // --- silence analysis (§3.7) ---
    {
        static const double edges[] = {80, 150, 300, 600, 1000, 2000};
        double total = 0, g100 = 0, g250 = 0, g600 = 0;
        for (const auto& p : a.pauses) {
            const double ms = 1000.0 * static_cast<double>(p.end - p.start) / kRate;
            std::size_t bin = 0;
            while (bin + 1 < kPauseHistBins && ms >= edges[bin + 1]) ++bin;
            ++a.pauseHist[bin];
            total += ms;
            if (ms > 100) g100 += ms;
            if (ms > 250) g250 += ms;
            if (ms > 600) g600 += ms;
        }
        if (total > 0) { a.pauseFracGt100 = g100 / total; a.pauseFracGt250 = g250 / total; a.pauseFracGt600 = g600 / total; }
    }

    // --- levels (§3.8) ---
    {
        double pk = 0.0, e = 0.0;
        for (float v : mono) { pk = std::max(pk, static_cast<double>(std::fabs(v))); e += static_cast<double>(v) * v; }
        a.peakDb = toDb(pk);
        a.rmsDb = n ? 10.0 * std::log10(std::max(e / static_cast<double>(n), 1e-12)) : -200.0;
        Meters m;
        m.prepare(kRate, 1);
        for (std::size_t s = 0; s < n; s += 4800) {
            const float* p = mono.data() + s;
            m.process(&p, static_cast<int>(std::min<std::size_t>(4800, n - s)));
        }
        const auto snap = m.snapshot();
        a.truePeakDb = snap.truePeakMaxDb.empty() ? -200.0 : snap.truePeakMaxDb[0];
        a.lufsI = snap.lufsI;
        const auto p56 = p56MethodB(mono.data(), n, kRate);
        a.aslDb = p56.valid ? p56.aslDb : a.rmsDb;
        a.activityPct = p56.activityPct;
        const auto fl = frameLevels(mono, a.regions);
        a.noiseFloorDb = std::max(fl.noiseDb, -120.0);
        a.snrDb = a.aslDb - a.noiseFloorDb;
        a.reverberant = estimateReverberant(mono, a.regions, a.noiseFloorDb);
    }

    // --- LTASS, bandwidth (§3.9) ---
    {
        const auto lt = analyzeLtass(mono, a.regions);
        if (lt.valid) {
            a.ltassPower = lt.bandPower;
            a.ltassDb = lt.bandDb;
            a.spectralCentroidHz = lt.centroidHz;
            a.bandwidthHz = lt.bandwidthHz;
        }
    }

    // --- F0 and speaking rate (§3.10, §3.11) ---
    {
        const auto mask = speechMask10(a.regions, (pcm16.size() + 159) / 160);
        a.f0 = yinF0(x16, mask);
        a.speakingRate = speakingRate(x16, mask, a.f0.track);
        a.nonSpeechFrac = nonSpeechFraction(x16);
        if (cfg.computeFingerprint) a.fingerprint = computeFingerprint(x16);
    }

    a.audio48k = std::move(mono);
    scoreQuality(a);
    return a;
}

}  // namespace bf::ingest
