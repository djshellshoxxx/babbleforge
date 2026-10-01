#include "core/rt/FileBackend.h"

#include <algorithm>
#include <chrono>

#include "core/io/WavWriter.h"

namespace bf::rt {

FileBackend::FileBackend(FileBackendConfig cfg) : cfg_(std::move(cfg)) {}

FileBackend::~FileBackend() {
    stop();
    close();
}

std::vector<AudioDeviceInfo> FileBackend::devices() const { return {AudioDeviceInfo{kDeviceId, "WAV file", "File", 64}}; }

BackendOpenResult FileBackend::open(const std::string& deviceId, double sampleRate, int bufferFrames, int numOutputs) {
    stop();
    BackendOpenResult r;
    if (deviceId != kDeviceId) {
        r.error = "unknown device '" + deviceId + "'";
        return r;
    }
    if (bufferFrames < 16 || bufferFrames > 8192 || !(sampleRate >= 8000.0 && sampleRate <= 192000.0) ||
        numOutputs < 1 || numOutputs > 64) {
        r.error = "unsupported configuration";
        return r;
    }
    fs_ = sampleRate;
    buffer_ = bufferFrames;
    nOut_ = numOutputs;
    bufs_.assign(static_cast<std::size_t>(nOut_), std::vector<float>(static_cast<std::size_t>(buffer_), 0.0f));
    ptrs_.resize(static_cast<std::size_t>(nOut_));
    for (std::size_t c = 0; c < ptrs_.size(); ++c) ptrs_[c] = bufs_[c].data();
    open_.store(true, std::memory_order_release);
    r.ok = true;
    r.sampleRate = fs_;
    r.bufferFrames = buffer_;
    r.numOutputs = nOut_;
    return r;
}

bool FileBackend::start(IAudioCallback* callback) {
    stop();
    if (!isOpen() || !callback) return false;
    callback_ = callback;
    quit_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    return true;
}

void FileBackend::stop() {
    quit_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
}

void FileBackend::close() {
    stop();
    open_.store(false, std::memory_order_release);
}

void FileBackend::setDeviceEventHandler(DeviceEventHandler handler) {
    std::lock_guard<std::mutex> lk(handlerMutex_);
    handler_ = std::move(handler);
}

void FileBackend::run() {
    WavWriter wav;
    bool writing = false;
    if (!cfg_.wavPath.empty()) {
        writing = wav.open(cfg_.wavPath, static_cast<std::uint32_t>(fs_), static_cast<std::uint16_t>(nOut_));
        if (!writing) writeFailed_.store(true, std::memory_order_release);
    }
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    std::uint64_t done = 0;
    while (!quit_.load(std::memory_order_acquire)) {
        if (cfg_.maxFrames && done >= cfg_.maxFrames) break;
        const int n = cfg_.maxFrames ? static_cast<int>(std::min<std::uint64_t>(static_cast<std::uint64_t>(buffer_),
                                                                                cfg_.maxFrames - done))
                                     : buffer_;
        if (cfg_.realtimeFactor > 0.0) {
            const double due = static_cast<double>(done) / (fs_ * cfg_.realtimeFactor);
            std::this_thread::sleep_until(t0 + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(due)));
        }
        callback_->audioCallback(ptrs_.data(), nOut_, n);
        if (writing && !wav.writePlanar(ptrs_.data(), static_cast<std::size_t>(n))) {
            writeFailed_.store(true, std::memory_order_release);
            writing = false;
        }
        done += static_cast<std::uint64_t>(n);
        frames_.store(done, std::memory_order_relaxed);
    }
    if (writing) wav.close();
    finished_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

}  // namespace bf::rt
