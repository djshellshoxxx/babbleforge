#pragma once
// Audio device abstraction of the real-time host (docs/REALTIME_ARCHITECTURE.md §3). The
// concrete device backend (JUCE AudioDeviceManager or another API) implements IAudioBackend;
// the engine never depends on it. NullBackend (timer thread) and FileBackend (as fast as
// possible, WAV output) are the backends used for tests and soak runs.
//
// Threading contract:
//  - open/start/stop/close/devices: Control thread only.
//  - IAudioCallback::audioCallback: the backend's audio thread (RT context).
//  - DeviceEventHandler: any non-RT thread (never the audio callback).
//  - xrunCount(): any thread, lock-free.
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bf::rt {

struct AudioDeviceInfo {
    std::string id;     // stable identity: type + name (+ channel count) of the device
    std::string name;
    std::string type;   // driver type, e.g. "Null", "File", "ASIO"
    int numOutputs = 2;
};

struct BackendOpenResult {
    bool ok = false;
    std::string error;
    double sampleRate = 0.0;  // actual rate (may differ from the request)
    int bufferFrames = 0;     // actual buffer size
    int numOutputs = 0;       // opened output channels
};

class IAudioCallback {
public:
    virtual ~IAudioCallback() = default;
    // out[c] for c < numOutputs, numFrames samples each; the callee overwrites every channel.
    virtual void audioCallback(float* const* out, int numOutputs, int numFrames) noexcept = 0;
};

enum class DeviceEventKind : std::uint8_t {
    Lost,               // device removed / failed; callbacks stopped
    Returned,           // the same device is available again
    RateChanged,        // the device now runs at sampleRate (callbacks stopped until reopened)
    BufferSizeChanged,  // informational
    Error,
};

struct DeviceEvent {
    DeviceEventKind kind = DeviceEventKind::Error;
    std::string deviceId;
    double sampleRate = 0.0;
    int bufferFrames = 0;
    std::string message;
};

std::string_view toString(DeviceEventKind k) noexcept;

using DeviceEventHandler = std::function<void(const DeviceEvent&)>;

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    virtual std::string typeName() const = 0;
    virtual std::vector<AudioDeviceInfo> devices() const = 0;
    virtual BackendOpenResult open(const std::string& deviceId, double sampleRate, int bufferFrames,
                                   int numOutputs) = 0;
    virtual bool start(IAudioCallback* callback) = 0;
    virtual void stop() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool isRunning() const = 0;
    virtual std::string openDeviceId() const = 0;
    virtual void setDeviceEventHandler(DeviceEventHandler handler) = 0;
    // Driver-reported xruns since open (0 if the driver does not report them). Lock-free.
    virtual std::uint64_t xrunCount() const noexcept = 0;
};

}  // namespace bf::rt
