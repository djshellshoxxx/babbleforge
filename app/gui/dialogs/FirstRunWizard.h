#pragma once
// First-run wizard (docs/GUI.md §60): four short pages shown when settings.json does not exist.
//   1 Welcome   2 Select Output (+ Test)   3 Choose Your Area   4 Recommended Setup [Start]
// No account, no tutorial: Esc / closing the dialog keeps the defaults. The wizard writes
// settings.json as soon as it opens, so it is never shown twice.
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Widgets.h"
#include "model/AppSettings.h"
#include "model/AppState.h"
#include "model/Catalog.h"
#include "model/EngineBridge.h"

namespace bf::gui {

class FirstRunWizard final : public juce::Component, private juce::Timer {
public:
    FirstRunWizard(AppState& state, AppSettings& settings, EngineBridge& bridge);
    ~FirstRunWizard() override;

    std::function<void()> requestClose;

    int page() const noexcept { return page_; }  // 0..3
    void next();
    void back();
    void selectDevice(int index);
    void selectArea(const std::string& areaId);
    void testOutput();
    // Applies the choices (area, output device) and optionally starts masking.
    void finish(bool start);
    juce::ComboBox& deviceBox() { return device_; }
    juce::TextButton& nextButton() { return next_; }
    juce::TextButton& backButton() { return back_; }
    juce::TextButton& testButton() { return test_; }
    ChoiceGroup& areaGroup() { return areas_; }
    juce::String setupText() const { return setup_.getText(); }
    juce::String titleText() const { return title_.getText(); }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void showPage(int p);

    AppState& state_;
    AppSettings& settings_;
    EngineBridge& bridge_;
    std::vector<Choice> areaList_;
    std::vector<rt::AudioDeviceInfo> devices_;
    std::string areaId_, deviceId_;
    int page_ = 0;
    double testEndMs_ = 0.0;
    bool testing_ = false, finished_ = false;

    juce::Label title_, text_, setup_;
    juce::ComboBox device_;
    juce::TextButton test_{"Test"}, next_{"Next"}, back_{"Back"};
    ChoiceGroup areas_;
};

std::unique_ptr<OverlayDialog> makeFirstRunDialog(AppState& state, AppSettings& settings, EngineBridge& bridge);

}  // namespace bf::gui
