#pragma once
// IAudioSource wrappers for the real-time host (RELIABILITY.md §2 failure injection, soak tests).
//  - LockedAudioSource: serialises reads of a non-thread-safe source (the decode pool has 2-4
//    threads plus the urgent lane).
//  - FaultInjectingSource: per-recording faults (missing file, corrupt data after N reads) and
//    read delays (slow disk / network drive). Thread-safe (serialises the inner source).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>

#include "core/corpus/CorpusSnapshot.h"
#include "core/rt/RtCheck.h"

namespace bf::rt {

class LockedAudioSource final : public IAudioSource {
public:
    explicit LockedAudioSource(IAudioSource& inner) : inner_(inner) {}
    bool read(RecordingId rec, std::uint64_t start, float* dst, std::size_t n) override {
        std::lock_guard<CheckedMutex> lk(m_);
        return inner_.read(rec, start, dst, n);
    }

private:
    IAudioSource& inner_;
    CheckedMutex m_;
};

class FaultInjectingSource final : public IAudioSource {
public:
    enum class Fault : std::uint8_t { None, Missing, Corrupt };

    explicit FaultInjectingSource(IAudioSource& inner) : inner_(inner) {}

    // Missing: every read fails. Corrupt: reads fail after `goodReads` successful ones.
    void setFault(RecordingId rec, Fault f, int goodReads = 0) {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        faults_[rec] = {f, goodReads};
    }
    void clearFaults() {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        faults_.clear();
    }
    // Every read sleeps delayMs (outside the inner-source lock).
    void setReadDelayMs(int delayMs) noexcept { delayMs_.store(delayMs, std::memory_order_relaxed); }

    bool read(RecordingId rec, std::uint64_t start, float* dst, std::size_t n) override {
        reads_.fetch_add(1, std::memory_order_relaxed);
        if (const int d = delayMs_.load(std::memory_order_relaxed); d > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(d));
        {
            std::lock_guard<std::mutex> lk(cfgMutex_);
            const auto it = faults_.find(rec);
            if (it != faults_.end()) {
                FaultState& fs = it->second;
                if (fs.fault == Fault::Missing || (fs.fault == Fault::Corrupt && fs.goodReads <= 0)) {
                    failures_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                if (fs.fault == Fault::Corrupt) --fs.goodReads;
            }
        }
        std::lock_guard<std::mutex> lk(innerMutex_);
        return inner_.read(rec, start, dst, n);
    }

    std::uint64_t reads() const noexcept { return reads_.load(std::memory_order_relaxed); }
    std::uint64_t failures() const noexcept { return failures_.load(std::memory_order_relaxed); }

private:
    struct FaultState {
        Fault fault = Fault::None;
        int goodReads = 0;
    };
    IAudioSource& inner_;
    std::mutex cfgMutex_, innerMutex_;
    std::map<RecordingId, FaultState> faults_;
    std::atomic<int> delayMs_{0};
    std::atomic<std::uint64_t> reads_{0}, failures_{0};
};

}  // namespace bf::rt
