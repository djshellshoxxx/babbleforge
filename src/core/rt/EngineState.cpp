#include "core/rt/EngineState.h"

#include <algorithm>

namespace bf::rt {

std::string_view toString(EngineState s) noexcept {
    switch (s) {
    case EngineState::Stopped: return "STOPPED";
    case EngineState::Preparing: return "PREPARING";
    case EngineState::Ready: return "READY";
    case EngineState::Starting: return "STARTING";
    case EngineState::Running: return "RUNNING";
    case EngineState::Degraded: return "DEGRADED";
    case EngineState::DeviceLost: return "DEVICE_LOST";
    case EngineState::Stopping: return "STOPPING";
    case EngineState::Error: return "ERROR";
    }
    return "?";
}

std::string_view headlineFor(EngineState s) noexcept {
    switch (s) {
    case EngineState::Stopped: return "MASKING STOPPED";
    case EngineState::Preparing: return "PREPARING";
    case EngineState::Ready: return "READY";
    case EngineState::Starting: return "STARTING MASKING";
    case EngineState::Running: return "MASKING ACTIVE";
    case EngineState::Degraded: return "MASKING ACTIVE (DEGRADED)";
    case EngineState::DeviceLost: return "OUTPUT DEVICE LOST";
    case EngineState::Stopping: return "STOPPING";
    case EngineState::Error: return "ERROR - ACTION REQUIRED";
    }
    return "";
}

bool isLegalTransition(EngineState from, EngineState to) noexcept {
    using S = EngineState;
    if (from == to) return false;
    if (to == S::Error) return true;  // fatal in any state
    switch (from) {
    case S::Stopped: return to == S::Preparing;
    case S::Preparing: return to == S::Ready || to == S::Stopped;
    case S::Ready: return to == S::Starting || to == S::Stopping || to == S::DeviceLost;
    case S::Starting: return to == S::Running || to == S::Degraded || to == S::DeviceLost || to == S::Stopping;
    case S::Running: return to == S::Degraded || to == S::DeviceLost || to == S::Stopping;
    case S::Degraded: return to == S::Running || to == S::DeviceLost || to == S::Stopping;
    case S::DeviceLost: return to == S::Preparing || to == S::Stopped;
    case S::Stopping: return to == S::Stopped || to == S::Preparing;
    case S::Error: return to == S::Stopped;
    }
    return false;
}

std::string_view toString(CommandResult r) noexcept {
    switch (r) {
    case CommandResult::Ok: return "Ok";
    case CommandResult::IllegalTransition: return "IllegalTransition";
    case CommandResult::InvalidArgument: return "InvalidArgument";
    case CommandResult::Failed: return "Failed";
    case CommandResult::Timeout: return "Timeout";
    }
    return "?";
}

namespace {
constexpr std::array<std::string_view, kNumDegradedReasons> kCodes = {
    "corpus.reduced",    "corpus.insufficient", "corpus.none",      "preload.behind",
    "audio.xruns",       "limiter.overload",    "output.clip",      "spectrum.design",
    "analysis.stalled",  "planner.stalled",     "fallback.stationary", "engine.rateUnsupported"};
}

std::string_view degradedReasonCode(std::uint32_t bit) noexcept {
    for (int i = 0; i < kNumDegradedReasons; ++i)
        if (bit == (1u << i)) return kCodes[static_cast<std::size_t>(i)];
    return "unknown";
}

std::uint32_t degradedReasonFromCode(std::string_view code) noexcept {
    for (int i = 0; i < kNumDegradedReasons; ++i)
        if (kCodes[static_cast<std::size_t>(i)] == code) return 1u << i;
    return 0;
}

std::vector<std::string> degradedReasonCodes(std::uint32_t bits) {
    std::vector<std::string> r;
    for (int i = 0; i < kNumDegradedReasons; ++i)
        if (bits & (1u << i)) r.emplace_back(kCodes[static_cast<std::size_t>(i)]);
    return r;
}

bool EngineStateMachine::transition(EngineState to, std::string_view reason) {
    const EngineState from = state_;
    if (!isLegalTransition(from, to)) {
        if (onIllegal) onIllegal(from, to, reason);
        return false;
    }
    state_ = to;
    if (onTransition) onTransition(from, to, reason);
    return true;
}

void DegradedTracker::setCondition(std::uint32_t bits, bool active, double nowS) {
    for (int i = 0; i < kNumDegradedReasons; ++i) {
        if (!(bits & (1u << i))) continue;
        const auto k = static_cast<std::size_t>(i);
        if (active) {
            active_[k] = true;
            seen_[k] = true;
            lastActive_[k] = nowS;
        } else if (active_[k]) {
            active_[k] = false;
            lastActive_[k] = nowS;  // hysteresis runs from the moment the condition cleared
        }
    }
}

std::uint32_t DegradedTracker::reasons(double nowS) const {
    std::uint32_t r = persistent_;
    for (int i = 0; i < kNumDegradedReasons; ++i) {
        const auto k = static_cast<std::size_t>(i);
        if (!seen_[k]) continue;
        if (active_[k] || nowS - lastActive_[k] < hyst_) r |= 1u << i;
    }
    return r;
}

void DegradedTracker::clear() {
    persistent_ = 0;
    active_.fill(false);
    seen_.fill(false);
    lastActive_.fill(0.0);
}

}  // namespace bf::rt
