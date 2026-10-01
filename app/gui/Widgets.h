#pragma once
// Reusable GUI building blocks shared by all pages.
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace bf::gui {

// Caps section heading ("MASKING STRENGTH"). `strong` = visual priority (GUI §63).
class SectionLabel final : public juce::Label {
public:
    explicit SectionLabel(const juce::String& text = {}, bool strong = false);
};

// A macro slider with a caption, end labels ("Natural ... Dense"), an optional value text and
// optional tick labels under the track. Drags are reported as gestures (one undo step each).
class MacroSlider final : public juce::Component {
public:
    MacroSlider(const juce::String& caption, const juce::String& left, const juce::String& right, bool primary = false);

    juce::Slider slider;
    std::function<void(double)> onChange;
    std::function<void()> onGestureStart, onGestureEnd;
    std::function<juce::String(double)> valueText;  // null: no value text

    void setRange(double lo, double hi, double step = 0.0);
    void setValueSilently(double v);
    double value() const { return slider.getValue(); }
    void setTooltip(const juce::String& t);
    void setTicks(std::vector<std::pair<double, juce::String>> ticks);
    void setNote(const juce::String& note, juce::Colour c);
    void setCaption(const juce::String& c) { caption_.setText(c, juce::dontSendNotification); }
    int preferredHeight() const;

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void updateValueText();
    SectionLabel caption_;
    juce::Label left_, right_, value_, note_;
    std::vector<std::pair<double, juce::String>> ticks_;
    bool primary_;
    bool silent_ = false;
};

// Radio-button group. Options can be disabled with an explanatory tooltip.
class ChoiceGroup final : public juce::Component {
public:
    ChoiceGroup(const juce::String& caption, const std::vector<juce::String>& options, bool horizontal = false);

    std::function<void(int)> onSelect;
    void setSelected(int index);  // no callback
    int selected() const;
    int size() const { return static_cast<int>(buttons_.size()); }
    juce::ToggleButton& button(int i) { return *buttons_[static_cast<std::size_t>(i)]; }
    void setOptionEnabled(int i, bool enabled, const juce::String& tooltip = {});
    void setOptionTooltip(int i, const juce::String& tooltip);
    void setCaption(const juce::String& c) { caption_.setText(c, juce::dontSendNotification); }
    int preferredHeight() const;
    void resized() override;

private:
    SectionLabel caption_;
    std::vector<std::unique_ptr<juce::ToggleButton>> buttons_;
    bool horizontal_;
    int groupId_;
};

// Collapsible panel with a clickable header ("▸ Talker Timing"). The content is not owned.
class CollapsiblePanel final : public juce::Component {
public:
    CollapsiblePanel(const juce::String& title, bool expanded);
    void setContent(juce::Component* content, std::function<int(int width)> contentHeight);
    bool isExpanded() const { return expanded_; }
    void setExpanded(bool e);
    std::function<void()> onToggle;  // the page re-lays itself out
    int preferredHeight(int width) const;
    void resized() override;
    void paint(juce::Graphics&) override;

private:
    juce::TextButton header_;
    juce::String title_;
    juce::Component* content_ = nullptr;
    std::function<int(int)> contentHeight_;
    bool expanded_;
};

// Advanced numeric parameter: name, slider and an editable value box with units.
class ParamRow final : public juce::Component {
public:
    ParamRow(const juce::String& name, double lo, double hi, double step, const juce::String& suffix);
    juce::Slider slider;
    std::function<void(double)> onChange;
    std::function<void()> onGestureStart, onGestureEnd;
    void setValueSilently(double v);
    void setTooltip(const juce::String& t);
    void resized() override;

private:
    juce::Label name_;
    bool silent_ = false;
};

// Vertical stack of components (visible ones only), used as collapsible-panel content.
class VStack final : public juce::Component {
public:
    void add(juce::Component& c, std::function<int(int width)> height, int gapAfter = 6);
    int preferredHeight(int width) const;
    void resized() override;

private:
    struct Item {
        juce::Component* c;
        std::function<int(int)> h;
        int gap;
    };
    std::vector<Item> items_;
};

// Horizontal activity meter (RUN page "Current activity").
class ActivityBar final : public juce::Component {
public:
    void setLevel(float level01);
    float level() const { return level_; }
    void paint(juce::Graphics&) override;

private:
    float level_ = 0.0f;
};

// Target spectrum graph (GUI §30): log-frequency x, dB y, curve normalised to 1 kHz.
class SpectrumGraph final : public juce::Component {
public:
    void setCurve(std::vector<std::pair<double, double>> hzDb);
    void paint(juce::Graphics&) override;

private:
    std::vector<std::pair<double, double>> curve_;
};

// A modal-looking overlay inside the main window (dims the page, centred card). Not a real
// modal loop: works headless and in tests.
class OverlayDialog final : public juce::Component {
public:
    OverlayDialog(const juce::String& title, std::unique_ptr<juce::Component> body, int bodyHeight, int width = 460);
    // Adds a button; the callback runs, then the dialog closes unless it returns false.
    juce::TextButton& addButton(const juce::String& text, std::function<bool()> onClick, bool primary = false);
    std::function<void()> onClose;  // the owner deletes the dialog here
    juce::Component* body() { return body_.get(); }
    juce::TextButton* findButton(const juce::String& text);
    void close();
    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;  // Escape closes
    void mouseDown(const juce::MouseEvent&) override {}

private:
    juce::Rectangle<int> card() const;
    juce::String title_;
    std::unique_ptr<juce::Component> body_;
    int bodyHeight_, width_;
    std::vector<std::unique_ptr<juce::TextButton>> buttons_;
};

}  // namespace bf::gui
