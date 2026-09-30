#include "core/corpus/CorpusSnapshot.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace bf {

namespace {

constexpr std::int64_t kWindow10s = 10 * kCorpusRate;
constexpr std::int64_t kWindow3s = 3 * kCorpusRate;
constexpr std::int64_t kMinRemaining = 2 * kCorpusRate;
constexpr std::int64_t kMinSpeechIn3s = kCorpusRate;  // 1.0 s

double spanSpeech(std::span<const SampleSpan> regs, std::int64_t a, std::int64_t b) {
    double s = 0.0;
    for (const auto& r : regs) {
        if (r.end <= a) continue;
        if (r.start >= b) break;
        s += static_cast<double>(std::min(r.end, b) - std::max(r.start, a));
    }
    return s;
}

}  // namespace

double CorpusSnapshot::totalSpeechSeconds() const noexcept {
    double s = 0.0;
    for (const auto& sp : speakers_) s += sp.usableSpeechS;
    return s;
}

double CorpusSnapshot::speechSecondsIn(RecordingId r, std::int64_t a, std::int64_t b) const {
    return spanSpeech(regionsOf(r), a, b) / static_cast<double>(kCorpusRate);
}

std::shared_ptr<const CorpusSnapshot> CorpusSnapshot::build(std::string corpusVersion,
                                                            std::vector<SpeakerInput> speakers,
                                                            std::vector<RecordingInput> recordings) {
    auto snap = std::shared_ptr<CorpusSnapshot>(new CorpusSnapshot());
    snap->version_ = std::move(corpusVersion);

    std::vector<std::size_t> order(recordings.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return recordings[a].speaker < recordings[b].speaker;
    });

    snap->speakers_.resize(speakers.size());
    for (std::size_t s = 0; s < speakers.size(); ++s) {
        auto& sp = snap->speakers_[s];
        sp.features = speakers[s].features;
        sp.weight = speakers[s].weight;
        sp.flags = speakers[s].healthy ? 0u : static_cast<std::uint32_t>(kSpeakerUnhealthy);
        sp.firstRecording = 0;
        sp.nRecordings = 0;
    }

    for (std::size_t oi = 0; oi < order.size(); ++oi) {
        RecordingInput& in = recordings[order[oi]];
        const auto rid = static_cast<RecordingId>(oi);
        RecordingRec rec;
        rec.speaker = in.speaker;
        rec.cacheFileIdx = static_cast<std::uint32_t>(order[oi]);
        rec.length = in.length;
        rec.quality = in.quality;

        // Merge regions closer than 80 ms; clip to recording.
        std::vector<SampleSpan> regs;
        std::vector<double> energy;
        const bool haveEnergy = in.speechEnergy.size() == in.speech.size() && !in.speech.empty();
        for (std::size_t i = 0; i < in.speech.size(); ++i) {
            SampleSpan r = in.speech[i];
            r.start = std::max<std::int64_t>(0, r.start);
            r.end = std::min(in.length, r.end);
            if (r.end <= r.start) continue;
            const double e = haveEnergy ? in.speechEnergy[i] : 0.0;
            if (!regs.empty() && r.start - regs.back().end < kMinPauseSamples) {
                regs.back().end = std::max(regs.back().end, r.end);
                energy.back() += e;
            } else {
                regs.push_back(r);
                energy.push_back(e);
            }
        }

        rec.firstRegion = static_cast<std::uint32_t>(snap->regions_.size());
        rec.nRegions = static_cast<std::uint32_t>(regs.size());
        rec.firstPause = static_cast<std::uint32_t>(snap->pauses_.size());
        double speechSamples = 0.0, totalEnergy = 0.0;
        for (std::size_t i = 0; i < regs.size(); ++i) {
            snap->regions_.push_back(regs[i]);
            speechSamples += static_cast<double>(regs[i].length());
            totalEnergy += energy[i];
            if (i + 1 < regs.size()) {
                PauseRec p{regs[i].end, regs[i + 1].start, false};
                p.phraseBoundary = (p.end - p.start) >= kPhraseBoundarySamples;
                snap->pauses_.push_back(p);
            }
        }
        rec.nPauses = static_cast<std::uint32_t>(snap->pauses_.size()) - rec.firstPause;
        rec.speechSeconds = speechSamples / static_cast<double>(kCorpusRate);
        if (haveEnergy && speechSamples > 0.0 && totalEnergy > 0.0)
            rec.aslDb = static_cast<float>(10.0 * std::log10(totalEnergy / speechSamples));
        else
            rec.aslDb = in.aslDb;

        auto& sp = snap->speakers_[in.speaker];
        if (sp.nRecordings == 0) sp.firstRecording = rid;
        ++sp.nRecordings;
        sp.usableSpeechS += rec.speechSeconds;
        snap->recordings_.push_back(rec);

        // Anchors (CORPUS.md §3.6): onset after a phrase-boundary pause (or >= 150 ms of
        // leading silence), >= 1.0 s speech in the next 3 s, >= 2 s remaining audio.
        std::span<const SampleSpan> rs(snap->regions_.data() + rec.firstRegion, rec.nRegions);
        for (std::size_t i = 0; i < regs.size(); ++i) {
            const std::int64_t prevEnd = i == 0 ? 0 : regs[i - 1].end;
            if (regs[i].start - prevEnd < kPhraseBoundarySamples) continue;
            const std::int64_t a = regs[i].start;
            if (in.length - a < kMinRemaining) continue;
            if (spanSpeech(rs, a, a + kWindow3s) < static_cast<double>(kMinSpeechIn3s)) continue;

            SegmentRec seg;
            seg.recording = rid;
            seg.anchor = a;
            seg.maxLen = in.length - a;
            const std::int64_t wEnd = std::min(in.length, a + kWindow10s);
            const double sp10 = spanSpeech(rs, a, wEnd);
            seg.speechFrac = static_cast<float>(sp10 / static_cast<double>(wEnd - a));
            // Longest continuous phrase: runs of regions joined across non-boundary pauses.
            double longest = 0.0;
            std::int64_t runStart = -1, runEnd = -1;
            double eSum = 0.0, eLen = 0.0;
            for (std::size_t k = i; k < regs.size() && regs[k].start < wEnd; ++k) {
                const std::int64_t s0 = regs[k].start, s1 = std::min(regs[k].end, wEnd);
                if (runStart < 0 || s0 - runEnd >= kPhraseBoundarySamples) runStart = s0;
                runEnd = s1;
                longest = std::max(longest, static_cast<double>(runEnd - runStart));
                const double frac = static_cast<double>(s1 - s0) / static_cast<double>(regs[k].length());
                eSum += energy[k] * frac;
                eLen += static_cast<double>(s1 - s0);
            }
            seg.longestPhraseS = static_cast<float>(longest / static_cast<double>(kCorpusRate));
            if (haveEnergy && eLen > 0.0 && eSum > 0.0)
                seg.aslDb = static_cast<float>(10.0 * std::log10(eSum / eLen));
            else
                seg.aslDb = rec.aslDb;
            if (seg.speechFrac > 0.9f && seg.longestPhraseS > 4.0f) seg.flags |= kSegSoloRisk;
            snap->segments_.push_back(seg);
        }
    }

    // Anchor ranges per speaker (segments are ordered by recording, recordings by speaker).
    for (auto& sp : snap->speakers_) { sp.firstAnchor = 0; sp.nAnchors = 0; }
    for (std::size_t g = 0; g < snap->segments_.size(); ++g) {
        const SpeakerId s = snap->recordings_[snap->segments_[g].recording].speaker;
        auto& sp = snap->speakers_[s];
        if (sp.nAnchors == 0) sp.firstAnchor = static_cast<std::uint32_t>(g);
        ++sp.nAnchors;
    }
    return snap;
}

}  // namespace bf
