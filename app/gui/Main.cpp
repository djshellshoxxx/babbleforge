// BabbleForge GUI application (docs/GUI.md, docs/GUI_ARCHITECTURE.md).
//
//   BabbleForge [--data-dir <dir>] [--corpus <dir>] [--state-dir <dir>]
//   BabbleForge --run-ui-tests        headless GUI tests (exit code = failures > 0)
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "LookAndFeel.h"
#include "MainComponent.h"
#include "dialogs/FirstRunWizard.h"
#include "model/Prefs.h"
#include "core/corpus/CorpusLoader.h"

#ifndef BF_DEFAULT_DATA_DIR
#define BF_DEFAULT_DATA_DIR "resources/data"
#endif

namespace bf::gui {
int runUiTests();  // tests/UiTests.cpp
}

namespace {

using namespace bf::gui;

juce::String argValue(const juce::StringArray& args, const juce::String& name) {
    const int i = args.indexOf(name);
    return (i >= 0 && i + 1 < args.size()) ? args[i + 1].unquoted() : juce::String();
}

class MainWindow final : public juce::DocumentWindow {
public:
    explicit MainWindow(MainComponent* content)
        : DocumentWindow("BabbleForge", Theme::get().bg, DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        setContentOwned(content, true);
        setResizable(true, true);
        setResizeLimits(760, 560, 10000, 10000);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        content->grabKeyboardFocus();
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class App final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "BabbleForge"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override {
        lnf_ = std::make_unique<BfLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel(lnf_.get());
        const auto args = juce::StringArray::fromTokens(commandLine, true);
        if (args.contains("--run-ui-tests")) {
            const int failures = runUiTests();
            setApplicationReturnValue(failures > 0 ? 1 : 0);
            quit();
            return;
        }

        juce::String dataDir = argValue(args, "--data-dir");
        if (dataDir.isEmpty()) {
            const auto beside = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("data");
            dataDir = beside.isDirectory() ? beside.getFullPathName() : juce::String(BF_DEFAULT_DATA_DIR);
        }
        auto dsr = bf::loadDataSet(toPath(dataDir));
        if (!dsr.ok) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "BabbleForge",
                                                   "The configuration data could not be loaded:\n" + juce::String(dsr.error),
                                                   "Quit", nullptr,
                                                   juce::ModalCallbackFunction::create([](int) { quit(); }));
            return;
        }
        data_ = std::make_unique<bf::DataSet>(std::move(dsr.data));

        juce::String stateDir = argValue(args, "--state-dir");
        const auto appDir = stateDir.isNotEmpty() ? toPath(stateDir) : AppSettings::defaultDirectory();
        std::error_code fsEc;
        const bool firstRun = !std::filesystem::exists(appDir / "settings.json", fsEc);  // GUI §60
        settings_ = std::make_unique<AppSettings>(appDir / "settings.json");
        const Prefs prefs = loadPrefs(settings_->get());
        juce::Desktop::getInstance().setGlobalScaleFactor(static_cast<float>(settings_->get().uiScale));

        EngineBridge::Options o;
        o.dataSet = data_.get();
        o.deviceId = settings_->get().deviceId;
        o.stateDir = appDir;
        {
            static const char* const levels[] = {"trace", "debug", "info", "warn", "error"};
            for (int i = 0; i < 5; ++i)
                if (prefs.logLevel == levels[i]) o.log.minLevel = static_cast<bf::rt::LogLevel>(i);
            o.log.redactPathsInLogs = prefs.redactLogs;
            o.sampleRate = prefs.sampleRate;
            o.bufferFrames = prefs.bufferFrames;
            o.redactPathsInExports = prefs.redactExports;
            // "Restore previous configuration" off: start from the factory configuration (GUI §51).
            if (!prefs.restorePrevious) o.preset = nlohmann::json::parse(bf::serializePreset(AppState::factoryPreset(*data_, "office", "balanced")));
        }
        juce::String corpusRoot = argValue(args, "--corpus");
        if (corpusRoot.isEmpty()) corpusRoot = juce::String::fromUTF8(settings_->get().corpusRoot.c_str());
        if (corpusRoot.isNotEmpty()) {
            std::string err;
            if (bf::loadCorpus(toPath(corpusRoot), corpus_, &err)) {
                o.corpus = corpus_.snapshot;
                o.audio = corpus_.audio.get();
            } else {
                DBG("voice library not loaded: " << err);
            }
        }
        bridge_ = std::make_unique<EngineBridge>(o);
        state_ = std::make_unique<AppState>(*data_, *settings_, undo_, bridge_->startupPreset());
        bridge_->attach(*state_);
        session_ = std::make_unique<PresetSession>(*state_, appDir / "user_presets");
        auto* main = new MainComponent(*state_, *session_, *bridge_, *settings_);
        window_ = std::make_unique<MainWindow>(main);
        if (firstRun) {
            main->showDialog(makeFirstRunDialog(*state_, *settings_, *bridge_));
        } else {
            if (prefs.startMinimized || args.contains("--minimized")) window_->setMinimised(true);
            // Automatic start needs a remembered output device (GUI §51).
            if (prefs.autoStart && prefs.rememberDevice && !settings_->get().deviceId.empty()) bridge_->start();
        }
    }

    void shutdown() override {
        window_.reset();
        if (bridge_ && settings_) {
            const auto dev = bridge_->deviceId();
            if (dev != settings_->get().deviceId) settings_->update([dev](AppSettingsData& d) { d.deviceId = dev; });
        }
        session_.reset();
        bridge_.reset();  // stops the engine; must go before the AppState it listens to
        state_.reset();
        settings_.reset();  // flushes settings.json
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        lnf_.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<BfLookAndFeel> lnf_;
    std::unique_ptr<bf::DataSet> data_;
    bf::LoadedCorpus corpus_;
    std::unique_ptr<AppSettings> settings_;
    juce::UndoManager undo_;
    std::unique_ptr<EngineBridge> bridge_;
    std::unique_ptr<AppState> state_;
    std::unique_ptr<PresetSession> session_;
    std::unique_ptr<MainWindow> window_;
};

}  // namespace

START_JUCE_APPLICATION(App)
