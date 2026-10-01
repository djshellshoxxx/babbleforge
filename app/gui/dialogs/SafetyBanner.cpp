#include "dialogs/SafetyBanner.h"

#include "LookAndFeel.h"

namespace bf::gui {

SafetyBanner::SafetyBanner(AppState& state, PresetSession& session) : state_(state), session_(session) {
    const auto& t = Theme::get();
    text_.setFont(fonts::body(14.0f));
    text_.setColour(juce::Label::textColourId, t.text);
    text_.setMinimumHorizontalScale(0.8f);
    text_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(text_);
    for (auto* b : {&review_, &keep_, &reset_}) {
        noFocus(*b);
        addAndMakeVisible(*b);
    }
    review_.setTooltip("See which settings differ from the recommendation.");
    keep_.setTooltip("Hide this message. Your settings stay as they are.");
    reset_.setTooltip("Return to the recommended settings. Strength is kept.");
    review_.onClick = [this] {
        if (onReview) onReview();
    };
    keep_.onClick = [this] { keepChanges(); };
    reset_.onClick = [this] { resetToRecommended(); };
    state_.addListener(this);
    setVisible(false);
    update();
}

SafetyBanner::~SafetyBanner() { state_.removeListener(this); }

void SafetyBanner::keepChanges() {
    dismissed_ = true;
    dismissedArea_ = state_.preset().area;
    update();
}

void SafetyBanner::resetToRecommended() { session_.resetToRecommended(); }

void SafetyBanner::appStateChanged(unsigned) { update(); }

void SafetyBanner::update() {
    const bool differs = session_.differsSignificantly();
    if (!differs || state_.preset().area != dismissedArea_) dismissed_ = false;
    const bool show = differs && !dismissed_;
    if (show) text_.setText("This configuration differs significantly from the recommended " + session_.areaName() + " preset.", juce::dontSendNotification);
    if (show != shown_) {
        shown_ = show;
        setVisible(show);
        if (onVisibilityChanged) onVisibilityChanged();
    }
}

void SafetyBanner::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    g.setColour(t.warn.withAlpha(0.14f));
    g.fillAll();
    g.setColour(t.warn.withAlpha(0.6f));
    g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
}

void SafetyBanner::resized() {
    auto r = getLocalBounds().reduced(16, 6);
    reset_.setBounds(r.removeFromRight(80));
    r.removeFromRight(8);
    keep_.setBounds(r.removeFromRight(130));
    r.removeFromRight(8);
    review_.setBounds(r.removeFromRight(140));
    r.removeFromRight(10);
    text_.setBounds(r);
}

}  // namespace bf::gui
