#include "LookAndFeel.h"

namespace bf::gui {

const Theme& Theme::get() {
    static const Theme t;
    return t;
}

namespace fonts {
juce::Font title(float h) { return juce::Font(juce::FontOptions(h, juce::Font::bold)); }
juce::Font heading(float h) { return juce::Font(juce::FontOptions(h, juce::Font::bold)).withExtraKerningFactor(0.08f); }
juce::Font body(float h) { return juce::Font(juce::FontOptions(h)); }
juce::Font big(float h) { return juce::Font(juce::FontOptions(h, juce::Font::bold)); }
}  // namespace fonts

void noFocus(juce::Component& c) {
    c.setWantsKeyboardFocus(false);
    c.setMouseClickGrabsKeyboardFocus(false);
}

BfLookAndFeel::BfLookAndFeel() {
    const auto& t = Theme::get();
    setColourScheme({t.bg, t.panel, t.panelAlt, t.outline, t.text, t.accent, juce::Colours::black, t.panelAlt, t.text});
    setColour(juce::ResizableWindow::backgroundColourId, t.bg);
    setColour(juce::DocumentWindow::backgroundColourId, t.bg);
    setColour(juce::Label::textColourId, t.text);
    setColour(juce::TextButton::buttonColourId, t.panelAlt);
    setColour(juce::TextButton::buttonOnColourId, t.accent);
    setColour(juce::TextButton::textColourOffId, t.text);
    setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    setColour(juce::ComboBox::backgroundColourId, t.panelAlt);
    setColour(juce::ComboBox::outlineColourId, t.outline);
    setColour(juce::ComboBox::textColourId, t.text);
    setColour(juce::ComboBox::arrowColourId, t.accent);
    setColour(juce::PopupMenu::backgroundColourId, t.panel);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, t.accent);
    setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
    setColour(juce::Slider::trackColourId, t.accent);
    setColour(juce::Slider::backgroundColourId, t.outline);
    setColour(juce::Slider::thumbColourId, t.text);
    setColour(juce::Slider::textBoxTextColourId, t.text);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ToggleButton::textColourId, t.text);
    setColour(juce::ToggleButton::tickColourId, t.accent);
    setColour(juce::TextEditor::backgroundColourId, t.panelAlt);
    setColour(juce::TextEditor::outlineColourId, t.outline);
    setColour(juce::TextEditor::focusedOutlineColourId, t.accent);
    setColour(juce::TextEditor::textColourId, t.text);
    setColour(juce::TooltipWindow::backgroundColourId, t.panelAlt);
    setColour(juce::TooltipWindow::textColourId, t.text);
    setColour(juce::TooltipWindow::outlineColourId, t.outline);
    setColour(juce::ScrollBar::thumbColourId, t.outline);
}

void BfLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool hi, bool down) {
    const auto& t = Theme::get();
    auto r = b.getLocalBounds().toFloat().reduced(1.0f);
    const bool primary = b.getProperties().getWithDefault("bf.primary", false);
    const bool running = b.getProperties().getWithDefault("bf.running", false);
    const bool segment = b.getProperties().getWithDefault("bf.segment", false);
    const bool card = b.getProperties().getWithDefault("bf.card", false);
    const bool on = b.getToggleState();
    juce::Colour fill = t.panelAlt;
    float corner = 6.0f;
    if (primary) {
        fill = running ? t.stop : t.accent;
        corner = r.getHeight() * 0.5f;
    } else if (segment) {
        fill = on ? t.accent.withAlpha(0.22f) : juce::Colours::transparentBlack;
        corner = 4.0f;
    } else if (card) {
        fill = on ? t.accent.withAlpha(0.18f) : t.panelAlt;
        corner = 10.0f;
    } else if (on) {
        fill = t.accent;
    } else if (b.isColourSpecified(juce::TextButton::buttonColourId)) {
        fill = b.findColour(juce::TextButton::buttonColourId);
    }
    if (!b.isEnabled()) fill = fill.withMultipliedAlpha(0.4f);
    if (down) fill = fill.brighter(0.15f);
    else if (hi) fill = fill.brighter(0.08f);
    if (segment && hi && !on) fill = t.panelAlt;
    g.setColour(fill);
    g.fillRoundedRectangle(r, corner);
    if (card || (!primary && !segment)) {
        g.setColour(on ? t.accent : t.outline);
        g.drawRoundedRectangle(r, corner, on && card ? 2.0f : 1.0f);
    }
    if (segment && on) {
        g.setColour(t.accent);
        g.fillRoundedRectangle(r.removeFromLeft(4.0f), 2.0f);
    }
}

void BfLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool) {
    const auto& t = Theme::get();
    const bool primary = b.getProperties().getWithDefault("bf.primary", false);
    const bool segment = b.getProperties().getWithDefault("bf.segment", false);
    juce::Colour c = t.text;
    if (primary) c = juce::Colours::black;
    else if (segment) c = b.getToggleState() ? t.accentStrong : t.textDim;
    else if (b.getToggleState() && !b.getProperties().getWithDefault("bf.card", false)) c = juce::Colours::black;
    else if (b.isColourSpecified(juce::TextButton::textColourOffId)) c = b.findColour(juce::TextButton::textColourOffId);
    if (!b.isEnabled()) c = c.withMultipliedAlpha(0.5f);
    g.setColour(c);
    g.setFont(getTextButtonFont(b, b.getHeight()));
    const auto just = segment && b.getProperties().getWithDefault("bf.left", false) ? juce::Justification::centredLeft
                                                                                 : juce::Justification::centred;
    g.drawFittedText(b.getButtonText(), b.getLocalBounds().reduced(segment ? 14 : 8, 2), just, 2);
}

juce::Font BfLookAndFeel::getTextButtonFont(juce::TextButton& b, int h) {
    if (b.getProperties().getWithDefault("bf.primary", false)) return fonts::big(juce::jlimit(18.0f, 30.0f, h * 0.36f));
    if (b.getProperties().getWithDefault("bf.segment", false)) return fonts::heading(14.0f);
    return fonts::body(juce::jlimit(13.0f, 16.0f, h * 0.45f));
}

int BfLookAndFeel::getSliderThumbRadius(juce::Slider& s) {
    return s.getProperties().getWithDefault("bf.primary", false) ? 11 : 8;
}

void BfLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                     juce::Slider::SliderStyle style, juce::Slider& s) {
    const auto& t = Theme::get();
    const bool primary = s.getProperties().getWithDefault("bf.primary", false);
    const float track = primary ? 8.0f : 5.0f;
    const bool vertical = style == juce::Slider::LinearVertical;
    const float alpha = s.isEnabled() ? 1.0f : 0.35f;
    const auto accent = (primary ? t.accentStrong : t.accent).withMultipliedAlpha(alpha);
    if (vertical) {
        const float cx = x + w * 0.5f;
        juce::Rectangle<float> bg(cx - track * 0.5f, (float)y, track, (float)h);
        g.setColour(t.outline.withMultipliedAlpha(alpha));
        g.fillRoundedRectangle(bg, track * 0.5f);
        // bipolar fill from the centre
        const float mid = y + h * 0.5f;
        g.setColour(accent);
        g.fillRoundedRectangle(juce::Rectangle<float>(cx - track * 0.5f, juce::jmin(mid, pos), track, std::abs(pos - mid)),
                               track * 0.5f);
        g.setColour(t.text.withMultipliedAlpha(alpha));
        g.fillEllipse(juce::Rectangle<float>(16.0f, 16.0f).withCentre({cx, pos}));
        return;
    }
    const float cy = y + h * 0.5f;
    juce::Rectangle<float> bg((float)x, cy - track * 0.5f, (float)w, track);
    g.setColour(t.outline.withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(bg, track * 0.5f);
    g.setColour(accent);
    g.fillRoundedRectangle(bg.withRight(pos), track * 0.5f);
    const float d = (float)getSliderThumbRadius(s) * 2.0f;
    g.setColour(t.text.withMultipliedAlpha(alpha));
    g.fillEllipse(juce::Rectangle<float>(d, d).withCentre({pos, cy}));
    g.setColour(accent);
    g.drawEllipse(juce::Rectangle<float>(d, d).withCentre({pos, cy}), 2.0f);
}

void BfLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool hi, bool) {
    const auto& t = Theme::get();
    const float alpha = b.isEnabled() ? 1.0f : 0.35f;
    const bool radio = b.getRadioGroupId() != 0;
    auto r = b.getLocalBounds().toFloat();
    const float d = 18.0f;
    auto box = juce::Rectangle<float>(d, d).withCentre({r.getX() + d * 0.5f + 2.0f, r.getCentreY()});
    g.setColour((hi ? t.text : t.textDim).withMultipliedAlpha(alpha));
    if (radio) g.drawEllipse(box, 1.6f);
    else g.drawRoundedRectangle(box, 4.0f, 1.6f);
    if (b.getToggleState()) {
        g.setColour(t.accent.withMultipliedAlpha(alpha));
        if (radio) g.fillEllipse(box.reduced(4.0f));
        else g.fillRoundedRectangle(box.reduced(4.0f), 2.0f);
    }
    g.setColour(t.text.withMultipliedAlpha(alpha));
    g.setFont(fonts::body(15.0f));
    g.drawFittedText(b.getButtonText(), r.withTrimmedLeft(d + 10.0f).toNearestInt(), juce::Justification::centredLeft, 2);
}

void BfLookAndFeel::drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& c) {
    const auto& t = Theme::get();
    const bool primary = c.getProperties().getWithDefault("bf.primary", false);
    auto r = juce::Rectangle<float>(0, 0, (float)w, (float)h).reduced(1.0f);
    g.setColour(t.panelAlt.withMultipliedAlpha(c.isEnabled() ? 1.0f : 0.5f));
    g.fillRoundedRectangle(r, 8.0f);
    g.setColour(primary ? t.accent : t.outline);
    g.drawRoundedRectangle(r, 8.0f, primary ? 2.0f : 1.0f);
    const float a = primary ? 7.0f : 5.0f;
    const float cx = w - h * 0.5f, cy = h * 0.5f;
    juce::Path p;
    p.addTriangle(cx - a, cy - a * 0.5f, cx + a, cy - a * 0.5f, cx, cy + a * 0.6f);
    g.setColour(t.accent);
    g.fillPath(p);
}

juce::Font BfLookAndFeel::getComboBoxFont(juce::ComboBox& c) {
    return c.getProperties().getWithDefault("bf.primary", false) ? fonts::title(22.0f) : fonts::body(15.0f);
}

void BfLookAndFeel::positionComboBoxText(juce::ComboBox& c, juce::Label& l) {
    l.setBounds(14, 1, c.getWidth() - c.getHeight() - 14, c.getHeight() - 2);
    l.setFont(getComboBoxFont(c));
}

juce::Font BfLookAndFeel::getPopupMenuFont() { return fonts::body(16.0f); }

juce::Font BfLookAndFeel::getLabelFont(juce::Label& l) { return l.getFont(); }

}  // namespace bf::gui
