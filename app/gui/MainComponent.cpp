#include "MainComponent.h"

#include "LookAndFeel.h"
#include "model/Catalog.h"

namespace bf::gui {

namespace {
constexpr int kHeaderH = 60, kPresetBarH = 48, kSidebarW = 168, kStatusH = 30;

juce::String arrow() { return juce::String::fromUTF8(" \xe2\x86\x92 "); }
}  // namespace

MainComponent::MainComponent(AppState& state, PresetSession& session, EngineBridge& bridge, AppSettings& settings)
    : state_(state),
      session_(session),
      bridge_(bridge),
      settings_(settings),
      ctx_{state, session, bridge, settings, nullptr},
      status_(bridge, state),
      safety_(state, session) {
    const auto& t = Theme::get();
    ctx_.showPage = [this](const std::string& id) { showPage(id); };
    setWantsKeyboardFocus(true);

    title_.setText("BabbleForge", juce::dontSendNotification);
    title_.setFont(fonts::title(22.0f));
    title_.setColour(juce::Label::textColourId, t.text);
    presetLabel_.setFont(fonts::body(15.0f));
    presetLabel_.setColour(juce::Label::textColourId, t.textDim);
    addAndMakeVisible(title_);
    addAndMakeVisible(presetLabel_);

    for (auto* b : {&resetBtn_, &saveBtn_, &modifiedBtn_, &simpleBtn_, &advancedBtn_}) {
        noFocus(*b);
        addAndMakeVisible(*b);
    }
    noFocus(gear_);
    addAndMakeVisible(gear_);
    resetBtn_.setTooltip("Return to the recommended settings for this area and mask type. Strength is kept.");
    resetBtn_.onClick = [this] { session_.resetToRecommended(); };
    saveBtn_.setTooltip("Save the current configuration as your own preset (Ctrl+S).");
    saveBtn_.onClick = [this] { openSaveDialog(); };
    modifiedBtn_.setTooltip("Show which advanced settings differ from the recommendation.");
    modifiedBtn_.setColour(juce::TextButton::buttonColourId, t.warn.withAlpha(0.18f));
    modifiedBtn_.setColour(juce::TextButton::textColourOffId, t.warn);
    modifiedBtn_.onClick = [this] { openChangesDialog(); };
    for (auto* b : {&simpleBtn_, &advancedBtn_}) b->getProperties().set("bf.segment", true);
    simpleBtn_.onClick = [this] { requestMode(UiMode::Simple); };
    advancedBtn_.onClick = [this] { requestMode(UiMode::Advanced); };
    simpleBtn_.setTooltip("Everyday controls only.");
    advancedBtn_.setTooltip("Show detailed audio and masking controls.");
    gear_.setTooltip("Settings");
    gear_.onClick = [this] { showPage("settings"); };

    viewport_.setScrollBarsShown(true, false);
    viewport_.setScrollBarThickness(10);
    addAndMakeVisible(viewport_);
    addAndMakeVisible(status_);
    addChildComponent(safety_);
    safety_.onReview = [this] { openChangesDialog(); };
    safety_.onVisibilityChanged = [this] { resized(); };

    state_.addListener(this);
    rebuildSidebar();
    updateHeader();
    std::string start = settings_.get().lastPage;
    const PageInfo* info = PageRegistry::find(start);
    if (!info || (info->advancedOnly && !state_.advanced())) start = "run";
    showPage(start);
    setSize(1100, 820);
}

MainComponent::~MainComponent() {
    if (keyTarget_) keyTarget_->removeKeyListener(&shortcuts_);
    state_.removeListener(this);
    dialog_.reset();
    viewport_.setViewedComponent(nullptr, false);
    pages_.clear();
}

void MainComponent::parentHierarchyChanged() {
    auto* top = getTopLevelComponent();
    if (top == this) top = nullptr;  // keyPressed() handles it
    if (top == keyTarget_) return;
    if (keyTarget_) keyTarget_->removeKeyListener(&shortcuts_);
    keyTarget_ = top;
    if (keyTarget_) keyTarget_->addKeyListener(&shortcuts_);
}

// ---- pages ---------------------------------------------------------------------------------

Page* MainComponent::page(const std::string& id) {
    if (auto it = pages_.find(id); it != pages_.end()) return it->second.get();
    const PageInfo* info = PageRegistry::find(id);
    if (!info || !info->create) return nullptr;
    auto p = info->create(ctx_);
    if (!p) return nullptr;
    p->onHeightChanged = [this, id] {
        if (id == current_) sizePage();
    };
    auto* raw = p.get();
    pages_[id] = std::move(p);
    return raw;
}

void MainComponent::showPage(const std::string& id) {
    const PageInfo* info = PageRegistry::find(id);
    if (!info) return;
    if (info->advancedOnly && !state_.advanced()) return;
    Page* p = page(id);
    if (!p) return;
    current_ = id;
    viewport_.setViewedComponent(p, false);
    viewport_.setViewPosition(0, 0);
    sizePage();
    for (std::size_t i = 0; i < sidebar_.size(); ++i) sidebar_[i]->setToggleState(sidebarIds_[i] == id, juce::dontSendNotification);
    gear_.setToggleState(id == "settings", juce::dontSendNotification);
    if (settings_.get().lastPage != id) settings_.update([id](AppSettingsData& d) { d.lastPage = id; });
}

void MainComponent::sizePage() {
    auto* p = current_.empty() ? nullptr : page(current_);
    if (!p) return;
    const int w = juce::jmax(0, viewport_.getWidth() - viewport_.getScrollBarThickness());
    const int h = p->layoutPage(w);
    p->setSize(w, juce::jmax(h, viewport_.getHeight()));
}

std::vector<juce::TextButton*> MainComponent::sidebarButtons() {
    std::vector<juce::TextButton*> v;
    for (auto& b : sidebar_) v.push_back(b.get());
    return v;
}

void MainComponent::rebuildSidebar() {
    for (auto& b : sidebar_) removeChildComponent(b.get());
    sidebar_.clear();
    sidebarIds_.clear();
    int n = 0;
    for (const auto& info : PageRegistry::sidebar(state_.advanced())) {
        auto b = std::make_unique<juce::TextButton>(info.title);
        b->getProperties().set("bf.segment", true);
        b->getProperties().set("bf.left", true);
        noFocus(*b);
        ++n;
        b->setTooltip(info.title + (n <= 9 ? " (Ctrl+" + juce::String(n) + ")" : juce::String()));
        const std::string id = info.id;
        b->onClick = [this, id] { showPage(id); };
        b->setToggleState(id == current_, juce::dontSendNotification);
        addAndMakeVisible(*b);
        sidebar_.push_back(std::move(b));
        sidebarIds_.push_back(id);
    }
    resized();
}

// ---- mode / header ------------------------------------------------------------------------------

void MainComponent::requestMode(UiMode m) {
    if (m == state_.mode()) return;
    if (m == UiMode::Advanced && !settings_.get().advancedIntroShown) {
        auto body = std::make_unique<juce::Label>();
        body->setText("Advanced mode exposes detailed audio and masking controls.\n\n"
                      "Changing advanced settings can alter the behavior of the selected preset.",
                      juce::dontSendNotification);
        body->setFont(fonts::body(15.5f));
        body->setJustificationType(juce::Justification::topLeft);
        auto d = std::make_unique<OverlayDialog>("Advanced mode", std::move(body), 96);
        d->addButton("Cancel", nullptr);
        d->addButton(
            "Enable Advanced",
            [this] {
                settings_.update([](AppSettingsData& s) { s.advancedIntroShown = true; });
                state_.setMode(UiMode::Advanced);
                return true;
            },
            true);
        showDialog(std::move(d));
        return;
    }
    state_.setMode(m);  // never touches audio (GUI §1)
}

void MainComponent::appStateChanged(unsigned changes) {
    if (changes & AppState::kModeChanged) {
        rebuildSidebar();
        const PageInfo* info = PageRegistry::find(current_);
        if (info && info->advancedOnly && !state_.advanced()) showPage("run");
        else sizePage();
    }
    updateHeader();
}

void MainComponent::updateHeader() {
    simpleBtn_.setToggleState(!state_.advanced(), juce::dontSendNotification);
    advancedBtn_.setToggleState(state_.advanced(), juce::dontSendNotification);
    const auto origin = session_.origin();
    presetLabel_.setText(session_.title() + juce::String::fromUTF8("  \xc2\xb7  ") + session_.stateText(), juce::dontSendNotification);
    presetLabel_.setColour(juce::Label::textColourId,
                           origin == PresetOrigin::Modified || origin == PresetOrigin::UserModified ? Theme::get().warn
                                                                                                    : Theme::get().textDim);
    resetBtn_.setVisible(origin != PresetOrigin::Factory);
    const auto adv = session_.advancedChanges();
    modifiedBtn_.setVisible(!adv.empty());
    modifiedBtn_.setButtonText(session_.differsSignificantly() ? juce::String::fromUTF8("DIFFERS SIGNIFICANTLY \xc2\xb7 REVIEW")
                                                               : juce::String("ADVANCED SETTINGS MODIFIED"));
    resized();
}

// ---- dialogs ---------------------------------------------------------------------------------------

void MainComponent::showDialog(std::unique_ptr<OverlayDialog> d) {
    dialog_ = std::move(d);
    dialog_->onClose = [this] {
        // Defer deletion: we are inside the dialog's button callback.
        juce::Component::SafePointer<MainComponent> self(this);
        juce::MessageManager::callAsync([self] {
            if (self) self->closeDialog();
        });
    };
    addAndMakeVisible(*dialog_);
    dialog_->setBounds(getLocalBounds());
    dialog_->toFront(true);
}

void MainComponent::closeDialog() {
    if (!dialog_) return;
    removeChildComponent(dialog_.get());
    dialog_.reset();
    if (isShowing()) grabKeyboardFocus();
}

void MainComponent::openSaveDialog() {
    struct Body final : juce::Component {
        juce::Label nameLabel{{}, "Name:"}, based, error;
        juce::TextEditor name;
        juce::ToggleButton remember{"Remember output device with preset"};
        Body() {
            for (auto* c : std::initializer_list<juce::Component*>{&nameLabel, &name, &based, &remember, &error}) addAndMakeVisible(c);
            name.setFont(fonts::body(16.0f));
            error.setColour(juce::Label::textColourId, Theme::get().danger);
            based.setColour(juce::Label::textColourId, Theme::get().textDim);
            noFocus(remember);
        }
        void resized() override {
            auto r = getLocalBounds();
            nameLabel.setBounds(r.removeFromTop(22));
            name.setBounds(r.removeFromTop(34));
            r.removeFromTop(8);
            based.setBounds(r.removeFromTop(24));
            remember.setBounds(r.removeFromTop(28));
            error.setBounds(r.removeFromTop(22));
        }
    };
    auto body = std::make_unique<Body>();
    auto* b = body.get();
    b->based.setText("Based on: " + session_.areaName() + " / " + session_.strategyName(), juce::dontSendNotification);
    b->name.setText(state_.preset().name == "Recommended" || state_.preset().name.empty()
                        ? juce::String()
                        : juce::String::fromUTF8(state_.preset().name.c_str()),
                    juce::dontSendNotification);
    auto d = std::make_unique<OverlayDialog>("Save As Preset", std::move(body), 150);
    d->addButton("Cancel", nullptr);
    d->addButton(
        "Save",
        [this, b] {
            juce::String err;
            if (session_.saveAs(b->name.getText(), b->remember.getToggleState(), bridge_.deviceId(), &err)) return true;
            b->error.setText(err, juce::dontSendNotification);
            return false;
        },
        true);
    showDialog(std::move(d));
    if (isShowing()) b->name.grabKeyboardFocus();
}

void MainComponent::openChangesDialog() {
    const auto changes = session_.advancedChanges();
    juce::String text;
    if (session_.differsSignificantly())
        text << "This configuration differs significantly from the recommended " << session_.areaName() << " preset.\n\n";
    text << "Changed from " << session_.areaName() << " default:\n\n";
    for (const auto& c : changes) text << c.label << ":  " << c.from << arrow() << c.to << "\n";
    if (changes.empty()) text << "(no advanced changes)\n";
    auto body = std::make_unique<juce::Label>();
    body->setText(text, juce::dontSendNotification);
    body->setFont(fonts::body(15.0f));
    body->setJustificationType(juce::Justification::topLeft);
    const int h = juce::jmin(420, 80 + 22 * static_cast<int>(changes.size()));
    auto d = std::make_unique<OverlayDialog>("Advanced Settings Modified", std::move(body), h, 520);
    d->addButton("Reset", [this] {
        session_.resetToRecommended();
        return true;
    });
    d->addButton("Keep Changes", nullptr, true);
    showDialog(std::move(d));
}

void MainComponent::openHelp() {
    auto body = std::make_unique<juce::Label>();
    body->setText("Space\tStart / Stop masking\n"
                  "Ctrl+1 ... Ctrl+5\tRun, Mask, Area, Output, Analysis\n"
                  "Ctrl+S\tSave preset\n"
                  "Ctrl+Z / Ctrl+Shift+Z\tUndo / Redo\n"
                  "F1\tThis help\n\n"
                  "Hover over any control for an explanation.",
                  juce::dontSendNotification);
    body->setFont(fonts::body(15.0f));
    body->setJustificationType(juce::Justification::topLeft);
    auto d = std::make_unique<OverlayDialog>("Help", std::move(body), 170);
    d->addButton("Close", nullptr, true);
    showDialog(std::move(d));
}

// ---- keyboard ------------------------------------------------------------------------------------

bool MainComponent::handleKey(const juce::KeyPress& k) {
    if (dialog_) {
        if (k == juce::KeyPress::escapeKey) {
            closeDialog();
            return true;
        }
        return false;  // a dialog owns the keyboard (its text fields get Space etc.)
    }
    const auto mods = k.getModifiers();
    const int code = static_cast<int>(juce::CharacterFunctions::toUpperCase(static_cast<juce::juce_wchar>(k.getKeyCode())));
    if (k.getKeyCode() == juce::KeyPress::spaceKey && !mods.isAnyModifierKeyDown()) {
        // Never while a text field is being edited (GUI §62).
        if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr) return false;
        bridge_.toggleStartStop();
        return true;
    }
    if (k.getKeyCode() == juce::KeyPress::F1Key) {
        openHelp();
        return true;
    }
    if (!mods.isCommandDown()) return false;
    if (code >= '1' && code <= '9' && !mods.isShiftDown()) {
        const auto pages = PageRegistry::sidebar(true);  // Ctrl+N = fixed position (Ctrl+5 Analysis)
        const auto idx = static_cast<std::size_t>(code - '1');
        if (idx < pages.size()) showPage(pages[idx].id);
        return true;
    }
    if (code == 'S') {
        openSaveDialog();
        return true;
    }
    if (code == 'Z') {
        if (mods.isShiftDown()) state_.redo();
        else state_.undo();
        return true;
    }
    if (code == 'Y') {
        state_.redo();
        return true;
    }
    return false;
}

// ---- painting / layout ------------------------------------------------------------------------------

void MainComponent::GearButton::paintButton(juce::Graphics& g, bool highlighted, bool down) {
    const auto& t = Theme::get();
    auto r = getLocalBounds().toFloat().reduced(6.0f);
    const float s = juce::jmin(r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    const float ro = s * 0.5f, ri = s * 0.36f;
    juce::Path p;
    const int teeth = 8;
    for (int i = 0; i < teeth * 2; ++i) {
        const float a0 = juce::MathConstants<float>::twoPi * (float)i / (teeth * 2);
        const float a1 = juce::MathConstants<float>::twoPi * (float)(i + 1) / (teeth * 2);
        const float rad = (i % 2 == 0) ? ro : ri;
        const auto p0 = c.getPointOnCircumference(rad, a0), p1 = c.getPointOnCircumference(rad, a1);
        if (i == 0) p.startNewSubPath(p0);
        else p.lineTo(p0);
        p.lineTo(p1);
    }
    p.closeSubPath();
    p.addEllipse(juce::Rectangle<float>(s * 0.3f, s * 0.3f).withCentre(c));
    p.setUsingNonZeroWinding(false);
    g.setColour(getToggleState() ? t.accentStrong : (highlighted || down ? t.text : t.textDim));
    g.fillPath(p);
}

void MainComponent::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    g.fillAll(t.bg);
    g.setColour(t.panel);
    g.fillRect(0, 0, getWidth(), kHeaderH);
    g.setColour(t.sidebar);
    g.fillRect(0, kHeaderH, kSidebarW, getHeight() - kHeaderH - kStatusH);
    g.setColour(t.outline);
    g.drawHorizontalLine(kHeaderH - 1, 0.0f, (float)getWidth());
    g.drawHorizontalLine(kHeaderH + kPresetBarH - 1, (float)kSidebarW, (float)getWidth());
    g.drawVerticalLine(kSidebarW - 1, (float)kHeaderH, (float)(getHeight() - kStatusH));
}

void MainComponent::resized() {
    auto r = getLocalBounds();
    status_.setBounds(r.removeFromBottom(kStatusH));
    auto header = r.removeFromTop(kHeaderH).reduced(16, 12);
    gear_.setBounds(header.removeFromRight(36));
    header.removeFromRight(14);
    advancedBtn_.setBounds(header.removeFromRight(112));
    simpleBtn_.setBounds(header.removeFromRight(92));
    title_.setBounds(header.removeFromLeft(220));

    auto side = r.removeFromLeft(kSidebarW).reduced(10, 14);
    for (auto& b : sidebar_) {
        b->setBounds(side.removeFromTop(44));
        side.removeFromTop(4);
    }

    // Preset bar: "Office / Balanced · Modified"  [ADVANCED SETTINGS MODIFIED] [Reset] [Save As]
    auto bar = r.removeFromTop(kPresetBarH).reduced(16, 8);
    saveBtn_.setBounds(bar.removeFromRight(130));
    bar.removeFromRight(8);
    if (resetBtn_.isVisible()) {
        resetBtn_.setBounds(bar.removeFromRight(176));
        bar.removeFromRight(8);
    }
    if (modifiedBtn_.isVisible()) {
        modifiedBtn_.setBounds(bar.removeFromRight(juce::jmin(250, juce::jmax(0, bar.getWidth() - 160))));
        bar.removeFromRight(10);
    }
    presetLabel_.setBounds(bar);
    safety_.setBounds(r.removeFromTop(safety_.preferredHeight()));  // preset safety prompt (GUI §48), non-blocking
    viewport_.setBounds(r);
    sizePage();
    if (dialog_) dialog_->setBounds(getLocalBounds());
}

}  // namespace bf::gui
