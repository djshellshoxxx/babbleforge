// Headless tests of the voice library dialogs: the import wizard appends to the library by default
// ("Add"), replaces it only with the explicit "Replace library" option, tells the application to
// hot-reload (library-changed handler), and the status shows the new talker count right away
// (category BabbleForgeUI).
#include <cmath>
#include <fstream>
#include <random>

#include <nlohmann/json.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../../tests/ingest_fixtures.h"
#include "dialogs/LibraryDialogs.h"
#include "model/AppSettings.h"

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

// Speech-like test recording (tests/ingest_fixtures.h), 20 s, 48 kHz mono 16-bit WAV.
void writeWav(const juce::File& f, unsigned seed) {
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    const auto x = bftest::speechLike(20.0, seed);
    const bool ok = bftest::writeWav(std::filesystem::path(f.getFullPathName().toStdString()), x);
    jassert(ok);
    juce::ignoreUnused(ok);
}

std::string versionOf(const std::filesystem::path& root) {
    std::ifstream f(root / "manifest.json");
    const auto j = nlohmann::json::parse(f, nullptr, false);
    return j.is_object() ? j.value("corpusVersion", std::string()) : std::string();
}

}  // namespace

class LibraryTests final : public juce::UnitTest {
public:
    LibraryTests() : juce::UnitTest("BabbleForge library: Add vs Replace", "BabbleForgeUI") {}

    void runTest() override {
        beginTest("The wizard adds to the library by default and replaces it only on request");
        const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("bf_library_tests_" + juce::String(juce::Time::currentTimeMillis()));
        tmp.createDirectory();
        const auto in1 = tmp.getChildFile("first"), in2 = tmp.getChildFile("second"), in3 = tmp.getChildFile("third");
        writeWav(in1.getChildFile("alice_1.wav"), 41);
        writeWav(in1.getChildFile("alice_2.wav"), 42);
        writeWav(in1.getChildFile("bob_1.wav"), 43);
        writeWav(in2.getChildFile("carol_1.wav"), 45);
        writeWav(in3.getChildFile("dave_1.wav"), 47);
        writeWav(in3.getChildFile("eve_1.wav"), 48);
        const auto root = toPath(tmp.getChildFile("library").getFullPathName());
        AppSettings settings({});

        int notified = 0;
        bool lastReplaced = false;
        setLibraryChangedHandler([&](const LibraryChange& c) {
            ++notified;
            lastReplaced = c.replaced;
            return true;
        });

        auto run = [&](const juce::File& folder, bool replace) -> std::unique_ptr<ImportWizard> {
            auto w = std::make_unique<ImportWizard>(settings, root, toPath(folder.getFullPathName()));
            w->setSize(560, 340);
            expect(!w->replaceLibrary(), "Add is the default");
            expect(w->replaceToggle().isVisible(), "the Replace option is offered on the first page");
            if (replace) w->replaceToggle().onClick(), w->setReplaceLibrary(true);
            w->startAnalysis();
            expect(pumpUntil([&] { return w->step() == ImportWizard::Review || w->step() == ImportWizard::SelectFiles; }, 120000), "analysis finished");
            expect(w->step() == ImportWizard::Review, "review reached: " + w->statusText());
            return w;
        };

        // 1. No library yet: "Add" simply creates it.
        {
            auto w = run(in1, false);
            expectEquals(w->nextButton().getButtonText(), juce::String("Add to Library"));
            expect(w->commit());
            expectEquals(notified, 1);
            expect(!lastReplaced, "reported as an addition");
            const auto s = readLibraryStatus(root);
            expect(s.exists && s.talkers == 2 && s.files == 3, "2 talkers, 3 files: " + juce::String(s.talkers) + "/" + juce::String(s.files));
            expect(w->statusText().containsIgnoreCase("in use now"), "hot reload announced: " + w->statusText());
        }
        const std::string v1 = versionOf(root);

        // 2. Add a second folder: the first library stays, the new talker is appended; status shows 3 talkers.
        {
            auto w = run(in2, false);
            expect(w->summaryText().contains("1 files analyzed") || w->summaryText().contains("1 file"), w->summaryText());
            expect(w->summaryText().contains("3 talkers"), "the review shows the resulting library: " + w->summaryText());
            expect(w->commit());
            expectEquals(notified, 2);
            expect(!lastReplaced);
            const auto s = readLibraryStatus(root);
            expect(s.talkers == 3 && s.files == 4, "3 talkers, 4 files after adding: " + juce::String(s.talkers) + "/" + juce::String(s.files));
            expect(w->summaryText().contains("The library now has 3 talkers"), w->summaryText());
        }
        const std::string v2 = versionOf(root);
        expect(!v2.empty() && v2 != v1, "the corpus version changes when recordings are added");

        // Manage Library shows the new talker count straight away.
        {
            auto h = makeManageLibraryDialog(settings, [](std::unique_ptr<OverlayDialog>) {}, [] {});
            h.dialog->setBounds(0, 0, 800, 600);
            expectEquals(h.body->value(0).getText(), juce::String("3"), "Manage Library: talkers");
            expectEquals(h.body->value(1).getText(), juce::String("4"), "Manage Library: audio files");
        }

        // 3. Adding the same recordings again finds only duplicates: same talkers, same corpus version.
        {
            auto w = run(in2, false);
            expect(w->summaryText().contains("1 Rejected"), w->summaryText());
            expect(w->commit());
            expect(readLibraryStatus(root).talkers == 3);
            expect(versionOf(root) == v2, "duplicates do not change the library version");
        }

        // 4. Replace: only the new folder remains.
        {
            auto w = run(in3, true);
            expect(w->replaceLibrary());
            expectEquals(w->nextButton().getButtonText(), juce::String("Replace Library"));
            expect(w->commit());
            expect(lastReplaced, "reported as a replacement");
            const auto s = readLibraryStatus(root);
            expect(s.talkers == 2 && s.files == 2, "replaced: 2 talkers, 2 files: " + juce::String(s.talkers) + "/" + juce::String(s.files));
            expect(versionOf(root) != v2);
        }
        {
            auto h = makeManageLibraryDialog(settings, [](std::unique_ptr<OverlayDialog>) {}, [] {});
            h.dialog->setBounds(0, 0, 800, 600);
            expectEquals(h.body->value(0).getText(), juce::String("2"), "Manage Library after replace: talkers");
        }

        // No handler (no engine): the dialog asks for a restart instead.
        setLibraryChangedHandler({});
        {
            auto w = run(in2, false);
            expect(w->commit());
            expect(w->statusText().containsIgnoreCase("restart"), w->statusText());
            expect(readLibraryStatus(root).talkers == 3, "added to the replaced library");
        }
        tmp.deleteRecursively();
    }
};

static LibraryTests libraryTests;

}  // namespace bf::gui
