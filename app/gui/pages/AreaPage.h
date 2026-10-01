#pragma once
// AREA page (docs/GUI.md §14-§21 Simple, §33-§37 Advanced): area type, size, speaker setup,
// contextual recommendations (§56), the speaker map; Advanced: spatial output mode, spread,
// Speaker Variation, speaker assignment (enabled / level / delay) and zones.
#include <memory>

#include "Widgets.h"
#include "model/Catalog.h"
#include "model/OutputModel.h"
#include "pages/AreaOutputWidgets.h"
#include "pages/Page.h"

namespace bf::gui {

class AreaPage final : public Page {
public:
    explicit AreaPage(PageContext& c);
    ~AreaPage() override;

    int layoutPage(int width) override;
    void refreshFromState() override;
    void refreshStatus(const EngineStatus& s) override;

    // Test / automation access.
    juce::ComboBox& areaBox() { return area_; }
    ChoiceGroup& sizeGroup() { return size_; }
    ChoiceGroup& speakerGroup() { return speakers_; }
    ChoiceGroup& outdoorSpeakerGroup() { return outdoorSpeakers_; }
    ChoiceGroup& spatialGroup() { return spatial_; }
    ChoiceGroup& variationGroup() { return variation_; }
    MacroSlider& spreadSlider() { return spread_; }
    juce::Label& recommendationLabel() { return recommendation_; }
    SpeakerMap& map() { return map_; }
    int speakerRowCount() const { return static_cast<int>(rows_.size()); }
    juce::ToggleButton& speakerEnabled(int i);
    juce::Slider& speakerLevel(int i);
    juce::Slider& speakerDelay(int i);
    juce::TextButton& addZoneButton() { return addZone_; }
    int zoneRowCount() const { return static_cast<int>(zoneRows_.size()); }
    juce::ToggleButton& zoneEnabled(int i);
    juce::Slider& zoneLevel(int i);
    juce::Slider& zoneMix(int i);

private:
    class SpeakerRow;
    class ZoneRow;
    void rebuildRows();
    bool outdoor();

    std::vector<Choice> areas_;
    SectionLabel areaCaption_{"AREA TYPE", true};
    juce::ComboBox area_;
    juce::Label areaDesc_, areaWarning_, recommendation_;
    juce::TextButton dismiss_{"Continue"};
    juce::String dismissed_;
    ChoiceGroup size_{"Approximate Area Size", {"Small", "Medium", "Large"}, true};
    ChoiceGroup speakers_{"Speaker Setup", {"2 Speakers", "4 Speakers", "More than 4"}};
    ChoiceGroup outdoorSpeakers_{"Speaker Layout", {"2", "4", "6", "8+"}, true};  // GUI §21
    SectionLabel mapCaption_{"SPEAKER MAP"};
    SpeakerMap map_;
    std::vector<float> activity_;
    double lastTalkerMs_ = 0.0;

    // Advanced (GUI §33-§37).
    SectionLabel spatialCaption_{"SPATIAL OUTPUT", true}, speakersListCaption_{"SPEAKER ASSIGNMENT"}, zonesCaption_{"ZONES"};
    ChoiceGroup spatial_{"Spatial Output", {"Mono", "Stereo", "4 Channel", "6 Channel", "8 Channel", "Custom"}, true};
    ParamRow customCount_{"Custom speakers", 3, 16, 1, ""};
    MacroSlider spread_{"Spatial Spread", "Narrow", "Wide"};
    ChoiceGroup variation_{"Speaker Variation", {"Low", "Medium", "High"}, true};
    juce::Label variationTech_, zonesHelp_;
    std::vector<std::unique_ptr<SpeakerRow>> rows_;
    std::vector<std::unique_ptr<ZoneRow>> zoneRows_;
    juce::TextButton addZone_{"Add zone"}, removeZone_{"Remove zone"};
    bool built_ = false;
};

}  // namespace bf::gui
