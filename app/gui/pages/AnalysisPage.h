#pragma once
// ANALYSIS page (docs/GUI.md §41-§45), Advanced mode only. Informative, not operational:
// four cards fed by the engine's MaskStatistics (EngineStatus::stats, 30 Hz). The page throttles
// its own updates to <= 15 Hz and keeps fixed-size arrays; painting does not allocate big
// objects.
//   VOICE ACTIVITY  talker counts, speech occupancy, gaps, scrolling per-voice strip
//   SPECTRUM        Octave | 1/3 Octave | FFT overlay target vs actual, average error, largest deviation
//   MODULATION      temporal density Low/Medium/High, "Show details": 0.5-16 Hz modulation spectrum
//   SPATIAL         per-speaker activity, channel correlation Low/Good, "Show details": coefficients
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "Widgets.h"
#include "core/engine/MaskEngine.h"
#include "dialogs/HelpParts.h"
#include "pages/Page.h"

namespace bf::gui {

enum class SpecMode { Octave, ThirdOctave, Fft };

struct SpectrumMetrics {
    bool valid = false;
    double avgErrorDb = 0.0;  // mean |deviation| over the bands, after removing the overall level offset
    double maxDevDb = 0.0;    // largest |deviation|
    double maxDevHz = 0.0;    // band centre of the largest deviation
};

// Shape deviation of `measDb` (26 x 1/3 octave, dB) from `refDb` over 125 Hz..8 kHz. The overall
// level offset (power-weighted by the target) is removed first, as in the engine statistics.
// Octave mode compares the 7 octave bands. `alignedDb` (optional, 26 values / 7 for Octave)
// receives the measured curve shifted by the offset.
SpectrumMetrics computeSpectrumMetrics(const ThirdOctArray& refDb, const ThirdOctArray& measDb, SpecMode mode,
                                       std::array<double, 26>* alignedDb = nullptr);

// Occupancy / gap numbers as shown on the card (pure; used by tests).
juce::String formatMs(double seconds);

class AnalysisPage final : public Page {
public:
    explicit AnalysisPage(PageContext& c);

    int layoutPage(int width) override;
    void refreshStatus(const EngineStatus& s) override;

    // Feeds a statistics snapshot (null: no data). Ignores the 15 Hz throttle when `force`.
    void applyStats(std::shared_ptr<const MaskStatistics> s, bool force = true);

    // ---- cards (public for tests) ----
    class VoiceStrip final : public juce::Component {
    public:
        static constexpr int kColumns = 150;  // 30 s at 5 columns / s
        void push(std::uint64_t slotMask);
        void clear();
        int rows() const noexcept { return rows_; }
        std::uint64_t column(int i) const noexcept { return cols_[static_cast<std::size_t>((head_ + i) % kColumns)]; }
        void paint(juce::Graphics&) override;
        bool hasData = false;

    private:
        std::array<std::uint64_t, kColumns> cols_{};
        int head_ = 0, rows_ = 4;
    };

    class VoiceCard final : public Card {
    public:
        VoiceCard();
        void setStats(const MaskStatistics* s);
        void resized() override;
        juce::Label& value(int i) { return values_[static_cast<std::size_t>(i)]; }
        VoiceStrip strip;
        juce::Label empty;

    private:
        std::array<juce::Label, 5> names_, values_;
    };

    class SpectrumView final : public juce::Component {
    public:
        void set(SpecMode m, const std::array<double, 26>& ref, const std::array<double, 26>& meas, bool haveMeas);
        // FFT mode: the engine's 1/24-octave smoothed FFT (kFftViewPoints dB values) with the same
        // overall offset removed as the band curves (offsetDb). Null: no FFT data yet.
        void setFft(const std::array<float, kFftViewPoints>* fftDb, double offsetDb);
        bool hasFft() const noexcept { return haveFft_; }
        void paint(juce::Graphics&) override;

    private:
        SpecMode mode_ = SpecMode::ThirdOctave;
        std::array<double, 26> ref_{}, meas_{};
        bool haveMeas_ = false;
        std::array<float, kFftViewPoints> fft_{};
        double fftOffset_ = 0.0;
        bool haveFft_ = false;
    };

    class SpectrumCard final : public Card {
    public:
        SpectrumCard();
        void setStats(std::shared_ptr<const MaskStatistics> s);
        void setMode(SpecMode m);
        SpecMode mode() const noexcept { return mode_; }
        void resized() override;
        juce::TextButton& modeButton(int i) { return modes_[static_cast<std::size_t>(i)]; }
        juce::Label errorLabel, deviationLabel;
        SpectrumView view;

    private:
        void refresh();
        std::array<juce::TextButton, 3> modes_;
        SpecMode mode_ = SpecMode::ThirdOctave;
        std::shared_ptr<const MaskStatistics> stats_;
    };

    class DensityMeter final : public juce::Component {
    public:
        void set(int level, bool have);  // 0 Low, 1 Medium, 2 High
        int level() const noexcept { return level_; }
        void paint(juce::Graphics&) override;

    private:
        int level_ = 1;
        bool have_ = false;
    };

    class ModSpectrumView final : public juce::Component {
    public:
        void set(const ModBandArray& m, bool have);
        void paint(juce::Graphics&) override;

    private:
        ModBandArray m_{};
        bool have_ = false;
    };

    class ModulationCard final : public Card {
    public:
        ModulationCard();
        void setStats(const MaskStatistics* s);
        bool detailsShown() const noexcept { return details_; }
        void setDetails(bool on);
        int preferredHeight() const;
        void resized() override;
        std::function<void()> onHeightChanged;
        DensityMeter meter;
        juce::TextButton detailsButton{"Show details"};
        ModSpectrumView spectrum;
        juce::Label detailText, caption;

    private:
        bool details_ = false;
    };

    class SpeakerMap final : public juce::Component {
    public:
        // pct: per-speaker time-active percentage (0..100) from MaskStatistics::outputActiveFraction.
        void set(std::vector<double> pct, bool have);
        const std::vector<double>& percent() const noexcept { return pct_; }
        void paint(juce::Graphics&) override;

    private:
        std::vector<double> pct_;
        bool have_ = false;
    };

    class SpatialCard final : public Card {
    public:
        SpatialCard();
        void setStats(const MaskStatistics* s);
        bool detailsShown() const noexcept { return details_; }
        void setDetails(bool on);
        int preferredHeight() const;
        void resized() override;
        std::function<void()> onHeightChanged;
        SpeakerMap map;
        juce::Label correlationName, correlationValue, detailText;
        juce::TextButton detailsButton{"Show details"};

    private:
        bool details_ = false;
    };

    VoiceCard& voice() { return voice_; }
    SpectrumCard& spectrum() { return spectrum_; }
    ModulationCard& modulation() { return modulation_; }
    SpatialCard& spatial() { return spatial_; }
    int updateCount() const noexcept { return updates_; }

private:
    VoiceCard voice_;
    SpectrumCard spectrum_;
    ModulationCard modulation_;
    SpatialCard spatial_;
    double lastUpdateMs_ = 0.0, lastColumnMs_ = 0.0;
    int updates_ = 0;
    bool hadStats_ = false;
};

}  // namespace bf::gui
