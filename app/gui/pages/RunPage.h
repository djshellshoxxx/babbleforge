#pragma once
// RUN page (docs/GUI.md §3-§9): area, mask type, START/STOP with status and elapsed time,
// Masking Strength, Character, Coverage, current activity and the Recommended quick cards.
#include <array>

#include "Widgets.h"
#include "model/Catalog.h"
#include "pages/Page.h"

namespace bf::gui {

class RunPage final : public Page {
public:
    explicit RunPage(PageContext& c);

    int layoutPage(int width) override;
    void refreshFromState() override;
    void refreshStatus(const EngineStatus& s) override;

    // Test / automation access.
    juce::ComboBox& areaBox() { return area_; }
    juce::ComboBox& maskBox() { return mask_; }
    juce::TextButton& startStopButton() { return startStop_; }
    MacroSlider& strengthSlider() { return strength_; }
    MacroSlider& characterSlider() { return character_; }
    ChoiceGroup& coverage() { return coverage_; }
    juce::Label& statusLabel() { return status_; }
    juce::Label& activityLabel() { return activityText_; }
    juce::TextButton& card(int i) { return cards_[static_cast<std::size_t>(i)]; }

private:
    std::vector<Choice> areas_, masks_;
    SectionLabel areaCaption_{"AREA", true}, maskCaption_{"MASK TYPE", true};
    juce::ComboBox area_, mask_;
    juce::Label areaAdvice_, maskDesc_;
    juce::Label status_, sub_, elapsed_;
    juce::TextButton startStop_, reconnect_{"Reconnect"}, chooseDevice_{"Choose Device"};
    MacroSlider strength_{"Masking Strength", "Quiet", "Loud", true};
    MacroSlider character_{"Character", "Natural", "Dense"};
    ChoiceGroup coverage_{"Coverage", {"Stereo", "Multi-Speaker"}};
    SectionLabel activityCaption_{"CURRENT ACTIVITY"}, cardsCaption_{"RECOMMENDED"};
    ActivityBar activity_;
    juce::Label activityText_;
    std::array<juce::TextButton, 3> cards_;
    static constexpr const char* kCardIds[3] = {"balanced", "natural", "dense"};
};

}  // namespace bf::gui
