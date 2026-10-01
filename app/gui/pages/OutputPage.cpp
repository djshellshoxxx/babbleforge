#include "pages/OutputPage.h"

#include <algorithm>
#include <cmath>

#include "LookAndFeel.h"
#include "model/Catalog.h"
#include "model/OutputModel.h"
#include "model/Prefs.h"

namespace bf::gui {

static PageRegistrar outputPageRegistration({"output", "OUTPUT", 40, PageInfo::Sidebar,
                                             [](PageContext& c) { return std::make_unique<OutputPage>(c); }});

using namespace outputs;
using rt::EngineState;

namespace {

void styleLabel(juce::Label& l, const juce::Font& f, juce::Colour c, juce::Justification j) {
    l.setFont(f);
    l.setColour(juce::Label::textColourId, c);
    l.setJustificationType(j);
    l.setInterceptsMouseClicks(false, false);
}

juce::String dbText(double db, const char* unit) {
    return db > -150.0 ? juce::String(db, 1) + " " + unit : juce::String(juce::CharPointer_UTF8("\xe2\x80\x94")) + " " + unit;
}

}  // namespace

OutputPage::OutputPage(PageContext& c) : Page(c) {
    const auto& t = Theme::get();

    styleLabel(banner_, fonts::big(30.0f), t.danger, juce::Justification::centred);
    styleLabel(bannerSub_, fonts::body(14.0f), t.warn, juce::Justification::centred);
    addChildComponent(banner_);
    addChildComponent(bannerSub_);
    for (auto* b : {&reconnect_, &chooseDevice_}) {
        noFocus(*b);
        addChildComponent(*b);
    }
    reconnect_.setTooltip("Try the same device again. BabbleForge never switches to another device by itself.");
    reconnect_.onClick = [this] { ctx.bridge.reconnect(); };
    chooseDevice_.setTooltip("Pick one of the available output devices.");
    chooseDevice_.onClick = [this] {
        ctx.bridge.refreshDevices();
        fillDevices();
        device_.grabKeyboardFocus();
        device_.showPopup();
    };

    // Device (GUI §22): explicit choice only.
    addAndMakeVisible(deviceCaption_);
    device_.getProperties().set("bf.primary", true);
    noFocus(device_);
    device_.setTooltip("The device that plays the masking. It is never changed automatically.");
    device_.onChange = [this] {
        if (silent_) return;
        const int i = device_.getSelectedId() - 1;
        if (i < 0 || i >= static_cast<int>(devices_.size())) return;
        const std::string id = devices_[static_cast<std::size_t>(i)].id;
        ctx.bridge.setDevice(id);
        ctx.settings.update([id](AppSettingsData& d) { d.deviceId = id; });
    };
    addAndMakeVisible(device_);
    noFocus(refresh_);
    refresh_.setTooltip("Scan for output devices.");
    refresh_.onClick = [this] {
        ctx.bridge.refreshDevices();
        ctx.bridge.waitIdle(2000);
        fillDevices();
    };
    addAndMakeVisible(refresh_);

    mode_.setOptionTooltip(0, "Two speakers (left and right).");
    mode_.setOptionTooltip(1, "Four or more speakers spread around the area.");
    mode_.onSelect = [this](int i) { state().setCoverage(i == 0 ? Coverage::Stereo : Coverage::MultiSpeaker); };
    addAndMakeVisible(mode_);

    master_.setTooltip("Overall masking level. Same control as Masking Strength on the RUN page.");
    master_.valueText = [this](double db) {
        const auto label = strengthLabelFor(state().data(), db);
        return advanced() ? formatDb(db) + "  (" + label + ")" : label;
    };
    master_.onGestureStart = [this] { state().beginGesture("Master level"); };
    master_.onGestureEnd = [this] { state().endGesture(); };
    master_.onChange = [this](double v) { state().setStrengthDb(v); };
    addAndMakeVisible(master_);

    addAndMakeVisible(metersCaption_);
    styleLabel(band_, fonts::title(20.0f), t.textDim, juce::Justification::centredLeft);
    styleLabel(status_, fonts::body(15.0f), t.textDim, juce::Justification::centredLeft);
    addAndMakeVisible(band_);
    addAndMakeVisible(status_);

    // TEST SPEAKERS (GUI §23).
    addAndMakeVisible(testCaption_);
    noFocus(test_);
    test_.setTooltip("Plays a short identification signal on each speaker in turn.");
    test_.onClick = [this] { ctx.bridge.testSpeakers(true); };
    addAndMakeVisible(test_);
    noFocus(stopTest_);
    stopTest_.getProperties().set("bf.primary", true);
    stopTest_.getProperties().set("bf.running", true);
    stopTest_.onClick = [this] { ctx.bridge.testSpeakers(false); };
    addChildComponent(stopTest_);
    styleLabel(testInfo_, fonts::body(15.0f), t.text, juce::Justification::centred);
    addChildComponent(testInfo_);

    // Advanced.
    for (auto* l : {&techCaption_, &devInfoCaption_, &limiterCaption_}) addChildComponent(*l);
    for (const char* n : {"RMS", "LUFS-S", "True Peak", "Limiter"}) addReadout(n, true);
    for (const char* n : {"Driver", "Sample Rate", "Buffer", "Channels", "Latency", "Audio Dropouts"}) addReadout(n, false);
    for (auto* c : std::initializer_list<juce::ComboBox*>{&rate_, &buffer_}) {
        noFocus(*c);
        c->onChange = [this] {
            if (!silent_) applyFormat();
        };
        addChildComponent(*c);
    }
    addChildComponent(rateCaption_);
    addChildComponent(bufferCaption_);
    rate_.setTooltip("Output sample rate. Masking runs at 44.1, 48, 88.2 and 96 kHz. Changing it restarts the audio briefly.");
    buffer_.setTooltip("Audio buffer size in samples. Smaller is lower latency, larger is safer against dropouts.");
    styleLabel(formatNote_, fonts::body(13.0f), t.textDim, juce::Justification::centredLeft);
    formatNote_.setText("Changing these fades the masking out and restarts the audio engine.", juce::dontSendNotification);
    addChildComponent(formatNote_);
    limiterOn_.setTooltip("The limiter protects the speakers and ears. Switching it off shows \"Limiter disabled\" in the status bar.");
    noFocus(limiterOn_);
    limiterOn_.onClick = [this] { setLimiterEnabled(state(), limiterOn_.getToggleState()); };
    addChildComponent(limiterOn_);
    ceiling_.setSliderStyle(juce::Slider::LinearHorizontal);
    ceiling_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 90, 24);
    ceiling_.setRange(-6.0, -0.1, 0.1);
    ceiling_.setTextValueSuffix(" dBTP");
    ceiling_.setWantsKeyboardFocus(false);
    ceiling_.setDoubleClickReturnValue(true, -1.0);
    ceiling_.onDragStart = [this] { state().beginGesture("Limiter ceiling"); };
    ceiling_.onDragEnd = [this] { state().endGesture(); };
    ceiling_.onValueChange = [this] {
        if (!silent_) setLimiterCeilingDbtp(state(), ceiling_.getValue());
    };
    addChildComponent(ceiling_);
    styleLabel(ceilingLabel_, fonts::body(14.5f), t.text, juce::Justification::centredLeft);
    ceilingLabel_.setText("Ceiling", juce::dontSendNotification);
    addChildComponent(ceilingLabel_);

    ctx.bridge.refreshDevices();
    refreshFromState();
    refreshStatus(ctx.bridge.status());
}

OutputPage::Readout& OutputPage::addReadout(const juce::String& name, bool technical) {
    (void)technical;
    Readout r;
    r.name = name;
    r.caption = std::make_unique<SectionLabel>(name);
    r.value = std::make_unique<juce::Label>();
    styleLabel(*r.value, fonts::title(17.0f), Theme::get().text, juce::Justification::centredLeft);
    addChildComponent(*r.caption);
    addChildComponent(*r.value);
    readouts_.push_back(std::move(r));
    return readouts_.back();
}

juce::Label& OutputPage::readout(const juce::String& name) {
    for (auto& r : readouts_)
        if (r.name == name) return *r.value;
    return band_;
}

void OutputPage::fillDevices() {
    devices_ = ctx.bridge.deviceList();
    const std::string cur = ctx.bridge.deviceId();
    silent_ = true;
    device_.clear(juce::dontSendNotification);
    int sel = -1;
    for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
        const auto& d = devices_[static_cast<std::size_t>(i)];
        device_.addItem(juce::String::fromUTF8(d.name.c_str()) + "  (" + juce::String::fromUTF8(d.type.c_str()) + ", " +
                            juce::String(d.numOutputs) + " ch)", i + 1);
        if (d.id == cur) sel = i;
    }
    fillFormatBoxes();
    if (sel >= 0) device_.setSelectedItemIndex(sel, juce::dontSendNotification);
    else device_.setText(cur.empty() ? juce::String("Choose an output device") : juce::String::fromUTF8(cur.c_str()) + " (not available)",
                         juce::dontSendNotification);
    silent_ = false;
}

void OutputPage::fillFormatBoxes() {
    // Sample rates the engine supports for babble (TALKER_ENGINE §8.1), limited to what the
    // device reports; buffer sizes 64..4096 from the device (default list when it reports none).
    const double curRate = ctx.bridge.requestedSampleRate();
    const int curBuf = ctx.bridge.requestedBufferFrames();
    const rt::AudioDeviceInfo* dev = nullptr;
    const std::string cur = ctx.bridge.deviceId();
    for (const auto& d : devices_)
        if (d.id == cur) dev = &d;
    rateValues_.clear();
    bufferValues_.clear();
    for (const double r : {44100.0, 48000.0, 88200.0, 96000.0}) {
        bool ok = !dev || dev->sampleRates.empty();
        if (!ok)
            for (const double x : dev->sampleRates) ok = ok || std::abs(x - r) < 1.0;
        if (ok || std::abs(r - curRate) < 1.0) rateValues_.push_back(r);
    }
    if (rateValues_.empty()) rateValues_ = {44100.0, 48000.0, 88200.0, 96000.0};
    std::vector<int> bufs = dev && !dev->bufferSizes.empty() ? dev->bufferSizes : std::vector<int>{64, 128, 256, 512, 1024, 2048, 4096};
    for (const int b : bufs)
        if (b >= 64 && b <= 4096) bufferValues_.push_back(b);
    if (std::find(bufferValues_.begin(), bufferValues_.end(), curBuf) == bufferValues_.end()) bufferValues_.push_back(curBuf);
    std::sort(bufferValues_.begin(), bufferValues_.end());
    bufferValues_.erase(std::unique(bufferValues_.begin(), bufferValues_.end()), bufferValues_.end());
    silent_ = true;
    rate_.clear(juce::dontSendNotification);
    buffer_.clear(juce::dontSendNotification);
    for (std::size_t i = 0; i < rateValues_.size(); ++i) {
        rate_.addItem(juce::String(rateValues_[i] / 1000.0, rateValues_[i] == 44100.0 || rateValues_[i] == 88200.0 ? 1 : 0) + " kHz", static_cast<int>(i) + 1);
        if (std::abs(rateValues_[i] - curRate) < 1.0) rate_.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
    }
    for (std::size_t i = 0; i < bufferValues_.size(); ++i) {
        buffer_.addItem(juce::String(bufferValues_[i]) + " samples", static_cast<int>(i) + 1);
        if (bufferValues_[i] == curBuf) buffer_.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
    }
    silent_ = false;
}

void OutputPage::applyFormat() {
    const int ri = rate_.getSelectedId() - 1, bi = buffer_.getSelectedId() - 1;
    if (ri < 0 || ri >= static_cast<int>(rateValues_.size()) || bi < 0 || bi >= static_cast<int>(bufferValues_.size())) return;
    const double sr = rateValues_[static_cast<std::size_t>(ri)];
    const int bf = bufferValues_[static_cast<std::size_t>(bi)];
    ctx.bridge.setAudioFormat(sr, bf);
    updatePrefs(ctx.settings, [&](Prefs& p) {
        p.sampleRate = sr;
        p.bufferFrames = bf;
    });
}

void OutputPage::rebuildMeters() {
    const Preset& p = state().preset();
    const OutputLayout L = currentLayout(p);
    if (static_cast<int>(meters_.size()) != L.size()) {
        meters_.clear();
        for (int i = 0; i < L.size(); ++i) {
            meters_.push_back(std::make_unique<ChannelMeter>());
            addAndMakeVisible(*meters_.back());
        }
    }
    for (int i = 0; i < L.size(); ++i) {
        juce::String name = L.size() == 1 ? "Mono" : L.size() == 2 ? (i == 0 ? "Left" : "Right") : "Speaker " + juce::String(i + 1);
        meters_[static_cast<std::size_t>(i)]->setName(name);
    }
}

void OutputPage::refreshFromState() {
    const bool adv = advanced();
    const Preset& p = state().preset();
    fillDevices();
    mode_.setSelected(state().coverage() == Coverage::Stereo ? 0 : 1);
    master_.setRange(state().strengthMinDb(), state().strengthMaxDb(), adv ? 0.5 : 0.0);
    if (adv) master_.setTicks({});
    else master_.setTicks(strengthLabels(state().data()));
    master_.setValueSilently(state().strengthDb());
    rebuildMeters();

    for (auto* c : std::initializer_list<juce::Component*>{&techCaption_, &devInfoCaption_, &limiterCaption_, &limiterOn_, &ceiling_, &ceilingLabel_,
                                                          &rateCaption_, &bufferCaption_, &rate_, &buffer_, &formatNote_})
        c->setVisible(adv);
    for (auto& r : readouts_) {
        r.caption->setVisible(adv);
        r.value->setVisible(adv);
    }
    silent_ = true;
    limiterOn_.setToggleState(limiterEnabled(state()), juce::dontSendNotification);
    ceiling_.setEnabled(limiterEnabled(state()));
    if (!ceiling_.isMouseButtonDown()) ceiling_.setValue(limiterCeilingDbtp(state()), juce::dontSendNotification);
    silent_ = false;
    (void)p;
    refreshStatus(last_.state == EngineState::Stopped && last_.statusSeq == 0 ? ctx.bridge.status() : last_);
    relayout();
}

void OutputPage::refreshStatus(const EngineStatus& s) {
    const auto& t = Theme::get();
    last_ = s;
    const bool adv = advanced();
    const bool lost = s.state == EngineState::DeviceLost;
    const bool degraded = s.state == EngineState::Degraded;
    const bool running = s.state == EngineState::Running || degraded;

    // Device lost banner (§58) and degraded status (§59).
    const bool bannerOn = lost || s.state == EngineState::Error || degraded;
    banner_.setText(lost ? juce::String("OUTPUT DEVICE LOST") : s.state == EngineState::Error ? juce::String("ERROR") : statusHeadline(s),
                    juce::dontSendNotification);
    banner_.setColour(juce::Label::textColourId, degraded ? t.warn : t.danger);
    juce::String sub = statusSubline(s);
    if (lost) sub = "Audio stopped. Your settings are kept. BabbleForge will not switch to another device by itself.";
    bannerSub_.setText(sub, juce::dontSendNotification);
    const bool relayoutNeeded = banner_.isVisible() != bannerOn || reconnect_.isVisible() != lost;
    banner_.setVisible(bannerOn);
    bannerSub_.setVisible(bannerOn);
    reconnect_.setVisible(lost);
    chooseDevice_.setVisible(lost);

    // Status line.
    juce::String st;
    juce::Colour sc = t.ok;
    if (lost) st = "Output device lost", sc = t.danger;
    else if (s.state == EngineState::Error) st = "Error", sc = t.danger;
    else if (degraded) st = "Degraded", sc = t.warn;
    else if (running) st = "Healthy";
    else if (s.state == EngineState::Starting || s.state == EngineState::Preparing || s.state == EngineState::Ready) st = "Starting", sc = t.textDim;
    else st = "Not running", sc = t.textDim;
    status_.setText("Status: " + st, juce::dontSendNotification);
    status_.setColour(juce::Label::textColourId, sc);

    // Meters.
    for (std::size_t i = 0; i < meters_.size(); ++i) {
        const double db = running && i < s.out.rmsFastDb.size() ? s.out.rmsFastDb[i] : -200.0;
        meters_[i]->setLevelDb(db, adv);
    }
    const auto band = running ? levelBand(s.out.leq60Db > -150.0 ? s.out.leq60Db : s.out.rmsDb, state().data().engineDefaults.lRefDbfs,
                                          s.out.limiterAbove05 > 0.01)
                              : LevelBand::None;
    band_.setText(levelBandName(band), juce::dontSendNotification);
    band_.setColour(juce::Label::textColourId, band == LevelBand::Good ? t.ok : band == LevelBand::High ? t.danger : band == LevelBand::Low ? t.warn : t.textDim);
    band_.setVisible(!adv);

    // Advanced readouts (§38, §39).
    if (adv) {
        readout("RMS").setText(dbText(s.out.rmsDb, "dBFS"), juce::dontSendNotification);
        readout("LUFS-S").setText(dbText(s.out.lufsS, "LUFS"), juce::dontSendNotification);
        readout("True Peak").setText(dbText(s.out.truePeakDbMax, "dBTP"), juce::dontSendNotification);
        readout("Limiter").setText(!s.limiterEnabled ? juce::String("disabled") : juce::String(-std::abs(s.out.limiterGrMaxDb), 1) + " dB",
                                   juce::dontSendNotification);
        readout("Driver").setText(s.driver.empty() ? juce::String("-") : juce::String::fromUTF8(s.driver.c_str()), juce::dontSendNotification);
        readout("Sample Rate").setText(s.sampleRate > 0 ? juce::String(juce::roundToInt(s.sampleRate)) + " Hz" : juce::String("-"),
                                       juce::dontSendNotification);
        readout("Buffer").setText(s.bufferFrames > 0 ? juce::String(s.bufferFrames) + " samples" : juce::String("-"), juce::dontSendNotification);
        readout("Channels").setText(juce::String(s.outputs > 0 ? s.outputs : outputCount(state().preset())), juce::dontSendNotification);
        readout("Latency").setText(running && s.out.latencyMs > 0 ? juce::String(s.out.latencyMs, 1) + " ms" : juce::String("-"),
                                   juce::dontSendNotification);
        readout("Audio Dropouts").setText(juce::String(static_cast<juce::int64>(s.xruns)), juce::dontSendNotification);
    }

    // TEST SPEAKERS.
    const bool testing = s.out.testRunning;
    const bool testable = !lost && s.state != EngineState::Stopping && s.state != EngineState::Error;
    test_.setEnabled(testable && !testing);
    if (stopTest_.isVisible() != testing) {
        stopTest_.setVisible(testing);
        testInfo_.setVisible(testing);
        test_.setVisible(!testing);
        relayoutNeeded ? void() : relayout();
    }
    if (testing) {
        const int n = std::max(1, s.out.testOutputs);
        const int cur = juce::jlimit(0, n - 1, static_cast<int>(s.out.testElapsedS / 1.5));
        juce::String name = n == 2 ? (cur == 0 ? "Left" : "Right") : "Speaker " + juce::String(cur + 1);
        testInfo_.setText("Testing: " + name + "  (" + juce::String(cur + 1) + " of " + juce::String(n) + ")", juce::dontSendNotification);
    }
    if (relayoutNeeded) relayout();
}

int OutputPage::layoutPage(int width) {
    const bool adv = advanced();
    auto col = column(width, 640, 20);
    int y = col.getY();
    const int x = col.getX(), w = col.getWidth();
    auto place = [&](juce::Component& c, int h, int gapAfter = 8) {
        c.setBounds(x, y, w, h);
        y += h + gapAfter;
    };
    auto hide = [](juce::Component& c) { c.setBounds(0, 0, 0, 0); };

    if (banner_.isVisible()) {
        place(banner_, 40, 2);
        place(bannerSub_, 22, 8);
        if (reconnect_.isVisible()) {
            reconnect_.setBounds(x + w / 2 - 170, y, 160, 38);
            chooseDevice_.setBounds(x + w / 2 + 10, y, 160, 38);
            y += 50;
        } else {
            hide(reconnect_);
            hide(chooseDevice_);
        }
        y += 6;
    } else {
        hide(banner_);
        hide(bannerSub_);
        hide(reconnect_);
        hide(chooseDevice_);
    }
    place(deviceCaption_, 22, 4);
    device_.setBounds(x, y, w - 100, 44);
    refresh_.setBounds(x + w - 90, y + 6, 90, 32);
    y += 52;
    place(mode_, mode_.preferredHeight(), 12);
    place(master_, master_.preferredHeight(), 14);

    place(metersCaption_, 20, 4);
    for (auto& m : meters_) {
        m->setBounds(x, y, w, 26);
        y += 28;
    }
    y += 4;
    if (!adv) {
        place(band_, 28, 2);
        status_.setBounds(x, y, w, 24);
        y += 30;
    } else {
        hide(band_);
        status_.setBounds(x, y, w, 24);
        y += 32;
    }

    // TEST SPEAKERS: STOP TEST replaces the button while a test runs.
    place(testCaption_, 20, 4);
    if (stopTest_.isVisible()) {
        place(testInfo_, 26, 4);
        stopTest_.setBounds(x, y, w, 64);
        hide(test_);
        y += 64 + 16;
    } else {
        test_.setBounds(x, y, juce::jmin(w, 280), 44);
        hide(testInfo_);
        hide(stopTest_);
        y += 44 + 20;
    }

    if (adv) {
        place(techCaption_, 24, 4);
        auto grid = [&](std::initializer_list<const char*> names, int cols) {
            const int cw = w / cols;
            int i = 0;
            for (const char* n : names) {
                for (auto& r : readouts_)
                    if (r.name == n) {
                        const int cx = x + (i % cols) * cw, cy = y + (i / cols) * 50;
                        r.caption->setBounds(cx, cy, cw - 8, 18);
                        r.value->setBounds(cx, cy + 18, cw - 8, 26);
                    }
                ++i;
            }
            y += ((i + cols - 1) / cols) * 50 + 8;
        };
        grid({"RMS", "LUFS-S", "True Peak", "Limiter"}, 4);
        place(devInfoCaption_, 24, 4);
        grid({"Driver", "Sample Rate", "Buffer", "Channels", "Latency", "Audio Dropouts"}, 3);
        {
            const int cw = (w - 12) / 2;
            rateCaption_.setBounds(x, y, cw, 18);
            bufferCaption_.setBounds(x + cw + 12, y, cw, 18);
            rate_.setBounds(x, y + 20, cw, 32);
            buffer_.setBounds(x + cw + 12, y + 20, cw, 32);
            formatNote_.setBounds(x, y + 56, w, 20);
            y += 84;
        }
        place(limiterCaption_, 24, 4);
        limiterOn_.setBounds(x, y, 160, 30);
        y += 34;
        ceilingLabel_.setBounds(x, y, 110, 28);
        ceiling_.setBounds(x + 110, y, w - 110, 28);
        y += 28 + 24;
    } else {
        for (auto* c : std::initializer_list<juce::Component*>{&techCaption_, &devInfoCaption_, &limiterCaption_, &limiterOn_, &ceiling_, &ceilingLabel_})
            hide(*c);
        for (auto& r : readouts_) {
            hide(*r.caption);
            hide(*r.value);
        }
        for (auto* c : std::initializer_list<juce::Component*>{&rateCaption_, &bufferCaption_, &rate_, &buffer_, &formatNote_}) hide(*c);
        y += 16;
    }
    return y;
}

}  // namespace bf::gui
