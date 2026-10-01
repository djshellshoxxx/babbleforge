#pragma once
// Deterministic synthetic corpus for tests and tools.
//
// K speakers x M recordings of pseudo-speech: speech regions (log-normal durations) separated
// by pauses (log-normal durations >= 80 ms, exact digital silence). Inside a region the signal
// is a harmonic "F0" tone plus speaker-specific band-shaped noise, amplitude-modulated at a
// per-region syllable rate of 3-6 Hz. The VAD regions, pauses, anchors and ASL metadata of the
// snapshot are therefore exact. Audio is synthesised procedurally on each read (random access,
// nothing is stored), so arbitrarily large corpora cost no memory.
//
// Signal::WhiteNoise fills speech regions with stationary white noise of the speaker level
// ("unit-power talkers" after ASL normalisation); its ASL is analytic.
#include <cstdint>
#include <memory>
#include <vector>

#include "core/corpus/CorpusSnapshot.h"

namespace bf {

struct SyntheticCorpusParams {
    enum class Signal { PseudoSpeech, WhiteNoise };
    std::uint32_t numSpeakers = 12;
    std::uint32_t recordingsPerSpeaker = 2;
    double recordingSeconds = 60.0;
    std::uint64_t seed = 0x5EEDC0DE5EEDULL;
    Signal signal = Signal::PseudoSpeech;
    // Measure ASL by synthesising every speech region once (exact). If false, the nominal
    // speaker level is stored (fast; metadata-only uses such as planner tests).
    bool measureAsl = true;
    double speechMedianS = 1.8, speechSigmaLn = 0.5;
    double pauseMedianS = 0.30, pauseSigmaLn = 0.6;
};

class SyntheticCorpus final : public IAudioSource {
public:
    explicit SyntheticCorpus(const SyntheticCorpusParams& p);

    std::shared_ptr<const CorpusSnapshot> snapshot() const { return snapshot_; }
    bool read(RecordingId rec, std::uint64_t startSample48k, float* dst, std::size_t n) override;

    // Test hook: make reads of a recording fail.
    void setFailing(RecordingId rec, bool failing);
    std::uint64_t readCount() const noexcept { return reads_; }

private:
    struct Speaker {
        double f0Hz, bandHz, levelLin, modLo, modHi;
        std::vector<float> fir;  // band-shaping FIR (unit energy)
    };
    struct Region {
        std::int64_t start, end;
        double modHz, modPhase, f0Hz;
    };
    struct Recording {
        std::uint32_t speaker;
        std::int64_t length;
        std::uint64_t noiseKey;
        std::vector<Region> regions;
        bool failing = false;
    };
    // Synthesises [a, b) of region `reg` into dst (dst[0] <-> sample a).
    void synthRegion(const Recording& rec, const Region& reg, std::int64_t a, std::int64_t b,
                     float* dst);

    SyntheticCorpusParams p_;
    std::vector<Speaker> speakers_;
    std::vector<Recording> recs_;  // indexed by RecordingId (already speaker-ordered)
    std::shared_ptr<const CorpusSnapshot> snapshot_;
    std::vector<float> noiseScratch_;
    std::uint64_t reads_ = 0;
};

}  // namespace bf
