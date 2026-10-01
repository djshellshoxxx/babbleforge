#include "StatusBar.h"

#include "LookAndFeel.h"

namespace bf::gui {

using rt::EngineState;

StatusBar::StatusBar(EngineBridge& bridge, AppState& state) : bridge_(bridge), state_(state) {
    bridge_.addListener(this);
    state_.addListener(this);
    engineStatusChanged(bridge_.status());
}

StatusBar::~StatusBar() {
    state_.removeListener(this);
    bridge_.removeListener(this);
}

static juce::String engineWord(EngineState s) {
    switch (s) {
    case EngineState::Stopped: return "Ready";
    case EngineState::Preparing:
    case EngineState::Ready:
    case EngineState::Starting: return "Starting";
    case EngineState::Running: return "Masking";
    case EngineState::Degraded: return "Degraded";
    case EngineState::DeviceLost: return "Device lost";
    case EngineState::Stopping: return "Stopping";
    case EngineState::Error: return "Error";
    }
    return {};
}

juce::String StatusBar::simpleText(const EngineStatus& s) {
    const juce::String dev = s.deviceName.empty() ? juce::String("not selected") : juce::String::fromUTF8(s.deviceName.c_str());
    return "Output: " + dev + "     Engine: " + engineWord(s.state) + "     CPU: " + (s.cpuPct > 50.0 ? "High" : "Normal");
}

juce::String StatusBar::advancedText(const EngineStatus& s) {
    const juce::String sep = " | ";
    juce::String t = s.driver.empty() ? juce::String("No driver") : juce::String::fromUTF8(s.driver.c_str());
    t << sep << (s.sampleRate > 0 ? juce::String(s.sampleRate / 1000.0, 1).trimCharactersAtEnd("0").trimCharactersAtEnd(".") + " kHz"
                                  : juce::String("- kHz"));
    t << sep << s.bufferFrames << " samples" << sep << "CPU " << juce::String(s.cpuPct, 1) << "%" << sep << "XRuns "
      << static_cast<juce::int64>(s.xruns) << sep << s.outputs << (s.outputs == 1 ? " output" : " outputs");
    t << sep << "Engine: " << engineWord(s.state);
    return t;
}

void StatusBar::engineStatusChanged(const EngineStatus& s) {
    EngineStatus st = s;
    if (st.outputs == 0 && state_.plan().ok) st.outputs = state_.plan().plan.spatial.activeOutputs;  // not running yet
    juce::String t = state_.advanced() ? advancedText(st) : simpleText(st);
    // GUI §40: a disabled limiter is always shown.
    juce::String w = state_.effective().limiterEnabled ? juce::String() : juce::String("Limiter disabled");
    if (t != text_ || w != warning_) {
        text_ = t;
        warning_ = w;
        repaint();
    }
}

void StatusBar::paint(juce::Graphics& g) {
    const auto& th = Theme::get();
    g.fillAll(th.sidebar);
    g.setColour(th.outline);
    g.drawHorizontalLine(0, 0.0f, (float)getWidth());
    auto r = getLocalBounds().reduced(14, 0);
    g.setFont(fonts::body(13.5f));
    if (warning_.isNotEmpty()) {
        g.setColour(th.warn);
        g.drawText(warning_, r.removeFromRight(160), juce::Justification::centredRight);
    }
    g.setColour(th.textDim);
    g.drawText(text_, r, juce::Justification::centredLeft);
}

}  // namespace bf::gui
