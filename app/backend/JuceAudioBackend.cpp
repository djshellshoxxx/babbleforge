#include "backend/JuceAudioBackend.h"

#include <algorithm>
#include <array>
#include <mutex>

namespace bf::rt {
namespace {

constexpr int kMaxCh = 64;

// Exposes the protected type factory; DirectSound is dropped.
struct TypeFactory : juce::AudioDeviceManager {
    void make(juce::OwnedArray<juce::AudioIODeviceType>& out) {
        createAudioDeviceTypes(out);
        for (int i = out.size(); --i >= 0;)
            if (out[i]->getTypeName().containsIgnoreCase("DirectSound")) out.remove(i);
    }
};

}  // namespace

struct JuceAudioBackend::Impl final : juce::AudioIODeviceCallback, juce::AudioIODeviceType::Listener {
    juce::OwnedArray<juce::AudioIODeviceType> types;
    std::vector<int> channelMap;

    std::unique_ptr<juce::AudioIODevice> device;
    juce::AudioIODeviceType* openType = nullptr;
    std::string openId, openName;
    double openRate = 0.0;
    int openFrames = 0, openOutputs = 0;
    bool lost = false;
    std::atomic<bool> running{false};

    std::atomic<IAudioCallback*> cb{nullptr};
    std::atomic<juce::AudioIODevice*> xrunDev{nullptr};
    std::atomic<int> nOutReq{0};
    std::array<float*, kMaxCh> ptrs{};

    std::mutex hMutex;
    DeviceEventHandler handler;

    Impl() {
        TypeFactory f;
        f.make(types);
        for (auto* t : types) {
            t->scanForDevices();
            t->addListener(this);
        }
    }
    ~Impl() override {
        closeDevice();
        for (auto* t : types) t->removeListener(this);
    }

    static std::string makeId(const juce::String& type, const juce::String& name) {
        return (type + ":" + name).toStdString();
    }

    void emit(DeviceEventKind k, const std::string& msg, double sr = 0.0, int bf = 0) {
        DeviceEventHandler h;
        {
            std::lock_guard<std::mutex> g(hMutex);
            h = handler;
        }
        if (!h) return;
        DeviceEvent e;
        e.kind = k;
        e.deviceId = openId;
        e.sampleRate = sr;
        e.bufferFrames = bf;
        e.message = msg;
        h(e);
    }

    void closeDevice() {
        running = false;
        cb.store(nullptr);
        if (device) {
            device->stop();
            xrunDev.store(nullptr);
            device->close();
            device.reset();
        }
        openType = nullptr;
    }

    // ---- AudioIODeviceCallback (audio thread) ----
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* out, int nOut, int n,
                                          const juce::AudioIODeviceCallbackContext&) override {
        IAudioCallback* c = cb.load(std::memory_order_acquire);
        const int want = nOutReq.load(std::memory_order_relaxed);
        const int m = std::min({nOut, want, kMaxCh});
        if (c != nullptr && m > 0) {
            for (int i = 0; i < m; ++i) ptrs[(size_t)i] = out[i];
            c->audioCallback(ptrs.data(), m, n);
        } else {
            for (int i = 0; i < m; ++i) juce::FloatVectorOperations::clear(out[i], n);
        }
        for (int i = m; i < nOut; ++i) juce::FloatVectorOperations::clear(out[i], n);  // unmapped
    }
    void audioDeviceAboutToStart(juce::AudioIODevice* d) override {
        xrunDev.store(d, std::memory_order_release);
        const double sr = d->getCurrentSampleRate();
        const int bs = d->getCurrentBufferSizeSamples();
        if (running.load() && openRate > 0.0 && std::abs(sr - openRate) > 0.5) {
            // The driver restarted at another rate: callbacks stop until the controller reopens.
            cb.store(nullptr);
            emit(DeviceEventKind::RateChanged, "device sample rate changed", sr, bs);
        } else if (running.load() && bs != openFrames) {
            emit(DeviceEventKind::BufferSizeChanged, "device buffer size changed", sr, bs);
        }
    }
    void audioDeviceStopped() override {}
    void audioDeviceError(const juce::String& message) override {
        cb.store(nullptr);
        lost = true;
        emit(DeviceEventKind::Lost, message.toStdString());
    }

    // ---- device list changes (message thread) ----
    void audioDeviceListChanged() override {
        if (!openType) return;
        openType->scanForDevices();
        const bool present = openType->getDeviceNames(false).contains(openName);
        if (!present && !lost) {
            lost = true;
            cb.store(nullptr);
            emit(DeviceEventKind::Lost, "device removed");
        } else if (present && lost) {
            lost = false;
            emit(DeviceEventKind::Returned, "device available again");
        }
    }
};

JuceAudioBackend::JuceAudioBackend() : impl_(std::make_unique<Impl>()) {}
JuceAudioBackend::~JuceAudioBackend() = default;

void JuceAudioBackend::setChannelMap(std::vector<int> m) { impl_->channelMap = std::move(m); }

std::vector<AudioDeviceInfo> JuceAudioBackend::devices() const {
    std::vector<AudioDeviceInfo> out;
    for (auto* t : impl_->types) {
        t->scanForDevices();
        for (const auto& name : t->getDeviceNames(false)) {
            AudioDeviceInfo i;
            i.type = t->getTypeName().toStdString();
            i.name = name.toStdString();
            i.id = Impl::makeId(t->getTypeName(), name);
            i.numOutputs = 0;
            if (std::unique_ptr<juce::AudioIODevice> d{t->createDevice(name, {})})
                i.numOutputs = d->getOutputChannelNames().size();
            out.push_back(std::move(i));
        }
    }
    return out;
}

BackendOpenResult JuceAudioBackend::open(const std::string& deviceId, double sampleRate, int bufferFrames,
                                         int numOutputs) {
    BackendOpenResult r;
    auto& s = *impl_;
    if (s.device) {
        r.error = "already open";
        return r;
    }
    const auto colon = deviceId.find(':');
    if (colon == std::string::npos || numOutputs <= 0 || numOutputs > kMaxCh) {
        r.error = "bad device id (expected \"<type>:<name>\") or channel count";
        return r;
    }
    const juce::String typeName(deviceId.substr(0, colon)), devName(deviceId.substr(colon + 1));
    juce::AudioIODeviceType* type = nullptr;
    for (auto* t : s.types)
        if (t->getTypeName() == typeName) type = t;
    if (!type) {
        r.error = "unknown driver type: " + typeName.toStdString();
        return r;
    }
    type->scanForDevices();
    if (!type->getDeviceNames(false).contains(devName)) {
        r.error = "device not found: " + deviceId;
        return r;
    }

    juce::BigInteger outs;
    if (s.channelMap.empty()) {
        outs.setRange(0, numOutputs, true);
    } else {
        if ((int)s.channelMap.size() != numOutputs || !std::is_sorted(s.channelMap.begin(), s.channelMap.end(), std::less_equal<int>{})) {
            r.error = "channel map must be strictly ascending and match the output count";
            return r;
        }
        for (int c : s.channelMap) outs.setBit(c, true);
    }

    std::unique_ptr<juce::AudioIODevice> dev{type->createDevice(devName, {})};
    if (!dev) {
        r.error = "cannot create device: " + deviceId;
        return r;
    }
    const auto err = dev->open({}, outs, sampleRate, bufferFrames);
    if (err.isNotEmpty()) {
        r.error = err.toStdString();
        return r;
    }
    s.device = std::move(dev);
    s.openType = type;
    s.openId = deviceId;
    s.openName = devName.toStdString();
    s.lost = false;
    s.openRate = s.device->getCurrentSampleRate();
    s.openFrames = s.device->getCurrentBufferSizeSamples();
    s.openOutputs = numOutputs;
    s.nOutReq.store(numOutputs);
    s.xrunDev.store(s.device.get());
    r.ok = true;
    r.sampleRate = s.openRate;
    r.bufferFrames = s.openFrames;
    r.numOutputs = numOutputs;
    return r;
}

bool JuceAudioBackend::start(IAudioCallback* callback) {
    auto& s = *impl_;
    if (!s.device || !callback || s.running) return false;
    s.cb.store(callback, std::memory_order_release);
    s.running = true;
    s.device->start(&s);
    if (!s.device->isPlaying()) {
        s.running = false;
        s.cb.store(nullptr);
        return false;
    }
    return true;
}

void JuceAudioBackend::stop() {
    auto& s = *impl_;
    s.running = false;
    s.cb.store(nullptr);
    if (s.device && s.device->isPlaying()) s.device->stop();
}

void JuceAudioBackend::close() { impl_->closeDevice(); }
bool JuceAudioBackend::isOpen() const { return impl_->device != nullptr && impl_->device->isOpen(); }
bool JuceAudioBackend::isRunning() const { return impl_->running.load() && impl_->device && impl_->device->isPlaying(); }
std::string JuceAudioBackend::openDeviceId() const { return impl_->device ? impl_->openId : std::string{}; }

void JuceAudioBackend::setDeviceEventHandler(DeviceEventHandler h) {
    std::lock_guard<std::mutex> g(impl_->hMutex);
    impl_->handler = std::move(h);
}

std::uint64_t JuceAudioBackend::xrunCount() const noexcept {
    auto* d = impl_->xrunDev.load(std::memory_order_acquire);
    if (!d) return 0;
    const int n = d->getXRunCount();
    return n > 0 ? (std::uint64_t)n : 0;
}

}  // namespace bf::rt
