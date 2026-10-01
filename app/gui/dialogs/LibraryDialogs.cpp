#include "dialogs/LibraryDialogs.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "LookAndFeel.h"
#include "core/corpus/CorpusDb.h"
#include "model/Prefs.h"

namespace bf::gui {

namespace fs = std::filesystem;

// ---- status ---------------------------------------------------------------------------------

juce::String formatDuration(double seconds) {
    const auto total = static_cast<juce::int64>(std::llround(std::max(0.0, seconds) / 60.0));
    return juce::String(total / 60) + "h " + juce::String(total % 60) + "m";
}

fs::path defaultLibraryRoot() { return AppSettings::defaultDirectory() / "corpus"; }

fs::path libraryRootOf(const AppSettingsData& d) { return d.corpusRoot.empty() ? defaultLibraryRoot() : fs::path(std::u8string(d.corpusRoot.begin(), d.corpusRoot.end())); }

LibraryStatus readLibraryStatus(const fs::path& root) {
    LibraryStatus s;
    std::ifstream f(root / "manifest.json");
    if (!f) return s;
    const auto j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return s;
    s.exists = true;
    auto num = [](const nlohmann::json& o, const char* k) { return o.is_object() && o.contains(k) && o[k].is_number() ? o[k].get<double>() : 0.0; };
    if (j.contains("counts")) {
        const auto& c = j["counts"];
        s.files = static_cast<int>(num(c, "files"));
        s.invalid = static_cast<int>(num(c, "rejected"));
        s.talkers = static_cast<int>(c.contains("usableSpeakers") ? num(c, "usableSpeakers") : num(c, "speakers"));
    }
    if (j.contains("durationS")) {
        s.totalS = num(j["durationS"], "total");
        s.usableSpeechS = num(j["durationS"], "usableSpeech");
    }
    return s;
}

// ---- background import --------------------------------------------------------------------------

struct CorpusImportJob::State {
    mutable std::mutex mu;
    Progress prog;
    ImportResult res;
    std::atomic<bool> started{false}, finished{false}, cancel{false};
};

CorpusImportJob::CorpusImportJob() : st_(std::make_shared<State>()) {}

CorpusImportJob::~CorpusImportJob() { st_->cancel.store(true); }

void CorpusImportJob::start(fs::path inputDir, fs::path stagingRoot) {
    if (st_->started.exchange(true)) return;
    auto st = st_;
    std::thread([st, inputDir = std::move(inputDir), stagingRoot = std::move(stagingRoot)] {
        ImportOptions opt;
        opt.inputDir = inputDir;
        opt.corpusRoot = stagingRoot;
        opt.cancel = &st->cancel;
        const unsigned hw = std::thread::hardware_concurrency();
        opt.threads = static_cast<int>(std::max(1u, hw > 2 ? hw - 2 : 1u));
        opt.progress = [st](std::size_t done, std::size_t total, const std::string& file) {
            std::lock_guard<std::mutex> lk(st->mu);
            st->prog = {done, total, file};
        };
        ImportResult r;
        try {
            r = importCorpus(opt);
        } catch (const std::exception& e) {
            r.ok = false;
            r.error = e.what();
        } catch (...) {
            r.ok = false;
            r.error = "unexpected error";
        }
        {
            std::lock_guard<std::mutex> lk(st->mu);
            st->res = std::move(r);
        }
        st->finished.store(true, std::memory_order_release);
    }).detach();
}

void CorpusImportJob::cancel() { st_->cancel.store(true); }
bool CorpusImportJob::started() const { return st_->started.load(); }
bool CorpusImportJob::finished() const { return st_->finished.load(std::memory_order_acquire); }
bool CorpusImportJob::cancelled() const { return st_->cancel.load(); }
CorpusImportJob::Progress CorpusImportJob::progress() const {
    std::lock_guard<std::mutex> lk(st_->mu);
    return st_->prog;
}
ImportResult CorpusImportJob::result() const {
    std::lock_guard<std::mutex> lk(st_->mu);
    return st_->res;
}

bool installImport(const fs::path& staging, const fs::path& root, std::string* error) {
    std::error_code ec;
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    if (!fs::exists(staging / "manifest.json", ec) || !fs::exists(staging / "corpus.sqlite", ec) || !fs::is_directory(staging / "cache", ec))
        return fail("The analysed library is incomplete.");
    fs::create_directories(root, ec);
    if (ec) return fail("Cannot create the library folder: " + ec.message());
    CorpusDb::purgeOldCaches(root.string());
    if (fs::exists(root / "cache", ec)) {
        std::string oldVersion = "unknown";
        std::ifstream mf(root / "manifest.json");
        const auto doc = nlohmann::json::parse(mf, nullptr, false);
        if (doc.is_object() && doc.contains("corpusVersion") && doc["corpusVersion"].is_string()) {
            std::string v = doc["corpusVersion"].get<std::string>();
            v.erase(std::remove_if(v.begin(), v.end(), [](unsigned char ch) { return !std::isalnum(ch); }), v.end());
            if (!v.empty()) oldVersion = v;
        }
        const fs::path oldCache = root / ("cache.old-" + oldVersion);
        fs::remove_all(oldCache, ec);
        fs::rename(root / "cache", oldCache, ec);
        if (ec) return fail("The current library is in use and cannot be replaced: " + ec.message());
    }
    ec.clear();
    fs::rename(staging / "cache", root / "cache", ec);
    if (ec) return fail("Cannot install the audio cache: " + ec.message());
    fs::remove(root / "corpus.sqlite", ec);
    fs::rename(staging / "corpus.sqlite", root / "corpus.sqlite", ec);
    if (ec) return fail("Cannot install the database: " + ec.message());
    fs::remove(root / "source_links.json", ec);
    if (fs::exists(staging / "source_links.json", ec)) fs::rename(staging / "source_links.json", root / "source_links.json", ec);
    fs::remove(root / "manifest.json", ec);
    fs::rename(staging / "manifest.json", root / "manifest.json", ec);  // manifest last
    if (ec) return fail("Cannot install the manifest: " + ec.message());
    fs::remove_all(staging, ec);
    return true;
}

ScanResult scanLibrary(const fs::path& root) {
    ScanResult r;
    std::string err;
    auto db = CorpusDb::open((root / "corpus.sqlite").string(), true, &err);
    if (!db) {
        r.error = err.empty() ? "No voice library found." : err;
        return r;
    }
    const auto rows = db->recordings();
    std::error_code ec;
    for (const auto& rec : rows) {
        ++r.files;
        r.totalS += rec.durationS;
        bool bad = rec.qualityClass == 0;
        if (!bad) {
            if (rec.cacheFile.empty() || !fs::exists(root / fs::path(std::u8string(rec.cacheFile.begin(), rec.cacheFile.end())), ec)) bad = true;
        }
        if (bad) ++r.invalid;
        else r.usableSpeechS += rec.speechS;
    }
    for (const auto& sp : db->speakers())
        if (sp.enabled) ++r.talkers;
    r.ok = true;
    return r;
}

// ---- wizard ------------------------------------------------------------------------------------

ImportWizard::ImportWizard(AppSettings& settings, fs::path libraryRoot, fs::path inputDir)
    : settings_(settings), root_(std::move(libraryRoot)), input_(std::move(inputDir)) {
    const auto& t = Theme::get();
    staging_ = root_.parent_path() / (root_.filename().string() + ".import");
    hint_.setText("Choose the folder that contains your voice recordings (WAV, AIFF or FLAC). Speakers are taken from the "
                  "file names (name_001.wav). BabbleForge checks speech, clipping, silence, level, sample rate and duplicates "
                  "automatically.\n\nAdding a folder replaces the current library with its contents.",
                  juce::dontSendNotification);
    hint_.setFont(fonts::body(14.0f));
    hint_.setColour(juce::Label::textColourId, t.textDim);
    hint_.setJustificationType(juce::Justification::topLeft);
    status_.setFont(fonts::body(14.0f));
    status_.setColour(juce::Label::textColourId, t.warn);
    status_.setJustificationType(juce::Justification::topLeft);
    summary_.setFont(fonts::title(20.0f));
    summary_.setColour(juce::Label::textColourId, t.text);
    summary_.setJustificationType(juce::Justification::topLeft);
    path_.setFont(fonts::body(14.5f));
    path_.setTextToShowWhenEmpty("Folder with audio files", t.textFaint);
    path_.onTextChange = [this] {
        input_ = toPath(path_.getText());
        std::error_code ec;
        next_.setEnabled(fs::is_directory(input_, ec));
    };
    detailsText_.setMultiLine(true, false);
    detailsText_.setReadOnly(true);
    detailsText_.setFont(fonts::body(12.5f));
    for (auto* b : {&choose_, &next_, &cancel_, &details_}) {
        noFocus(*b);
    }
    choose_.onClick = [this] { chooseFolder(); };
    next_.onClick = [this] {
        if (step_ == SelectFiles) startAnalysis();
        else if (step_ == Review) commit();
        else if (step_ == Add && requestClose) requestClose();
    };
    cancel_.onClick = [this] {
        if (step_ == Analyze) cancelAnalysis();
        else {
            if (step_ == Review) cleanupStaging();
            if (requestClose) requestClose();
        }
    };
    details_.onClick = [this] {
        detailsText_.setVisible(!detailsText_.isVisible());
        details_.setButtonText(detailsText_.isVisible() ? "Hide details" : "Show details");
        resized();
    };
    details_.setTooltip("Technical details for each file: quality class and the reasons for it.");
    for (juce::Component* c : std::initializer_list<juce::Component*>{&hint_, &status_, &summary_, &path_, &choose_, &next_, &cancel_, &details_, &bar_, &detailsText_})
        addChildComponent(c);
    path_.setText(fromPath(input_), juce::dontSendNotification);
    next_.setTooltip("Analyse the recordings in the background.");
    std::error_code ec;
    goTo(SelectFiles);
    next_.setEnabled(!input_.empty() && fs::is_directory(input_, ec));
}

ImportWizard::~ImportWizard() {
    stopTimer();
    if (job_->started() && !job_->finished()) job_->cancel();  // the worker removes its own staging data
    else if (!committed_) cleanupStaging();
}

void ImportWizard::cleanupStaging() {
    std::error_code ec;
    fs::remove_all(staging_, ec);
}

void ImportWizard::chooseFolder() {
    chooser_ = std::make_unique<juce::FileChooser>("Choose the folder with your voice recordings", juce::File(), "*", true);
    juce::Component::SafePointer<ImportWizard> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [self](const juce::FileChooser& fc) {
                              if (!self) return;
                              const auto f = fc.getResult();
                              if (f != juce::File()) self->setInputDir(toPath(f.getFullPathName()));
                          });
}

void ImportWizard::setInputDir(const fs::path& dir) {
    input_ = dir;
    path_.setText(fromPath(dir), juce::dontSendNotification);
    std::error_code ec;
    next_.setEnabled(fs::is_directory(dir, ec));
}

void ImportWizard::goTo(Step s) {
    step_ = s;
    for (juce::Component* c : std::initializer_list<juce::Component*>{&hint_, &status_, &summary_, &path_, &choose_, &next_, &cancel_, &details_, &bar_, &detailsText_})
        c->setVisible(false);
    status_.setVisible(true);
    cancel_.setVisible(true);
    cancel_.setEnabled(true);
    switch (s) {
    case SelectFiles:
        hint_.setVisible(true);
        path_.setVisible(true);
        choose_.setVisible(true);
        next_.setVisible(true);
        next_.setButtonText("Analyze");
        cancel_.setButtonText("Cancel");
        break;
    case Analyze:
        bar_.setVisible(true);
        cancel_.setButtonText("Cancel");
        break;
    case Review:
        summary_.setVisible(true);
        details_.setVisible(true);
        next_.setVisible(true);
        next_.setEnabled(true);
        next_.setButtonText("Add to Library");
        cancel_.setButtonText("Cancel");
        details_.setButtonText("Show details");
        break;
    case Add:
        summary_.setVisible(true);
        next_.setVisible(true);
        next_.setEnabled(true);
        next_.setButtonText("Done");
        cancel_.setVisible(false);
        break;
    }
    resized();
    repaint();
}

void ImportWizard::startAnalysis() {
    std::error_code ec;
    if (input_.empty() || !fs::is_directory(input_, ec)) {
        status_.setText("Choose a folder first.", juce::dontSendNotification);
        return;
    }
    if (job_->started()) job_ = std::make_unique<CorpusImportJob>();  // a previous (cancelled) run keeps its own state
    cleanupStaging();
    cancelling_ = false;
    progress_ = 0.0;
    status_.setText("Looking for audio files...", juce::dontSendNotification);
    goTo(Analyze);
    job_->start(input_, staging_);
    startTimerHz(10);
}

void ImportWizard::cancelAnalysis() {
    if (step_ != Analyze) return;
    cancelling_ = true;
    job_->cancel();
    cancel_.setEnabled(false);
    status_.setText("Cancelling...", juce::dontSendNotification);
}

void ImportWizard::timerCallback() {
    if (step_ != Analyze) {
        stopTimer();
        return;
    }
    const auto p = job_->progress();
    if (p.total > 0 && !cancelling_) {
        progress_ = static_cast<double>(p.done) / static_cast<double>(p.total);
        status_.setText("Analysing " + juce::String(static_cast<int>(p.done)) + " of " + juce::String(static_cast<int>(p.total)) + ": " +
                            juce::String::fromUTF8(p.file.c_str()),
                        juce::dontSendNotification);
        status_.setColour(juce::Label::textColourId, Theme::get().textDim);
    }
    if (!job_->finished()) return;
    stopTimer();
    result_ = job_->result();
    if (cancelling_ || (!result_.ok && result_.error == "cancelled")) {
        cleanupStaging();
        goTo(SelectFiles);
        status_.setColour(juce::Label::textColourId, Theme::get().warn);
        status_.setText("Analysis cancelled. Nothing was added.", juce::dontSendNotification);
        return;
    }
    if (!result_.ok) {
        cleanupStaging();
        goTo(SelectFiles);
        status_.setColour(juce::Label::textColourId, Theme::get().danger);
        status_.setText("The analysis failed: " + juce::String::fromUTF8(result_.error.c_str()), juce::dontSendNotification);
        return;
    }
    showReview();
}

void ImportWizard::showReview() {
    analyzed_ = static_cast<int>(result_.files.size());
    juce::String s;
    s << analyzed_ << " files analyzed\n\n" << static_cast<int>(result_.nGood) << " Good\n" << static_cast<int>(result_.nUsable)
      << " Usable\n" << static_cast<int>(result_.nRejected) << " Rejected";
    summary_.setText(s, juce::dontSendNotification);
    juce::String d;
    for (const auto& f : result_.files) {
        const char* cls = f.cls == ingest::QualityClass::Good ? "Good    " : f.cls == ingest::QualityClass::Usable ? "Usable  " : "Rejected";
        d << cls << "  " << juce::String::fromUTF8(f.relPath.c_str());
        if (!f.reasons.empty()) {
            d << "  (";
            for (std::size_t i = 0; i < f.reasons.size(); ++i) d << (i ? ", " : "") << juce::String::fromUTF8(f.reasons[i].c_str());
            d << ")";
        }
        d << "\n";
    }
    detailsText_.setText(d, false);
    status_.setText({}, juce::dontSendNotification);
    next_.setTooltip("Install the analysed recordings as your voice library.");
    goTo(Review);
}

bool ImportWizard::commit() {
    if (step_ != Review) return false;
    std::string err;
    if (!installImport(staging_, root_, &err)) {
        status_.setColour(juce::Label::textColourId, Theme::get().danger);
        status_.setText(juce::String::fromUTF8(err.c_str()), juce::dontSendNotification);
        return false;
    }
    committed_ = true;
    const std::string rootUtf8 = fromPath(root_).toStdString(), inUtf8 = fromPath(input_).toStdString();
    settings_.update([&](AppSettingsData& d) { d.corpusRoot = rootUtf8; });
    updatePrefs(settings_, [&](Prefs& p) { p.lastImportDir = inUtf8; });
    goTo(Add);
    summary_.setText("Voice library updated.\n\n" + juce::String(static_cast<int>(result_.nGood + result_.nUsable)) + " recordings from " +
                         juce::String(static_cast<int>(result_.nUsableSpeakers)) + " talkers added.",
                     juce::dontSendNotification);
    status_.setColour(juce::Label::textColourId, Theme::get().textDim);
    status_.setText("Restart BabbleForge to use the new library.", juce::dontSendNotification);
    if (onFinished) onFinished(true);
    return true;
}

void ImportWizard::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    static const char* const names[4] = {"1  Select Files", "2  Analyze", "3  Review", "4  Add"};
    auto r = getLocalBounds().removeFromTop(26);
    const int w = r.getWidth() / 4;
    g.setFont(fonts::heading(13.0f));
    for (int i = 0; i < 4; ++i) {
        g.setColour(i == static_cast<int>(step_) ? t.accentStrong : (i < static_cast<int>(step_) ? t.textDim : t.textFaint));
        g.drawText(names[i], r.removeFromLeft(w), juce::Justification::centredLeft);
    }
    g.setColour(t.outline);
    g.drawHorizontalLine(28, 0.0f, static_cast<float>(getWidth()));
}

void ImportWizard::resized() {
    auto r = getLocalBounds();
    r.removeFromTop(38);
    auto buttons = r.removeFromBottom(38);
    next_.setBounds(buttons.removeFromRight(150));
    buttons.removeFromRight(10);
    cancel_.setBounds(buttons.removeFromRight(110));
    r.removeFromBottom(8);
    switch (step_) {
    case SelectFiles: {
        hint_.setBounds(r.removeFromTop(120));
        auto row = r.removeFromTop(32);
        choose_.setBounds(row.removeFromRight(150));
        row.removeFromRight(8);
        path_.setBounds(row);
        r.removeFromTop(8);
        status_.setBounds(r.removeFromTop(48));
        break;
    }
    case Analyze:
        r.removeFromTop(30);
        bar_.setBounds(r.removeFromTop(24));
        r.removeFromTop(10);
        status_.setBounds(r.removeFromTop(48));
        break;
    case Review:
    case Add: {
        summary_.setBounds(r.removeFromTop(step_ == Review ? 150 : 90));
        if (step_ == Review) {
            auto row = r.removeFromTop(30);
            details_.setBounds(row.removeFromLeft(140));
            if (detailsText_.isVisible()) {
                r.removeFromTop(6);
                detailsText_.setBounds(r);
            }
        }
        if (step_ == Add || !detailsText_.isVisible()) status_.setBounds(r.removeFromTop(40));
        else status_.setBounds({});
        break;
    }
    }
}

std::unique_ptr<OverlayDialog> makeImportWizardDialog(AppSettings& settings, fs::path libraryRoot, fs::path inputDir, bool autoStart,
                                                      std::function<void(bool)> onDone) {
    auto body = std::make_unique<ImportWizard>(settings, std::move(libraryRoot), std::move(inputDir));
    auto* w = body.get();
    auto d = std::make_unique<OverlayDialog>("Add Audio", std::move(body), 340, 600);
    OverlayDialog* dlg = d.get();
    w->requestClose = [dlg] { dlg->close(); };
    w->onFinished = std::move(onDone);
    if (autoStart) w->startAnalysis();
    return d;
}

// ---- Manage Library -----------------------------------------------------------------------------

struct ManageLibraryBody::ScanState {
    std::mutex mu;
    ScanResult res;
    std::atomic<bool> done{false};
};

ManageLibraryBody::ManageLibraryBody(AppSettings& settings, std::function<void(std::unique_ptr<OverlayDialog>)> openDialog,
                                     std::function<void()> onChanged)
    : settings_(settings), openDialog_(std::move(openDialog)), onChanged_(std::move(onChanged)) {
    const auto& t = Theme::get();
    static const char* const names[5] = {"Talkers", "Audio files", "Total duration", "Usable speech", "Invalid files"};
    for (std::size_t i = 0; i < 5; ++i) {
        names_[i].setText(names[i], juce::dontSendNotification);
        names_[i].setFont(fonts::body(14.5f));
        names_[i].setColour(juce::Label::textColourId, t.textDim);
        values_[i].setFont(fonts::title(17.0f));
        values_[i].setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(names_[i]);
        addAndMakeVisible(values_[i]);
    }
    message_.setFont(fonts::body(13.5f));
    message_.setColour(juce::Label::textColourId, t.textDim);
    message_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(message_);
    for (auto* b : {&add_, &scan_, &rebuild_}) {
        noFocus(*b);
        addAndMakeVisible(*b);
    }
    add_.setTooltip("Import a folder of recordings. They are analysed automatically before anything is added.");
    scan_.setTooltip("Check that every file in the library is still readable.");
    rebuild_.setTooltip("Analyse the original recordings again, for example after a BabbleForge update.");
    add_.onClick = [this] { addAudio(); };
    scan_.onClick = [this] { scan(); };
    rebuild_.onClick = [this] { rebuildAnalysis(); };
    refreshStatus();
}

ManageLibraryBody::~ManageLibraryBody() { stopTimer(); }

void ManageLibraryBody::refreshStatus() {
    const auto s = readLibraryStatus(libraryRootOf(settings_.get()));
    auto set = [&](int i, const juce::String& v) { values_[static_cast<std::size_t>(i)].setText(v, juce::dontSendNotification); };
    if (!s.exists) {
        for (int i = 0; i < 5; ++i) set(i, "-");
        message_.setText("No voice library yet. Use Add Audio to import recordings.", juce::dontSendNotification);
        return;
    }
    set(0, juce::String(s.talkers));
    set(1, juce::String(s.files));
    set(2, formatDuration(s.totalS));
    set(3, formatDuration(s.usableSpeechS));
    set(4, juce::String(s.invalid));
}

void ManageLibraryBody::scan() {
    if (scanning_) return;
    scanning_ = true;
    scan_.setEnabled(false);
    message_.setText("Scanning the library...", juce::dontSendNotification);
    scanState_ = std::make_shared<ScanState>();
    auto st = scanState_;
    const auto root = libraryRootOf(settings_.get());
    std::thread([st, root] {
        auto r = scanLibrary(root);
        {
            std::lock_guard<std::mutex> lk(st->mu);
            st->res = std::move(r);
        }
        st->done.store(true, std::memory_order_release);
    }).detach();
    startTimerHz(10);
}

void ManageLibraryBody::timerCallback() {
    if (!scanState_ || !scanState_->done.load(std::memory_order_acquire)) return;
    stopTimer();
    ScanResult r;
    {
        std::lock_guard<std::mutex> lk(scanState_->mu);
        r = scanState_->res;
    }
    scanState_.reset();
    scanning_ = false;
    scan_.setEnabled(true);
    if (!r.ok) {
        message_.setText("Scan failed: " + juce::String::fromUTF8(r.error.c_str()), juce::dontSendNotification);
        return;
    }
    value(0).setText(juce::String(r.talkers), juce::dontSendNotification);
    value(1).setText(juce::String(r.files), juce::dontSendNotification);
    value(2).setText(formatDuration(r.totalS), juce::dontSendNotification);
    value(3).setText(formatDuration(r.usableSpeechS), juce::dontSendNotification);
    value(4).setText(juce::String(r.invalid), juce::dontSendNotification);
    message_.setText(r.invalid == 0 ? "Scan complete. All files are readable." : "Scan complete. Some files are unusable.",
                     juce::dontSendNotification);
}

void ManageLibraryBody::addAudio() {
    if (!openDialog_) return;
    juce::Component::SafePointer<ManageLibraryBody> self(this);
    openDialog_(makeImportWizardDialog(settings_, libraryRootOf(settings_.get()), {}, false, [self](bool) {
        if (self) {
            self->refreshStatus();
            if (self->onChanged_) self->onChanged_();
        }
    }));
}

void ManageLibraryBody::rebuildAnalysis() {
    if (!openDialog_) return;
    const Prefs p = loadPrefs(settings_.get());
    std::error_code ec;
    const fs::path dir = toPath(juce::String::fromUTF8(p.lastImportDir.c_str()));
    const bool have = !p.lastImportDir.empty() && fs::is_directory(dir, ec);
    if (!have) message_.setText("The original recordings folder is not known. Choose it in the next step.", juce::dontSendNotification);
    juce::Component::SafePointer<ManageLibraryBody> self(this);
    openDialog_(makeImportWizardDialog(settings_, libraryRootOf(settings_.get()), have ? dir : fs::path{}, have, [self](bool) {
        if (self) {
            self->refreshStatus();
            if (self->onChanged_) self->onChanged_();
        }
    }));
}

void ManageLibraryBody::resized() {
    auto r = getLocalBounds();
    for (std::size_t i = 0; i < 5; ++i) {
        auto row = r.removeFromTop(26);
        names_[i].setBounds(row.removeFromLeft(row.getWidth() / 2));
        values_[i].setBounds(row);
    }
    r.removeFromTop(10);
    auto row = r.removeFromTop(36);
    const int w = (row.getWidth() - 16) / 3;
    add_.setBounds(row.removeFromLeft(w));
    row.removeFromLeft(8);
    scan_.setBounds(row.removeFromLeft(w));
    row.removeFromLeft(8);
    rebuild_.setBounds(row);
    r.removeFromTop(8);
    message_.setBounds(r);
}

ManageLibraryHandles makeManageLibraryDialog(AppSettings& settings, std::function<void(std::unique_ptr<OverlayDialog>)> openDialog,
                                             std::function<void()> onLibraryChanged) {
    auto body = std::make_unique<ManageLibraryBody>(settings, std::move(openDialog), std::move(onLibraryChanged));
    ManageLibraryHandles h;
    h.body = body.get();
    h.dialog = std::make_unique<OverlayDialog>("Manage Library", std::move(body), 250, 520);
    h.dialog->addButton("Close", nullptr, true);
    return h;
}

}  // namespace bf::gui
