#pragma once
// Best-effort scheduling priority for the engine's service threads
// (docs/REALTIME_ARCHITECTURE.md §2 "Threading model": the decode/preload pool and the
// analysis thread run below normal, the logger low).
//
// The audio callback thread belongs to the driver (or the test backend) and is never changed
// here. Lowering the service threads keeps bursts of preload work (decode + resampling,
// heaviest at 88.2/96 kHz) and analysis from preempting the callback when the machine has
// fewer free cores than runnable threads. Raising priority above normal usually needs
// privileges and is not attempted. Called once at thread start, never on the RT thread.
namespace bf::rt {

enum class ThreadPriority {
    Low,          // logger, ingestion
    BelowNormal,  // decode/preload pool, analysis
    Normal,
};

// Applies to the calling thread. Returns false if the platform refused or has no mapping.
bool setCurrentThreadPriority(ThreadPriority p) noexcept;

}  // namespace bf::rt
