#pragma once
// Engine state machine (docs/RELIABILITY.md §1): states, the legal transition table (§1.3),
// DEGRADED reason bitfield with hysteresis (§1.3 "cleared for >= 30 s") and the asynchronous
// status event delivered to the UI (§1.4).
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bf::rt {

enum class EngineState : std::uint8_t {
    Stopped, Preparing, Ready, Starting, Running, Degraded, DeviceLost, Stopping, Error
};
inline constexpr int kNumEngineStates = 9;
inline constexpr std::array<EngineState, kNumEngineStates> kAllEngineStates = {
    EngineState::Stopped,  EngineState::Preparing,  EngineState::Ready,    EngineState::Starting, EngineState::Running,
    EngineState::Degraded, EngineState::DeviceLost, EngineState::Stopping, EngineState::Error};

std::string_view toString(EngineState s) noexcept;
std::string_view headlineFor(EngineState s) noexcept;  // "MASKING ACTIVE", "OUTPUT DEVICE LOST", ...
// RELIABILITY.md §1.3. Self-transitions are not transitions (false).
bool isLegalTransition(EngineState from, EngineState to) noexcept;

enum class CommandResult : std::uint8_t { Ok, IllegalTransition, InvalidArgument, Failed, Timeout };
std::string_view toString(CommandResult r) noexcept;

// DEGRADED reasons (RELIABILITY.md §2).
enum DegradedReason : std::uint32_t {
    kDegCorpusReduced = 1u << 0,        // corpus.reduced
    kDegCorpusInsufficient = 1u << 1,   // corpus.insufficient
    kDegCorpusNone = 1u << 2,           // corpus.none
    kDegPreloadBehind = 1u << 3,        // preload.behind (> 5 starvations / min)
    kDegAudioXruns = 1u << 4,           // audio.xruns (> 10 / min for 2 min)
    kDegLimiterOverload = 1u << 5,      // limiter.overload
    kDegOutputClip = 1u << 6,           // output.clip
    kDegSpectrumDesign = 1u << 7,       // spectrum.design
    kDegAnalysisStalled = 1u << 8,      // analysis.stalled
    kDegPlannerStalled = 1u << 9,       // planner.stalled
    kDegFallbackStationary = 1u << 10,  // fallback.stationary (babble replaced by stationary)
    kDegRateUnsupported = 1u << 11,     // engine.rateUnsupported (babble needs fs = 48 kHz in V1)
};
inline constexpr int kNumDegradedReasons = 12;
std::string_view degradedReasonCode(std::uint32_t bit) noexcept;
std::uint32_t degradedReasonFromCode(std::string_view code) noexcept;  // 0 if unknown
std::vector<std::string> degradedReasonCodes(std::uint32_t bits);

// Pure state holder with the legal table (Control thread).
class EngineStateMachine {
public:
    using Listener = std::function<void(EngineState from, EngineState to, std::string_view reason)>;
    EngineState state() const noexcept { return state_; }
    // Legal: state changes, onTransition fires, true. Illegal: rejected, onIllegal fires, false.
    bool transition(EngineState to, std::string_view reason);
    Listener onTransition, onIllegal;

private:
    EngineState state_ = EngineState::Stopped;
};

// DEGRADED reasons with hysteresis: a reason stays reported until its condition has been
// absent for `hysteresisS`; persistent reasons (plan-level) stay until cleared explicitly.
class DegradedTracker {
public:
    explicit DegradedTracker(double hysteresisS = 30.0) : hyst_(hysteresisS) {}
    void setHysteresis(double s) noexcept { hyst_ = s; }
    void setCondition(std::uint32_t bits, bool active, double nowS);
    void setPersistent(std::uint32_t bits) noexcept { persistent_ = bits; }
    std::uint32_t persistent() const noexcept { return persistent_; }
    std::uint32_t reasons(double nowS) const;
    void clear();

private:
    double hyst_;
    std::uint32_t persistent_ = 0;
    std::array<double, kNumDegradedReasons> lastActive_{};
    std::array<bool, kNumDegradedReasons> active_{}, seen_{};
};

// RELIABILITY.md §1.4.
struct EngineStatusEvent {
    std::uint64_t seq = 0;
    std::chrono::system_clock::time_point wall{};
    std::uint64_t engineSample = 0;
    EngineState state = EngineState::Stopped, previous = EngineState::Stopped;
    std::uint32_t degradedReasons = 0;
    std::string headline;
    std::string detailCode;
    std::vector<std::string> newReasonCodes;  // coalesced since the previous poll
};

}  // namespace bf::rt
