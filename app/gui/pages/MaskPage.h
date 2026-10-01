#pragma once
// MASK page (docs/GUI.md §10-§13 Simple; §26-§32 Advanced): masking style, Voice Amount,
// Voice Variety, Clear Voice Reduction, Mask Mix (Hybrid only); Advanced adds the Talker
// Engine, Talker Timing, Stationary Masker and Spectrum panels.
#include <array>
#include <memory>

#include "Widgets.h"
#include "model/Catalog.h"
#include "pages/Page.h"

namespace bf::gui {

class MaskPage final : public Page {
public:
    explicit MaskPage(PageContext& c);
    ~MaskPage() override;

    int layoutPage(int width) override;
    void refreshFromState() override;
    void refreshStatus(const EngineStatus& s) override;

    // Test / automation access.
    ChoiceGroup& style() { return style_; }
    MacroSlider& voiceAmount() { return voiceAmount_; }
    MacroSlider& mix() { return mix_; }
    ParamRow& pool() { return pool_; }
    ParamRow& average() { return avg_; }
    ParamRow& minimum() { return min_; }
    ParamRow& maximum() { return max_; }
    CollapsiblePanel& talkerPanel() { return talkerPanel_; }
    CollapsiblePanel& timingPanel() { return timingPanel_; }
    CollapsiblePanel& stationaryPanel() { return stationaryPanel_; }
    CollapsiblePanel& spectrumPanel() { return spectrumPanel_; }
    ChoiceGroup& spectrumMode() { return spectrumMode_; }
    juce::Slider& eqBand(int i) { return eq_[static_cast<std::size_t>(i)]; }
    juce::ToggleButton& correctionToggle() { return correction_; }

private:
    void buildTalkerPanel();
    void buildTimingPanel();
    void buildStationaryPanel();
    void buildSpectrumPanel();
    void refreshSpectrumGraph();
    void wireRow(ParamRow& r, const juce::String& name, std::function<void(double)> set);
    static int spectrumIndex(const std::string& target);

    std::vector<Choice> styles_;
    ChoiceGroup style_;
    juce::Label styleDesc_;
    MacroSlider voiceAmount_{"Voice Amount", "Few", "Many"};
    MacroSlider variety_{"Voice Variety", "Low", "High"};
    MacroSlider cvr_{"Clear Voice Reduction", "Low", "High"};
    MacroSlider mix_{"Mask Mix", "Steady", "Voices"};
    SectionLabel advancedCaption_{"ADVANCED"};

    // Talker Engine (GUI §26, §28).
    CollapsiblePanel talkerPanel_{"Talker Engine", true};
    VStack talkerBody_;
    ParamRow pool_{"Talker Pool", 2, 64, 1, ""};
    ParamRow avg_{"Average Active", 1, 32, 0.5, ""};
    ParamRow min_{"Minimum Active", 0, 32, 1, ""};
    ParamRow max_{"Maximum Active", 1, 32, 1, ""};
    juce::Label talkerInfo_;

    // Talker Timing (GUI §27).
    CollapsiblePanel timingPanel_{"Talker Timing", false};
    VStack timingBody_;
    ParamRow gap_{"Maximum Internal Gap", 20, 1000, 5, " ms"};
    ParamRow segMin_{"Minimum Segment Length", 0.5, 30, 0.5, " s"};
    ParamRow segMax_{"Maximum Segment Length", 1, 60, 0.5, " s"};
    ParamRow gainVar_{"Talker Gain Variation", 0, 6, 0.1, " dB"};
    ParamRow fade_{"Fade Time", 10, 1000, 5, " ms"};

    // Stationary masker (GUI §29).
    CollapsiblePanel stationaryPanel_{"Stationary Masker", false};
    VStack stationaryBody_;
    juce::ToggleButton stationaryOn_{"Enabled"};
    ParamRow energy_{"Energy Contribution", 0, 100, 1, " %"};
    ChoiceGroup stationarySpectrum_{"Spectrum", {"Match Speech", "LTASS", "Privacy Curve", "Custom"}};
    ChoiceGroup seed_{"Noise Seed", {"Random", "Fixed"}, true};

    // Spectrum (GUI §30-§32).
    CollapsiblePanel spectrumPanel_{"Spectrum", false};
    VStack spectrumBody_;
    ChoiceGroup spectrumMode_{"Mode", {"Speech Matched", "Universal LTASS", "Privacy -5 dB/oct", "Privacy -7 dB/oct",
                                       "Privacy -9 dB/oct", "Custom"}};
    SpectrumGraph graph_;
    struct EqBox final : juce::Component {
        std::function<void()> layout;
        void resized() override {
            if (layout) layout();
        }
    } eqBox_;
    std::array<juce::Slider, 7> eq_;
    std::array<juce::Label, 7> eqLabels_;
    juce::ToggleButton eqWide_{juce::String::fromUTF8("Allow up to \xc2\xb1" "12 dB")};
    juce::ToggleButton correction_{"Maintain Target Spectrum"};
    ChoiceGroup speed_{"Correction Speed", {"Slow", "Normal", "Fast"}, true};

    bool eqSilent_ = false;
};

}  // namespace bf::gui
