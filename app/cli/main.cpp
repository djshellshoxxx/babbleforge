// babbleforge-cli: real-time player over JuceAudioBackend (no GUI).
//   babbleforge-cli --list-devices | list
//   babbleforge-cli play --device "<type>:<name>" --preset file --corpus root [--seconds N]
//                        [--rate 48000] [--buffer 512] [--outputs 2] [--channels 0,1] [--data dir]
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <juce_events/juce_events.h>

#include "backend/JuceAudioBackend.h"
#include "core/config/DataSet.h"
#include "core/corpus/CorpusLoader.h"
#include "core/rt/EngineController.h"

#ifndef BF_DEFAULT_DATA_DIR
#define BF_DEFAULT_DATA_DIR "resources/data"
#endif

namespace {
std::atomic<bool> gStop{false};
void onSignal(int) { gStop = true; }

int listDevices() {
    bf::rt::JuceAudioBackend be;
    const auto devs = be.devices();
    if (devs.empty()) std::cout << "(no audio devices found)\n";
    for (const auto& d : devs) std::cout << d.id << "  [" << d.numOutputs << " out]\n";
    return 0;
}

void usage() {
    std::cerr << "usage: babbleforge-cli --list-devices\n"
                 "       babbleforge-cli play --device \"<type>:<name>\" --preset file --corpus root\n"
                 "                        [--seconds N] [--rate 48000] [--buffer 512] [--outputs 2]\n"
                 "                        [--channels 0,1] [--data dir]\n";
}
}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::string cmd, device, presetPath, corpusRoot, dataDir = BF_DEFAULT_DATA_DIR, channels;
    double seconds = 0.0, rate = 48000.0;
    int buffer = 512, outputs = 2;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };
        if (a == "--list-devices" || a == "list") cmd = "list";
        else if (a == "play") cmd = "play";
        else if (a == "--device") device = next();
        else if (a == "--preset") presetPath = next();
        else if (a == "--corpus") corpusRoot = next();
        else if (a == "--data") dataDir = next();
        else if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--rate") rate = std::atof(next().c_str());
        else if (a == "--buffer") buffer = std::atoi(next().c_str());
        else if (a == "--outputs") outputs = std::atoi(next().c_str());
        else if (a == "--channels") channels = next();
        else { usage(); return 2; }
    }
    if (cmd == "list") return listDevices();
    if (cmd != "play" || device.empty() || presetPath.empty() || corpusRoot.empty()) {
        usage();
        return 2;
    }

    const bf::DataSetResult dsr = bf::loadDataSet(dataDir);
    if (!dsr.ok) { std::cerr << "data set: " << dsr.error << "\n"; return 2; }

    nlohmann::json preset;
    {
        std::ifstream f(presetPath);
        if (!f) { std::cerr << "cannot read preset " << presetPath << "\n"; return 2; }
        std::stringstream ss; ss << f.rdbuf();
        preset = nlohmann::json::parse(ss.str(), nullptr, false);
        if (preset.is_discarded()) { std::cerr << "preset is not valid JSON\n"; return 2; }
    }
    bf::LoadedCorpus corpus;
    std::string err;
    if (!bf::loadCorpus(corpusRoot, corpus, &err)) { std::cerr << "corpus: " << err << "\n"; return 2; }

    bf::rt::JuceAudioBackend backend;
    if (!channels.empty()) {
        std::vector<int> map;
        std::stringstream ss(channels);
        for (std::string t; std::getline(ss, t, ',');) map.push_back(std::atoi(t.c_str()));
        outputs = (int)map.size();
        backend.setChannelMap(map);
    }

    bf::rt::EngineControllerConfig cfg;
    cfg.dataSet = &dsr.data;
    cfg.corpus = corpus.snapshot;
    cfg.audio = corpus.audio.get();
    cfg.backend = &backend;
    cfg.deviceId = device;
    cfg.sampleRate = rate;
    cfg.bufferFrames = buffer;
    cfg.preset = preset;
    bf::rt::EngineController ctl(cfg);
    backend.setDeviceEventHandler([&ctl](const bf::rt::DeviceEvent& e) { ctl.onDeviceEvent(e); });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::cout << "starting on " << device << " (seed " << ctl.sessionSeed() << ")\n";
    if (ctl.start() != bf::rt::CommandResult::Ok) { std::cerr << "start rejected\n"; return 1; }

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto nextMeter = t0 + std::chrono::seconds(1);
    int rc = 0;
    while (!gStop) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);  // device-list change events
        while (auto st = ctl.pollStatus())
            std::cout << "[status] " << bf::rt::toString(st->state) << " " << st->headline
                      << (st->detailCode.empty() ? "" : " (" + st->detailCode + ")") << "\n";
        const auto now = clock::now();
        const double el = std::chrono::duration<double>(now - t0).count();
        if (now >= nextMeter) {
            nextMeter += std::chrono::seconds(1);
            const auto m = ctl.metrics();
            std::printf("t=%5.0fs %-9s cb=%llu xrun=%llu load p50=%.1f%% p99=%.1f%% starv=%llu\n", el,
                        std::string(bf::rt::toString(ctl.state())).c_str(), (unsigned long long)m.callbacks,
                        (unsigned long long)backend.xrunCount(), m.dspLoadP50 * 100.0, m.dspLoadP99 * 100.0,
                        (unsigned long long)m.starvations);
            std::fflush(stdout);
        }
        if (ctl.state() == bf::rt::EngineState::Error) { rc = 1; break; }
        if (seconds > 0.0 && el >= seconds) break;
    }
    std::cout << "stopping\n";
    ctl.stop();
    ctl.waitForState(bf::rt::EngineState::Stopped, 5.0);
    return rc;
}
