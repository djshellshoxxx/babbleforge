// BabbleForge GUI shell: empty main window + status bar bound to the EngineController status
// (docs/GUI.md §61). The full GUI comes later.
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "backend/JuceAudioBackend.h"
#include "core/config/DataSet.h"
#include "core/rt/EngineController.h"

#ifndef BF_DEFAULT_DATA_DIR
#define BF_DEFAULT_DATA_DIR "resources/data"
#endif

namespace {

class StatusBar final : public juce::Component, private juce::Timer {
public:
    explicit StatusBar(bf::rt::EngineController* c) : ctl_(c) {
        refresh();
        startTimerHz(10);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId).darker(0.3f));
        g.setColour(juce::Colours::lightgrey);
        g.setFont(14.0f);
        g.drawText(text_, getLocalBounds().reduced(8, 0), juce::Justification::centredLeft);
    }

private:
    void timerCallback() override { refresh(); }
    void refresh() {
        if (!ctl_) return;
        (void)ctl_->pollStatus();  // coalesced status; state() below is always current
        const auto m = ctl_->metrics();
        const auto dev = ctl_->config().deviceId;
        text_ = juce::String("Output: ") + (dev.empty() ? "none" : dev) +
                "     Engine: " + std::string(bf::rt::headlineFor(ctl_->state())) +
                "     CPU: " + (m.dspLoadP99 > 0.5 ? "High" : "Normal");
        repaint();
    }
    bf::rt::EngineController* ctl_;
    juce::String text_;
};

class MainComponent final : public juce::Component {
public:
    explicit MainComponent(bf::rt::EngineController* c) : bar_(c) {
        addAndMakeVisible(bar_);
        setSize(900, 600);
    }
    void paint(juce::Graphics& g) override { g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId)); }
    void resized() override { bar_.setBounds(getLocalBounds().removeFromBottom(28)); }

private:
    StatusBar bar_;
};

class MainWindow final : public juce::DocumentWindow {
public:
    explicit MainWindow(bf::rt::EngineController* c)
        : DocumentWindow("BabbleForge", juce::Colours::darkgrey, DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainComponent(c), true);
        setResizable(true, true);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class App final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "BabbleForge"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    void initialise(const juce::String&) override {
        const auto dsr = bf::loadDataSet(BF_DEFAULT_DATA_DIR);
        if (dsr.ok) {
            data_ = std::make_unique<bf::DataSet>(dsr.data);
            bf::rt::EngineControllerConfig cfg;
            cfg.dataSet = data_.get();
            cfg.backend = &backend_;
            ctl_ = std::make_unique<bf::rt::EngineController>(cfg);
        }
        window_ = std::make_unique<MainWindow>(ctl_.get());
    }
    void shutdown() override {
        window_.reset();
        ctl_.reset();
    }
    void systemRequestedQuit() override { quit(); }

private:
    bf::rt::JuceAudioBackend backend_;
    std::unique_ptr<bf::DataSet> data_;
    std::unique_ptr<bf::rt::EngineController> ctl_;
    std::unique_ptr<MainWindow> window_;
};

}  // namespace

START_JUCE_APPLICATION(App)
