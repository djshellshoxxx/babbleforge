#pragma once
// AppState: the single editable model behind every page (docs/GUI_ARCHITECTURE.md).
//
// Holds the current preset document (bf::Preset — area, strategy, macros, strength, outputs
// and advanced overrides) plus the UI mode (Simple/Advanced, persisted through AppSettings).
// Every preset edit goes through juce::UndoManager as a whole-preset snapshot action; edits
// that share a coalesce key inside one gesture (a slider drag) merge into a single undo step.
//
// After every change the preset is composed synchronously (bf::compose + bf::composePlan,
// ~0.1 ms) so pages always display effective values; the EngineBridge pushes the preset to
// the engine on its own debounce/worker thread.
//
// All methods: message thread only.
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <juce_data_structures/juce_data_structures.h>

#include "core/config/Compose.h"
#include "core/config/DataSet.h"
#include "core/strategy/PlanComposer.h"
#include "model/AppSettings.h"

namespace bf::gui {

enum class UiMode { Simple, Advanced };
enum class Coverage { Stereo, MultiSpeaker };
enum class TalkerField { Pool, Average, Minimum, Maximum };

// Talker-count limits (GUI §26).
struct TalkerLimits {
    static constexpr int kPoolMin = 2, kPoolMax = 64;
    static constexpr double kAvgMin = 1.0, kAvgMax = 32.0;
    static constexpr int kMinMin = 0, kMaxMax = 32;
};

struct TalkerCounts {
    int pool = 14;
    double average = 6.5;
    int minimum = 4;
    int maximum = 9;
};

// Custom spectrum: 7 octave bands (GUI §31), offsets relative to Universal LTASS.
inline constexpr std::array<double, 7> kEqBandsHz = {125, 250, 500, 1000, 2000, 4000, 8000};
inline juce::String eqBandLabel(std::size_t b) {
    static const char* const names[] = {"125", "250", "500", "1k", "2k", "4k", "8k"};
    return b < 7 ? juce::String(names[b]) : juce::String();
}
inline constexpr double kEqMaxDb = 12.0;      // maximum correction
inline constexpr double kEqDefaultRangeDb = 6.0;  // default recommended user range

class AppState {
public:
    enum Change : unsigned { kPresetChanged = 1u, kModeChanged = 2u };

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void appStateChanged(unsigned changes) = 0;
    };

    AppState(const DataSet& ds, AppSettings& settings, juce::UndoManager& undo, Preset initial);
    ~AppState();

    // Recommended (factory) preset for an Area x Strategy pair: macros and advanced fields
    // inherit (null), Strength = the Area default, stereo, limiter on (PRESETS §2, §10).
    static Preset factoryPreset(const DataSet& ds, const std::string& area, const std::string& strategy);

    const DataSet& data() const noexcept { return ds_; }
    AppSettings& settings() noexcept { return settings_; }
    juce::UndoManager& undoManager() noexcept { return undo_; }

    // ---- mode --------------------------------------------------------------
    UiMode mode() const noexcept { return mode_; }
    bool advanced() const noexcept { return mode_ == UiMode::Advanced; }
    void setMode(UiMode m);  // persisted; never touches audio (GUI §1)

    // ---- preset / effective values -----------------------------------------
    const Preset& preset() const noexcept { return preset_; }
    const EffectiveConfig& effective() const noexcept { return eff_; }
    const ComposedPlan& plan() const noexcept { return plan_; }  // macro-applied plan (display)
    std::uint64_t revision() const noexcept { return revision_; }

    // ---- undo transactions -------------------------------------------------
    // A gesture (slider drag) groups all edits into one undo step; edits outside a gesture
    // each start their own transaction.
    void beginGesture(const juce::String& name);
    void endGesture();
    bool gestureActive() const noexcept { return gesture_; }
    // Applies fn to a copy of the preset and performs it as an undoable action. Edits with the
    // same non-empty coalesceKey inside one transaction merge.
    void edit(const juce::String& name, const std::string& coalesceKey, const std::function<void(Preset&)>& fn);
    void replacePreset(const Preset& p, const juce::String& name);  // undoable whole replacement
    void applyPresetNoUndo(const Preset& p);                          // used by undo actions / loading
    bool undo();
    bool redo();

    // ---- RUN / MASK simple controls ---------------------------------------
    void setArea(const std::string& id);          // loads the area's recommended settings (GUI §5)
    void setStrategy(const std::string& id);      // strategy-dependent macros return to defaults
    void setStrengthDb(double db);
    void setCharacter(double c);
    void setVoiceAmount(double v);
    void setVoiceVariety(const std::string& v);   // "low" | "balanced" | "high"
    void setClearVoiceReduction(double r);
    void setBabbleFraction(double b);             // Mask Mix: 0 steady ... 1 voices
    void setCoverage(Coverage c);

    double strengthDb() const;
    double character() const;
    double voiceAmount() const;
    std::string voiceVariety() const;
    double clearVoiceReduction() const;
    double babbleFraction() const;
    Coverage coverage() const;
    double strengthMinDb() const;  // Simple / Advanced range (ENGINE §3.1)
    double strengthMaxDb() const;

    // ---- Advanced: talker engine (GUI §26-§28) -----------------------------
    TalkerCounts talkerCounts() const;  // explicit overrides, else the composed plan
    void setTalkerValue(TalkerField f, double v);
    // min <= average <= max <= pool, all inside TalkerLimits; `changed` wins.
    static TalkerCounts enforceTalkerCounts(TalkerCounts c, TalkerField changed);

    // ---- Advanced: timing (GUI §27) ----------------------------------------
    struct Timing {
        double maxInternalGapMs = 250, segmentMinS = 1.5, segmentMaxS = 20, gainVariationDb = 2, fadeMs = 150;
    };
    Timing timing() const;
    void setMaxInternalGapMs(double v);
    void setSegmentMinS(double v);  // keeps min <= max
    void setSegmentMaxS(double v);
    void setGainVariationDb(double v);
    void setFadeMs(double v);

    // ---- Advanced: stationary / spectrum (GUI §29-§32) ---------------------
    bool stationaryEnabled() const;
    void setStationaryEnabled(bool on);
    std::string seedMode() const;
    void setSeedMode(const std::string& m);  // "session" (random) | "fixed"
    std::string spectrumTarget() const;      // ltass_universal | slope_-5 | ... | custom
    void setSpectrumTarget(const std::string& id);
    std::array<double, 7> customEqDb() const;
    void setCustomEqDb(int band, double db);
    bool correctionEnabled() const;
    void setCorrectionEnabled(bool on);
    std::string correctionSpeed() const;
    void setCorrectionSpeed(const std::string& s);  // slow | normal | fast
    double ltassAt(double hz) const;                // Universal LTASS level at a band centre

    // ---- listeners ---------------------------------------------------------
    void addListener(Listener* l) { listeners_.add(l); }
    void removeListener(Listener* l) { listeners_.remove(l); }

private:
    void recompute();
    void notify(unsigned changes);
    void ensureTransaction(const juce::String& name);

    const DataSet& ds_;
    AppSettings& settings_;
    juce::UndoManager& undo_;
    UiMode mode_ = UiMode::Simple;
    Preset preset_;
    EffectiveConfig eff_;
    ComposedPlan plan_;
    std::uint64_t revision_ = 0;
    bool gesture_ = false;
    juce::ListenerList<Listener> listeners_;
};

}  // namespace bf::gui
