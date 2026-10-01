#pragma once
// BabbleForge look & feel: dark, high-contrast theme with large primary controls
// (docs/GUI.md §63 visual priority: Area, Mask type, Start/Stop, Strength are emphasised;
// everything else is quieter). Scaling uses the JUCE desktop scale factor (AppSettings).
//
// Components opt into emphasis with properties:
//   "bf.primary"  (bool)  big accent Start/Stop button, thick accent slider
//   "bf.running"  (bool)  Start/Stop button in its "stop" colour
//   "bf.segment"  (bool)  segmented toggle (SIMPLE | ADVANCED, sidebar entries)
//   "bf.card"     (bool)  quick-preset card button
#include <juce_gui_basics/juce_gui_basics.h>

namespace bf::gui {

struct Theme {
    juce::Colour bg{0xff111418}, panel{0xff1a1f26}, panelAlt{0xff222831}, sidebar{0xff0c0f12}, outline{0xff313945},
        text{0xfff2f4f7}, textDim{0xffa3adba}, textFaint{0xff6d7785}, accent{0xff2fb7a4}, accentStrong{0xff3ad6bf},
        stop{0xffe0603a}, warn{0xffe8b13a}, danger{0xffe5484d}, ok{0xff46c37b};
    static const Theme& get();
};

namespace fonts {
juce::Font title(float h = 22.0f);    // bold headings
juce::Font heading(float h = 13.0f);  // caps section labels
juce::Font body(float h = 15.0f);
juce::Font big(float h = 28.0f);      // primary values / status
}  // namespace fonts

class BfLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    BfLookAndFeel();

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;
    int getSliderThumbRadius(juce::Slider&) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawComboBox(juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getLabelFont(juce::Label&) override;
};

// Makes a control ignore keyboard focus so global shortcuts (Space) stay with the window.
void noFocus(juce::Component& c);

}  // namespace bf::gui
