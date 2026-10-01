// Headless GUI tests: `BabbleForge --run-ui-tests` (ctest: gui_ui_tests).
// Builds the full main component offscreen on a NullBackend and drives it through the model
// and the widgets: Simple/Advanced, Strength/Character, undo/redo, debounced plan pushes,
// talker-count invariants, MASK advanced controls, preset session, shortcuts, start/stop.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

#include <juce_gui_basics/juce_gui_basics.h>

#include "MainComponent.h"
#include "core/rt/NullBackend.h"
#include "model/Catalog.h"
#include "pages/MaskPage.h"
#include "pages/RunPage.h"

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

juce::KeyPress cmdKey(int c, bool shift = false) {
    return juce::KeyPress(c, juce::ModifierKeys::commandModifier | (shift ? juce::ModifierKeys::shiftModifier : 0), 0);
}

}  // namespace

class UiTests final : public juce::UnitTest {
public:
    UiTests() : juce::UnitTest("BabbleForge GUI", "BabbleForgeUI") {}

    void runTest() override {
        beginTest("Data set");
        const auto dsr = loadDataSet(BF_DEFAULT_DATA_DIR);
        expect(dsr.ok, juce::String(dsr.error));
        if (!dsr.ok) return;
        const DataSet& ds = dsr.data;

        const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("bf_ui_tests_" + juce::String(juce::Time::currentTimeMillis()));
        tmp.createDirectory();

        AppSettings settings({});  // in memory
        rt::NullBackend backend;
        backend.addDevice({"Null:Test Speakers", "Test Speakers", "Null", 2});
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
        main.setSize(1100, 820);
        pump(50);

        snapshot(main, "run_simple");
        runLayout(main, state);
        runModes(main, state, settings);
        runStrengthCharacterUndo(main, state);
        runPlanPushes(state, bridge);
        runTalkerInvariants(main, state);
        runMaskAdvanced(main, state);
        main.showPage("mask");
        snapshot(main, "mask_advanced");
        main.showPage("run");
        snapshot(main, "run_advanced");
        runPresetSession(main, state, session, tmp);
        runShortcutsAndEngine(main, state, bridge);

        tmp.deleteRecursively();
    }

private:
    // BF_UI_SNAPSHOT_DIR=<dir>: writes PNG snapshots of the main component (visual review).
    static void snapshot(MainComponent& main, const char* name) {
        const auto dir = juce::SystemStats::getEnvironmentVariable("BF_UI_SNAPSHOT_DIR", {});
        if (dir.isEmpty()) return;
        // Render the whole scrollable page, not only the visible part.
        const std::string id = main.currentPageId();
        auto* p = main.page(id);
        const auto img = main.createComponentSnapshot(main.getLocalBounds());
        juce::PNGImageFormat png;
        juce::FileOutputStream out(juce::File(dir).getChildFile(juce::String(name) + ".png"));
        if (out.openedOk()) {
            out.setPosition(0);
            out.truncate();
            png.writeImageToStream(img, out);
        }
        if (p != nullptr) {
            const auto full = p->createComponentSnapshot(p->getLocalBounds());
            juce::FileOutputStream o2(juce::File(dir).getChildFile(juce::String(name) + "_page.png"));
            if (o2.openedOk()) {
                o2.setPosition(0);
                o2.truncate();
                png.writeImageToStream(full, o2);
            }
        }
    }

    void runLayout(MainComponent& main, AppState& state) {
        beginTest("Main window layout and page registry");
        for (const char* id : {"run", "mask", "area", "output", "analysis", "settings"})
            expect(PageRegistry::find(id) != nullptr, juce::String("page registered: ") + id);
        expectEquals(juce::String(main.currentPageId()), juce::String("run"));
        expect(!state.advanced(), "Simple is the default");
        expectEquals(static_cast<int>(main.sidebarButtons().size()), 4, "Simple: RUN MASK AREA OUTPUT");
        expect(dynamic_cast<RunPage*>(main.page("run")) != nullptr);
        expect(dynamic_cast<MaskPage*>(main.page("mask")) != nullptr);
        expect(main.statusBar().text().contains("Output: Test Speakers"), main.statusBar().text());
        expect(main.statusBar().text().contains("Engine: Ready"), main.statusBar().text());
        auto* run = dynamic_cast<RunPage*>(main.page("run"));
        expectEquals(run->startStopButton().getButtonText(), juce::String("START MASKING"));
        expectEquals(run->statusLabel().getText(), juce::String("READY"));
        expect(run->getHeight() > 600, "RUN page laid out");
    }

    void runModes(MainComponent& main, AppState& state, AppSettings& settings) {
        beginTest("Simple / Advanced toggle with one-time explanation");
        main.requestMode(UiMode::Advanced);
        expect(main.dialog() != nullptr, "first activation shows the explanation");
        expect(!state.advanced(), "not advanced until confirmed");
        auto* enable = main.dialog() ? main.dialog()->findButton("Enable Advanced") : nullptr;
        expect(enable != nullptr);
        if (enable) enable->onClick();
        pump(30);
        expect(main.dialog() == nullptr, "dialog closed");
        expect(state.advanced());
        expect(settings.get().advanced, "mode persisted");
        expect(settings.get().advancedIntroShown);
        expectEquals(static_cast<int>(main.sidebarButtons().size()), 5, "Advanced adds ANALYSIS");
        expect(main.statusBar().text().contains("Null | "), main.statusBar().text());
        expect(main.handleKey(cmdKey('5')));
        expectEquals(juce::String(main.currentPageId()), juce::String("analysis"));
        main.requestMode(UiMode::Simple);
        expect(!state.advanced());
        expectEquals(juce::String(main.currentPageId()), juce::String("run"), "ANALYSIS hidden in Simple");
        main.handleKey(cmdKey('5'));
        expectEquals(juce::String(main.currentPageId()), juce::String("run"), "Ctrl+5 ignored in Simple");
        main.requestMode(UiMode::Advanced);
        expect(main.dialog() == nullptr, "explanation not shown again");
        expect(state.advanced());
        main.handleKey(cmdKey('2'));
        expectEquals(juce::String(main.currentPageId()), juce::String("mask"));
        main.handleKey(cmdKey('1'));
        expectEquals(juce::String(main.currentPageId()), juce::String("run"));
    }

    void runStrengthCharacterUndo(MainComponent& main, AppState& state) {
        beginTest("Strength labels, Character and undo/redo");
        const DataSet& ds = state.data();
        expectEquals(strengthLabelFor(ds, 0.0), juce::String("Normal"));
        expectEquals(strengthLabelFor(ds, -12.0), juce::String("Gentle"));
        expectEquals(strengthLabelFor(ds, -6.0), juce::String("Low"));
        expectEquals(strengthLabelFor(ds, 5.0), juce::String("Strong"));
        expectEquals(strengthLabelFor(ds, 9.0), juce::String("Very Strong"));

        auto* run = dynamic_cast<RunPage*>(main.page("run"));
        auto& strength = run->strengthSlider();
        const double s0 = state.strengthDb();
        expectWithinAbsoluteError(s0, 0.0, 1e-9, "default strength Normal (0 dB)");
        // One drag = one undo step, however many values it passes through.
        strength.onGestureStart();
        for (double v : {1.0, 2.0, 3.5, 5.0}) strength.slider.setValue(v, juce::sendNotificationSync);
        strength.onGestureEnd();
        expectWithinAbsoluteError(state.strengthDb(), 5.0, 1e-9);
        expect(state.advanced() && state.strengthDb() > -100.0);
        expect(state.undo());
        expectWithinAbsoluteError(state.strengthDb(), s0, 1e-9, "drag undone in one step");
        expect(state.redo());
        expectWithinAbsoluteError(state.strengthDb(), 5.0, 1e-9, "redo");
        expect(strength.value() > 4.9, "slider follows undo/redo");

        auto& character = run->characterSlider();
        const double c0 = state.character();
        character.slider.setValue(0.8, juce::sendNotificationSync);  // two separate (non-drag) edits
        character.slider.setValue(0.3, juce::sendNotificationSync);
        expectWithinAbsoluteError(state.character(), 0.3, 1e-9);
        expect(main.handleKey(cmdKey('z')), "Ctrl+Z");
        expectWithinAbsoluteError(state.character(), 0.8, 1e-9);
        main.handleKey(cmdKey('z'));
        expectWithinAbsoluteError(state.character(), c0, 1e-6);
        expect(main.handleKey(cmdKey('z', true)), "Ctrl+Shift+Z");
        expectWithinAbsoluteError(state.character(), 0.8, 1e-9);
        expectWithinAbsoluteError(character.value(), 0.8, 1e-9, "slider shows redone value");

        // Advanced shows dB; Simple range is -18..+9.
        expect(strength.valueText(5.0).contains("dB"));
        state.setMode(UiMode::Simple);
        expectWithinAbsoluteError(strength.slider.getMaximum(), ds.engineDefaults.simpleMaxDb, 1e-9);
        expectEquals(strength.valueText(5.0), juce::String("Strong"));
        state.setMode(UiMode::Advanced);
        expectWithinAbsoluteError(strength.slider.getMaximum(), ds.engineDefaults.advancedMaxDb, 1e-9);

        // Area selection loads the recommendation but keeps Strength; mask type via quick card.
        state.setArea("open_office");
        expectEquals(juce::String(state.preset().area), juce::String("open_office"));
        expectWithinAbsoluteError(state.strengthDb(), 5.0, 1e-9, "area change keeps Strength");
        expectEquals(run->areaBox().getText(), juce::String("Open Office"));
        run->card(1).onClick();
        expectEquals(juce::String(state.preset().strategy), juce::String("natural"));
        expect(run->card(1).getToggleState());
        expectEquals(run->maskBox().getText(), juce::String("Natural"));
        state.undo();
        expectEquals(juce::String(state.preset().strategy), juce::String("balanced"));
        run->coverage().button(1).setToggleState(true, juce::sendNotificationSync);
        expect(state.coverage() == Coverage::MultiSpeaker);
        state.undo();
        expect(state.coverage() == Coverage::Stereo);
    }

    void runPlanPushes(AppState& state, EngineBridge& bridge) {
        beginTest("Debounced plan pushes (worker thread)");
        expect(bridge.waitIdle(3000));
        pump(250);
        expect(bridge.waitIdle(3000));
        const auto n0 = bridge.pushCount();
        const auto t0 = std::chrono::steady_clock::now();
        state.setStrengthDb(-3.0);
        state.setStrengthDb(-4.0);
        state.setStrengthDb(-5.0);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        logMessage("3 edits on the message thread: " + juce::String(ms, 2) + " ms");
        expect(ms < 50.0, "edits do not block the UI");
        expect(bridge.pushPending(), "push is debounced");
        expectEquals(static_cast<juce::int64>(bridge.pushCount()), static_cast<juce::int64>(n0), "nothing pushed yet");
        pump(60);
        expect(bridge.pushPending(), "still inside the 150 ms debounce");
        expect(pumpUntil([&] { return !bridge.pushPending(); }, 1000), "debounce elapsed");
        expect(bridge.waitIdle(3000));
        expectEquals(static_cast<juce::int64>(bridge.pushCount()), static_cast<juce::int64>(n0 + 1), "coalesced to one push");
        const auto last = bridge.lastPushedPreset();
        expect(last.has_value() && last->macros.strengthDb && std::abs(*last->macros.strengthDb + 5.0) < 1e-9,
               "latest preset pushed");
        const auto doc = bridge.controller()->currentPreset();
        expect(doc.contains("macros") && std::abs(doc["macros"].value("strengthDb", 0.0) + 5.0) < 1e-9,
               "controller received the preset");
    }

    void runTalkerInvariants(MainComponent& main, AppState& state) {
        beginTest("Talker engine: min <= average <= max <= pool enforced");
        main.showPage("mask");
        auto* mask = dynamic_cast<MaskPage*>(main.page("mask"));
        expect(mask->talkerPanel().isVisible(), "Talker Engine panel in Advanced");
        auto check = [&](const juce::String& what) {
            const TalkerCounts c = state.talkerCounts();
            expect(c.minimum <= c.average + 1e-9 && c.average <= c.maximum + 1e-9 && c.maximum <= c.pool,
                   what + ": " + juce::String(c.minimum) + " <= " + juce::String(c.average) + " <= " + juce::String(c.maximum) +
                       " <= " + juce::String(c.pool));
            expect(c.pool >= TalkerLimits::kPoolMin && c.pool <= TalkerLimits::kPoolMax);
            expect(c.average >= TalkerLimits::kAvgMin && c.average <= TalkerLimits::kAvgMax);
            expect(state.plan().ok, "plan composes");
        };
        mask->minimum().slider.setValue(12, juce::sendNotificationSync);
        check("min raised");
        expectEquals(state.talkerCounts().minimum, 12);
        expect(state.talkerCounts().average >= 12.0);
        mask->maximum().slider.setValue(3, juce::sendNotificationSync);
        check("max lowered");
        expectEquals(state.talkerCounts().maximum, 3);
        mask->average().slider.setValue(20, juce::sendNotificationSync);
        check("average raised");
        expect(state.talkerCounts().maximum >= 20);
        mask->pool().slider.setValue(5, juce::sendNotificationSync);
        check("pool lowered");
        expect(state.talkerCounts().maximum <= 5);
        expectEquals(static_cast<int>(mask->maximum().slider.getValue()), state.talkerCounts().maximum, "UI shows enforced value");

        std::mt19937 rng(7);
        for (int i = 0; i < 300; ++i) {
            const auto f = static_cast<TalkerField>(rng() % 4);
            const double v = std::uniform_real_distribution<double>(-5.0, 70.0)(rng);
            state.setTalkerValue(f, v);
            const TalkerCounts c = state.talkerCounts();
            if (!(c.minimum <= c.average && c.average <= c.maximum && c.maximum <= c.pool)) {
                check("fuzz " + juce::String(i));
                break;
            }
        }
        check("after fuzz");
        // Pure function.
        const TalkerCounts e = AppState::enforceTalkerCounts({10, 2.0, 30, 1}, TalkerField::Minimum);
        expect(e.minimum <= e.average && e.average <= e.maximum && e.maximum <= e.pool);
        expect(state.undo(), "talker edits are undoable");
    }

    void runMaskAdvanced(MainComponent& main, AppState& state) {
        beginTest("MASK page: mix, stationary, spectrum, custom EQ, correction");
        auto* mask = dynamic_cast<MaskPage*>(main.page("mask"));
        state.setStrategy("hybrid");
        expect(mask->mix().isVisible(), "Mask Mix only for Hybrid");
        mask->mix().slider.setValue(0.25, juce::sendNotificationSync);
        expectWithinAbsoluteError(state.babbleFraction(), 0.25, 1e-9);
        state.setStrategy("balanced");
        expect(!mask->mix().isVisible());

        mask->stationaryPanel().setExpanded(true);
        state.setStationaryEnabled(false);
        expect(!state.stationaryEnabled());
        state.setStationaryEnabled(true);

        mask->spectrumPanel().setExpanded(true);
        mask->spectrumMode().button(5).setToggleState(true, juce::sendNotificationSync);
        expectEquals(juce::String(state.spectrumTarget()), juce::String("custom"));
        expectWithinAbsoluteError(mask->eqBand(3).getMaximum(), kEqDefaultRangeDb, 1e-9, "default range +-6 dB");
        mask->eqBand(3).setValue(4.0, juce::sendNotificationSync);
        expectWithinAbsoluteError(state.customEqDb()[3], 4.0, 1e-6);
        state.setCustomEqDb(6, -10.0);
        expectWithinAbsoluteError(state.customEqDb()[6], -10.0, 1e-6);
        expectWithinAbsoluteError(mask->eqBand(6).getMinimum(), -kEqMaxDb, 1e-9, "range widened to +-12 dB");
        state.setCustomEqDb(0, 30.0);
        expectWithinAbsoluteError(state.customEqDb()[0], kEqMaxDb, 1e-6, "clamped to +-12 dB");
        bool unknownTarget = false;
        for (const auto& a : state.plan().adjustments) unknownTarget = unknownTarget || a.fieldPath == "spectrum.target";
        expect(state.plan().ok && !unknownTarget, "custom target composes");
        expectEquals(juce::String(state.plan().plan.target.id), juce::String("custom"));

        mask->correctionToggle().setToggleState(false, juce::sendNotificationSync);
        expect(!state.correctionEnabled());
        state.setCorrectionSpeed("slow");
        expectEquals(juce::String(state.correctionSpeed()), juce::String("slow"));
        expect(mask->getHeight() > 1000, "advanced panels laid out");
    }

    void runPresetSession(MainComponent& main, AppState& state, PresetSession& session, const juce::File& tmp) {
        beginTest("Preset session: Modified, changes list, Reset, Save As");
        expect(session.isModified());
        expect(!session.advancedChanges().empty(), "advanced changes listed");
        expect(main.modifiedButton().isVisible(), "ADVANCED SETTINGS MODIFIED shown");
        expect(main.resetButton().isVisible());
        main.openChangesDialog();
        expect(main.dialog() != nullptr);
        main.closeDialog();
        state.setStrengthDb(2.0);
        session.resetToRecommended();
        expect(!session.isModified(), "reset to recommended");
        expect(session.origin() == PresetOrigin::Factory);
        expectWithinAbsoluteError(state.strengthDb(), 2.0, 1e-9, "Reset keeps Strength");
        expect(!main.modifiedButton().isVisible());
        expect(main.presetLabel().getText().contains("Recommended"), main.presetLabel().getText());
        state.undo();
        expect(session.isModified(), "reset is undoable");
        session.resetToRecommended();

        state.setTalkerValue(TalkerField::Average, 9.0);
        juce::String err;
        expect(session.saveAs("Server Room Privacy", false, "Null:Test Speakers", &err), err);
        const auto file = tmp.getChildFile("user_presets").getChildFile("Server Room Privacy.bfpreset");
        expect(file.existsAsFile(), "preset written");
        expect(file.loadFileAsString().contains("\"name\": \"Server Room Privacy\""));
        expect(!file.loadFileAsString().contains("Null:Test Speakers"), "device not stored by default");
        expect(session.origin() == PresetOrigin::User);
        expectEquals(session.title(), juce::String("Server Room Privacy"));
        state.setCharacter(0.1);
        expect(session.origin() == PresetOrigin::UserModified);
        expect(session.loadUserPreset(toPath(file.getFullPathName()), &err), err);
        expect(session.origin() == PresetOrigin::User);
        expect(!session.saveAs("  ", false, {}, &err), "empty name rejected");
    }

    void runShortcutsAndEngine(MainComponent& main, AppState& state, EngineBridge& bridge) {
        beginTest("Shortcuts: Ctrl+S dialog, Space blocked in dialogs, Space start/stop");
        expect(main.handleKey(cmdKey('s')));
        expect(main.dialog() != nullptr && main.dialog()->findButton("Save") != nullptr, "Save As dialog");
        expect(!main.handleKey(juce::KeyPress(juce::KeyPress::spaceKey)), "Space not handled while a dialog is open");
        if (juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr) {
            main.addToDesktop(juce::ComponentPeer::windowAppearsOnTaskbar);
            main.setVisible(true);
            main.toFront(true);
            pump(150);
            if (auto* body = main.dialog()->body()) {
                juce::TextEditor* ed = nullptr;
                for (auto* c : body->getChildren())
                    if (auto* t = dynamic_cast<juce::TextEditor*>(c)) ed = t;
                if (ed) {
                    main.closeDialog();
                    // Focus a text field outside a dialog: Space must still not start masking.
                    juce::TextEditor field;
                    main.addAndMakeVisible(field);
                    field.setBounds(200, 70, 100, 24);
                    if (auto* peer = main.getPeer()) peer->grabFocus();
                    pump(100);
                    field.grabKeyboardFocus();
                    pump(100);
                    if (field.hasKeyboardFocus(false))
                        expect(!main.handleKey(juce::KeyPress(juce::KeyPress::spaceKey)), "Space ignored in text field");
                    else logMessage("(window focus unavailable; text-field focus check skipped)");
                    main.removeChildComponent(&field);
                }
            }
        }
        main.closeDialog();
        pump(20);

        auto* run = dynamic_cast<RunPage*>(main.page("run"));
        expect(main.handleKey(juce::KeyPress(juce::KeyPress::spaceKey)), "Space = Start");
        const bool started = pumpUntil(
            [&] {
                const auto s = bridge.status().state;
                return s == rt::EngineState::Running || s == rt::EngineState::Degraded || s == rt::EngineState::Error;
            },
            15000);
        expect(started, "engine reached a running state");
        logMessage("engine state: " + juce::String(std::string(rt::toString(bridge.status().state))) + " " +
                   run->statusLabel().getText() + " / " + run->activityLabel().getText());
        expect(bridge.status().state != rt::EngineState::Error, "started without error");
        expectEquals(run->startStopButton().getButtonText(), juce::String("STOP MASKING"));
        expectEquals(run->statusLabel().getText(), juce::String("MASKING ACTIVE"));
        // Switching modes never stops audio (GUI §1).
        state.setMode(UiMode::Simple);
        state.setMode(UiMode::Advanced);
        pump(200);
        expect(bridge.masking(), "mode switch keeps masking");
        // Live edit while running.
        const auto n0 = bridge.pushCount();
        state.setStrengthDb(-2.0);
        expect(pumpUntil([&] { return bridge.pushCount() > n0; }, 3000), "live push while running");
        pumpUntil([&] { return bridge.elapsedSeconds() >= 1.0; }, 3000);
        expect(bridge.elapsedSeconds() >= 1.0, "elapsed time runs");
        main.handleKey(juce::KeyPress(juce::KeyPress::spaceKey));
        expect(pumpUntil([&] { return bridge.status().state == rt::EngineState::Stopped; }, 10000), "Space = Stop");
        pump(50);
        expectEquals(run->startStopButton().getButtonText(), juce::String("START MASKING"));
        if (main.isOnDesktop()) main.removeFromDesktop();
    }
};

static UiTests uiTests;

int runUiTests() {
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.setPassesAreLogged(false);
    runner.runTestsInCategory("BabbleForgeUI");
    int failures = 0, passes = 0;
    for (int i = 0; i < runner.getNumResults(); ++i) {
        failures += runner.getResult(i)->failures;
        passes += runner.getResult(i)->passes;
    }
    std::printf("UI tests: %d passed, %d failed\n", passes, failures);
    std::fflush(stdout);
    return failures;
}

}  // namespace bf::gui
