#pragma once
// Preset safety comparison (docs/GUI.md §48): a non-blocking banner under the preset bar when
// the advanced changes differ significantly from the recommended preset:
//   "This configuration differs significantly from the recommended Open Office preset."
//   [Review Changes] [Keep Changes] [Reset]
// Never modal: nothing waits for an answer. "Keep Changes" hides it until the configuration
// is back inside the normal range (or the area changes).
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/AppState.h"
#include "model/PresetSession.h"

namespace bf::gui {

class SafetyBanner final : public juce::Component, private AppState::Listener {
public:
    SafetyBanner(AppState& state, PresetSession& session);
    ~SafetyBanner() override;

    std::function<void()> onReview;          // opens the changes dialog
    std::function<void()> onVisibilityChanged;  // the owner re-lays out
    int preferredHeight() const noexcept { return shown_ ? 44 : 0; }
    bool shown() const noexcept { return shown_; }
    void keepChanges();
    void resetToRecommended();
    juce::TextButton& reviewButton() { return review_; }
    juce::TextButton& keepButton() { return keep_; }
    juce::TextButton& resetButton() { return reset_; }
    juce::String message() const { return text_.getText(); }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void appStateChanged(unsigned changes) override;
    void update();

    AppState& state_;
    PresetSession& session_;
    juce::Label text_;
    juce::TextButton review_{"Review Changes"}, keep_{"Keep Changes"}, reset_{"Reset"};
    bool shown_ = false, dismissed_ = false;
    std::string dismissedArea_;
};

}  // namespace bf::gui
