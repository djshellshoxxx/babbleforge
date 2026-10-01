// Headless tests of the AREA and OUTPUT pages (registered in the BabbleForgeUI category, so
// `BabbleForge --run-ui-tests` runs them): area / speaker setup update the plan, zone and speaker
// edits reach the preset outputs block, the limiter toggle shows "Limiter disabled", TEST SPEAKERS
// / STOP TEST and the device-lost banner.
#include <cmath>

#include <juce_gui_basics/juce_gui_basics.h>

#include "MainComponent.h"
#include "core/rt/NullBackend.h"
#include "model/OutputModel.h"
#include "pages/AreaPage.h"
#include "pages/OutputPage.h"

#ifndef BF_DEFAULT_DATA_DIR
#define BF_DEFAULT_DATA_DIR "resources/data"
#endif

namespace bf::gui {

namespace {

void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

template <typename Pred>
bool pumpUntil(Pred p, int timeoutMs) {
    const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    while (juce::Time::getMillisecondCounter() < end) {
        if (p()) return true;
        pump(20);
    }
    return p();
}

// Button::triggerClick() is asynchronous: simulate the click synchronously.
void click(juce::Button& b) {
    if (b.getClickingTogglesState()) b.setToggleState(b.getRadioGroupId() != 0 ? true : !b.getToggleState(), juce::dontSendNotification);
    if (b.onClick) b.onClick();
}
void setChecked(juce::ToggleButton& b, bool on) {
    if (b.getToggleState() != on) click(b);
}

}  // namespace

class AreaOutputTests final : public juce::UnitTest {
public:
    AreaOutputTests() : juce::UnitTest("BabbleForge AREA / OUTPUT pages", "BabbleForgeUI") {}

    void runTest() override {
        const auto dsr = loadDataSet(BF_DEFAULT_DATA_DIR);
        expect(dsr.ok, juce::String(dsr.error));
        if (!dsr.ok) return;
        const DataSet& ds = dsr.data;
        const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("bf_area_output_tests_" + juce::String(juce::Time::currentTimeMillis()));
        tmp.createDirectory();

        AppSettings settings({});
        rt::NullBackend backend;
        backend.addDevice({"Null:Test Speakers", "Test Speakers", "Null", 2});
        backend.addDevice({"Null:Other", "Other Device", "Null", 8});
        EngineBridge::Options o;
        o.dataSet = &ds;
        o.backend = &backend;
        o.deviceId = "Null:Test Speakers";
        EngineBridge bridge(o);
        juce::UndoManager undo;
        AppState state(ds, settings, undo, bridge.startupPreset());
        bridge.attach(state);
        PresetSession session(state, toPath(tmp.getChildFile("user_presets").getFullPathName()));
        MainComponent main(state, session, bridge, settings);
        main.setSize(1100, 900);
        pump(50);

        areaSimple(main, state);
        areaAdvanced(main, state);
        limiterStatus(main, state, bridge);
        outputSimple(main, state, bridge);
        testSpeakersAndMeters(main, state, bridge);
        deviceLost(main, state, bridge, backend);

        tmp.deleteRecursively();
    }

private:
    void areaSimple(MainComponent& main, AppState& state) {
        beginTest("AREA page: area type, size and speaker setup update the plan");
        main.showPage("area");
        auto* page = dynamic_cast<AreaPage*>(main.page("area"));
        expect(page != nullptr, "AREA page registered");
        if (page == nullptr) return;
        expect(page->getHeight() > 400, "laid out");

        state.setArea("office");
        const double talkersOffice = state.plan().plan.talkers.targetMean();
        // Select Open Office through the combo box (area change loads its recommendation).
        for (int i = 0; i < page->areaBox().getNumItems(); ++i)
            if (page->areaBox().getItemText(i) == "Open Office") page->areaBox().setSelectedItemIndex(i, juce::sendNotificationSync);
        expectEquals(state.preset().area, std::string("open_office"));
        expect(state.plan().ok, "plan composed");
        expect(state.plan().plan.talkers.targetMean() > talkersOffice, "Open Office has a denser talker plan than Office");

        // Speaker setup: 4 speakers -> ring4 layout, plan has 4 active outputs.
        click(page->speakerGroup().button(1));
        expectEquals(state.preset().outputs.layout.value_or(""), std::string("ring4"));
        expectEquals(state.plan().plan.spatial.activeOutputs, 4);
        expectEquals(page->map().speakerCount(), 4);
        // Area size sets the spread.
        click(page->sizeGroup().button(2));
        expect(state.preset().spatial.spread.value_or(0.0) > 0.75, "Large area is a wide spread");
        click(page->sizeGroup().button(0));
        expect(state.preset().spatial.spread.value_or(1.0) < 0.45, "Small area is a narrow spread");

        // Contextual recommendation is non-modal text (GUI §56).
        state.setArea("large_room");
        click(page->speakerGroup().button(0));
        expect(page->recommendationLabel().getText().containsIgnoreCase("speakers"), page->recommendationLabel().getText());
        expect(main.dialog() == nullptr, "no modal dialog");
        // Outdoor shows the warning and the 2 / 4 / 6 / 8+ choices.
        state.setArea("free_field");
        expect(page->outdoorSpeakerGroup().isVisible() && !page->speakerGroup().isVisible(), "outdoor speaker layout");
        click(page->outdoorSpeakerGroup().button(3));
        expectEquals(state.plan().plan.spatial.activeOutputs, 8);
        state.setArea("office");
        click(page->speakerGroup().button(0));
    }

    void areaAdvanced(MainComponent& main, AppState& state) {
        beginTest("AREA page Advanced: spatial mode, speakers, zones write the outputs block");
        state.setMode(UiMode::Advanced);
        main.showPage("area");
        pump(30);
        auto* page = dynamic_cast<AreaPage*>(main.page("area"));
        if (page == nullptr) return;
        expect(page->spatialGroup().isVisible() && page->spreadSlider().isVisible(), "Advanced controls shown");
        click(page->spatialGroup().button(2));  // 4 Channel
        expectEquals(state.plan().plan.spatial.activeOutputs, 4);
        expectEquals(page->speakerRowCount(), 4);

        // Speaker 3: level -1.5 dB, delay 3.2 ms.
        page->speakerLevel(2).setValue(-1.5, juce::sendNotificationSync);
        page->speakerDelay(2).setValue(3.2, juce::sendNotificationSync);
        expectEquals(static_cast<int>(state.preset().outputs.channels.size()), 4);
        expectWithinAbsoluteError(state.preset().outputs.channels[2].gainDb, -1.5, 0.01);
        expectWithinAbsoluteError(state.preset().outputs.channels[2].delayMs, 3.2, 0.01);
        // Disable speaker 4: three active outputs in the plan.
        setChecked(page->speakerEnabled(3), false);
        expect(!state.preset().outputs.channels[3].enabled, "speaker 4 disabled");
        expectEquals(state.plan().plan.spatial.activeOutputs, 3);
        setChecked(page->speakerEnabled(3), true);
        expectEquals(state.plan().plan.spatial.activeOutputs, 4);

        // Spread and Speaker Variation.
        page->spreadSlider().slider.setValue(0.9, juce::sendNotificationSync);
        expectWithinAbsoluteError(state.preset().spatial.spread.value_or(0.0), 0.9, 0.001);
        click(page->variationGroup().button(2));
        expectEquals(state.preset().spatial.speakerVariation.value_or(""), std::string("high"));

        // Zones A / B: channels split, level and mix offset edits land in the outputs.zones block.
        click(page->addZoneButton());
        expectEquals(static_cast<int>(state.preset().outputs.zones.size()), 2);
        expectEquals(page->zoneRowCount(), 2);
        expectEquals(state.preset().outputs.channels[0].zone, 0);
        expectEquals(state.preset().outputs.channels[3].zone, 1);
        page->zoneLevel(1).setValue(-4.0, juce::sendNotificationSync);
        page->zoneMix(1).setValue(20.0, juce::sendNotificationSync);
        expectWithinAbsoluteError(state.preset().outputs.zones[1].levelDb, -4.0, 0.01);
        expectWithinAbsoluteError(state.preset().outputs.zones[1].babbleFractionOffset, 0.2, 0.001);
        setChecked(page->zoneEnabled(0), false);
        expect(!state.preset().outputs.zones[0].enabled, "zone A disabled");
        setChecked(page->zoneEnabled(0), true);
        expect(state.undoManager().canUndo(), "edits are undoable");
        // Back to the simple stereo setup for the following tests.
        outputs::setSpatialMode(state, outputs::SpatialMode::Stereo);
        expect(state.preset().outputs.zones.empty() && state.preset().outputs.channels.empty(), "layout change resets zones");
        state.setMode(UiMode::Simple);
    }

    void limiterStatus(MainComponent& main, AppState& state, EngineBridge& bridge) {
        beginTest("OUTPUT page: limiter toggle shows \"Limiter disabled\" in the status bar");
        state.setMode(UiMode::Advanced);
        main.showPage("output");
        pump(30);
        auto* page = dynamic_cast<OutputPage*>(main.page("output"));
        expect(page != nullptr, "OUTPUT page registered");
        if (page == nullptr) return;
        expect(page->limiterToggle().isVisible(), "limiter controls in Advanced");
        expect(main.statusBar().warning().isEmpty(), "limiter on: no warning");
        click(page->limiterToggle());  // switch off
        pump(40);
        expect(!state.preset().outputs.limiterEnabled.value_or(true), "preset limiter off");
        expectEquals(main.statusBar().warning(), juce::String("Limiter disabled"));
        expect(!page->ceilingSlider().isEnabled(), "ceiling disabled while the limiter is off");
        click(page->limiterToggle());
        pump(40);
        expect(main.statusBar().warning().isEmpty(), "limiter back on");
        page->ceilingSlider().setValue(-2.0, juce::sendNotificationSync);
        expectWithinAbsoluteError(state.preset().outputs.limiterCeilingDbtp.value_or(0.0), -2.0, 0.001);
        state.setMode(UiMode::Simple);
        bridge.waitIdle(3000);
    }

    void outputSimple(MainComponent& main, AppState& state, EngineBridge& bridge) {
        beginTest("OUTPUT page Simple: device, mode, master level");
        main.showPage("output");
        pump(100);
        auto* page = dynamic_cast<OutputPage*>(main.page("output"));
        if (page == nullptr) return;
        expect(!page->limiterToggle().isVisible(), "limiter controls hidden in Simple");
        expect(page->deviceBox().getNumItems() >= 1, "devices listed");
        expectEquals(bridge.deviceId(), std::string("Null:Test Speakers"));
        // Never auto-switches: only an explicit choice changes the device.
        auto selectDevice = [&](const juce::String& name) {
            for (int i = 0; i < page->deviceBox().getNumItems(); ++i)
                if (page->deviceBox().getItemText(i).startsWith(name)) page->deviceBox().setSelectedItemIndex(i, juce::sendNotificationSync);
        };
        selectDevice("Other Device");
        expect(pumpUntil([&] { return bridge.deviceId() == "Null:Other"; }, 3000), "explicit device choice");
        expectEquals(page->deviceBox().getText().upToFirstOccurrenceOf("  (", false, false), juce::String("Other Device"));
        selectDevice("Test Speakers");
        expect(pumpUntil([&] { return bridge.deviceId() == "Null:Test Speakers"; }, 3000), "back to the first device");
        click(page->modeGroup().button(1));
        expect(state.coverage() == Coverage::MultiSpeaker, "Multi-Speaker mode");
        expectEquals(page->meterCount(), 4);
        click(page->modeGroup().button(0));
        expectEquals(page->meterCount(), 2);
        expectEquals(page->meter(0).name(), juce::String("Left"));
        page->masterSlider().slider.setValue(-6.0, juce::sendNotificationSync);
        expectWithinAbsoluteError(state.strengthDb(), -6.0, 0.01);
        bridge.waitIdle(5000);
    }

    void testSpeakersAndMeters(MainComponent& main, AppState& state, EngineBridge& bridge) {
        beginTest("OUTPUT page: TEST SPEAKERS, STOP TEST and meters");
        state.setMode(UiMode::Simple);
        main.showPage("output");
        auto* page = dynamic_cast<OutputPage*>(main.page("output"));
        if (page == nullptr) return;
        expect(!page->stopTestButton().isVisible(), "no STOP TEST before a test");
        click(page->testButton());
        const bool on = pumpUntil([&] { return bridge.status().out.testRunning; }, 25000);
        expect(on, "test signal running");
        pump(200);
        expect(page->stopTestButton().isVisible(), "prominent STOP TEST while testing");
        expect(page->testLabel().getText().contains("Testing"), page->testLabel().getText());
        expect(page->stopTestButton().getHeight() >= 48, "STOP TEST is large");
        // Meters and the LOW / GOOD / HIGH band run while audio plays.
        pumpUntil([&] { return page->meter(0).level() > 0.0f; }, 5000);
        logMessage("meter level " + juce::String(page->meter(0).level()) + " band " + page->bandLabel().getText());
        expect(page->bandLabel().getText() == "LOW" || page->bandLabel().getText() == "GOOD" || page->bandLabel().getText() == "HIGH" ||
                   page->bandLabel().getText() == "-",
               page->bandLabel().getText());
        click(page->stopTestButton());
        expect(pumpUntil([&] { return !bridge.status().out.testRunning; }, 5000), "test stopped");
        pump(100);
        expect(!page->stopTestButton().isVisible() && page->testButton().isVisible(), "TEST SPEAKERS button back");
        // The engine that the test started is stopped again.
        expect(pumpUntil([&] { return bridge.status().state == rt::EngineState::Stopped; }, 10000), "engine stopped after the test");
    }

    void deviceLost(MainComponent& main, AppState& state, EngineBridge& bridge, rt::NullBackend& backend) {
        beginTest("Device lost: large status, Reconnect / Choose Device");
        state.setMode(UiMode::Simple);
        main.showPage("output");
        auto* page = dynamic_cast<OutputPage*>(main.page("output"));
        if (page == nullptr) return;
        expect(!page->bannerLabel().isVisible(), "no banner while healthy");
        bridge.start();
        expect(pumpUntil([&] { return bridge.masking() && bridge.status().state != rt::EngineState::Preparing; }, 20000), "running");
        expect(pumpUntil([&] { return bridge.status().state == rt::EngineState::Running || bridge.status().state == rt::EngineState::Degraded; }, 20000));
        backend.injectDeviceLost();
        expect(pumpUntil([&] { return bridge.status().state == rt::EngineState::DeviceLost; }, 10000), "device lost state");
        pump(100);
        expect(page->bannerLabel().isVisible(), "banner shown");
        expectEquals(page->bannerLabel().getText(), juce::String("OUTPUT DEVICE LOST"));
        expect(page->reconnectButton().isVisible() && page->chooseDeviceButton().isVisible(), "Reconnect / Choose Device buttons");
        expect(page->bannerLabel().getHeight() >= 36, "large status");
        expectEquals(bridge.deviceId(), std::string("Null:Test Speakers"), "no silent device switch");
        expect(page->statusLabel().getText().contains("lost"), page->statusLabel().getText());
        // The device returns: Reconnect resumes on the same device.
        backend.injectDeviceReturn();
        click(page->reconnectButton());
        expect(pumpUntil([&] { return bridge.status().state == rt::EngineState::Running || bridge.status().state == rt::EngineState::Degraded; }, 20000),
               "reconnected");
        pump(100);
        expect(!page->bannerLabel().isVisible() || bridge.status().state == rt::EngineState::Degraded, "banner cleared");
        expectEquals(bridge.deviceId(), std::string("Null:Test Speakers"));
        bridge.stop();
        pumpUntil([&] { return bridge.status().state == rt::EngineState::Stopped; }, 10000);
    }
};

static AreaOutputTests areaOutputTests;

}  // namespace bf::gui
