#include "dialogs/FirstRunWizard.h"

#include "LookAndFeel.h"
#include "model/Prefs.h"

namespace bf::gui {

namespace {
std::vector<juce::String> namesOf(const std::vector<Choice>& v) {
    std::vector<juce::String> out;
    for (const auto& c : v) out.push_back(c.name);
    return out;
}
// The six areas of GUI §60 (Small Room, Office, Conference, Open Office, Large Room, Outdoor).
std::vector<Choice> wizardAreas(const DataSet& ds) {
    static const char* const ids[] = {"small_room", "office", "conference", "open_office", "large_room", "free_field"};
    std::vector<Choice> out;
    for (const auto& c : areaChoices(ds))
        for (const char* id : ids)
            if (c.id == id) out.push_back(c);
    return out;
}

}  // namespace

FirstRunWizard::FirstRunWizard(AppState& state, AppSettings& settings, EngineBridge& bridge)
    : state_(state),
      settings_(settings),
      bridge_(bridge),
      areaList_(wizardAreas(state.data())),
      areas_("", namesOf(areaList_)) {
    const auto& t = Theme::get();
    areaId_ = state_.preset().area;
    deviceId_ = bridge_.deviceId();
    for (auto* l : {&title_, &text_, &setup_}) {
        l->setInterceptsMouseClicks(false, false);
        addAndMakeVisible(*l);
    }
    title_.setFont(fonts::title(24.0f));
    title_.setColour(juce::Label::textColourId, t.text);
    text_.setFont(fonts::body(15.0f));
    text_.setColour(juce::Label::textColourId, t.textDim);
    text_.setJustificationType(juce::Justification::topLeft);
    setup_.setFont(fonts::title(22.0f));
    setup_.setColour(juce::Label::textColourId, t.accentStrong);
    setup_.setJustificationType(juce::Justification::topLeft);

    bridge_.refreshDevices();
    addChildComponent(device_);
    noFocus(device_);
    device_.setTooltip("The speakers or audio interface that will play the masking sound.");
    device_.onChange = [this] { selectDevice(device_.getSelectedItemIndex()); };
    for (auto* b : {&test_, &next_, &back_}) {
        noFocus(*b);
        addAndMakeVisible(*b);
    }
    test_.setTooltip("Plays a short identification tone on each speaker in turn. Press again to stop.");
    test_.onClick = [this] { testOutput(); };
    next_.onClick = [this] { next(); };
    back_.onClick = [this] { back(); };
    addChildComponent(areas_);
    for (int i = 0; i < static_cast<int>(areaList_.size()); ++i) areas_.setOptionTooltip(i, areaList_[static_cast<std::size_t>(i)].description);
    areas_.onSelect = [this](int i) {
        if (i >= 0 && i < static_cast<int>(areaList_.size())) areaId_ = areaList_[static_cast<std::size_t>(i)].id;
    };
    for (int i = 0; i < static_cast<int>(areaList_.size()); ++i)
        if (areaList_[static_cast<std::size_t>(i)].id == areaId_) areas_.setSelected(i);

    // Never show the wizard twice, even when it is closed with Esc.
    updatePrefs(settings_, [](Prefs& p) { p.firstRunDone = true; });
    settings_.flush();
    showPage(0);
}

FirstRunWizard::~FirstRunWizard() {
    stopTimer();
    if (testing_) bridge_.testSpeakers(false);
}

void FirstRunWizard::showPage(int p) {
    page_ = juce::jlimit(0, 3, p);
    device_.setVisible(page_ == 1);
    test_.setVisible(page_ == 1);
    areas_.setVisible(page_ == 2);
    setup_.setVisible(page_ == 3);
    back_.setVisible(page_ > 0);
    next_.setButtonText(page_ == 3 ? "Start" : "Next");
    next_.getProperties().set("bf.primary", page_ == 3);
    switch (page_) {
    case 0:
        title_.setText("Welcome to BabbleForge", juce::dontSendNotification);
        text_.setText("BabbleForge adds natural, speech-like masking sound to protect conversations from being overheard.\n\n"
                      "Three quick choices and you are ready. No account required.",
                      juce::dontSendNotification);
        break;
    case 1: {
        title_.setText("Select Output", juce::dontSendNotification);
        text_.setText("Where should the masking sound play?", juce::dontSendNotification);
        devices_ = bridge_.deviceList();
        device_.clear(juce::dontSendNotification);
        int sel = -1;
        for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
            const auto& d = devices_[static_cast<std::size_t>(i)];
            if (d.numOutputs <= 0) continue;
            device_.addItem(juce::String::fromUTF8(d.name.c_str()), i + 1);
            if (d.id == deviceId_) sel = i + 1;
        }
        if (sel > 0) device_.setSelectedId(sel, juce::dontSendNotification);
        else if (device_.getNumItems() > 0) {
            device_.setSelectedItemIndex(0, juce::dontSendNotification);
            selectDevice(0);
        }
        test_.setEnabled(device_.getNumItems() > 0);
        break;
    }
    case 2:
        title_.setText("Choose Your Area", juce::dontSendNotification);
        text_.setText("This sets the recommended masking for the space.", juce::dontSendNotification);
        break;
    default: {
        title_.setText("Recommended Setup", juce::dontSendNotification);
        state_.setArea(areaId_);
        state_.setStrategy("balanced");
        text_.setText({}, juce::dontSendNotification);
        setup_.setText(areaName(state_.data(), areaId_) + "\nBalanced\n" + strengthLabelFor(state_.data(), state_.strengthDb()) +
                           " Strength",
                       juce::dontSendNotification);
        break;
    }
    }
    resized();
    repaint();
}

void FirstRunWizard::next() {
    if (page_ == 3) {
        finish(true);
        return;
    }
    if (page_ == 1 && testing_) stopTest();
    showPage(page_ + 1);
}

void FirstRunWizard::back() { showPage(page_ - 1); }

void FirstRunWizard::selectDevice(int index) {
    const int id = device_.getItemId(index);
    if (id > 0 && id - 1 < static_cast<int>(devices_.size())) deviceId_ = devices_[static_cast<std::size_t>(id - 1)].id;
}

void FirstRunWizard::selectArea(const std::string& areaId) {
    areaId_ = areaId;
    for (int i = 0; i < static_cast<int>(areaList_.size()); ++i)
        if (areaList_[static_cast<std::size_t>(i)].id == areaId) areas_.setSelected(i);
}

void FirstRunWizard::testOutput() {
    if (deviceId_.empty()) return;
    if (testing_) {  // the button reads "Stop test" while the sequence runs
        stopTest();
        return;
    }
    // TEST SPEAKERS (CalibrationBus ChannelId): channel-identification pink bursts, one output at
    // a time, ending with STOP. Starts the engine on the selected device when needed.
    if (deviceId_ != bridge_.deviceId()) bridge_.setDevice(deviceId_);
    bridge_.testSpeakers(true);
    testing_ = true;
    testSeenRunning_ = false;
    test_.setButtonText("Stop test");
    testEndMs_ = juce::Time::getMillisecondCounterHiRes() + 30000.0;  // engine start + sequence guard
    startTimerHz(10);
}

void FirstRunWizard::stopTest() {
    if (!testing_) return;
    testing_ = false;
    stopTimer();
    bridge_.testSpeakers(false);
    test_.setEnabled(true);
    test_.setButtonText("Test");
}

void FirstRunWizard::timerCallback() {
    if (!testing_) return;
    const bool running = bridge_.status().out.testRunning;
    testSeenRunning_ = testSeenRunning_ || running;
    if ((testSeenRunning_ && !running) || juce::Time::getMillisecondCounterHiRes() >= testEndMs_) stopTest();  // sequence finished
}

void FirstRunWizard::finish(bool start) {
    if (finished_) return;
    finished_ = true;
    stopTimer();
    stopTest();
    if (!areaId_.empty() && (state_.preset().area != areaId_ || state_.preset().strategy != "balanced")) {
        state_.setArea(areaId_);
        state_.setStrategy("balanced");
    }
    if (!deviceId_.empty()) {
        const std::string id = deviceId_;
        if (id != bridge_.deviceId()) bridge_.setDevice(id);
        settings_.update([&](AppSettingsData& d) { d.deviceId = id; });
        updatePrefs(settings_, [](Prefs& p) { p.rememberDevice = true; });
    }
    settings_.flush();
    if (start) bridge_.start();
    if (requestClose) requestClose();
}

void FirstRunWizard::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    const float cy = static_cast<float>(getHeight() - 28);
    for (int i = 0; i < 4; ++i) {
        g.setColour(i == page_ ? t.accentStrong : t.outline);
        g.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre({static_cast<float>(getWidth() / 2 - 33 + i * 22), cy}));
    }
}

void FirstRunWizard::resized() {
    auto r = getLocalBounds();
    auto buttons = r.removeFromBottom(40);
    next_.setBounds(buttons.removeFromRight(130));
    buttons.removeFromRight(8);
    if (back_.isVisible()) back_.setBounds(buttons.removeFromRight(100));
    title_.setBounds(r.removeFromTop(40));
    r.removeFromTop(6);
    switch (page_) {
    case 0: text_.setBounds(r.removeFromTop(150)); break;
    case 1: {
        text_.setBounds(r.removeFromTop(30));
        auto row = r.removeFromTop(38);
        test_.setBounds(row.removeFromRight(100));
        row.removeFromRight(8);
        device_.setBounds(row);
        break;
    }
    case 2:
        text_.setBounds(r.removeFromTop(28));
        areas_.setBounds(r.removeFromTop(juce::jmin(r.getHeight() - 10, 6 * 30)));
        break;
    default: setup_.setBounds(r.removeFromTop(120)); break;
    }
}

std::unique_ptr<OverlayDialog> makeFirstRunDialog(AppState& state, AppSettings& settings, EngineBridge& bridge) {
    auto body = std::make_unique<FirstRunWizard>(state, settings, bridge);
    auto* w = body.get();
    auto d = std::make_unique<OverlayDialog>("", std::move(body), 270, 480);
    OverlayDialog* dlg = d.get();
    w->requestClose = [dlg] { dlg->close(); };
    return d;
}

}  // namespace bf::gui
