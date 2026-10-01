#pragma once
// Immutable in-memory runtime corpus index (docs/CORPUS.md §3.6, §6).
//
// All positions are 48 kHz sample indices (int64) inside a recording. The snapshot is built
// once (CorpusSnapshot::build) and then shared read-only by planner, selector and preparer
// through std::shared_ptr<const CorpusSnapshot>.
//
// Deviation from the packed CORPUS.md §6 layout: fields are stored unpacked (int64 anchors,
// float dB / fractions) because the packing is a storage concern; the content is identical.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bf {

using SpeakerId = std::uint32_t;
using RecordingId = std::uint32_t;
using SegmentId = std::uint32_t;

inline constexpr std::int64_t kCorpusRate = 48000;
inline constexpr std::int64_t kMinPauseSamples = 3840;        // 80 ms (CORPUS.md §3.6)
inline constexpr std::int64_t kPhraseBoundarySamples = 7200;  // 150 ms

struct SampleSpan {
    std::int64_t start = 0, end = 0;  // [start, end)
    std::int64_t length() const noexcept { return end - start; }
};

struct PauseRec {
    std::int64_t start = 0, end = 0;  // gap between two speech regions (>= 80 ms)
    bool phraseBoundary = false;      // >= 150 ms
};

enum SegmentFlag : std::uint8_t {
    kSegSoloRisk = 1,  // speechFrac(10 s) > 0.9 and longest continuous phrase > 4 s (MASK §6.2)
};

// Start anchor + derived per-anchor statistics (CORPUS.md §3.6 "segment record").
struct SegmentRec {
    RecordingId recording = 0;
    std::int64_t anchor = 0;        // speech onset (source sample)
    std::int64_t maxLen = 0;        // source samples from anchor to recording end
    float aslDb = 0.0f;             // active speech level of the next 10 s (dBFS)
    float speechFrac = 0.0f;        // speech fraction of the next 10 s
    float longestPhraseS = 0.0f;    // longest continuous phrase in the next 10 s
    std::uint8_t flags = 0;
};

struct RecordingRec {
    SpeakerId speaker = 0;
    std::uint32_t cacheFileIdx = 0;
    std::uint32_t firstPause = 0, nPauses = 0;
    std::uint32_t firstRegion = 0, nRegions = 0;
    std::int64_t length = 0;  // samples
    float aslDb = 0.0f;
    float quality = 1.0f;
    double speechSeconds = 0.0;
};

enum SpeakerFeature : std::size_t {
    kFeatF0MedianSt = 0,  // semitones re 100 Hz
    kFeatF0RangeSt = 1,
    kFeatSpeakingRate = 2,
    kFeatCentroidLog2 = 3,
    kFeatLtassPc1 = 4,
    kFeatLtassPc2 = 5,
    kFeatLtassPc3 = 6,
    kFeatLanguage = 7,  // categorical code
    kNumSpeakerFeatures = 8
};

enum SpeakerFlag : std::uint32_t { kSpeakerUnhealthy = 1 };

struct SpeakerRec {
    std::array<float, kNumSpeakerFeatures> features{};
    std::uint32_t firstRecording = 0, nRecordings = 0;
    std::uint32_t firstAnchor = 0, nAnchors = 0;
    float weight = 1.0f;
    std::uint32_t flags = 0;
    double usableSpeechS = 0.0;  // speech seconds over all recordings
};

// Builder inputs.
struct SpeakerInput {
    std::array<float, kNumSpeakerFeatures> features{};
    float weight = 1.0f;
    bool healthy = true;
};
struct RecordingInput {
    SpeakerId speaker = 0;
    std::int64_t length = 0;
    std::vector<SampleSpan> speech;     // VAD regions, sorted, non-overlapping
    std::vector<double> speechEnergy;   // optional: sum of squares per region (exact ASL)
    float aslDb = -26.0f;               // used when speechEnergy is empty
    float quality = 1.0f;
    // Optional overrides from a stored segment table (sorted by anchor): per-anchor ASL of the
    // next 10 s (replaces the region-energy estimate) and anchors to leave out (excluded).
    std::vector<std::pair<std::int64_t, float>> anchorAsl;
    std::vector<std::int64_t> excludedAnchors;
};

class CorpusSnapshot {
public:
    // Regions closer than 80 ms are merged; pauses, anchors and segment stats are derived.
    // Recordings are re-ordered by speaker (stable); RecordingId = index after ordering.
    static std::shared_ptr<const CorpusSnapshot> build(std::string corpusVersion,
                                                       std::vector<SpeakerInput> speakers,
                                                       std::vector<RecordingInput> recordings);

    const std::string& corpusVersion() const noexcept { return version_; }
    std::size_t numSpeakers() const noexcept { return speakers_.size(); }
    std::size_t numRecordings() const noexcept { return recordings_.size(); }
    std::size_t numSegments() const noexcept { return segments_.size(); }

    const SpeakerRec& speaker(SpeakerId s) const { return speakers_[s]; }
    const RecordingRec& recording(RecordingId r) const { return recordings_[r]; }
    const SegmentRec& segment(SegmentId g) const { return segments_[g]; }
    bool speakerHealthy(SpeakerId s) const { return (speakers_[s].flags & kSpeakerUnhealthy) == 0; }

    // Anchors of a speaker (contiguous SegmentId range firstAnchor .. firstAnchor + nAnchors).
    std::span<const SegmentRec> anchorsOf(SpeakerId s) const {
        const auto& sp = speakers_[s];
        return {segments_.data() + sp.firstAnchor, sp.nAnchors};
    }
    std::span<const SampleSpan> regionsOf(RecordingId r) const {
        const auto& rr = recordings_[r];
        return {regions_.data() + rr.firstRegion, rr.nRegions};
    }
    std::span<const PauseRec> pausesOf(RecordingId r) const {
        const auto& rr = recordings_[r];
        return {pauses_.data() + rr.firstPause, rr.nPauses};
    }
    double usableSpeechSeconds(SpeakerId s) const { return speakers_[s].usableSpeechS; }
    double totalSpeechSeconds() const noexcept;

    // Speech seconds within [a, b) of recording r.
    double speechSecondsIn(RecordingId r, std::int64_t a, std::int64_t b) const;

private:
    std::string version_;
    std::vector<SpeakerRec> speakers_;
    std::vector<RecordingRec> recordings_;
    std::vector<SegmentRec> segments_;
    std::vector<SampleSpan> regions_;
    std::vector<PauseRec> pauses_;
};

// Stable identity of the speakers / recordings of a loaded snapshot (database ids plus content
// keys), used to carry runtime state across a hot reload (CorpusLoader fills it).
struct CorpusIdentity {
    std::vector<std::int64_t> speakerDbId;       // SpeakerId   -> speaker.speaker_id
    std::vector<std::int64_t> recordingDbId;     // RecordingId -> recording.recording_id
    std::vector<std::string> speakerExternalId;  // SpeakerId   -> speaker.external_id
    std::vector<std::string> recordingPcmSha;    // RecordingId -> recording.pcm_sha256
};

// Index mapping between two snapshots of (nominally) the same library: -1 = absent in the new one.
// A speaker / recording is the same when its database id and its content key (external id / PCM
// hash) agree, so a replaced library never inherits state from unrelated recordings.
struct CorpusMigration {
    std::vector<std::int32_t> speaker, recording;  // old index -> new index
    std::size_t keptSpeakers = 0, keptRecordings = 0;
    SpeakerId mapSpeaker(SpeakerId s) const noexcept { return s < speaker.size() && speaker[s] >= 0 ? static_cast<SpeakerId>(speaker[s]) : kNoSpeaker; }
    RecordingId mapRecording(RecordingId r) const noexcept { return r < recording.size() && recording[r] >= 0 ? static_cast<RecordingId>(recording[r]) : kNoRecording; }
    static constexpr SpeakerId kNoSpeaker = 0xFFFFFFFFu;
    static constexpr RecordingId kNoRecording = 0xFFFFFFFFu;
};
CorpusMigration makeCorpusMigration(const CorpusIdentity& from, const CorpusIdentity& to);

// 48 kHz mono float source audio, addressed by RecordingId. Samples outside the recording
// are written as zeros. Returns false on a read error (missing file, decode failure).
class IAudioSource {
public:
    virtual ~IAudioSource() = default;
    virtual bool read(RecordingId rec, std::uint64_t startSample48k, float* dst, std::size_t n) = 0;
};

}  // namespace bf
