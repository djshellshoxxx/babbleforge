#include "Widgets.h"

#include <cmath>

#include "LookAndFeel.h"

namespace bf::gui {

// ---- SectionLabel --------------------------------------------------------------------------

SectionLabel::SectionLabel(const juce::String& text, bool strong) : juce::Label({}, text) {
    const auto& t = Theme::get();
    setFont(fonts::heading(strong ? 15.0f : 12.5f));
    setColour(juce::Label::textColourId, strong ? t.text : t.textDim);
    setJustificationType(juce::Justification::centredLeft);
    setBorderSize({0, 0, 0, 0});
    setInterceptsMouseClicks(false, false);
}

// ---- MacroSlider ---------------------------------------------------------------------------

MacroSlider::MacroSlider(const juce::String& caption, const juce::String& left, const juce::String& right, bool primary)
    : caption_(caption.toUpperCase(), primary), primary_(primary) {
    const auto& t = Theme::get();
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    slider.setRange(0.0, 1.0, 0.0);
    slider.setDoubleClickReturnValue(false, 0.0);
    slider.setWantsKeyboardFocus(false);
    if (primary) slider.getProperties().set("bf.primary", true);
    slider.onDragStart = [this] {
        if (onGestureStart) onGestureStart();
    };
    slider.onDragEnd = [this] {
        if (onGestureEnd) onGestureEnd();
    };
    slider.onValueChange = [this] {
        updateValueText();
        if (!silent_ && onChange) onChange(slider.getValue());
    };
    for (auto* l : {&left_, &right_}) {
        l->setFont(fonts::body(primary ? 15.0f : 14.0f));
        l->setColour(juce::Label::textColourId, t.textDim);
        l->setInterceptsMouseClicks(false, false);
    }
    left_.setText(left, juce::dontSendNotification);
    right_.setText(right, juce::dontSendNotification);
    left_.setJustificationType(juce::Justification::centredRight);
    right_.setJustificationType(juce::Justification::centredLeft);
    value_.setFont(primary ? fonts::title(19.0f) : fonts::body(14.0f));
    value_.setColour(juce::Label::textColourId, primary ? t.accentStrong : t.text);
    value_.setJustificationType(juce::Justification::centredRight);
    value_.setInterceptsMouseClicks(false, false);
    note_.setFont(fonts::body(13.0f));
    note_.setInterceptsMouseClicks(false, false);
    for (auto* c : std::initializer_list<juce::Component*>{&caption_, &left_, &right_, &value_, &slider, &note_})
        addAndMakeVisible(c);
}

void MacroSlider::setRange(double lo, double hi, double step) {
    silent_ = true;
    slider.setRange(lo, hi, step);
    silent_ = false;
}

void MacroSlider::setValueSilently(double v) {
    if (slider.isMouseButtonDown()) return;  // never fight the user's drag
    silent_ = true;
    slider.setValue(v, juce::dontSendNotification);
    silent_ = false;
    updateValueText();
}

void MacroSlider::setTooltip(const juce::String& t) {
    slider.setTooltip(t);
    caption_.setTooltip(t);
}

void MacroSlider::setTicks(std::vector<std::pair<double, juce::String>> ticks) {
    ticks_ = std::move(ticks);
    resized();
    repaint();
}

void MacroSlider::setNote(const juce::String& note, juce::Colour c) {
    if (note_.getText() == note) return;
    note_.setText(note, juce::dontSendNotification);
    note_.setColour(juce::Label::textColourId, c);
    resized();
}

void MacroSlider::updateValueText() {
    value_.setText(valueText ? valueText(slider.getValue()) : juce::String(), juce::dontSendNotification);
}

int MacroSlider::preferredHeight() const {
    int h = (primary_ ? 26 : 20) + (primary_ ? 40 : 32);
    if (!ticks_.empty()) h += 18;
    if (note_.getText().isNotEmpty()) h += 18;
    return h + 6;
}

void MacroSlider::resized() {
    auto r = getLocalBounds();
    auto top = r.removeFromTop(primary_ ? 26 : 20);
    value_.setBounds(top.removeFromRight(juce::jmin(200, top.getWidth() / 2)));
    caption_.setBounds(top);
    auto row = r.removeFromTop(primary_ ? 40 : 32);
    const int labelW = juce::jlimit(50, 90, getWidth() / 7);
    left_.setBounds(row.removeFromLeft(labelW));
    right_.setBounds(row.removeFromRight(labelW));
    slider.setBounds(row.reduced(8, 0));
    if (!ticks_.empty()) r.removeFromTop(18);
    note_.setBounds(r.removeFromTop(note_.getText().isNotEmpty() ? 18 : 0).withTrimmedLeft(labelW + 8));
}

void MacroSlider::paint(juce::Graphics& g) {
    if (ticks_.empty()) return;
    const auto& t = Theme::get();
    const auto sb = slider.getBounds();
    const float thumb = static_cast<float>(slider.getLookAndFeel().getSliderThumbRadius(slider));
    const float x0 = sb.getX() + thumb, x1 = sb.getRight() - thumb;
    const double lo = slider.getMinimum(), hi = slider.getMaximum();
    g.setFont(fonts::body(12.5f));
    const int y = sb.getBottom() - 4;
    for (const auto& [v, text] : ticks_) {
        if (v < lo - 1e-9 || v > hi + 1e-9) continue;
        const float x = x0 + static_cast<float>((v - lo) / (hi - lo)) * (x1 - x0);
        g.setColour(t.textFaint);
        g.fillRect(x - 0.5f, (float)y - 2.0f, 1.0f, 5.0f);
        g.setColour(t.textDim);
        g.drawText(text, juce::Rectangle<float>(x - 45.0f, (float)y + 2.0f, 90.0f, 16.0f), juce::Justification::centredTop);
    }
}

// ---- ChoiceGroup ---------------------------------------------------------------------------

static int nextRadioGroup() {
    static int id = 1000;
    return ++id;
}

ChoiceGroup::ChoiceGroup(const juce::String& caption, const std::vector<juce::String>& options, bool horizontal)
    : caption_(caption.toUpperCase()), horizontal_(horizontal), groupId_(nextRadioGroup()) {
    addAndMakeVisible(caption_);
    caption_.setVisible(caption.isNotEmpty());
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        auto b = std::make_unique<juce::ToggleButton>(options[static_cast<std::size_t>(i)]);
        b->setRadioGroupId(groupId_, juce::dontSendNotification);
        noFocus(*b);
        b->onClick = [this, i] {
            if (buttons_[static_cast<std::size_t>(i)]->getToggleState() && onSelect) onSelect(i);
        };
        addAndMakeVisible(*b);
        buttons_.push_back(std::move(b));
    }
}

void ChoiceGroup::setSelected(int index) {
    for (int i = 0; i < size(); ++i)
        buttons_[static_cast<std::size_t>(i)]->setToggleState(i == index, juce::dontSendNotification);
}

int ChoiceGroup::selected() const {
    for (int i = 0; i < static_cast<int>(buttons_.size()); ++i)
        if (buttons_[static_cast<std::size_t>(i)]->getToggleState()) return i;
    return -1;
}

void ChoiceGroup::setOptionEnabled(int i, bool enabled, const juce::String& tooltip) {
    if (i < 0 || i >= size()) return;
    auto& b = *buttons_[static_cast<std::size_t>(i)];
    b.setEnabled(enabled);
    if (tooltip.isNotEmpty()) b.setTooltip(tooltip);
}

void ChoiceGroup::setOptionTooltip(int i, const juce::String& tooltip) {
    if (i >= 0 && i < size()) buttons_[static_cast<std::size_t>(i)]->setTooltip(tooltip);
}

int ChoiceGroup::preferredHeight() const {
    const int cap = caption_.isVisible() ? 22 : 0;
    return cap + (horizontal_ ? 32 : 30 * size());
}

void ChoiceGroup::resized() {
    auto r = getLocalBounds();
    if (caption_.isVisible()) caption_.setBounds(r.removeFromTop(22));
    if (horizontal_) {
        const int w = size() > 0 ? r.getWidth() / size() : 0;
        for (auto& b : buttons_) b->setBounds(r.removeFromLeft(w).removeFromTop(32));
    } else {
        for (auto& b : buttons_) b->setBounds(r.removeFromTop(30));
    }
}

// ---- CollapsiblePanel ------------------------------------------------------------------------

CollapsiblePanel::CollapsiblePanel(const juce::String& title, bool expanded) : title_(title), expanded_(expanded) {
    header_.getProperties().set("bf.segment", true);
    header_.getProperties().set("bf.left", true);
    noFocus(header_);
    header_.onClick = [this] { setExpanded(!expanded_); };
    addAndMakeVisible(header_);
    setExpanded(expanded);
}

void CollapsiblePanel::setContent(juce::Component* content, std::function<int(int)> contentHeight) {
    content_ = content;
    contentHeight_ = std::move(contentHeight);
    if (content_) {
        addChildComponent(content_);
        content_->setVisible(expanded_);
    }
}

void CollapsiblePanel::setExpanded(bool e) {
    expanded_ = e;
    header_.setButtonText(juce::String::fromUTF8(e ? "\xe2\x96\xbe  " : "\xe2\x96\xb8  ") + title_.toUpperCase());
    header_.setToggleState(e, juce::dontSendNotification);
    if (content_) content_->setVisible(e);
    if (onToggle) onToggle();
}

int CollapsiblePanel::preferredHeight(int width) const {
    int h = 36;
    if (expanded_ && contentHeight_) h += contentHeight_(width - 24) + 16;
    return h;
}

void CollapsiblePanel::resized() {
    auto r = getLocalBounds();
    header_.setBounds(r.removeFromTop(36));
    if (content_) content_->setBounds(r.reduced(12, 8));
}

void CollapsiblePanel::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    g.setColour(t.panel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
    g.setColour(t.outline);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 8.0f, 1.0f);
}

// ---- ParamRow -------------------------------------------------------------------------------

ParamRow::ParamRow(const juce::String& name, double lo, double hi, double step, const juce::String& suffix) {
    name_.setText(name, juce::dontSendNotification);
    name_.setFont(fonts::body(14.5f));
    name_.setColour(juce::Label::textColourId, Theme::get().text);
    addAndMakeVisible(name_);
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 86, 26);
    slider.setRange(lo, hi, step);
    slider.setTextValueSuffix(suffix);
    slider.setWantsKeyboardFocus(false);
    slider.onDragStart = [this] {
        if (onGestureStart) onGestureStart();
    };
    slider.onDragEnd = [this] {
        if (onGestureEnd) onGestureEnd();
    };
    slider.onValueChange = [this] {
        if (!silent_ && onChange) onChange(slider.getValue());
    };
    addAndMakeVisible(slider);
}

void ParamRow::setValueSilently(double v) {
    if (slider.isMouseButtonDown()) return;
    silent_ = true;
    slider.setValue(v, juce::dontSendNotification);
    silent_ = false;
}

void ParamRow::setTooltip(const juce::String& t) {
    slider.setTooltip(t);
    name_.setTooltip(t);
}

void ParamRow::resized() {
    auto r = getLocalBounds();
    name_.setBounds(r.removeFromLeft(juce::jmin(220, r.getWidth() * 2 / 5)));
    slider.setBounds(r);
}

// ---- VStack ---------------------------------------------------------------------------------

void VStack::add(juce::Component& c, std::function<int(int)> height, int gapAfter) {
    addAndMakeVisible(c);
    items_.push_back({&c, std::move(height), gapAfter});
}

int VStack::preferredHeight(int width) const {
    int h = 0;
    for (const auto& i : items_)
        if (i.c->isVisible()) h += i.h(width) + i.gap;
    return h;
}

void VStack::resized() {
    int y = 0;
    for (const auto& i : items_) {
        if (!i.c->isVisible()) continue;
        const int h = i.h(getWidth());
        i.c->setBounds(0, y, getWidth(), h);
        y += h + i.gap;
    }
}

// ---- ActivityBar ------------------------------------------------------------------------------

void ActivityBar::setLevel(float level01) {
    level01 = juce::jlimit(0.0f, 1.0f, level01);
    if (std::abs(level01 - level_) < 0.002f) return;
    level_ = level01;
    repaint();
}

void ActivityBar::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    auto r = getLocalBounds().toFloat();
    g.setColour(t.panelAlt);
    g.fillRoundedRectangle(r, 4.0f);
    const int segs = 24;
    const float gap = 3.0f, w = (r.getWidth() - gap * (segs - 1)) / segs;
    const int lit = juce::roundToInt(level_ * segs);
    for (int i = 0; i < segs; ++i) {
        g.setColour(i < lit ? t.accent : t.outline.withAlpha(0.6f));
        g.fillRoundedRectangle(r.getX() + i * (w + gap), r.getY() + 3.0f, w, r.getHeight() - 6.0f, 2.0f);
    }
}

// ---- SpectrumGraph ---------------------------------------------------------------------------

void SpectrumGraph::setCurve(std::vector<std::pair<double, double>> hzDb) {
    curve_ = std::move(hzDb);
    repaint();
}

void SpectrumGraph::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    auto r = getLocalBounds().toFloat();
    g.setColour(t.panelAlt);
    g.fillRoundedRectangle(r, 6.0f);
    auto plot = r.reduced(34.0f, 12.0f).withTrimmedBottom(10.0f);
    const double fLo = 50.0, fHi = 16000.0, dbLo = -30.0, dbHi = 15.0;
    auto xOf = [&](double f) { return plot.getX() + float(std::log(f / fLo) / std::log(fHi / fLo)) * plot.getWidth(); };
    auto yOf = [&](double db) {
        return plot.getBottom() - float((juce::jlimit(dbLo, dbHi, db) - dbLo) / (dbHi - dbLo)) * plot.getHeight();
    };
    g.setFont(fonts::body(11.0f));
    for (double f : {125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0}) {
        g.setColour(t.outline.withAlpha(0.5f));
        g.drawVerticalLine(juce::roundToInt(xOf(f)), plot.getY(), plot.getBottom());
        g.setColour(t.textFaint);
        g.drawText(f >= 1000 ? juce::String(f / 1000.0, 0) + "k" : juce::String(f, 0),
                   juce::Rectangle<float>(xOf(f) - 20, plot.getBottom() + 2, 40, 12), juce::Justification::centred);
    }
    for (double db : {-20.0, -10.0, 0.0, 10.0}) {
        g.setColour(t.outline.withAlpha(db == 0.0 ? 0.9f : 0.4f));
        g.drawHorizontalLine(juce::roundToInt(yOf(db)), plot.getX(), plot.getRight());
        g.setColour(t.textFaint);
        g.drawText(juce::String((int)db), juce::Rectangle<float>(r.getX() + 2, yOf(db) - 6, 28, 12),
                   juce::Justification::centredRight);
    }
    if (curve_.size() < 2) return;
    juce::Path p;
    bool first = true;
    for (const auto& [f, db] : curve_) {
        if (f < fLo || f > fHi) continue;
        if (first) p.startNewSubPath(xOf(f), yOf(db));
        else p.lineTo(xOf(f), yOf(db));
        first = false;
    }
    g.setColour(t.accentStrong);
    g.strokePath(p, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved));
    g.setColour(t.textDim);
    g.drawText("target", plot.removeFromTop(14).removeFromRight(60), juce::Justification::centredRight);
}

// ---- OverlayDialog -----------------------------------------------------------------------------

OverlayDialog::OverlayDialog(const juce::String& title, std::unique_ptr<juce::Component> body, int bodyHeight, int width)
    : title_(title), body_(std::move(body)), bodyHeight_(bodyHeight), width_(width) {
    setWantsKeyboardFocus(true);
    if (body_) addAndMakeVisible(*body_);
}

juce::TextButton& OverlayDialog::addButton(const juce::String& text, std::function<bool()> onClick, bool primary) {
    auto b = std::make_unique<juce::TextButton>(text);
    if (primary) b->setColour(juce::TextButton::buttonColourId, Theme::get().accent.withAlpha(0.35f));
    noFocus(*b);
    b->onClick = [this, fn = std::move(onClick)] {
        if (!fn || fn()) close();
    };
    addAndMakeVisible(*b);
    buttons_.push_back(std::move(b));
    resized();
    return *buttons_.back();
}

juce::TextButton* OverlayDialog::findButton(const juce::String& text) {
    for (auto& b : buttons_)
        if (b->getButtonText() == text) return b.get();
    return nullptr;
}

void OverlayDialog::close() {
    setVisible(false);
    if (auto cb = onClose) cb();  // may delete this
}

juce::Rectangle<int> OverlayDialog::card() const {
    const int h = 52 + bodyHeight_ + 60;
    return juce::Rectangle<int>(juce::jmin(width_, getWidth() - 32), juce::jmin(h, getHeight() - 32)).withCentre(getLocalBounds().getCentre());
}

void OverlayDialog::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    g.fillAll(juce::Colours::black.withAlpha(0.55f));
    const auto c = card().toFloat();
    g.setColour(t.panel);
    g.fillRoundedRectangle(c, 12.0f);
    g.setColour(t.outline);
    g.drawRoundedRectangle(c, 12.0f, 1.0f);
    g.setColour(t.text);
    g.setFont(fonts::title(19.0f));
    g.drawText(title_, c.reduced(22.0f, 0).removeFromTop(52.0f), juce::Justification::centredLeft);
}

void OverlayDialog::resized() {
    auto c = card().reduced(22, 0);
    c.removeFromTop(52);
    if (body_) body_->setBounds(c.removeFromTop(bodyHeight_));
    auto row = c.removeFromBottom(56).withSizeKeepingCentre(c.getWidth(), 38);
    for (auto it = buttons_.rbegin(); it != buttons_.rend(); ++it) {
        const int w = juce::jmax(110, (*it)->getBestWidthForHeight(38) + 24);
        (*it)->setBounds(row.removeFromRight(w));
        row.removeFromRight(10);
    }
}

bool OverlayDialog::keyPressed(const juce::KeyPress& k) {
    if (k == juce::KeyPress::escapeKey) {
        close();
        return true;
    }
    return false;
}

}  // namespace bf::gui
