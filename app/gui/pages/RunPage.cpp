#include "pages/RunPage.h"

#include <cmath>

#include "LookAndFeel.h"

namespace bf::gui {

static PageRegistrar runPageRegistration({"run", "RUN", 10, PageInfo::Sidebar,
                                          [](PageContext& c) { return std::make_unique<RunPage>(c); }});

namespace {

void styleLabel(juce::Label& l, const juce::Font& f, juce::Colour c, juce::Justification j) {
    l.setFont(f);
    l.setColour(juce::Label::textColourId, c);
    l.setJustificationType(j);
    l.setInterceptsMouseClicks(false, false);
}

}  // namespace

RunPage::RunPage(PageContext& c) : Page(c) {
    const auto& t = Theme::get();
    const DataSet& ds = state().data();
    areas_ = areaChoices(ds);
    masks_ = maskTypeChoices(ds);

    for (auto* box : {&area_, &mask_}) {
        box->getProperties().set("bf.primary", true);
        noFocus(*box);
        addAndMakeVisible(*box);
    }
    for (int i = 0; i < static_cast<int>(areas_.size()); ++i) area_.addItem(areas_[static_cast<std::size_t>(i)].name, i + 1);
    for (int i = 0; i < static_cast<int>(masks_.size()); ++i) mask_.addItem(masks_[static_cast<std::size_t>(i)].name, i + 1);
    area_.setTooltip("Choose the kind of space. Recommended masking settings are loaded automatically.");
    mask_.setTooltip("Choose what the masking sounds like.");
    area_.onChange = [this] {
        const int i = area_.getSelectedItemIndex();
        if (i >= 0 && i < static_cast<int>(areas_.size())) state().setArea(areas_[static_cast<std::size_t>(i)].id);
    };
    mask_.onChange = [this] {
        const int i = mask_.getSelectedItemIndex();
        if (i >= 0 && i < static_cast<int>(masks_.size())) state().setStrategy(masks_[static_cast<std::size_t>(i)].id);
    };

    styleLabel(areaAdvice_, fonts::body(13.5f), t.warn, juce::Justification::centredLeft);
    styleLabel(maskDesc_, fonts::body(15.0f), t.textDim, juce::Justification::topLeft);
    styleLabel(status_, fonts::big(26.0f), t.text, juce::Justification::centred);
    styleLabel(sub_, fonts::body(14.0f), t.warn, juce::Justification::centred);
    styleLabel(elapsed_, fonts::title(20.0f), t.textDim, juce::Justification::centred);
    for (auto* l : {&areaCaption_, &maskCaption_}) addAndMakeVisible(*l);
    for (auto* l : {&areaAdvice_, &maskDesc_, &status_, &sub_, &elapsed_}) addAndMakeVisible(*l);

    startStop_.getProperties().set("bf.primary", true);
    noFocus(startStop_);
    startStop_.setTooltip("Start or stop masking (Space)");
    startStop_.onClick = [this] { ctx.bridge.toggleStartStop(); };
    addAndMakeVisible(startStop_);
    for (auto* b : {&reconnect_, &chooseDevice_}) {
        noFocus(*b);
        addChildComponent(*b);
    }
    reconnect_.onClick = [this] { ctx.bridge.reconnect(); };
    chooseDevice_.onClick = [this] {
        if (ctx.showPage) ctx.showPage("output");
    };

    // Strength (GUI §7; ENGINE §3.1 labels Gentle -12 / Low -6 / Normal 0 / Strong +5 / Very Strong +9).
    strength_.setTooltip("How loud the masking is. Normal suits most rooms.");
    strength_.valueText = [this](double db) {
        const auto label = strengthLabelFor(state().data(), db);
        return advanced() ? formatDb(db) + "  (" + label + ")" : label;
    };
    strength_.onGestureStart = [this] { state().beginGesture("Masking Strength"); };
    strength_.onGestureEnd = [this] { state().endGesture(); };
    strength_.onChange = [this](double v) { state().setStrengthDb(v); };
    addAndMakeVisible(strength_);

    // Character (GUI §8).
    character_.setTooltip("Natural: conversation-like movement with more pauses. Dense: more overlapping voices "
                          "and fewer gaps. The overall level stays about the same.");
    character_.valueText = [this](double v) {
        return advanced() ? juce::String(juce::roundToInt(v * 100.0)) + "% dense" : juce::String();
    };
    character_.onGestureStart = [this] { state().beginGesture("Character"); };
    character_.onGestureEnd = [this] { state().endGesture(); };
    character_.onChange = [this](double v) { state().setCharacter(v); };
    addAndMakeVisible(character_);

    coverage_.setOptionTooltip(0, "Two speakers (left and right).");
    coverage_.setOptionTooltip(1, "Four or more speakers spread around the area.");
    coverage_.onSelect = [this](int i) { state().setCoverage(i == 0 ? Coverage::Stereo : Coverage::MultiSpeaker); };
    addAndMakeVisible(coverage_);

    addAndMakeVisible(activityCaption_);
    addAndMakeVisible(activity_);
    styleLabel(activityText_, fonts::body(15.0f), t.textDim, juce::Justification::centredLeft);
    addAndMakeVisible(activityText_);

    addAndMakeVisible(cardsCaption_);
    const char* names[3] = {"Balanced", "Natural", "Dense"};
    for (int i = 0; i < 3; ++i) {
        auto& b = cards_[static_cast<std::size_t>(i)];
        b.setButtonText(names[i]);
        b.getProperties().set("bf.card", true);
        b.setClickingTogglesState(false);
        noFocus(b);
        const std::string id = kCardIds[i];
        b.onClick = [this, id] { state().setStrategy(id); };
        addAndMakeVisible(b);
    }
    refreshFromState();
    refreshStatus(ctx.bridge.status());
}

void RunPage::refreshFromState() {
    const auto& p = state().preset();
    for (int i = 0; i < static_cast<int>(areas_.size()); ++i)
        if (areas_[static_cast<std::size_t>(i)].id == p.area) area_.setSelectedItemIndex(i, juce::dontSendNotification);
    int maskIdx = -1;
    for (int i = 0; i < static_cast<int>(masks_.size()); ++i)
        if (masks_[static_cast<std::size_t>(i)].id == p.strategy) maskIdx = i;
    if (maskIdx >= 0) mask_.setSelectedItemIndex(maskIdx, juce::dontSendNotification);
    else mask_.setText(maskTypeName(state().data(), p.strategy), juce::dontSendNotification);
    maskDesc_.setText(maskTypeDescription(state().data(), p.strategy), juce::dontSendNotification);
    areaAdvice_.setText(areaAdvisory(state().data(), p.area), juce::dontSendNotification);

    strength_.setRange(state().strengthMinDb(), state().strengthMaxDb(), advanced() ? 0.5 : 0.0);
    // Label ticks in Simple; Advanced shows the exact dB value instead (its range is too wide).
    if (advanced()) strength_.setTicks({});
    else strength_.setTicks(strengthLabels(state().data()));
    strength_.setValueSilently(state().strengthDb());
    const auto& t = Theme::get();
    if (advanced() && state().strengthDb() > 6.0) strength_.setNote("Limiter may engage", t.warn);
    else strength_.setNote({}, t.textDim);

    character_.setValueSilently(state().character());
    // Character has no effect for steady speech noise.
    character_.setEnabled(p.strategy != "speech_noise");
    coverage_.setSelected(state().coverage() == Coverage::Stereo ? 0 : 1);
    for (int i = 0; i < 3; ++i) cards_[static_cast<std::size_t>(i)].setToggleState(p.strategy == kCardIds[i], juce::dontSendNotification);
    relayout();
}

void RunPage::refreshStatus(const EngineStatus& s) {
    using rt::EngineState;
    const auto& t = Theme::get();
    status_.setText(statusHeadline(s), juce::dontSendNotification);
    juce::Colour sc = t.text;
    if (s.state == EngineState::Running) sc = t.ok;
    else if (s.state == EngineState::Degraded) sc = t.warn;
    else if (s.state == EngineState::DeviceLost || s.state == EngineState::Error) sc = t.danger;
    status_.setColour(juce::Label::textColourId, sc);
    sub_.setText(statusSubline(s), juce::dontSendNotification);

    const bool active = ctx.bridge.masking() || s.state == EngineState::DeviceLost;
    startStop_.setButtonText(active ? "STOP MASKING" : "START MASKING");
    if (static_cast<bool>(startStop_.getProperties().getWithDefault("bf.running", false)) != active) {
        startStop_.getProperties().set("bf.running", active);
        startStop_.repaint();
    }
    startStop_.setEnabled(s.state != EngineState::Stopping);
    elapsed_.setText(active || ctx.bridge.elapsedSeconds() > 0 ? formatElapsed(ctx.bridge.elapsedSeconds()) : juce::String(),
                     juce::dontSendNotification);
    const bool lost = s.state == EngineState::DeviceLost;
    if (reconnect_.isVisible() != lost) {
        reconnect_.setVisible(lost);
        chooseDevice_.setVisible(lost);
        relayout();
    }

    const bool masking = s.state == EngineState::Running || s.state == EngineState::Degraded;
    activity_.setLevel(masking ? static_cast<float>(juce::jlimit(0.0, 1.0, (s.outputLevelDb + 60.0) / 50.0)) : 0.0f);
    juce::String text;
    if (masking) {
        const int n = juce::roundToInt(s.voicesActive);
        text = s.voicesActive > 0.0 ? juce::String(n) + (n == 1 ? " voice active" : " voices active") : juce::String("Steady masking");
        text << juce::String::fromUTF8("  \xc2\xb7  ") << (s.state == EngineState::Running ? "Output healthy" : "Output degraded");
    } else if (lost) {
        text = "Output device lost";
    } else if (s.state == EngineState::Starting || s.state == EngineState::Preparing) {
        text = "Starting" + juce::String::fromUTF8("\xe2\x80\xa6");
    } else {
        text = "Not masking";
    }
    activityText_.setText(text, juce::dontSendNotification);
}

int RunPage::layoutPage(int width) {
    auto col = column(width, 640, 20);
    int y = col.getY();
    const int x = col.getX(), w = col.getWidth();
    auto place = [&](juce::Component& c, int h, int gapAfter = 8) {
        c.setBounds(x, y, w, h);
        y += h + gapAfter;
    };
    place(areaCaption_, 22, 4);
    place(area_, 48, 4);
    if (areaAdvice_.getText().isNotEmpty()) place(areaAdvice_, 20, 6);
    else areaAdvice_.setBounds(0, 0, 0, 0);
    y += 6;
    place(maskCaption_, 22, 4);
    place(mask_, 48, 4);
    place(maskDesc_, 40, 14);

    // Start / Stop: the largest control (GUI §4), status directly above, elapsed below.
    place(status_, 34, 0);
    place(sub_, 20, 6);
    const int bw = juce::jmin(w, 360);
    startStop_.setBounds(x + (w - bw) / 2, y, bw, 76);
    y += 82;
    place(elapsed_, 26, 4);
    if (reconnect_.isVisible()) {
        reconnect_.setBounds(x + w / 2 - 170, y, 160, 34);
        chooseDevice_.setBounds(x + w / 2 + 10, y, 160, 34);
        y += 42;
    }
    y += 10;
    place(strength_, strength_.preferredHeight(), 14);
    place(character_, character_.preferredHeight(), 12);
    place(coverage_, coverage_.preferredHeight(), 16);
    place(activityCaption_, 20, 4);
    place(activity_, 22, 4);
    place(activityText_, 22, 18);
    place(cardsCaption_, 20, 6);
    const int cw = (w - 2 * 12) / 3;
    for (int i = 0; i < 3; ++i) cards_[static_cast<std::size_t>(i)].setBounds(x + i * (cw + 12), y, cw, 56);
    y += 56 + 24;
    return y;
}

}  // namespace bf::gui
