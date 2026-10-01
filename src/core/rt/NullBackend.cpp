#include "core/rt/NullBackend.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace bf::rt {

std::string_view toString(DeviceEventKind k) noexcept {
    switch (k) {
    case DeviceEventKind::Lost: return "lost";
    case DeviceEventKind::Returned: return "returned";
    case DeviceEventKind::RateChanged: return "rateChanged";
    case DeviceEventKind::BufferSizeChanged: return "bufferSizeChanged";
    case DeviceEventKind::Error: return "error";
    }
    return "error";
}

NullBackend::NullBackend() {
    devices_.push_back(AudioDeviceInfo{"Null:Null Output:32", "Null Output", "Null", 32});
}

NullBackend::~NullBackend() {
    stop();
    close();
}

void NullBackend::addDevice(const AudioDeviceInfo& info) {
    std::lock_guard<std::mutex> lk(mutex_);
    devices_.push_back(info);
}

std::vector<AudioDeviceInfo> NullBackend::devices() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<AudioDeviceInfo> r;
    for (const auto& d : devices_)
        if (std::find(missing_.begin(), missing_.end(), d.id) == missing_.end()) r.push_back(d);
    return r;
}

BackendOpenResult NullBackend::open(const std::string& deviceId, double sampleRate, int bufferFrames, int numOutputs) {
    stop();
    std::lock_guard<std::mutex> lk(mutex_);
    BackendOpenResult r;
    const auto it = std::find_if(devices_.begin(), devices_.end(), [&](const AudioDeviceInfo& d) { return d.id == deviceId; });
    if (it == devices_.end()) {
        r.error = "unknown device '" + deviceId + "'";
        return r;
    }
    if (std::find(missing_.begin(), missing_.end(), deviceId) != missing_.end()) {
        r.error = "device '" + deviceId + "' is not available";
        return r;
    }
    if (bufferFrames < 16 || bufferFrames > 8192 || !(sampleRate >= 8000.0 && sampleRate <= 192000.0)) {
        r.error = "unsupported buffer size or sample rate";
        return r;
    }
    fs_ = forcedRate_ > 0.0 ? forcedRate_ : sampleRate;
    buffer_ = bufferFrames;
    nOut_ = std::clamp(numOutputs, 1, it->numOutputs);
    bufs_.assign(static_cast<std::size_t>(nOut_), std::vector<float>(static_cast<std::size_t>(buffer_), 0.0f));
    ptrs_.resize(static_cast<std::size_t>(nOut_));
    for (std::size_t c = 0; c < ptrs_.size(); ++c) ptrs_[c] = bufs_[c].data();
    open_ = true;
    openId_ = deviceId;
    openHistory_.push_back(deviceId);
    xruns_.store(0, std::memory_order_relaxed);
    r.ok = true;
    r.sampleRate = fs_;
    r.bufferFrames = buffer_;
    r.numOutputs = nOut_;
    return r;
}

bool NullBackend::start(IAudioCallback* callback) {
    stop();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!open_ || !callback) return false;
        if (std::find(missing_.begin(), missing_.end(), openId_) != missing_.end()) return false;
        callback_ = callback;
    }
    quit_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    return true;
}

void NullBackend::joinThread() {
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

void NullBackend::stop() {
    quit_.store(true, std::memory_order_release);
    joinThread();
    running_.store(false, std::memory_order_release);
}

void NullBackend::close() {
    stop();
    std::lock_guard<std::mutex> lk(mutex_);
    open_ = false;
    openId_.clear();
    callback_ = nullptr;
}

bool NullBackend::isOpen() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return open_;
}

std::string NullBackend::openDeviceId() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return openId_;
}

void NullBackend::setDeviceEventHandler(DeviceEventHandler handler) {
    std::lock_guard<std::mutex> lk(handlerMutex_);
    handler_ = std::move(handler);
}

void NullBackend::emit(const DeviceEvent& e) {
    DeviceEventHandler h;
    {
        std::lock_guard<std::mutex> lk(handlerMutex_);
        h = handler_;
    }
    if (h) h(e);
}

void NullBackend::injectDeviceLost() {
    std::string id;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        id = openId_.empty() && !devices_.empty() ? devices_.front().id : openId_;
        if (std::find(missing_.begin(), missing_.end(), id) == missing_.end()) missing_.push_back(id);
    }
    stop();
    emit(DeviceEvent{DeviceEventKind::Lost, id, 0.0, 0, "device removed"});
}

void NullBackend::injectDeviceReturn() {
    std::string id;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (missing_.empty()) return;
        id = missing_.front();
        missing_.erase(missing_.begin());
    }
    emit(DeviceEvent{DeviceEventKind::Returned, id, 0.0, 0, "device available again"});
}

void NullBackend::injectRateChange(double newRate) {
    std::string id;
    int buf = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        forcedRate_ = newRate;
        fs_ = newRate;
        id = openId_;
        buf = buffer_;
    }
    stop();
    emit(DeviceEvent{DeviceEventKind::RateChanged, id, newRate, buf, "device sample rate changed"});
}

void NullBackend::injectStall(int ms) { stallMs_.store(ms, std::memory_order_release); }
void NullBackend::setJitterMs(double maxMs) { jitterMs_.store(std::max(0.0, maxMs), std::memory_order_relaxed); }

std::vector<std::string> NullBackend::openHistory() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return openHistory_;
}

double NullBackend::sampleRate() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return fs_;
}

int NullBackend::bufferFrames() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return buffer_;
}

void NullBackend::run() {
    using clock = std::chrono::steady_clock;
    double fs;
    int frames, nOut;
    IAudioCallback* cb;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        fs = fs_;
        frames = buffer_;
        nOut = nOut_;
        cb = callback_;
    }
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(static_cast<double>(frames) / fs));
    std::minstd_rand rng(12345u);
#ifdef _WIN32
    // The default Windows sleep granularity (~15.6 ms) exceeds a typical callback period and would
    // register false xruns; a high-resolution waitable timer keeps the simulated device on time.
    HANDLE hrTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const auto sleepUntil = [&](clock::time_point t) {
        if (hrTimer) {
            const auto d = t - clock::now();
            if (d <= clock::duration::zero()) return;
            LARGE_INTEGER due;
            due.QuadPart = -std::max<long long>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(d).count() / 100);
            if (SetWaitableTimer(hrTimer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(hrTimer, INFINITE);
                return;
            }
        }
        std::this_thread::sleep_until(t);
    };
#else
    const auto sleepUntil = [](clock::time_point t) { std::this_thread::sleep_until(t); };
#endif
    auto deadline = clock::now();
    while (!quit_.load(std::memory_order_acquire)) {
        if (const int st = stallMs_.exchange(0, std::memory_order_acq_rel); st > 0) {
            // A stalled driver: sleep in small steps so stop() stays responsive.
            const auto until = clock::now() + std::chrono::milliseconds(st);
            while (clock::now() < until && !quit_.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            deadline = clock::now();
        }
        const double jit = jitterMs_.load(std::memory_order_relaxed);
        auto wake = deadline;
        if (jit > 0.0) {
            const double u = static_cast<double>(rng() - std::minstd_rand::min()) /
                             static_cast<double>(std::minstd_rand::max() - std::minstd_rand::min());
            wake += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double, std::milli>(u * jit));
        }
        sleepUntil(wake);
        if (quit_.load(std::memory_order_acquire)) break;
        const auto now = clock::now();
        if (now > deadline + (periods_.load(std::memory_order_relaxed) - 1) * period) {  // the device ran dry
            xruns_.fetch_add(1, std::memory_order_relaxed);
            deadline = now;
        }
        cb->audioCallback(ptrs_.data(), nOut, frames);
        if (Observer* o = observer_.load(std::memory_order_acquire)) o->onOutput(ptrs_.data(), nOut, frames);
        callbacks_.fetch_add(1, std::memory_order_relaxed);
        frames_.fetch_add(static_cast<std::uint64_t>(frames), std::memory_order_relaxed);
        deadline += period;
    }
#ifdef _WIN32
    if (hrTimer) CloseHandle(hrTimer);
#endif
    running_.store(false, std::memory_order_release);
}

}  // namespace bf::rt
