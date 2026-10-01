#pragma once
// Voice library UI (docs/GUI.md §52-§54): library status, the Manage Library dialog (Add Audio,
// Scan Library, Rebuild Analysis) and the four-step corpus import wizard
// (Select Files -> Analyze -> Review -> Add). The analysis runs on a background thread
// (CorpusImportJob); the message thread only polls its progress, so it is never blocked and
// Cancel works at any time. The wizard imports into a staging folder next to the library and
// installs it only in the last step.
#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Widgets.h"
#include "core/corpus/CorpusImporter.h"
#include "model/AppSettings.h"

namespace bf::gui {

struct LibraryStatus {
    bool exists = false;
    int talkers = 0, files = 0, invalid = 0;
    double totalS = 0.0, usableSpeechS = 0.0;
    juce::String stateText() const { return exists ? (usableSpeechS > 0.0 ? "Ready" : "Empty") : "Not set up"; }
};
// Reads <root>/manifest.json (cheap). `exists` is false when there is no library.
LibraryStatus readLibraryStatus(const std::filesystem::path& root);
juce::String formatDuration(double seconds);  // "11h 23m"
// Default library location: <app data>/corpus.
std::filesystem::path defaultLibraryRoot();
// The configured library root (settings) or the default.
std::filesystem::path libraryRootOf(const AppSettingsData& d);

// The library on disk changed (the wizard installed an import). The application registers a handler
// that hot-reloads the running engine (EngineController::reloadCorpus) without a restart; it returns
// true when the new library is being taken into use. With no handler (or false) the dialogs ask for a
// restart instead.
struct LibraryChange {
    std::filesystem::path root;
    bool replaced = false;  // true: "Replace library" (unrelated content), false: recordings were added
};
using LibraryChangedHandler = std::function<bool(const LibraryChange&)>;
void setLibraryChangedHandler(LibraryChangedHandler handler);
bool notifyLibraryChanged(const LibraryChange& change);  // true: a running engine is switching over

// Background import: scan, analyse, duplicate detection, aggregation, into `stagingRoot`.
// `baseRoot` + `addToExisting`: append to the existing library at baseRoot (the merged library is
// written to stagingRoot; see ImportOptions::addToExisting).
class CorpusImportJob {
public:
    struct Progress {
        std::size_t done = 0, total = 0;
        std::string file;
    };
    CorpusImportJob();
    ~CorpusImportJob();  // cancels; the worker thread detaches (never blocks the caller)
    CorpusImportJob(const CorpusImportJob&) = delete;
    CorpusImportJob& operator=(const CorpusImportJob&) = delete;

    void start(std::filesystem::path inputDir, std::filesystem::path stagingRoot, std::filesystem::path baseRoot = {},
               bool addToExisting = false);
    void cancel();
    bool started() const;
    bool finished() const;
    bool cancelled() const;
    Progress progress() const;
    ImportResult result() const;  // valid once finished()

private:
    struct State;
    std::shared_ptr<State> st_;
};

// Moves a finished staging import into the library (cache, database, links, manifest last).
bool installImport(const std::filesystem::path& stagingRoot, const std::filesystem::path& libraryRoot, std::string* error);

struct ScanResult {
    bool ok = false;
    std::string error;
    int files = 0, talkers = 0, invalid = 0;
    double totalS = 0.0, usableSpeechS = 0.0;
};
// Opens the database and verifies the cache files (call on a background thread).
ScanResult scanLibrary(const std::filesystem::path& root);

class ImportWizard final : public juce::Component, private juce::Timer {
public:
    enum Step { SelectFiles = 0, Analyze = 1, Review = 2, Add = 3 };

    ImportWizard(AppSettings& settings, std::filesystem::path libraryRoot, std::filesystem::path inputDir = {});
    ~ImportWizard() override;

    std::function<void()> requestClose;
    std::function<void(bool committed)> onFinished;

    // Actions (also driven by the buttons).
    void setInputDir(const std::filesystem::path& dir);
    void startAnalysis();
    void cancelAnalysis();
    // "Replace library" (off by default): the import replaces the library with the new recordings
    // instead of adding them to it.
    void setReplaceLibrary(bool replace);
    bool replaceLibrary() const noexcept { return replace_; }
    juce::ToggleButton& replaceToggle() { return replaceToggle_; }
    bool commit();
    Step step() const noexcept { return step_; }
    const CorpusImportJob& job() const noexcept { return *job_; }
    juce::String summaryText() const { return summary_.getText(); }
    juce::String statusText() const { return status_.getText(); }
    juce::TextButton& nextButton() { return next_; }
    juce::TextButton& cancelButton() { return cancel_; }
    juce::TextButton& detailsButton() { return details_; }
    bool detailsShown() const { return detailsText_.isVisible(); }
    int analyzedFiles() const noexcept { return analyzed_; }
    bool committed() const noexcept { return committed_; }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void goTo(Step s);
    void cleanupStaging();
    void chooseFolder();
    void showReview();

    AppSettings& settings_;
    std::filesystem::path root_, staging_, input_;
    std::unique_ptr<CorpusImportJob> job_ = std::make_unique<CorpusImportJob>();
    Step step_ = SelectFiles;
    bool cancelling_ = false, committed_ = false, replace_ = false;
    int analyzed_ = 0;
    ImportResult result_;
    double progress_ = 0.0;

    juce::TextEditor path_;
    juce::TextButton choose_{"Choose Folder..."}, next_{"Analyze"}, cancel_{"Cancel"}, details_{"Show details"};
    juce::ToggleButton replaceToggle_{"Replace library"};
    juce::Label hint_, status_, summary_;
    juce::ProgressBar bar_{progress_};
    juce::TextEditor detailsText_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

// Dialog factories (the caller shows them with MainComponent::showDialog).
std::unique_ptr<OverlayDialog> makeImportWizardDialog(AppSettings& settings, std::filesystem::path libraryRoot,
                                                      std::filesystem::path inputDir, bool autoStart,
                                                      std::function<void(bool committed)> onDone, bool replaceLibrary = false);

class ManageLibraryBody;
struct ManageLibraryHandles {
    std::unique_ptr<OverlayDialog> dialog;
    ManageLibraryBody* body = nullptr;
};
// `openDialog` shows another dialog (the import wizard) in place of this one.
ManageLibraryHandles makeManageLibraryDialog(AppSettings& settings,
                                             std::function<void(std::unique_ptr<OverlayDialog>)> openDialog,
                                             std::function<void()> onLibraryChanged);

class ManageLibraryBody final : public juce::Component, private juce::Timer {
public:
    ManageLibraryBody(AppSettings& settings, std::function<void(std::unique_ptr<OverlayDialog>)> openDialog,
                      std::function<void()> onChanged);
    ~ManageLibraryBody() override;
    void refreshStatus();  // from the manifest
    void scan();           // background
    void addAudio();
    void rebuildAnalysis();
    bool scanning() const noexcept { return scanning_; }
    juce::Label& value(int i) { return values_[static_cast<std::size_t>(i)]; }
    juce::TextButton& addButton() { return add_; }
    juce::TextButton& scanButton() { return scan_; }
    juce::TextButton& rebuildButton() { return rebuild_; }
    juce::Label& message() { return message_; }
    void resized() override;

private:
    void timerCallback() override;
    AppSettings& settings_;
    std::function<void(std::unique_ptr<OverlayDialog>)> openDialog_;
    std::function<void()> onChanged_;
    std::array<juce::Label, 5> names_, values_;
    juce::TextButton add_{"Add Audio"}, scan_{"Scan Library"}, rebuild_{"Rebuild Analysis"};
    juce::Label message_;
    struct ScanState;
    std::shared_ptr<ScanState> scanState_;
    bool scanning_ = false;
};

}  // namespace bf::gui
