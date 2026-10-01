// Headless GUI tests for the ANALYSIS and Settings pages, the corpus import wizard, the
// first-run wizard, the Save As dialog and the preset safety banner (category BabbleForgeUI).
#include <cmath>
#include <cstdio>
#include <random>

#include <juce_gui_basics/juce_gui_basics.h>

#include "MainComponent.h"
#include "core/config/Preset.h"
#include "core/rt/NullBackend.h"
#include "dialogs/FirstRunWizard.h"
#include "dialogs/LibraryDialogs.h"
#include "model/Catalog.h"
#include "model/Prefs.h"
#include "pages/AnalysisPage.h"
#include "pages/SettingsPage.h"

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

// Synthetic, speech-like test recording: harmonic carrier gated at a syllable rate with pauses.
void writeTestWav(const juce::File& f, double f0, double seconds, unsigned seed) {
    constexpr double fs = 48000.0;
    const auto n = static_cast<int>(seconds * fs);
    std::vector<float> buf(static_cast<std::size_t>(n));
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (int i = 0; i < n; ++i) {
        const double t = i / fs;
        double s = 0.0;
        for (int k = 1; k <= 24; ++k) s += std::sin(2.0 * juce::MathConstants<double>::pi * k * f0 * t) / k;
        const double syll = std::max(0.0, std::sin(2.0 * juce::MathConstants<double>::pi * 3.7 * t));
        const double gate = std::fmod(t, 1.3) < 0.9 ? 1.0 : 0.0;  // pauses
        buf[static_cast<std::size_t>(i)] = static_cast<float>(0.25 * s * syll * gate + 0.0005 * noise(rng));
    }
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    // 16-bit mono PCM WAV.
    juce::MemoryOutputStream m;
    const auto bytes = static_cast<juce::uint32>(n * 2);
    m.write("RIFF", 4);
    m.writeInt(static_cast<int>(36 + bytes));
    m.write("WAVEfmt ", 8);
    m.writeInt(16);
    m.writeShort(1);
    m.writeShort(1);
    m.writeInt(static_cast<int>(fs));
    m.writeInt(static_cast<int>(fs) * 2);
    m.writeShort(2);
    m.writeShort(16);
    m.write("data", 4);
    m.writeInt(static_cast<int>(bytes));
    for (int i = 0; i < n; ++i) m.writeShort(static_cast<short>(juce::jlimit(-32767.0f, 32767.0f, buf[static_cast<std::size_t>(i)] * 32767.0f)));
    f.replaceWithData(m.getData(), m.getDataSize());
}

ThirdOctArray flatDb(double v) {
    ThirdOctArray a{};
    a.fill(v);
    return a;
}

}  // namespace

class UiTestsAnalysis final : public juce::UnitTest {
public:
    UiTestsAnalysis() : juce::UnitTest("BabbleForge GUI analysis and settings", "BabbleForgeUI") {}

    void runTest() override {
        const auto dsr = loadDataSet(BF_DEFAULT_DATA_DIR);
        beginTest("Data set (analysis tests)");
        expect(dsr.ok, juce::String(dsr.error));
        if (!dsr.ok) return;
        const DataSet& ds = dsr.data;
        const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("bf_ui_tests_an_" + juce::String(juce::Time::currentTimeMillis()));
        tmp.createDirectory();

        AppSettings settings({});
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
        state.setMode(UiMode::Advanced);

        runSpectrumMetrics();
        runAnalysisPage(main);
        runImportWizard(settings, tmp);
        runSettingsPage(main, state, settings, bridge, tmp);
        runFirstRun(state, bridge, tmp);
        runSaveAs(main, tmp);
        runSafetyBanner(main, state, session);
        tmp.deleteRecursively();
    }

private:
    // ---- ANALYSIS -------------------------------------------------------------------------------

    static std::shared_ptr<MaskStatistics> syntheticStats() {
        auto s = std::make_shared<MaskStatistics>();
        s->samples = 48000 * 120;
        s->seconds = 120.0;
        s->numChannels = 4;
        s->babbleActive = true;
        s->meanActive = 7.4;
        s->slotActive.assign(64, 0);
        for (int i = 0; i < 8; ++i) s->slotActive[static_cast<std::size_t>(i)] = 1;
        s->occupancy60s = 0.96;
        s->gapMean60s = 0.072;
        s->gapMax60s = 0.181;
        s->haveSpectrumView = true;
        s->referenceDb = flatDb(0.0);
        s->measuredDb = flatDb(5.0);  // uniform offset is removed
        const auto& fc = thirdOctNominalHz();
        for (std::size_t b = 0; b < kNumThirdOctBands; ++b) {
            if (fc[b] == 250.0) s->measuredDb[b] += 2.0;
            if (fc[b] == 4000.0) s->measuredDb[b] -= 2.0;
        }
        s->temporalDensity = TemporalDensity::High;
        s->haveModulation = true;
        for (std::size_t k = 0; k < kNumModBands; ++k) s->modulation[k] = 0.6 / (1.0 + static_cast<double>(k) * 0.3);
        s->modulationDepth10s = 0.8;
        s->crest60sDb = 9.5;
        s->outputRmsChDb = {-30.0, -30.4, -31.1, -30.8};
        s->haveCorrelation = true;
        s->adjacentCorrelation = {0.05, -0.1, 0.08};
        return s;
    }

    void runSpectrumMetrics() {
        beginTest("Spectrum metrics: target vs actual");
        const auto s = syntheticStats();
        const auto m = computeSpectrumMetrics(s->referenceDb, s->measuredDb, SpecMode::ThirdOctave);
        expect(m.valid);
        expectWithinAbsoluteError(m.maxDevDb, 2.0, 1e-9, "largest deviation, uniform offset removed");
        expect(m.maxDevHz == 250.0 || m.maxDevHz == 4000.0);
        expectWithinAbsoluteError(m.avgErrorDb, 4.0 / 19.0, 1e-9, "average error");
        const auto o = computeSpectrumMetrics(s->referenceDb, s->measuredDb, SpecMode::Octave);
        expect(o.valid && o.maxDevDb > 0.0 && o.maxDevDb <= 2.0 + 1e-9, "octave mode");
        expect(!computeSpectrumMetrics(flatDb(0.0), flatDb(-200.0), SpecMode::Fft).valid, "no measurement yet");
    }

    void runAnalysisPage(MainComponent& main) {
        beginTest("ANALYSIS page renders synthetic statistics");
        main.showPage("analysis");
        expectEquals(juce::String(main.currentPageId()), juce::String("analysis"));
        auto* page = dynamic_cast<AnalysisPage*>(main.page("analysis"));
        expect(page != nullptr);
        if (!page) return;
        page->applyStats(nullptr);
        expectEquals(page->voice().value(0).getText(), juce::String::fromUTF8("\xe2\x80\x94"), "no data placeholder");
        const auto s = syntheticStats();
        page->applyStats(s);
        expectEquals(page->voice().value(0).getText(), juce::String("8"), "active talkers");
        expectEquals(page->voice().value(1).getText(), juce::String("7.4"), "average talkers");
        expectEquals(page->voice().value(2).getText(), juce::String("96%"), "occupancy");
        expectEquals(page->voice().value(3).getText(), juce::String("72 ms"), "average gap");
        expectEquals(page->voice().value(4).getText(), juce::String("181 ms"), "longest recent gap");
        for (int i = 0; i < 40; ++i) page->applyStats(s);  // scrolling strip
        expect(page->voice().strip.hasData && page->voice().strip.rows() >= 8, "per-voice strip has 8 voices");
        expect(page->voice().strip.column(AnalysisPage::VoiceStrip::kColumns - 1) == 0xFFull, "newest column: voices 1-8 active");

        expect(page->spectrum().errorLabel.getText().contains("0.2 dB"), page->spectrum().errorLabel.getText());
        expect(page->spectrum().deviationLabel.getText().contains("2.0 dB at"), page->spectrum().deviationLabel.getText());
        for (int m = 0; m < 3; ++m) {
            page->spectrum().modeButton(m).onClick();
            expect(static_cast<int>(page->spectrum().mode()) == m, "spectrum mode switch");
            expect(page->spectrum().deviationLabel.getText().contains("dB at"), "metrics in every mode");
        }

        expectEquals(page->modulation().meter.level(), 2, "temporal density High");
        expect(!page->modulation().spectrum.isVisible() && !page->modulation().detailsShown(), "modulation details hidden by default");
        const int h0 = page->layoutPage(1000);
        page->modulation().detailsButton.onClick();
        expect(page->modulation().spectrum.isVisible(), "Show details reveals the modulation spectrum");
        expect(page->layoutPage(1000) > h0, "page grows with the details");
        page->modulation().detailsButton.onClick();
        expect(!page->modulation().spectrum.isVisible());

        const auto& pct = page->spatial().map.percent();
        expectEquals(static_cast<int>(pct.size()), 4, "four speakers");
        if (pct.size() == 4) {
            expectWithinAbsoluteError(pct[0], 100.0, 1e-6, "loudest speaker = 100%");
            expect(pct[2] < pct[0] && pct[2] > 50.0);
        }
        expect(page->spatial().correlationValue.getText().startsWith("Low"), page->spatial().correlationValue.getText());
        auto high = std::make_shared<MaskStatistics>(*s);
        high->adjacentCorrelation = {0.9, 0.8, 0.85};
        page->applyStats(high);
        expect(page->spatial().correlationValue.getText().startsWith("High"), "high correlation flagged");
        expect(!page->spatial().detailsShown());
        page->spatial().detailsButton.onClick();
        expect(page->spatial().detailText.isVisible() && page->spatial().detailText.getText().contains("0.90"), "coefficients only in details");
        page->spatial().detailsButton.onClick();

        // <= 15 Hz: a tight loop of 100 status events changes the page at most twice.
        EngineStatus st;
        st.stats = s;
        const int u0 = page->updateCount();
        for (int i = 0; i < 100; ++i) page->refreshStatus(st);
        expect(page->updateCount() - u0 <= 2, "updates throttled: " + juce::String(page->updateCount() - u0));
        pump(120);
        page->refreshStatus(st);
        expect(page->updateCount() - u0 <= 3);

        const auto img = page->createComponentSnapshot(page->getLocalBounds());  // paints every card
        expect(img.isValid());
        const auto dir = juce::SystemStats::getEnvironmentVariable("BF_UI_SNAPSHOT_DIR", {});
        if (dir.isNotEmpty()) {
            juce::PNGImageFormat png;
            juce::FileOutputStream out(juce::File(dir).getChildFile("analysis_page.png"));
            if (out.openedOk()) {
                out.setPosition(0);
                out.truncate();
                png.writeImageToStream(img, out);
            }
        }
        page->applyStats(nullptr);
    }

    // ---- import wizard -------------------------------------------------------------------------------

    void runImportWizard(AppSettings& settings, const juce::File& tmp) {
        beginTest("Corpus import wizard on a tiny generated WAV set (analyse, cancel, review, add)");
        const auto in = tmp.getChildFile("wavs");
        writeTestWav(in.getChildFile("alice_1.wav"), 110.0, 4.0, 1);
        writeTestWav(in.getChildFile("alice_2.wav"), 112.0, 4.0, 2);
        writeTestWav(in.getChildFile("bob_1.wav"), 135.0, 4.0, 3);
        writeTestWav(in.getChildFile("bob_2.wav"), 140.0, 4.0, 4);
        writeTestWav(in.getChildFile("carol_1.wav"), 190.0, 4.0, 5);
        const auto root = toPath(tmp.getChildFile("library").getFullPathName());

        // Cancel: never installs anything, never blocks.
        {
            ImportWizard w(settings, root, toPath(in.getFullPathName()));
            w.setSize(560, 340);
            expect(w.step() == ImportWizard::SelectFiles);
            expect(w.nextButton().isEnabled(), "Analyze enabled for an existing folder");
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            w.startAnalysis();
            expect(w.step() == ImportWizard::Analyze);
            w.cancelAnalysis();
            const double blocked = juce::Time::getMillisecondCounterHiRes() - t0;
            expect(blocked < 500.0, "start + cancel do not block the message thread: " + juce::String(blocked, 0) + " ms");
            expect(pumpUntil([&] { return w.step() == ImportWizard::SelectFiles; }, 60000), "cancel returns to Select Files");
            expect(w.statusText().containsIgnoreCase("cancel"), w.statusText());
            expect(!juce::File(fromPath(root)).getChildFile("manifest.json").existsAsFile(), "nothing installed after cancel");
            expect(!w.committed());
        }

        // Full run.
        ImportWizard w(settings, root, toPath(in.getFullPathName()));
        w.setSize(560, 340);
        bool committedCallback = false;
        w.onFinished = [&](bool c) { committedCallback = c; };
        w.startAnalysis();
        expect(pumpUntil([&] { return w.step() == ImportWizard::Review || w.step() == ImportWizard::SelectFiles; }, 120000), "analysis finished");
        expect(w.step() == ImportWizard::Review, "Review step reached: " + w.statusText());
        if (w.step() != ImportWizard::Review) return;
        expectEquals(w.analyzedFiles(), 5);
        expect(w.summaryText().contains("5 files analyzed"), w.summaryText());
        expect(w.summaryText().contains("Good") && w.summaryText().contains("Usable") && w.summaryText().contains("Rejected"), w.summaryText());
        expect(!w.detailsShown(), "technical details hidden until expanded");
        w.detailsButton().onClick();
        expect(w.detailsShown(), "details expand");
        expect(w.commit(), "Add installs the library");
        expect(w.step() == ImportWizard::Add && committedCallback);
        const auto status = readLibraryStatus(root);
        expect(status.exists && status.files == 5, "library manifest written");
        expectEquals(juce::String(settings.get().corpusRoot), fromPath(root), "library root remembered");
        expect(!juce::File(fromPath(root.parent_path() / (root.filename().string() + ".import"))).exists(), "staging removed");

        // Manage Library: status + background scan.
        std::unique_ptr<OverlayDialog> opened;
        auto h = makeManageLibraryDialog(settings, [&](std::unique_ptr<OverlayDialog> d) { opened = std::move(d); }, [] {});
        h.dialog->setBounds(0, 0, 800, 600);
        expectEquals(h.body->value(1).getText(), juce::String("5"), "audio files");
        h.body->scan();
        expect(h.body->scanning());
        expect(pumpUntil([&] { return !h.body->scanning(); }, 20000), "scan finished");
        expectEquals(h.body->value(1).getText(), juce::String("5"), "scan: audio files");
        expect(h.body->message().getText().containsIgnoreCase("scan"), h.body->message().getText());
        h.body->addButton().onClick();
        expect(opened != nullptr, "Add Audio opens the import wizard");
        opened.reset();
        h.body->rebuildButton().onClick();
        expect(opened != nullptr, "Rebuild Analysis opens the wizard with the original folder");
        pump(100);
        opened.reset();  // cancels the analysis it started
        pump(300);
    }

    // ---- Settings ----------------------------------------------------------------------------------------

    void runSettingsPage(MainComponent& main, AppState& state, AppSettings& settings, EngineBridge& bridge, const juce::File& tmp) {
        beginTest("Settings persist (startup rules, logging, fallback policy, appearance)");
        main.showPage("settings");
        auto* page = dynamic_cast<SettingsPage*>(main.page("settings"));
        expect(page != nullptr);
        if (!page) return;
        expect(page->libraryStatus().getText().startsWith("Status: ") && !page->libraryStatus().getText().contains("Not set up"), page->libraryStatus().getText());
        expect(page->libraryTalkers().getText().contains("Talkers:"), page->libraryTalkers().getText());
        expect(pumpUntil([&] { return page->deviceBox().getNumItems() >= 1; }, 3000), "output devices listed");

        // §51: automatic start requires a remembered output device.
        expect(!page->autoStartToggle().getToggleState() && !page->rememberToggle().getToggleState());
        page->autoStartToggle().setToggleState(true, juce::sendNotificationSync);
        expect(page->rememberToggle().getToggleState(), "turning on auto-start remembers the output device");
        expect(loadPrefs(settings.get()).autoStart && loadPrefs(settings.get()).rememberDevice);
        expectEquals(juce::String(settings.get().deviceId), juce::String("Null:Test Speakers"), "device stored");
        page->rememberToggle().setToggleState(false, juce::sendNotificationSync);
        expect(!page->autoStartToggle().getToggleState(), "forgetting the device turns auto-start off");
        expect(!loadPrefs(settings.get()).autoStart);
        expect(page->startupNote().getText().isNotEmpty(), "explained to the user");
        page->restoreToggle().setToggleState(false, juce::sendNotificationSync);
        expect(!loadPrefs(settings.get()).restorePrevious);
        page->minimizedToggle().setToggleState(true, juce::sendNotificationSync);
        expect(loadPrefs(settings.get()).startMinimized);

        // Logging.
        page->logLevelBox().setSelectedItemIndex(1, juce::sendNotificationSync);  // Debug
        expectEquals(juce::String(loadPrefs(settings.get()).logLevel), juce::String("debug"));
        page->redactLogsToggle().setToggleState(true, juce::sendNotificationSync);
        page->redactExportsToggle().setToggleState(false, juce::sendNotificationSync);
        expect(loadPrefs(settings.get()).redactLogs && !loadPrefs(settings.get()).redactExports);

        // Fallback policy is a preset field.
        page->fallbackGroup().button(0).setToggleState(true, juce::sendNotificationSync);
        expect(state.preset().fallbackPolicy.has_value() && *state.preset().fallbackPolicy == "strict", "Strict selected");
        state.undo();
        pump(20);
        expect(page->fallbackGroup().selected() == 2, "undo restores Continuous in the selector");
        page->fallbackGroup().button(1).setToggleState(true, juce::sendNotificationSync);
        expect(state.preset().fallbackPolicy && *state.preset().fallbackPolicy == "safe");

        // Appearance.
        page->scaleBox().setSelectedItemIndex(3, juce::sendNotificationSync);
        expectWithinAbsoluteError(settings.get().uiScale, 1.25, 1e-9);
        juce::Desktop::getInstance().setGlobalScaleFactor(1.0f);
        page->scaleBox().setSelectedItemIndex(2, juce::sendNotificationSync);

        // Persistence round trip through settings.json (unknown keys preserved).
        const auto file = toPath(tmp.getChildFile("persist/settings.json").getFullPathName());
        {
            AppSettings s2(file);
            s2.update([](AppSettingsData& d) { d.extra["x-future"] = 7; });
            savePrefs(s2, loadPrefs(settings.get()));
            s2.flush();
        }
        {
            AppSettings s3(file);
            const Prefs p = loadPrefs(s3.get());
            expectEquals(juce::String(p.logLevel), juce::String("debug"));
            expect(p.redactLogs && !p.redactExports && !p.restorePrevious && p.startMinimized, "prefs round trip");
            expect(s3.get().extra.contains("x-future"), "unknown settings keys preserved");
        }

        // Diagnostics zip.
        const auto zipFile = tmp.getChildFile("diag/diagnostics.zip");
        juce::String err;
        expect(page->saveDiagnosticsZip(zipFile, &err), err);
        expect(zipFile.existsAsFile() && zipFile.getSize() > 100, "zip written");
        juce::ZipFile zip(zipFile);
        const int idx = zip.getIndexOfFileName("diagnostics.json");
        expect(idx >= 0, "diagnostics.json in the zip");
        if (idx >= 0) {
            std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(idx));
            const auto text = in ? in->readEntireStreamAsString() : juce::String();
            const auto j = nlohmann::json::parse(text.toStdString(), nullptr, false);
            expect(j.is_object() && j.contains("schema"), "valid diagnostics snapshot");
        }
        expect(zip.getIndexOfFileName("preset.json") >= 0);
        expect(page->logDirectory().getFullPathName().isNotEmpty());
        // Voice library button opens the dialog without blocking.
        page->manageButton().onClick();
        expect(main.dialog() != nullptr && main.dialog()->findButton("Close") != nullptr, "Manage Library dialog");
        main.closeDialog();
        juce::ignoreUnused(bridge);
        main.showPage("run");
    }

    // ---- first run ----------------------------------------------------------------------------------------

    void runFirstRun(AppState& state, EngineBridge& bridge, const juce::File& tmp) {
        beginTest("First-run wizard: output, area, recommended setup, start");
        const auto file = toPath(tmp.getChildFile("firstrun/settings.json").getFullPathName());
        AppSettings s(file);
        expect(!std::filesystem::exists(file), "no settings.json before the first run");
        auto dlg = makeFirstRunDialog(state, s, bridge);
        expect(std::filesystem::exists(file), "settings.json written when the wizard opens (shown only once)");
        auto* w = dynamic_cast<FirstRunWizard*>(dlg->body());
        expect(w != nullptr);
        if (!w) return;
        dlg->setBounds(0, 0, 900, 700);
        expectEquals(w->page(), 0);
        expect(w->titleText().contains("Welcome to BabbleForge"));
        w->nextButton().onClick();
        expectEquals(w->page(), 1);
        expect(w->titleText().contains("Select Output") && w->deviceBox().getNumItems() >= 1 && w->testButton().isVisible());
        w->backButton().onClick();
        expectEquals(w->page(), 0);
        w->nextButton().onClick();
        w->nextButton().onClick();
        expectEquals(w->page(), 2);
        expect(w->areaGroup().size() == 6, "six areas");
        w->selectArea("open_office");
        w->nextButton().onClick();
        expectEquals(w->page(), 3);
        expect(w->setupText().contains("Open Office") && w->setupText().contains("Balanced") && w->setupText().contains("Normal"), w->setupText());
        expectEquals(w->nextButton().getButtonText(), juce::String("Start"));
        w->nextButton().onClick();  // Start
        expectEquals(juce::String(state.preset().area), juce::String("open_office"), "area applied");
        expect(loadPrefs(s.get()).firstRunDone && loadPrefs(s.get()).rememberDevice);
        expectEquals(juce::String(s.get().deviceId), juce::String("Null:Test Speakers"));
        expect(pumpUntil([&] { return bridge.masking(); }, 20000), "Start starts masking");
        bridge.stop();
        expect(pumpUntil([&] { return !bridge.masking(); }, 15000));
        pump(50);
        dlg.reset();
        state.setArea("office");
    }

    // ---- Save As dialog ---------------------------------------------------------------------------------------

    void runSaveAs(MainComponent& main, const juce::File& tmp) {
        beginTest("Save As dialog writes a valid .bfpreset (remember device)");
        main.openSaveDialog();
        auto* body = main.dialog() ? main.dialog()->body() : nullptr;
        expect(body != nullptr);
        if (!body) return;
        juce::TextEditor* name = nullptr;
        juce::ToggleButton* remember = nullptr;
        for (auto* c : body->getChildren()) {
            if (auto* t = dynamic_cast<juce::TextEditor*>(c)) name = t;
            if (auto* b = dynamic_cast<juce::ToggleButton*>(c)) remember = b;
        }
        expect(name != nullptr && remember != nullptr, "name field and remember-device checkbox");
        if (!name || !remember) return;
        expect(remember->getButtonText().contains("Remember output device"));
        name->setText("Dialog Preset", juce::dontSendNotification);
        remember->setToggleState(true, juce::dontSendNotification);
        main.dialog()->findButton("Save")->onClick();
        pump(50);
        expect(main.dialog() == nullptr, "dialog closes after saving");
        const auto file = tmp.getChildFile("user_presets").getChildFile("Dialog Preset.bfpreset");
        expect(file.existsAsFile(), "file written");
        const auto text = file.loadFileAsString().toStdString();
        const auto parsed = parsePreset(text);
        expect(parsed.ok, parsed.error);
        expect(parsed.preset.name == "Dialog Preset", "name saved");
        expect(parsed.preset.outputs.rememberDevice && parsed.preset.outputs.device && *parsed.preset.outputs.device == "Null:Test Speakers",
               "device stored when requested");
    }

    // ---- safety prompt -----------------------------------------------------------------------------------------

    void runSafetyBanner(MainComponent& main, AppState& state, PresetSession& session) {
        beginTest("Preset safety comparison is non-blocking");
        session.resetToRecommended();
        expect(!main.safetyBanner().shown());
        state.setTalkerValue(TalkerField::Pool, 40);
        state.setTalkerValue(TalkerField::Average, 14);
        state.setTalkerValue(TalkerField::Maximum, 20);
        state.setMaxInternalGapMs(900);
        expect(session.differsSignificantly(), "significant divergence");
        expect(main.safetyBanner().shown(), "banner shown");
        expect(main.safetyBanner().message().contains("differs significantly"), main.safetyBanner().message());
        expect(main.dialog() == nullptr, "no modal dialog: the user is never blocked");
        main.safetyBanner().reviewButton().onClick();
        expect(main.dialog() != nullptr, "Review Changes opens the list");
        main.closeDialog();
        main.safetyBanner().keepButton().onClick();
        expect(!main.safetyBanner().shown(), "Keep Changes dismisses it");
        state.setFadeMs(200);
        expect(!main.safetyBanner().shown(), "stays dismissed while the configuration stays this way");
        main.safetyBanner().resetButton().onClick();  // no-op while hidden: use the session directly
        session.resetToRecommended();
        expect(!session.differsSignificantly() && !main.safetyBanner().shown(), "Reset clears it");
        state.setTalkerValue(TalkerField::Pool, 40);
        state.setTalkerValue(TalkerField::Average, 14);
        state.setTalkerValue(TalkerField::Maximum, 20);
        state.setMaxInternalGapMs(900);
        expect(main.safetyBanner().shown(), "returns for a new divergence");
        main.safetyBanner().resetButton().onClick();
        expect(!main.safetyBanner().shown() && !session.isModified(), "banner Reset returns to the recommendation");
    }
};

static UiTestsAnalysis uiTestsAnalysis;

}  // namespace bf::gui
