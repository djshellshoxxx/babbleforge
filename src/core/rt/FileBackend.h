#pragma once
// FileBackend: runs the audio callback as fast as possible (or at a fixed multiple of real
// time) on its own thread and optionally writes the output to a float32 WAV file. Used for
// soak tests and unattended renders through the real-time host. Device id: "File".
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/rt/AudioBackend.h"

namespace bf::rt {

struct FileBackendConfig {
    std::string wavPath;           // empty: discard the output
    std::uint64_t maxFrames = 0;   // stop after this many frames (0: until stop())
    double realtimeFactor = 0.0;   // 0: as fast as possible; k > 0: paced at k x real time
};

class FileBackend final : public IAudioBackend {
public:
    static constexpr const char* kDeviceId = "File";

    explicit FileBackend(FileBackendConfig cfg = {});
    ~FileBackend() override;

    std::string typeName() const override { return "File"; }
    std::vector<AudioDeviceInfo> devices() const override;
    BackendOpenResult open(const std::string& deviceId, double sampleRate, int bufferFrames, int numOutputs) override;
    bool start(IAudioCallback* callback) override;
    void stop() override;
    void close() override;
    bool isOpen() const override { return open_.load(std::memory_order_acquire); }
    bool isRunning() const override { return running_.load(std::memory_order_acquire); }
    std::string openDeviceId() const override { return isOpen() ? kDeviceId : ""; }
    void setDeviceEventHandler(DeviceEventHandler handler) override;
    std::uint64_t xrunCount() const noexcept override { return 0; }

    std::uint64_t framesProcessed() const noexcept { return frames_.load(std::memory_order_relaxed); }
    bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }
    bool writeFailed() const noexcept { return writeFailed_.load(std::memory_order_acquire); }

private:
    void run();

    FileBackendConfig cfg_;
    double fs_ = 48000.0;
    int buffer_ = 512, nOut_ = 2;
    IAudioCallback* callback_ = nullptr;
    std::thread thread_;
    std::atomic<bool> open_{false}, running_{false}, quit_{false}, finished_{false}, writeFailed_{false};
    std::atomic<std::uint64_t> frames_{0};
    std::vector<std::vector<float>> bufs_;
    std::vector<float*> ptrs_;
    std::mutex handlerMutex_;
    DeviceEventHandler handler_;
};

}  // namespace bf::rt
