#pragma once
// bf::rt::IAudioBackend over JUCE (docs/REALTIME_ARCHITECTURE.md §3).
//
// Device ids are "<type>:<name>" (e.g. "Windows Audio (Exclusive Mode):Focusrite USB", "ASIO:...",
// "ALSA:...", "CoreAudio:..."). The device is selected explicitly; this backend NEVER falls back
// to another device or driver type: if the requested device cannot be opened, open() fails.
// DirectSound is not offered. ASIO is available when built with JUCE_ASIO (BF_ASIO_SDK_DIR).
//
// The device is driven through juce::AudioIODeviceType / AudioIODevice directly (types are
// created by a juce::AudioDeviceManager subclass) so that no default-device logic of
// AudioDeviceManager can ever substitute a different device.
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>

#include "core/rt/AudioBackend.h"

namespace bf::rt {

class JuceAudioBackend final : public IAudioBackend {
public:
    JuceAudioBackend();
    ~JuceAudioBackend() override;

    // Output device channel indices (0-based, strictly ascending) to drive; empty: the first
    // numOutputs channels. Applies to the next open(); open's numOutputs must equal its size.
    void setChannelMap(std::vector<int> deviceChannels);

    std::string typeName() const override { return "JUCE"; }
    std::vector<AudioDeviceInfo> devices() const override;
    BackendOpenResult open(const std::string& deviceId, double sampleRate, int bufferFrames,
                           int numOutputs) override;
    bool start(IAudioCallback* callback) override;
    void stop() override;
    void close() override;
    bool isOpen() const override;
    bool isRunning() const override;
    std::string openDeviceId() const override;
    void setDeviceEventHandler(DeviceEventHandler handler) override;
    std::uint64_t xrunCount() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bf::rt
