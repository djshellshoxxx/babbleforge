#pragma once
// Persistent bottom status bar (docs/GUI.md §61; §40 "Limiter disabled").
//   Simple:   Output: Focusrite USB     Engine: Ready     CPU: Normal
//   Advanced: ASIO | 48 kHz | 256 samples | CPU 2.1% | XRuns 0 | 8 outputs
#include <juce_gui_basics/juce_gui_basics.h>

#include "model/EngineBridge.h"

namespace bf::gui {

class StatusBar final : public juce::Component, private EngineBridge::Listener, private AppState::Listener {
public:
    StatusBar(EngineBridge& bridge, AppState& state);
    ~StatusBar() override;
    const juce::String& text() const noexcept { return text_; }
    static juce::String simpleText(const EngineStatus& s);
    static juce::String advancedText(const EngineStatus& s);
    void paint(juce::Graphics&) override;

private:
    void engineStatusChanged(const EngineStatus& s) override;
    void appStateChanged(unsigned) override { engineStatusChanged(bridge_.status()); }
    EngineBridge& bridge_;
    AppState& state_;
    juce::String text_, warning_;
};

}  // namespace bf::gui
