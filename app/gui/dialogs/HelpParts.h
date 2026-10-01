#pragma once
// Shared pieces of the pages in this folder: the "?" help button (GUI §55: explain results
// rather than terminology) and the card container used by ANALYSIS.
#include <juce_gui_basics/juce_gui_basics.h>

#include "Widgets.h"

namespace bf::gui {

// A small round "?" button. Hover shows the tooltip; click opens a call-out with the text.
class HelpButton final : public juce::Button {
public:
    explicit HelpButton(const juce::String& help);
    void setHelp(const juce::String& help);
    const juce::String& help() const noexcept { return help_; }
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
    void showHelp();

private:
    juce::String help_;
};

// Rounded panel with a caps title and an optional "?" button in the corner. Owners add their
// own children and position them below `contentArea()`.
class Card : public juce::Component {
public:
    Card(const juce::String& title, const juce::String& help);
    juce::Rectangle<int> contentArea() const;
    void paint(juce::Graphics&) override;
    void resized() override;
    HelpButton& helpButton() { return help_; }
    const juce::String& title() const noexcept { return titleText_; }

private:
    juce::String titleText_;
    SectionLabel title_;
    HelpButton help_;
};

}  // namespace bf::gui
