#pragma once
// RT voice renderer (docs/TALKER_ENGINE.md §9, ENGINE.md §3.2).
//
// Renders TalkerEvents into an N-channel babble bus. RT-safe: process() never allocates or
// locks. Work is split into sub-blocks on a fixed 256-frame grid anchored to the absolute
// sample counter, events start sample-accurately inside a sub-block, and every smoothing
// state advances per sample, so the output is bit-identical for any host block size
// (REALTIME_ARCHITECTURE.md §8.4). Voices are mixed in ascending eventId order.
//
//  - equal-power fades: sin(pi/2 x) in, cos(pi/2 x) out
//  - per-voice gain: one-pole 20 ms towards segGain; per-channel gains one-pole 50 ms
//  - per-slot gain vectors: default constant-power pan (ch 0/1) from the event's pan,
//    overridable with setGains() (placeholder for the Spatial Renderer)
//  - bus: x g_bnorm (linear 2 s ramp to each new value, applied at its stamped grid
//    boundary) x g_btrim (external slow trim, dB, applied at stamped boundaries, one-pole 20 ms)
//  - publishes k_a / k_s per sub-block (value at the first sample of each 256-grid cell)
//
// Real-time mode (prepare(..., realtime = true); the real-time host, TALKER_ENGINE.md §3.1,
// §8.3): event audio is filled asynchronously by the preload pool, so
//  - an event starts only when its chain holds min(length, max(fadeIn + 1 s, 1.25 s)); if not,
//    the start is postponed in 100 ms steps (lateStart, once per event) up to 1 s, then the
//    event is dropped (starvation);
//  - an active voice whose buffered audio falls below 20 ms + one sub-block (and whose chain is
//    incomplete) fades out over 20 ms (underflow / fast fade);
//  - these transitions are reported through an RT -> planner feedback FIFO;
//  - an event may carry its initial channel gains (spatial placement done by the planner).
// The offline path (realtime = false) is unchanged: bfrender fills chains synchronously.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/talker/SourcePreparer.h"
#include "core/talker/TalkerPlanner.h"

namespace bf {

inline constexpr std::size_t kVoiceMaxChannels = 32;

struct VoiceEvent {
    TalkerEvent ev;
    BlockChain* chain = nullptr;         // processed audio
    std::int64_t chainOffset = 0;        // event-relative sample held at chain position 0
    const SampleSpan* speech = nullptr;  // speech mask, event-relative
    std::uint32_t numSpeech = 0;
    bool hasGains = false;                // gains[] = initial per-channel gains (real-time mode)
    std::array<float, kVoiceMaxChannels> gains{};
};

// RT -> planner voice feedback (real-time mode).
struct VoiceFeedback {
    enum Kind : std::uint8_t { Started, LateStart, Starvation, Underflow };
    Kind kind = Started;
    std::uint32_t slot = 0;
    RecordingId recording = 0;
    std::uint64_t eventId = 0;
    std::int64_t sample = 0;  // renderer time
};

struct OccupancyFrame {
    std::int64_t sample = 0;
    std::uint16_t activeTalkers = 0;    // k_a (slots)
    std::uint16_t speakingTalkers = 0;  // k_s (slots)
};

class VoiceRenderer {
public:
    static constexpr std::size_t kSubBlock = 256;
    static constexpr std::size_t kMaxChannels = kVoiceMaxChannels;
    static constexpr std::size_t kMaxSlots = 64;
    static constexpr std::size_t kFifoSize = 1024;
    static constexpr std::size_t kOccSize = 8192;

    static constexpr std::int64_t kPostponeStep = 4800;  // 100 ms (TALKER_ENGINE §3.1)
    static constexpr int kMaxPostpones = 10;              // 1 s, then starvation
    static constexpr std::int64_t kFastFade = 960;        // 20 ms

    // Non-RT. fs is the engine rate (48 kHz in V1). realtime: see the header comment.
    void prepare(std::size_t numChannels, std::size_t maxVoices, const BlockPool* pool, double fs = 48000.0,
                 bool realtime = false);

    // Producer side (planner / preloader thread): SPSC event FIFO. False if full.
    bool pushEvent(const VoiceEvent& e) noexcept;
    // Producer: finished chains the producer may recycle (SPSC, RT -> producer).
    bool popFinished(BlockChain*& chain) noexcept;

    // Control: feed-forward count normalisation, ramped linearly over 2 s from the first
    // 256-grid boundary >= effectiveSample. immediate = no ramp (plan start).
    void setCountNorm(double gLin, std::int64_t effectiveSample, bool immediate = false) noexcept;
    // Control: slow trim (dB) applied from the first grid boundary >= effectiveSample.
    void setTrimDb(double db, std::int64_t effectiveSample) noexcept;
    // Control: drop events of epochs < minEpoch that start at or after freezeSample.
    void dropStale(std::uint32_t minEpoch, std::int64_t freezeSample) noexcept;
    // Per-slot channel gains (placeholder spatial API). n <= numChannels.
    void setGains(std::uint32_t slot, const float* gains, std::size_t n) noexcept;
    void clearGains(std::uint32_t slot) noexcept;

    // RT.
    void process(float* const* out, std::size_t nFrames) noexcept;

    std::int64_t position() const noexcept { return pos_; }
    bool popOccupancy(OccupancyFrame& f) noexcept;
    // Real-time mode, consumer = planner thread.
    bool popFeedback(VoiceFeedback& f) noexcept;
    std::uint64_t lateStarts() const noexcept { return lateStarts_.load(std::memory_order_relaxed); }
    std::uint64_t starvations() const noexcept { return starvations_.load(std::memory_order_relaxed); }
    std::uint64_t fastFades() const noexcept { return fastFades_.load(std::memory_order_relaxed); }
    std::uint64_t feedbackDropped() const noexcept { return feedbackDropped_.load(std::memory_order_relaxed); }
    std::uint64_t underflows() const noexcept { return underflows_.load(std::memory_order_relaxed); }
    std::uint64_t droppedEvents() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    std::uint32_t lastActive() const noexcept { return lastKa_.load(std::memory_order_relaxed); }
    std::uint32_t lastSpeaking() const noexcept { return lastKs_.load(std::memory_order_relaxed); }
    double currentCountNorm() const noexcept { return gbCur_; }

private:
    struct Voice {
        VoiceEvent e;
        bool used = false;
        double gain = 0.0, gainTarget = 0.0;
        std::array<float, kMaxChannels> chGain{}, chTarget{};
        std::uint32_t speechIdx = 0;
        std::uint32_t gainVer = 0;
        bool started = false;          // real-time mode: readiness confirmed
        bool fastFade = false;
        int postpones = 0;
        std::int64_t nextCheck = 0;    // real-time mode: next readiness check (renderer time)
    };
    template <class T, std::size_t N>
    struct Spsc {
        std::array<T, N> buf{};
        std::atomic<std::size_t> head{0}, tail{0};
        bool push(const T& v) noexcept {
            const std::size_t h = head.load(std::memory_order_relaxed);
            const std::size_t n = (h + 1) % N;
            if (n == tail.load(std::memory_order_acquire)) return false;
            buf[h] = v;
            head.store(n, std::memory_order_release);
            return true;
        }
        bool pop(T& v) noexcept {
            const std::size_t t = tail.load(std::memory_order_relaxed);
            if (t == head.load(std::memory_order_acquire)) return false;
            v = buf[t];
            tail.store((t + 1) % N, std::memory_order_release);
            return true;
        }
    };
    enum class CmdType : std::uint8_t { CountNorm, CountNormNow, Trim, Drop };
    struct Cmd { CmdType type = CmdType::CountNorm; double value = 0.0; std::int64_t effective = 0; std::uint32_t epoch = 0; };

    void drain() noexcept;
    void pollCommands() noexcept;
    void applyControls(std::int64_t cellStart) noexcept;
    void publishOccupancy(std::int64_t sample) noexcept;
    void renderSub(float* const* out, std::size_t offset, std::size_t n) noexcept;
    void computeTargets(Voice& v) noexcept;
    void freeVoice(std::size_t i) noexcept;
    static bool speechAt(Voice& v, std::int64_t rel) noexcept;
    // Real-time mode: readiness / postponement / fast fade. False: skip the voice this sub-block.
    bool rtGate(Voice& v, std::uint32_t vi, std::int64_t t0, std::int64_t t1, bool& freed) noexcept;
    void feedback(VoiceFeedback::Kind k, const Voice& v, std::int64_t t) noexcept;

    std::size_t nCh_ = 1;
    double fs_ = 48000.0;
    const BlockPool* pool_ = nullptr;
    std::vector<Voice> voices_;
    std::vector<std::uint32_t> order_;  // used voices sorted by eventId
    std::vector<float> src_;            // kSubBlock scratch
    std::vector<float> gainBuf_;
    std::unique_ptr<Spsc<VoiceEvent, kFifoSize>> fifo_;
    std::unique_ptr<Spsc<BlockChain*, kFifoSize>> finished_;
    std::unique_ptr<Spsc<Cmd, 64>> cmds_;
    std::unique_ptr<Spsc<OccupancyFrame, kOccSize>> occ_;
    std::unique_ptr<Spsc<VoiceFeedback, kFifoSize>> feedback_;
    bool realtime_ = false;
    std::int64_t readyMin_ = 60000;  // 1.25 s
    // Per-slot gain overrides (written by control, read by RT).
    std::unique_ptr<std::array<std::atomic<float>, kMaxSlots * kMaxChannels>> slotGains_;
    std::unique_ptr<std::array<std::atomic<std::uint32_t>, kMaxSlots>> slotGainVer_;  // 0 = none
    std::uint32_t minEpoch_ = 0;
    std::int64_t freeze_ = 0;
    // Pending (stamped) gain commands, applied at grid boundaries in arrival order.
    std::array<Cmd, 16> pending_{};
    std::size_t numPending_ = 0;
    double gbCur_ = 1.0, gbStart_ = 1.0, gbTarget_ = 1.0;
    std::int64_t gbRampPos_ = 0, gbRampLen_ = 96000;
    double trimCur_ = 1.0, trimTarget_ = 1.0;
    double coefGain_ = 0.0, coefCh_ = 0.0;
    std::int64_t pos_ = 0;
    std::atomic<std::uint64_t> underflows_{0}, dropped_{0};
    std::atomic<std::uint32_t> lastKa_{0}, lastKs_{0};
    std::atomic<std::uint64_t> lateStarts_{0}, starvations_{0}, fastFades_{0}, feedbackDropped_{0};
};

}  // namespace bf
