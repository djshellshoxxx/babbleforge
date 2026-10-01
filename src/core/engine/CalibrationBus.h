#pragma once
// Calibration signal bus and output-source selector (docs/V2_EXTENSION_POINTS.md §2, §3).
//
//   masker (after master gain) ──┐
//                                ├─► SourceSelector (MASKER | CALIBRATION | MUTE, 500 ms
//   CalibrationBus (generators) ─┘     equal-power crossfade) ──► OutputMatrix → limiter
//
// The masker keeps running silently while CALIBRATION or MUTE is selected, so returning to
// MASKER is seamless. The calibration path goes through the output matrix and limiter.
//
// Generators (all deterministic; RT-safe process(): no allocation, no locks):
//   Sine          double phase accumulator, 10 ms raised-cosine ramps, level dBFS RMS
//   PinkNoise     seeded white noise through a unit-energy pink min-phase FIR (FirDesigner,
//                 PartitionedConvolver via StationaryMaskEngine), independent per output
//   SpeechNoise   same engine with a selectable speech-shaped (LTASS) target
//   Sweep         Farina exponential sweep (closed form), fades, silence tail; the inverse
//                 filter is an offline function (makeSweepInverseFilter)
//   ChannelId     per output in turn: 1.0 s pink burst + 0.5 s gap, optional 2-tone code
//   OctaveNoise   octave-band noise 125 Hz..8 kHz (designed windowed-sinc bandpass)
// Safety: default -26 dBFS RMS; levels above -12 dBFS need confirmLoud; hard 5 minute
// timeout; stop() fades out over 50 ms (the selector returns to the previous source within
// 50 ms), so a stop completes within 100 ms.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/config/Types.h"
#include "core/spectrum/SpectrumTarget.h"

namespace bf {

enum class OutputSource : std::uint8_t { Masker = 0, Calibration = 1, Mute = 2 };
const char* toString(OutputSource s) noexcept;

enum class CalSignal : std::uint8_t { Sine, PinkNoise, SpeechNoise, Sweep, ChannelId, OctaveNoise };
const char* toString(CalSignal s) noexcept;

struct CalibrationParams {
    CalSignal signal = CalSignal::PinkNoise;
    double levelDbfs = -26.0;          // RMS (sine, noises, channel id); sweep uses sweepPeakDbfs
    bool confirmLoud = false;          // required for a level above -12 dBFS
    std::vector<int> outputs;          // logical outputs to drive (empty = all)
    std::uint64_t seed = 1;

    double sineHz = 1000.0;            // 20 Hz .. 20 kHz
    double sweepF1Hz = 50.0, sweepF2Hz = 20000.0;
    double sweepDurationS = 10.0;      // 1 .. 30 s
    double sweepFadeS = 0.05, sweepTailS = 2.0;
    double sweepPeakDbfs = -20.0;
    int octaveBandHz = 1000;           // 125, 250, ..., 8000
    std::optional<ThirdOctArray> speechBandsDb;  // SpeechNoise target (26 bands); default LTASS
    bool codedTones = false;           // ChannelId: unique 2-tone pair per output
    bool loop = false;                 // ChannelId: repeat the sequence (until stop / timeout)
    double burstS = 1.0, gapS = 0.5;   // ChannelId timing

    static CalibrationParams sine(double hz = 1000.0, double levelDbfs = -20.0);
};

class CalibrationBus {
public:
    static constexpr double kMaxSeconds = 300.0;       // hard timeout
    static constexpr double kStopFadeS = 0.05;
    static constexpr double kRampS = 0.01;
    static constexpr double kLoudThresholdDbfs = -12.0;
    static constexpr double kMaxLevelDbfs = -3.0;
    static constexpr double kDefaultLevelDbfs = -26.0;

    CalibrationBus();
    ~CalibrationBus();
    CalibrationBus(const CalibrationBus&) = delete;
    CalibrationBus& operator=(const CalibrationBus&) = delete;

    // Non-RT. Resets everything.
    void prepare(double fs, int numOutputs);

    // Control thread: validates, designs filters, publishes the job (picked up by the next
    // process()). Returns false with *error on invalid parameters or a missing confirmation.
    // Call stop() first to replace a running signal without a click.
    bool start(const CalibrationParams& p, std::string* error = nullptr);
    // Any thread: 50 ms fade-out, then the bus is idle.
    void stop() noexcept;
    // True while a job is pending or running.
    bool running() const noexcept { return pending_.load(std::memory_order_acquire) != nullptr || active_.load(std::memory_order_acquire); }
    double elapsedSeconds() const noexcept { return static_cast<double>(elapsed_.load(std::memory_order_relaxed)) / fs_; }

    // RT: writes numOutputs planar channels of n frames (zeros when idle).
    void process(float* const* out, int n) noexcept;
    // RT: true once after a job ended (completed, stopped or timed out).
    bool consumeFinished() noexcept { return finished_.exchange(false, std::memory_order_acq_rel); }
    // Control thread: frees retired jobs.
    void collectGarbage() noexcept;

    int numOutputs() const noexcept { return nOut_; }
    double sampleRate() const noexcept { return fs_; }

    // Pure helpers (offline / tests).
    static double sweepInstantaneousHz(double f1, double f2, double durationS, double t) noexcept;
    static double sweepPhaseRad(double f1, double f2, double durationS, double t) noexcept;
    // Unique tone pair for an output (up to 32 distinct pairs).
    static std::pair<double, double> channelIdTones(int output) noexcept;
    static std::vector<float> designPinkKernel(double fs);
    static std::vector<float> designSpeechKernel(double fs, const std::optional<ThirdOctArray>& bandsDb);
    static std::vector<float> designOctaveKernel(double fs, double centreHz, std::size_t taps = 8192);

private:
    struct Job;
    void retire(Job* j) noexcept;

    double fs_ = 48000.0;
    int nOut_ = 0;
    std::atomic<Job*> pending_{nullptr};
    Job* job_ = nullptr;  // RT-owned
    Job* held_ = nullptr;  // RT: retired job waiting for a free ring slot
    std::array<std::atomic<Job*>, 8> retired_{};
    std::atomic<bool> active_{false}, stopReq_{false}, finished_{false};
    std::atomic<std::int64_t> elapsed_{0};
    std::vector<float*> optr_;
};

// Farina exponential sweep: x(t) = sin(2 pi f1 L (exp(t/L) - 1)), L = T / ln(f2/f1), with
// raised-cosine fades. Inverse filter: time-reversed sweep with exp(-t/L) amplitude
// weighting, scaled so that convolving a unit-amplitude sweep with it gives unity gain across
// [f1, f2] (a band-limited delta peaking at index N-1). Offline.
std::vector<float> makeSweepInverseFilter(double fs, double f1, double f2, double durationS, double fadeS = 0.05);
std::vector<float> makeSweepSignal(double fs, double f1, double f2, double durationS, double fadeS = 0.05);

// Source selector: MASKER | CALIBRATION | MUTE with an equal-power crossfade.
class SourceSelector {
public:
    static constexpr double kCrossfadeS = 0.5;
    void prepare(double fs) noexcept;
    // Any thread; applied by the RT thread at the start of its next block.
    void request(OutputSource s, double fadeSeconds = kCrossfadeS) noexcept;
    OutputSource requested() const noexcept;
    // RT: apply a pending request (call once per block before gains()).
    void poll() noexcept;
    // RT: switch immediately (internal, e.g. calibration finished).
    void applyNow(OutputSource s, double fadeSeconds) noexcept;
    bool steadyMasker() const noexcept { return len_ == 0 && gm_ == 1.0 && gc_ == 0.0; }
    OutputSource current() const noexcept { return cur_; }
    // RT: per-sample masker / calibration gains for the next n samples.
    void gains(int n, float* gm, float* gc) noexcept;
    double masterGainNow() const noexcept { return gm_; }
    double calibrationGainNow() const noexcept { return gc_; }

private:
    static std::uint64_t pack(std::uint32_t seq, std::uint32_t fade, OutputSource s) noexcept;
    double fs_ = 48000.0;
    std::atomic<std::uint64_t> req_{0};
    std::atomic<std::uint32_t> seq_{0};
    std::uint64_t applied_ = 0;
    OutputSource cur_ = OutputSource::Masker;
    double gm_ = 1.0, gc_ = 0.0;     // current gains
    double m0_ = 1.0, c0_ = 0.0, m1_ = 1.0, c1_ = 0.0;
    std::int64_t pos_ = 0, len_ = 0;
};

// Bus + selector + scratch: the single hook inserted into the engine (after master gain,
// before the output matrix).
class OutputSourceStage {
public:
    static constexpr double kStopCrossfadeS = CalibrationBus::kStopFadeS;
    void prepare(double fs, int numOutputs, int maxFrames);
    CalibrationBus& bus() noexcept { return bus_; }
    SourceSelector& selector() noexcept { return sel_; }

    // Control thread: starts the signal and selects CALIBRATION (500 ms crossfade).
    bool startTest(const CalibrationParams& p, std::string* error = nullptr);
    // Control thread: "STOP TEST": 50 ms fades; back to the previous source within 100 ms.
    void stopTest() noexcept;
    // RT: io = masker after master gain (modified in place), planar numOutputs x n.
    void process(float* const* io, int n) noexcept;

private:
    CalibrationBus bus_;
    SourceSelector sel_;
    std::atomic<std::uint8_t> prev_{0};
    int cap_ = 0, nOut_ = 0;
    std::vector<std::vector<float>> cal_;
    std::vector<float*> calPtr_, ioPtr_;
    std::vector<float> gm_, gc_;
};

// ---- "Test Speakers" helper (GUI §23) ----------------------------------------------------
struct TestSpeakersOptions {
    double levelDbfs = CalibrationBus::kDefaultLevelDbfs;
    bool confirmLoud = false;
    bool codedTones = false;
    bool loop = false;
    std::uint64_t seed = 1;
};
// Logical outputs that are enabled (channel enabled, not muted, zone enabled). Missing
// channel entries count as enabled; empty zones = all zones enabled.
std::vector<int> enabledOutputs(const std::vector<OutputChannel>& channels, const std::vector<OutputZone>& zones,
                                int numOutputs);
// Sequential channel-identification test over `outputs` (1.0 s pink burst + 0.5 s gap each).
bool startTestSpeakers(OutputSourceStage& stage, const std::vector<int>& outputs, const TestSpeakersOptions& opt,
                       std::string* error = nullptr);

}  // namespace bf
