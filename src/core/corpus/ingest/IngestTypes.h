#pragma once
// Shared types of the corpus ingestion pipeline (docs/CORPUS.md §3).
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/corpus/CorpusSnapshot.h"

namespace bf::ingest {

inline constexpr const char* kAnalyzerVersion = "bfanalyzer-1.0.0";
inline constexpr int kCacheRate = 48000;
inline constexpr int kNumLtassBands = 26;
inline constexpr int kF0HistBins = 12;
inline constexpr int kPauseHistBins = 6;

// Stored in recording.quality_class (higher is better).
enum class QualityClass : int { Rejected = 0, Usable = 1, Good = 2 };

// speaker.flags bits.
enum SpeakerFlagBits : std::uint32_t {
    kSpkPossibleDuplicate = 1,  // speaker.possibleDuplicate
    kSpkUnknownIdentity = 2,    // speaker.unknown (one speaker per file)
};

enum class VadEngine { WebRtc, EnergyFlatness };

struct AnalyzerConfig {
    VadEngine vad = VadEngine::WebRtc;
    int vadMode = 2;             // WebRTC aggressiveness
    bool computeFingerprint = true;
};

struct DecodedAudio {
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    int bitDepth = 0;
    bool lossy = false;
    std::string format;        // "wav" | "aiff" | "flac"
    std::vector<float> data;   // interleaved
    std::uint64_t frames() const { return channels ? data.size() / channels : 0; }
};

struct ClipResult {
    double ratio = 0.0;         // clipped samples / samples
    double longestRunMs = 0.0;  // longest merged clipped run
};

struct P56Result {
    double aslDb = -200.0;      // active speech level (dBFS, mean-square)
    double activityPct = 0.0;
    double rmsDb = -200.0;      // whole-signal RMS
    bool valid = false;
};

struct F0Stats {
    double medianHz = 0, meanHz = 0, p5Hz = 0, p95Hz = 0, rangeSt = 0, voicedRatio = 0;
    std::array<float, kF0HistBins> hist{};
    std::vector<float> track;   // Hz per 10 ms frame (0 = unvoiced)
};

struct SegmentRow {
    std::int64_t anchor = 0, maxEnd = 0;
    float speechFrac = 0, longestPhraseS = 0, asl10sDb = 0;
    bool soloRisk = false;
};

struct RecordingAnalysis {
    // Identity / provenance
    std::string sourcePath, sourceSha256, pcmSha256, sourceFormat;
    std::uint32_t sourceRate = 0;
    std::uint16_t sourceChannels = 0;
    bool lossy = false, multichannelSelected = false, reverberant = false;
    std::string vadEngine;

    // Processed 48 kHz mono audio (DC removed). Released after the cache write.
    std::vector<float> audio48k;
    std::int64_t length = 0;  // samples at 48 kHz

    double durationS = 0, speechS = 0, speechRatio = 0;
    double peakDb = -200, truePeakDb = -200, rmsDb = -200, aslDb = -200, activityPct = 0;
    double lufsI = -200, noiseFloorDb = -120, snrDb = 0, bandwidthHz = 0;
    double nonSpeechFrac = 0;  // fraction of frames flagged music/non-speech (§1.3)
    // Anchors (48 kHz samples) inside content overlapping an earlier recording (§3.13).
    std::vector<SampleSpan> overlapSpans;
    double clipRatio = 0, clipRunMs = 0, dcOffset = 0;
    double spectralCentroidHz = 0, speakingRate = 0;
    F0Stats f0;
    std::array<float, kNumLtassBands> ltassDb{};
    std::array<double, kNumLtassBands> ltassPower{};  // linear band fractions (sum 1)
    std::array<std::uint32_t, kPauseHistBins> pauseHist{};
    double pauseFracGt100 = 0, pauseFracGt250 = 0, pauseFracGt600 = 0;

    std::vector<SampleSpan> regions;   // merged speech regions (48 kHz)
    std::vector<PauseRec> pauses;
    std::vector<SegmentRow> segments;
    std::vector<std::uint32_t> fingerprint;

    double qualityScore = 0;
    QualityClass qualityClass = QualityClass::Rejected;
    std::vector<std::string> reasons;  // reason codes (decode.*, reject.*, warn.*, ...)
    bool decodeFailed = false;

    void addReason(const std::string& r) {
        for (auto& x : reasons) if (x == r) return;
        reasons.push_back(r);
    }
    bool hasReason(const std::string& r) const {
        for (auto& x : reasons) if (x == r) return true;
        return false;
    }
};

}  // namespace bf::ingest
