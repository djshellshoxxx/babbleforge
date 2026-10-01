#include "pages/SettingsPage.h"

#include <algorithm>

#include "LookAndFeel.h"
#include "MainComponent.h"
#include "core/rt/Logging.h"

namespace bf::gui {

static PageRegistrar settingsReg({"settings", "SETTINGS", 100, PageInfo::Gear,
                                  [](PageContext& c) { return std::make_unique<SettingsPage>(c); }});

namespace {

constexpr const char* kLevelIds[] = {"trace", "debug", "info", "warn", "error"};
constexpr const char* kLevelNames[] = {"Trace", "Debug", "Info", "Warning", "Error"};
constexpr double kScales[] = {0.8, 0.9, 1.0, 1.25, 1.5, 2.0};
constexpr const char* kFallbackIds[] = {"strict", "safe", "continuous"};

void caption(juce::Label& l, const juce::String& text) {
    l.setText(text, juce::dontSendNotification);
    l.setFont(fonts::body(14.5f));
    l.setColour(juce::Label::textColourId, Theme::get().textDim);
    l.setInterceptsMouseClicks(false, false);
}

rt::LogLevel levelOf(const std::string& id) {
    if (id == "trace") return rt::LogLevel::Trace;
    if (id == "debug") return rt::LogLevel::Debug;
    if (id == "warn") return rt::LogLevel::Warn;
    if (id == "error") return rt::LogLevel::Error;
    return rt::LogLevel::Info;
}

}  // namespace

SettingsPage::SettingsPage(PageContext& c) : Page(c) {
    const auto& t = Theme::get();
    juce::ignoreUnused(t);

    // ---- Audio -----------------------------------------------------------------------------
    addHeader(hAudio_);
    caption(deviceCaption_, "Output device");
    addAndMakeVisible(deviceCaption_);
    addRow(deviceCaption_, 22);
    noFocus(device_);
    device_.setTooltip("Choose the speakers or audio interface for masking.");
    device_.onChange = [this] {
        if (building_) return;
        const int i = device_.getSelectedItemIndex();
        if (i < 0 || i >= static_cast<int>(deviceIds_.size())) return;
        const std::string id = deviceIds_[static_cast<std::size_t>(i)];
        ctx.bridge.setDevice(id);
        if (loadPrefs(ctx.settings.get()).rememberDevice) ctx.settings.update([&](AppSettingsData& d) { d.deviceId = id; });
    };
    addAndMakeVisible(device_);
    addAndMakeVisible(deviceHelp_);
    addRow(device_, 36, &deviceHelp_);
    caption(deviceInfo_, {});
    addAndMakeVisible(deviceInfo_);
    addRow(deviceInfo_, 22);
    noFocus(refreshDevices_);
    refreshDevices_.setTooltip("Look for newly connected audio devices.");
    refreshDevices_.onClick = [this] { ctx.bridge.refreshDevices(); };
    addAndMakeVisible(refreshDevices_);
    addRow(refreshDevices_, 32);

    // ---- Startup ---------------------------------------------------------------------------
    addHeader(hStartup_);
    toggle(launch_, "Open BabbleForge when you sign in to this computer.", [this](bool on) {
        const bool ok = applyLaunchAtLogin(on, loadPrefs(ctx.settings.get()).startMinimized);
        updatePrefs(ctx.settings, [&](Prefs& p) { p.launchAtLogin = on; });
        startupNote_.setText(ok ? juce::String() : juce::String("This computer's startup list could not be changed. The setting is saved."),
                             juce::dontSendNotification);
    });
    addRow(launch_, 30);
    toggle(minimized_, "Open BabbleForge minimized.", [this](bool on) {
        updatePrefs(ctx.settings, [&](Prefs& p) { p.startMinimized = on; });
        const Prefs p = loadPrefs(ctx.settings.get());
        if (p.launchAtLogin) applyLaunchAtLogin(true, on);
    });
    addRow(minimized_, 30);
    toggle(autoStart_, "Start masking as soon as BabbleForge opens.", [this](bool on) {
        Prefs p = loadPrefs(ctx.settings.get());
        startupNote_.setText({}, juce::dontSendNotification);
        if (on && !p.rememberDevice) {
            // §51: automatic start requires a remembered output device.
            const std::string dev = ctx.bridge.deviceId();
            if (dev.empty()) {
                startupNote_.setText("Choose an output device first. Automatic start needs a remembered output device.", juce::dontSendNotification);
                refreshPrefs();
                return;
            }
            p.rememberDevice = true;
            ctx.settings.update([&](AppSettingsData& d) { d.deviceId = dev; });
            startupNote_.setText("Output device remembered. Automatic start needs it.", juce::dontSendNotification);
        }
        p.autoStart = on;
        savePrefs(ctx.settings, p);
        refreshPrefs();
    });
    addAndMakeVisible(autoHelp_);
    addRow(autoStart_, 30, &autoHelp_);
    toggle(restore_, "Open with the configuration you used last time.", [this](bool on) {
        updatePrefs(ctx.settings, [&](Prefs& p) { p.restorePrevious = on; });
    });
    addRow(restore_, 30);
    toggle(remember_, "Keep using the same output device the next time BabbleForge starts.", [this](bool on) {
        Prefs p = loadPrefs(ctx.settings.get());
        startupNote_.setText({}, juce::dontSendNotification);
        p.rememberDevice = on;
        if (on) {
            const std::string dev = ctx.bridge.deviceId();
            ctx.settings.update([&](AppSettingsData& d) { d.deviceId = dev; });
        } else if (p.autoStart) {
            p.autoStart = false;
            startupNote_.setText("Automatic start was turned off: it needs a remembered output device.", juce::dontSendNotification);
        }
        savePrefs(ctx.settings, p);
        refreshPrefs();
    });
    addAndMakeVisible(rememberHelp_);
    addRow(remember_, 30, &rememberHelp_);
    startupNote_.setFont(fonts::body(13.5f));
    startupNote_.setColour(juce::Label::textColourId, Theme::get().warn);
    startupNote_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(startupNote_);
    addRow(startupNote_, 38);

    // ---- Voice library -----------------------------------------------------------------------
    addHeader(hLibrary_);
    for (auto* l : {&libStatus_, &libTalkers_, &libSpeech_}) {
        l->setFont(fonts::body(15.0f));
        l->setColour(juce::Label::textColourId, Theme::get().text);
        l->setInterceptsMouseClicks(false, false);
        addAndMakeVisible(*l);
    }
    addAndMakeVisible(libHelp_);
    addRow(libStatus_, 24, &libHelp_);
    addRow(libTalkers_, 24);
    addRow(libSpeech_, 24);
    caption(libPath_, {});
    libPath_.setFont(fonts::body(12.5f));
    addAndMakeVisible(libPath_);
    addRow(libPath_, 22);
    noFocus(manage_);
    manage_.setTooltip("Add recordings, check the library or analyse it again.");
    manage_.onClick = [this] { openManageLibrary(); };
    addAndMakeVisible(manage_);
    addRow(manage_, 36);

    // ---- Appearance --------------------------------------------------------------------------
    addHeader(hLook_);
    caption(scaleCaption_, "Interface size");
    addAndMakeVisible(scaleCaption_);
    addRow(scaleCaption_, 22);
    for (int i = 0; i < 6; ++i) scale_.addItem(juce::String(juce::roundToInt(kScales[i] * 100.0)) + "%", i + 1);
    noFocus(scale_);
    scale_.setTooltip("Make everything larger or smaller.");
    scale_.onChange = [this] {
        if (building_) return;
        const int i = scale_.getSelectedItemIndex();
        if (i < 0 || i >= 6) return;
        const double s = kScales[i];
        ctx.settings.update([&](AppSettingsData& d) { d.uiScale = s; });
        juce::Desktop::getInstance().setGlobalScaleFactor(static_cast<float>(s));
    };
    addAndMakeVisible(scale_);
    addAndMakeVisible(scaleHelp_);
    addRow(scale_, 36, &scaleHelp_);
    noFocus(showIntro_);
    showIntro_.setTooltip("Show the explanation the next time Advanced mode is switched on.");
    showIntro_.onClick = [this] { ctx.settings.update([](AppSettingsData& d) { d.advancedIntroShown = false; }); };
    addAndMakeVisible(showIntro_);
    addRow(showIntro_, 36);

    // ---- Logging -----------------------------------------------------------------------------
    addHeader(hLog_);
    caption(levelCaption_, "Log detail");
    addAndMakeVisible(levelCaption_);
    addRow(levelCaption_, 22);
    for (int i = 0; i < 5; ++i) logLevel_.addItem(kLevelNames[i], i + 1);
    noFocus(logLevel_);
    logLevel_.setTooltip("How much detail is written to the log files.");
    logLevel_.onChange = [this] {
        if (building_) return;
        const int i = logLevel_.getSelectedItemIndex();
        if (i < 0 || i >= 5) return;
        const std::string id = kLevelIds[i];
        updatePrefs(ctx.settings, [&](Prefs& p) { p.logLevel = id; });
        ctx.bridge.setLogLevel(levelOf(id));  // also kept for controllers created by a device change
    };
    addAndMakeVisible(logLevel_);
    addAndMakeVisible(levelHelp_);
    addRow(logLevel_, 36, &levelHelp_);
    toggle(redactLogs_, "Replace folder names in the log files with short codes (applies at the next start).", [this](bool on) {
        updatePrefs(ctx.settings, [&](Prefs& p) { p.redactLogs = on; });
    });
    addAndMakeVisible(redactLogsHelp_);
    addRow(redactLogs_, 30, &redactLogsHelp_);
    toggle(redactExports_, "Remove folder and user names from exported diagnostics.", [this](bool on) {
        updatePrefs(ctx.settings, [&](Prefs& p) { p.redactExports = on; });
    });
    addAndMakeVisible(redactExportsHelp_);
    addRow(redactExports_, 30, &redactExportsHelp_);
    noFocus(openLogs_);
    openLogs_.setTooltip("Open the folder that contains the log files.");
    openLogs_.onClick = [this] { openLogFolder(); };
    addAndMakeVisible(openLogs_);
    addRow(openLogs_, 36);

    // ---- Diagnostics -------------------------------------------------------------------------
    addHeader(hDiag_);
    caption(diagNote_, "Saves a small zip file with the current state and recent log entries for support.");
    diagNote_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(diagNote_);
    addRow(diagNote_, 38);
    noFocus(saveDiag_);
    saveDiag_.setTooltip("Save a diagnostics zip file.");
    saveDiag_.onClick = [this] { chooseDiagnosticsFile(); };
    addAndMakeVisible(saveDiag_);
    addRow(saveDiag_, 36);
    saveStatus_.setFont(fonts::body(13.5f));
    saveStatus_.setColour(juce::Label::textColourId, Theme::get().textDim);
    addAndMakeVisible(saveStatus_);
    addRow(saveStatus_, 22);
    fallback_.setOptionTooltip(0, "Stop with an error if the voice library has problems.");
    fallback_.setOptionTooltip(1, "Switch to steady masking noise if the voice library has problems.");
    fallback_.setOptionTooltip(2, "Keep the voices going and replace what is missing.");
    fallback_.onSelect = [this](int i) {
        if (building_ || i < 0 || i > 2) return;
        const std::string v = kFallbackIds[i];
        state().edit("Fallback policy", "", [&](Preset& p) { p.fallbackPolicy = v; });
    };
    addAndMakeVisible(fallback_);
    addAndMakeVisible(fallbackHelp_);
    addRow(fallback_, fallback_.preferredHeight(), &fallbackHelp_);

    building_ = false;
    ctx.bridge.refreshDevices();
    refreshPrefs();
    refreshDevices();
    refreshLibrary();
    refreshFromState();
}

void SettingsPage::addHeader(SectionLabel& l) {
    addAndMakeVisible(l);
    rows_.push_back({&l, nullptr, 26, true});
}

void SettingsPage::addRow(juce::Component& c, int h, HelpButton* help) { rows_.push_back({&c, help, h, false}); }

void SettingsPage::toggle(juce::ToggleButton& t, const juce::String& tip, std::function<void(bool)> fn) {
    noFocus(t);
    t.setTooltip(tip);
    t.onClick = [this, &t, fn = std::move(fn)] {
        if (!building_) fn(t.getToggleState());
    };
    addAndMakeVisible(t);
}

int SettingsPage::layoutPage(int width) {
    auto col = column(width, 640, 24);
    int y = col.getY();
    for (const auto& r : rows_) {
        if (r.header) y += 14;
        auto row = juce::Rectangle<int>(col.getX(), y, col.getWidth(), r.h);
        if (r.help) {
            r.help->setBounds(row.removeFromRight(26).withSizeKeepingCentre(24, 24));
            row.removeFromRight(6);
        }
        if (dynamic_cast<juce::TextButton*>(r.c)) row = row.removeFromLeft(juce::jmin(row.getWidth(), 360));
        r.c->setBounds(row);
        y += r.h + (r.header ? 6 : 4);
    }
    return y + 30;
}

void SettingsPage::refreshPrefs() {
    const bool was = building_;
    building_ = true;
    const Prefs p = loadPrefs(ctx.settings.get());
    launch_.setToggleState(p.launchAtLogin, juce::dontSendNotification);
    minimized_.setToggleState(p.startMinimized, juce::dontSendNotification);
    autoStart_.setToggleState(p.autoStart, juce::dontSendNotification);
    restore_.setToggleState(p.restorePrevious, juce::dontSendNotification);
    remember_.setToggleState(p.rememberDevice, juce::dontSendNotification);
    redactLogs_.setToggleState(p.redactLogs, juce::dontSendNotification);
    redactExports_.setToggleState(p.redactExports, juce::dontSendNotification);
    int lvl = 2;
    for (int i = 0; i < 5; ++i)
        if (p.logLevel == kLevelIds[i]) lvl = i;
    logLevel_.setSelectedItemIndex(lvl, juce::dontSendNotification);
    int sc = 2;
    for (int i = 0; i < 6; ++i)
        if (std::abs(ctx.settings.get().uiScale - kScales[i]) < 0.01) sc = i;
    scale_.setSelectedItemIndex(sc, juce::dontSendNotification);
    building_ = was;
}

void SettingsPage::refreshDevices() {
    const bool was = building_;
    building_ = true;
    const auto list = ctx.bridge.deviceList();
    std::string key;
    for (const auto& d : list) key += d.id + "|";
    key += ctx.bridge.deviceId();
    if (key != lastDeviceList_) {
        lastDeviceList_ = key;
        device_.clear(juce::dontSendNotification);
        deviceIds_.clear();
        int sel = -1;
        for (const auto& d : list) {
            if (d.numOutputs <= 0) continue;
            deviceIds_.push_back(d.id);
            device_.addItem(juce::String::fromUTF8(d.name.c_str()) + (d.type.empty() ? juce::String() : " (" + juce::String::fromUTF8(d.type.c_str()) + ")"),
                            static_cast<int>(deviceIds_.size()));
            if (d.id == ctx.bridge.deviceId()) sel = static_cast<int>(deviceIds_.size()) - 1;
        }
        if (sel >= 0) device_.setSelectedItemIndex(sel, juce::dontSendNotification);
        else device_.setText(ctx.bridge.deviceId().empty() ? juce::String("No output selected") : juce::String::fromUTF8(ctx.bridge.deviceId().c_str()),
                             juce::dontSendNotification);
    }
    building_ = was;
}

void SettingsPage::refreshStatus(const EngineStatus& s) {
    refreshDevices();
    juce::String info;
    if (s.sampleRate > 0.0) info << juce::String(s.sampleRate / 1000.0, 1) << " kHz, " << s.bufferFrames << " samples, " << s.outputs << " outputs";
    deviceInfo_.setText(info, juce::dontSendNotification);
}

bool SettingsPage::forcedFallback() {
    const auto it = state().data().strategies.find(state().preset().strategy);
    return it != state().data().strategies.end() && it->second.forced.fallbackPolicy.has_value();
}

void SettingsPage::refreshFromState() {
    const bool was = building_;
    building_ = true;
    const std::string eff = state().effective().fallbackPolicy;
    int idx = 2;
    for (int i = 0; i < 3; ++i)
        if (eff == kFallbackIds[i]) idx = i;
    fallback_.setSelected(idx);
    const bool forced = forcedFallback();
    for (int i = 0; i < 3; ++i)
        fallback_.setOptionEnabled(i, !forced, forced ? "This mask type always uses " + juce::String(eff) + "." : juce::String());
    building_ = was;
}

// ---- library --------------------------------------------------------------------------------

void SettingsPage::refreshLibrary() {
    const auto root = libraryRootOf(ctx.settings.get());
    const auto s = readLibraryStatus(root);
    libStatus_.setText("Status: " + s.stateText(), juce::dontSendNotification);
    libTalkers_.setText("Talkers: " + (s.exists ? juce::String(s.talkers) : juce::String("-")), juce::dontSendNotification);
    libSpeech_.setText("Usable speech: " + (s.exists ? formatDuration(s.usableSpeechS) : juce::String("-")), juce::dontSendNotification);
    libPath_.setText(fromPath(root), juce::dontSendNotification);
}

void SettingsPage::openManageLibrary() {
    auto* main = findParentComponentOfClass<MainComponent>();
    if (!main) return;
    juce::Component::SafePointer<SettingsPage> self(this);
    auto h = makeManageLibraryDialog(
        ctx.settings, [main](std::unique_ptr<OverlayDialog> d) { main->showDialog(std::move(d)); },
        [self] {
            if (self) self->refreshLibrary();
        });
    main->showDialog(std::move(h.dialog));
}

// ---- logging / diagnostics -------------------------------------------------------------------------

juce::File SettingsPage::logDirectory() const {
    if (auto c = ctx.bridge.controller()) {
        const auto& d = c->config().log.dir;
        if (!d.empty()) return juce::File(fromPath(d));
    }
    return juce::File(fromPath(AppSettings::defaultDirectory() / "logs"));
}

void SettingsPage::openLogFolder() {
    const auto dir = logDirectory();
    dir.createDirectory();
    if (!dir.startAsProcess()) dir.revealToUser();
}

void SettingsPage::chooseDiagnosticsFile() {
    chooser_ = std::make_unique<juce::FileChooser>(
        "Save diagnostics", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("BabbleForge-diagnostics.zip"), "*.zip");
    juce::Component::SafePointer<SettingsPage> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [self](const juce::FileChooser& fc) {
        if (!self) return;
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (!f.hasFileExtension("zip")) f = f.withFileExtension("zip");
        juce::String err;
        const bool ok = self->saveDiagnosticsZip(f, &err);
        self->saveStatus_.setText(ok ? "Saved " + f.getFileName() : "Could not save: " + err, juce::dontSendNotification);
    });
}

bool SettingsPage::saveDiagnosticsZip(const juce::File& target, juce::String* error) {
    auto fail = [&](const juce::String& m) {
        if (error) *error = m;
        return false;
    };
    auto c = ctx.bridge.controller();
    if (!c) return fail("The audio engine is not available.");
    const bool redact = loadPrefs(ctx.settings.get()).redactExports;
    nlohmann::json diag = c->diagnostics();
    nlohmann::json preset = c->currentPreset();
    if (redact) {
        rt::Logger::redactJson(diag);
        rt::Logger::redactJson(preset);
    }
    juce::ZipFile::Builder zip;
    const auto now = juce::Time::getCurrentTime();
    auto addText = [&](const juce::String& name, const std::string& text) {
        zip.addEntry(new juce::MemoryInputStream(text.data(), text.size(), true), 6, name, now);
    };
    addText("diagnostics.json", diag.dump(2));
    addText("preset.json", preset.dump(2));
    int logs = 0;
    for (const auto& f : c->logger().files()) {
        std::string text;
        {
            juce::File jf(fromPath(f));
            if (!jf.existsAsFile()) continue;
            text = jf.loadFileAsString().toStdString();
        }
        if (redact) text = rt::Logger::redactPaths(text);
        addText("logs/" + fromPath(f.filename()), text);
        ++logs;
    }
    target.getParentDirectory().createDirectory();
    target.deleteFile();
    juce::FileOutputStream out(target);
    if (!out.openedOk()) return fail("The file cannot be written.");
    if (!zip.writeToStream(out, nullptr)) return fail("The zip file could not be created.");
    out.flush();
    return true;
}

}  // namespace bf::gui
