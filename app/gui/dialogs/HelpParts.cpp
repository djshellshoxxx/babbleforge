#include "dialogs/HelpParts.h"

#include "LookAndFeel.h"

namespace bf::gui {

HelpButton::HelpButton(const juce::String& help) : juce::Button("help"), help_(help) {
    setTooltip(help);
    noFocus(*this);
    setSize(22, 22);
    onClick = [this] { showHelp(); };
}

void HelpButton::setHelp(const juce::String& help) {
    help_ = help;
    setTooltip(help);
}

void HelpButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& t = Theme::get();
    auto r = getLocalBounds().toFloat().reduced(2.0f);
    const float d = juce::jmin(r.getWidth(), r.getHeight());
    r = juce::Rectangle<float>(d, d).withCentre(r.getCentre());
    g.setColour(highlighted || down ? t.accent : t.outline);
    g.drawEllipse(r, 1.4f);
    g.setColour(highlighted || down ? t.text : t.textDim);
    g.setFont(fonts::heading(13.0f));
    g.drawText("?", r, juce::Justification::centred);
}

void HelpButton::showHelp() {
    auto label = std::make_unique<juce::Label>();
    label->setText(help_, juce::dontSendNotification);
    label->setFont(fonts::body(14.0f));
    label->setColour(juce::Label::textColourId, Theme::get().text);
    label->setJustificationType(juce::Justification::topLeft);
    label->setSize(320, 24 + 18 * (1 + help_.length() / 42));
    if (!isShowing()) return;
    juce::CallOutBox::launchAsynchronously(std::move(label), getScreenBounds(), nullptr);
}

Card::Card(const juce::String& title, const juce::String& help)
    : titleText_(title), title_(title.toUpperCase(), true), help_(help) {
    addAndMakeVisible(title_);
    addAndMakeVisible(help_);
}

juce::Rectangle<int> Card::contentArea() const { return getLocalBounds().reduced(16, 14).withTrimmedTop(30); }

void Card::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(t.panel);
    g.fillRoundedRectangle(r, 10.0f);
    g.setColour(t.outline);
    g.drawRoundedRectangle(r, 10.0f, 1.0f);
}

void Card::resized() {
    auto top = getLocalBounds().reduced(16, 14).removeFromTop(24);
    help_.setBounds(top.removeFromRight(24));
    title_.setBounds(top);
}

}  // namespace bf::gui
