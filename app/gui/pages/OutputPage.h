#pragma once
// OUTPUT page (docs/GUI.md §22-§24 Simple, §38-§40 Advanced, §58-§59 device lost / degraded):
// device chooser (explicit selection, never auto-switched), Stereo / Multi-Speaker mode, master
// level, per-channel meters (LOW / GOOD / HIGH in Simple; RMS, LUFS-S, true peak and limiter GR
// in Advanced), driver / sample rate / buffer / channels, latency and dropouts, limiter and
// TEST SPEAKERS with a prominent STOP TEST.
#include <memory>

#include "Widgets.h"
#include "pages/AreaOutputWidgets.h"
#include "pages/Page.h"

namespace bf::gui {

class OutputPage final : public Page {
public:
    explicit OutputPage(PageContext& c);

    int layoutPage(int width) override;
    void refreshFromState() override;
    void refreshStatus(const EngineStatus& s) override;

    // Test / automation access.
    juce::ComboBox& deviceBox() { return device_; }
    ChoiceGroup& modeGroup() { return mode_; }
    MacroSlider& masterSlider() { return master_; }
    juce::Label& bannerLabel() { return banner_; }
    juce::TextButton& reconnectButton() { return reconnect_; }
    juce::TextButton& chooseDeviceButton() { return chooseDevice_; }
    juce::TextButton& testButton() { return test_; }
    juce::TextButton& stopTestButton() { return stopTest_; }
    juce::Label& statusLabel() { return status_; }
    juce::Label& bandLabel() { return band_; }
    juce::Label& testLabel() { return testInfo_; }
    juce::ToggleButton& limiterToggle() { return limiterOn_; }
    juce::Slider& ceilingSlider() { return ceiling_; }
    juce::Label& readout(const juce::String& name);  // "RMS", "LUFS-S", "True Peak", "Limiter", "Driver", ...
    int meterCount() const { return static_cast<int>(meters_.size()); }
    ChannelMeter& meter(int i) { return *meters_[static_cast<std::size_t>(i)]; }

private:
    void fillDevices();
    void rebuildMeters();
    struct Readout {
        juce::String name;
        std::unique_ptr<SectionLabel> caption;
        std::unique_ptr<juce::Label> value;
    };
    Readout& addReadout(const juce::String& name, bool technical);

    // Device-lost / degraded banner (GUI §58, §59).
    juce::Label banner_, bannerSub_;
    juce::TextButton reconnect_{"Reconnect"}, chooseDevice_{"Choose Device"};

    SectionLabel deviceCaption_{"OUTPUT DEVICE", true};
    juce::ComboBox device_;
    juce::TextButton refresh_{"Refresh"};
    std::vector<rt::AudioDeviceInfo> devices_;
    ChoiceGroup mode_{"Mode", {"Stereo", "Multi-Speaker"}};
    MacroSlider master_{"Master Level", "Quiet", "Loud"};
    SectionLabel metersCaption_{"OUTPUT LEVEL"};
    std::vector<std::unique_ptr<ChannelMeter>> meters_;
    juce::Label band_, status_;
    SectionLabel testCaption_{"TEST SPEAKERS"};
    juce::TextButton test_{"TEST SPEAKERS"}, stopTest_{"STOP TEST"};
    juce::Label testInfo_;

    // Advanced.
    SectionLabel techCaption_{"MASTER LEVEL METERS", true}, devInfoCaption_{"AUDIO DEVICE", true}, limiterCaption_{"OUTPUT LIMITER", true};
    std::vector<Readout> readouts_;
    juce::ToggleButton limiterOn_{"Enabled"};
    juce::Slider ceiling_;
    juce::Label ceilingLabel_;
    bool silent_ = false;
    EngineStatus last_;
};

}  // namespace bf::gui
