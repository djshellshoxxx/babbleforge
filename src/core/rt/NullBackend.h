#pragma once
// NullBackend: a timer thread that calls the audio callback at the buffer cadence
// (std::chrono::steady_clock deadlines) and discards the output, with fault injection for
// reliability tests (RELIABILITY.md §2): device loss / return, device-initiated sample-rate
// change, callback stall, wake-up jitter. The emulated device holds `periods` buffers of audio
// (default 2, double buffering): it reports an xrun whenever a wake-up is later than
// (periods - 1) buffer periods past its deadline (a real device would have run dry). The timer
// thread runs at normal priority, so host scheduling hiccups show up as xruns with 2 periods.
//
// An optional observer sees every output buffer on the audio thread (tests: level and
// spectrum checks); it must be RT-safe (e.g. push into a preallocated ring).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/rt/AudioBackend.h"

namespace bf::rt {

class NullBackend final : public IAudioBackend {
public:
    class Observer {
    public:
        virtual ~Observer() = default;
        virtual void onOutput(const float* const* out, int numOutputs, int numFrames) noexcept = 0;
    };

    NullBackend();
    ~NullBackend() override;

    // Device list management (Control / test thread).
    void addDevice(const AudioDeviceInfo& info);
    void setObserver(Observer* obs) noexcept { observer_.store(obs, std::memory_order_release); }

    std::string typeName() const override { return "Null"; }
    std::vector<AudioDeviceInfo> devices() const override;
    BackendOpenResult open(const std::string& deviceId, double sampleRate, int bufferFrames, int numOutputs) override;
    bool start(IAudioCallback* callback) override;
    void stop() override;
    void close() override;
    bool isOpen() const override;
    bool isRunning() const override { return running_.load(std::memory_order_acquire); }
    std::string openDeviceId() const override;
    void setDeviceEventHandler(DeviceEventHandler handler) override;
    std::uint64_t xrunCount() const noexcept override { return xruns_.load(std::memory_order_relaxed); }

    // ---- fault injection (any non-RT thread) ----
    // The device disappears: callbacks stop, Lost is reported, open() fails until it returns.
    void injectDeviceLost();
    // The same device is available again: Returned is reported.
    void injectDeviceReturn();
    // The device switches to newRate: callbacks stop, RateChanged is reported; open() then
    // yields newRate whatever rate is requested.
    void injectRateChange(double newRate);
    // The audio thread sleeps `ms` once before its next callback.
    void injectStall(int ms);
    // Every wake-up is delayed by a uniform random amount in [0, maxMs].
    void setJitterMs(double maxMs);
    // Hardware buffering of the emulated device (>= 2 periods).
    void setDevicePeriods(int periods) noexcept { periods_.store(periods < 2 ? 2 : periods, std::memory_order_relaxed); }

    // ---- observation ----
    std::uint64_t callbacks() const noexcept { return callbacks_.load(std::memory_order_relaxed); }
    // Late wake-ups of the emulated device's timer thread that ran the device dry although the
    // callbacks themselves were on time (host scheduling; not counted as xruns).
    std::uint64_t timerLateCount() const noexcept { return timerLate_.load(std::memory_order_relaxed); }
    std::uint64_t framesProcessed() const noexcept { return frames_.load(std::memory_order_relaxed); }
    std::vector<std::string> openHistory() const;  // every device id passed to a successful open()
    double sampleRate() const;
    int bufferFrames() const;

private:
    void run();
    void emit(const DeviceEvent& e);
    void joinThread();

    mutable std::mutex mutex_;
    std::vector<AudioDeviceInfo> devices_;
    std::vector<std::string> missing_;       // device ids currently unavailable
    std::vector<std::string> openHistory_;
    std::string openId_;
    bool open_ = false;
    double fs_ = 48000.0, forcedRate_ = 0.0;
    int buffer_ = 512, nOut_ = 2;
    DeviceEventHandler handler_;
    std::mutex handlerMutex_;

    IAudioCallback* callback_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false}, quit_{false};
    std::atomic<int> stallMs_{0};
    std::atomic<int> periods_{2};
    std::atomic<double> jitterMs_{0.0};
    std::atomic<std::uint64_t> xruns_{0}, timerLate_{0}, callbacks_{0}, frames_{0};
    std::atomic<Observer*> observer_{nullptr};
    std::vector<std::vector<float>> bufs_;
    std::vector<float*> ptrs_;
};

}  // namespace bf::rt
