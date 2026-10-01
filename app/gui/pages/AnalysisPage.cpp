#include "pages/AnalysisPage.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "LookAndFeel.h"
#include "core/spectrum/SpectrumTarget.h"

namespace bf::gui {

static PageRegistrar analysisReg({"analysis", "ANALYSIS", 50, PageInfo::Sidebar,
                                  [](PageContext& c) { return std::make_unique<AnalysisPage>(c); }, true});

namespace {

constexpr std::size_t kFirstBand = 4, kLastBand = 22;  // 125 Hz .. 8 kHz in the 26-band array

void styleLabel(juce::Label& l, const juce::Font& f, juce::Colour c, juce::Justification j) {
    l.setFont(f);
    l.setColour(juce::Label::textColourId, c);
    l.setJustificationType(j);
    l.setInterceptsMouseClicks(false, false);
}

juce::String hzText(double hz) { return hz >= 1000.0 ? juce::String(hz / 1000.0, hz >= 10000.0 ? 0 : 1) + " kHz" : juce::String(juce::roundToInt(hz)) + " Hz"; }

juce::String dash() { return juce::String::fromUTF8("\xe2\x80\x94"); }

}  // namespace

juce::String formatMs(double seconds) { return juce::String(juce::roundToInt(seconds * 1000.0)) + " ms"; }

SpectrumMetrics computeSpectrumMetrics(const ThirdOctArray& refDb, const ThirdOctArray& measDb, SpecMode mode,
                                       std::array<double, 26>* alignedDb) {
    SpectrumMetrics m;
    std::array<double, 26> ref{}, meas{}, hz{};
    std::size_t n = 0;
    if (mode == SpecMode::Octave) {
        const OctaveArray ro = octaveFromThirdOct(refDb), mo = octaveFromThirdOct(measDb);
        const auto& fc = octaveNominalHz();
        for (std::size_t k = 0; k < kNumOctaveBands; ++k) {
            ref[k] = ro[k];
            meas[k] = mo[k];
            hz[k] = fc[k];
        }
        n = kNumOctaveBands;
    } else {
        const auto& fc = thirdOctNominalHz();
        for (std::size_t b = kFirstBand; b <= kLastBand; ++b) {
            ref[n] = refDb[b];
            meas[n] = measDb[b];
            hz[n] = fc[b];
            ++n;
        }
    }
    double wsum = 0.0, mu = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(meas[i]) || meas[i] < -150.0) return m;  // no measurement yet
        const double w = std::pow(10.0, ref[i] / 10.0);
        mu += w * (meas[i] - ref[i]);
        wsum += w;
    }
    mu /= std::max(wsum, 1e-30);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = meas[i] - ref[i] - mu;
        sum += std::fabs(d);
        if (std::fabs(d) >= m.maxDevDb) {
            m.maxDevDb = std::fabs(d);
            m.maxDevHz = hz[i];
        }
        if (alignedDb) (*alignedDb)[i] = meas[i] - mu;
    }
    m.avgErrorDb = sum / static_cast<double>(n);
    m.valid = true;
    return m;
}

// ================================================================================== VoiceStrip

void AnalysisPage::VoiceStrip::push(std::uint64_t mask) {
    cols_[static_cast<std::size_t>(head_)] = mask;
    head_ = (head_ + 1) % kColumns;
    hasData = true;
    int top = 0;
    for (const auto c : cols_)
        if (c) top = std::max(top, 64 - static_cast<int>(std::countl_zero(c)));
    rows_ = juce::jlimit(4, 16, top);
    repaint();
}

void AnalysisPage::VoiceStrip::clear() {
    cols_.fill(0);
    hasData = false;
    repaint();
}

void AnalysisPage::VoiceStrip::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    auto r = getLocalBounds().toFloat();
    g.setColour(t.panelAlt);
    g.fillRoundedRectangle(r, 6.0f);
    const auto plot = r.reduced(30.0f, 6.0f);
    const float rowH = plot.getHeight() / static_cast<float>(rows_);
    const float colW = plot.getWidth() / static_cast<float>(kColumns);
    g.setFont(fonts::body(10.5f));
    for (int row = 0; row < rows_; ++row) {
        const float y = plot.getY() + static_cast<float>(row) * rowH;
        g.setColour(t.textFaint);
        g.drawText("V" + juce::String(row + 1), juce::Rectangle<float>(r.getX() + 2.0f, y, 26.0f, rowH), juce::Justification::centredRight);
        // Runs of consecutive active columns become one rectangle.
        g.setColour(t.accent.withAlpha(0.85f));
        int runStart = -1;
        for (int c = 0; c <= kColumns; ++c) {
            const bool on = c < kColumns && ((column(c) >> row) & 1u);
            if (on && runStart < 0) runStart = c;
            if (!on && runStart >= 0) {
                g.fillRect(juce::Rectangle<float>(plot.getX() + static_cast<float>(runStart) * colW, y + 1.5f,
                                                  static_cast<float>(c - runStart) * colW, juce::jmax(2.0f, rowH - 3.0f)));
                runStart = -1;
            }
        }
    }
}

// ================================================================================== VoiceCard

AnalysisPage::VoiceCard::VoiceCard()
    : Card("Voice Activity", "Shows how many voices are talking and how often the babble pauses. Natural conversation has "
                             "short gaps; a denser mask has fewer.") {
    static const char* const names[5] = {"Active Talkers", "Average Talkers", "Speech Occupancy", "Average Gap", "Longest Recent Gap"};
    const auto& t = Theme::get();
    for (std::size_t i = 0; i < 5; ++i) {
        names_[i].setText(names[i], juce::dontSendNotification);
        styleLabel(names_[i], fonts::body(14.5f), t.textDim, juce::Justification::centredLeft);
        styleLabel(values_[i], fonts::title(17.0f), t.text, juce::Justification::centredRight);
        values_[i].setText(dash(), juce::dontSendNotification);
        addAndMakeVisible(names_[i]);
        addAndMakeVisible(values_[i]);
    }
    addAndMakeVisible(strip);
    styleLabel(empty, fonts::body(13.5f), t.textFaint, juce::Justification::centred);
    empty.setText("Start masking to see live voice activity.", juce::dontSendNotification);
    addAndMakeVisible(empty);
}

void AnalysisPage::VoiceCard::setStats(const MaskStatistics* s) {
    const bool have = s && s->babbleActive;
    if (!have) {
        for (auto& v : values_) v.setText(dash(), juce::dontSendNotification);
        empty.setText(s ? "No voice masking in the current plan." : "Start masking to see live voice activity.", juce::dontSendNotification);
        empty.setVisible(true);
        return;
    }
    empty.setVisible(false);
    int active = 0;
    for (const auto b : s->slotActive) active += b ? 1 : 0;
    values_[0].setText(juce::String(active), juce::dontSendNotification);
    values_[1].setText(juce::String(s->meanActive, 1), juce::dontSendNotification);
    values_[2].setText(juce::String(juce::roundToInt(s->occupancy60s * 100.0)) + "%", juce::dontSendNotification);
    values_[3].setText(s->gapMean60s > 0.0 ? formatMs(s->gapMean60s) : dash(), juce::dontSendNotification);
    values_[4].setText(s->gapMax60s > 0.0 ? formatMs(s->gapMax60s) : dash(), juce::dontSendNotification);
}

void AnalysisPage::VoiceCard::resized() {
    Card::resized();
    auto r = contentArea();
    for (std::size_t i = 0; i < 5; ++i) {
        auto row = r.removeFromTop(24);
        names_[i].setBounds(row.removeFromLeft(row.getWidth() / 2));
        values_[i].setBounds(row);
    }
    r.removeFromTop(10);
    strip.setBounds(r);
    empty.setBounds(r);
}

// ================================================================================== Spectrum

void AnalysisPage::SpectrumView::set(SpecMode m, const std::array<double, 26>& ref, const std::array<double, 26>& meas, bool haveMeas) {
    mode_ = m;
    ref_ = ref;
    meas_ = meas;
    haveMeas_ = haveMeas;
    repaint();
}

void AnalysisPage::SpectrumView::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    auto r = getLocalBounds().toFloat();
    g.setColour(t.panelAlt);
    g.fillRoundedRectangle(r, 6.0f);
    auto plot = r.reduced(34.0f, 12.0f).withTrimmedBottom(10.0f);
    const bool oct = mode_ == SpecMode::Octave;
    std::array<double, 26> hz{};
    std::size_t n = 0;
    if (oct) {
        for (const double f : octaveNominalHz()) hz[n++] = f;
    } else {
        for (std::size_t b = kFirstBand; b <= kLastBand; ++b) hz[n++] = thirdOctNominalHz()[b];
    }
    double lo = 1e9, hi = -1e9;
    for (std::size_t i = 0; i < n; ++i) {
        lo = std::min(lo, ref_[i]);
        hi = std::max(hi, ref_[i]);
        if (haveMeas_) {
            lo = std::min(lo, meas_[i]);
            hi = std::max(hi, meas_[i]);
        }
    }
    lo = std::floor((lo - 3.0) / 5.0) * 5.0;
    hi = std::ceil((hi + 3.0) / 5.0) * 5.0;
    const double f0 = hz[0] / 1.15, f1 = hz[n - 1] * 1.15;
    auto xOf = [&](double f) { return plot.getX() + static_cast<float>(std::log(f / f0) / std::log(f1 / f0)) * plot.getWidth(); };
    auto yOf = [&](double db) { return plot.getBottom() - static_cast<float>((db - lo) / (hi - lo)) * plot.getHeight(); };
    g.setFont(fonts::body(10.5f));
    for (const double f : {125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0}) {
        g.setColour(t.outline.withAlpha(0.5f));
        g.drawVerticalLine(juce::roundToInt(xOf(f)), plot.getY(), plot.getBottom());
        g.setColour(t.textFaint);
        g.drawText(f >= 1000 ? juce::String(juce::roundToInt(f / 1000.0)) + "k" : juce::String(juce::roundToInt(f)),
                   juce::Rectangle<float>(xOf(f) - 20.0f, plot.getBottom() + 2.0f, 40.0f, 12.0f), juce::Justification::centred);
    }
    for (double db = lo; db <= hi + 0.01; db += 5.0) {
        g.setColour(t.outline.withAlpha(0.35f));
        g.drawHorizontalLine(juce::roundToInt(yOf(db)), plot.getX(), plot.getRight());
        g.setColour(t.textFaint);
        g.drawText(juce::String(juce::roundToInt(db)), juce::Rectangle<float>(r.getX() + 2.0f, yOf(db) - 6.0f, 28.0f, 12.0f),
                   juce::Justification::centredRight);
    }
    auto curve = [&](const std::array<double, 26>& v, juce::Colour c, bool dashed, float width) {
        juce::Path p;
        for (std::size_t i = 0; i < n; ++i) {
            const juce::Point<float> pt(xOf(hz[i]), yOf(v[i]));
            if (i == 0) p.startNewSubPath(pt);
            else p.lineTo(pt);
        }
        g.setColour(c);
        if (dashed) {
            juce::Path d;
            const float dl[] = {6.0f, 4.0f};
            juce::PathStrokeType(width).createDashedStroke(d, p, dl, 2);
            g.fillPath(d);
        } else {
            g.strokePath(p, juce::PathStrokeType(width, mode_ == SpecMode::Fft ? juce::PathStrokeType::curved : juce::PathStrokeType::mitered));
        }
        if (!dashed && mode_ != SpecMode::Fft)
            for (std::size_t i = 0; i < n; ++i) g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre({xOf(hz[i]), yOf(v[i])}));
    };
    curve(ref_, t.textDim, true, 1.8f);
    if (haveMeas_) curve(meas_, t.accentStrong, false, 2.2f);
    else {
        g.setColour(t.textFaint);
        g.setFont(fonts::body(13.0f));
        g.drawText("No measurement yet", plot, juce::Justification::centred);
    }
}

AnalysisPage::SpectrumCard::SpectrumCard()
    : Card("Spectrum", "Compares the tone of the masking sound with its target. Small differences are normal; large ones "
                       "mean the masking may not cover speech evenly.") {
    static const char* const names[3] = {"Octave", "1/3 Octave", "FFT"};
    for (std::size_t i = 0; i < 3; ++i) {
        auto& b = modes_[i];
        b.setButtonText(names[i]);
        b.getProperties().set("bf.segment", true);
        b.setRadioGroupId(7311);
        b.setClickingTogglesState(true);
        noFocus(b);
        b.setToggleState(i == 1, juce::dontSendNotification);
        b.onClick = [this, i] { setMode(static_cast<SpecMode>(i)); };
        addAndMakeVisible(b);
    }
    modes_[0].setTooltip("Seven octave bands, 125 Hz to 8 kHz.");
    modes_[1].setTooltip("Finer detail: 19 one-third-octave bands.");
    modes_[2].setTooltip("Smooth curve through the measured bands.");
    const auto& t = Theme::get();
    styleLabel(errorLabel, fonts::body(14.5f), t.text, juce::Justification::centredLeft);
    styleLabel(deviationLabel, fonts::body(14.5f), t.text, juce::Justification::centredLeft);
    errorLabel.setText("Average target error  " + dash(), juce::dontSendNotification);
    deviationLabel.setText("Largest deviation  " + dash(), juce::dontSendNotification);
    addAndMakeVisible(errorLabel);
    addAndMakeVisible(deviationLabel);
    addAndMakeVisible(view);
    refresh();
}

void AnalysisPage::SpectrumCard::setMode(SpecMode m) {
    mode_ = m;
    for (std::size_t i = 0; i < 3; ++i) modes_[i].setToggleState(static_cast<std::size_t>(m) == i, juce::dontSendNotification);
    refresh();
}

void AnalysisPage::SpectrumCard::setStats(std::shared_ptr<const MaskStatistics> s) {
    stats_ = std::move(s);
    refresh();
}

void AnalysisPage::SpectrumCard::refresh() {
    std::array<double, 26> ref{}, aligned{};
    bool have = false;
    SpectrumMetrics m;
    if (stats_ && stats_->haveSpectrumView) {
        m = computeSpectrumMetrics(stats_->referenceDb, stats_->measuredDb, mode_, &aligned);
        have = m.valid;
        if (mode_ == SpecMode::Octave) {
            const OctaveArray ro = octaveFromThirdOct(stats_->referenceDb);
            for (std::size_t k = 0; k < kNumOctaveBands; ++k) ref[k] = ro[k];
        } else {
            for (std::size_t b = kFirstBand, i = 0; b <= kLastBand; ++b, ++i) ref[i] = stats_->referenceDb[b];
        }
    } else if (stats_ && stats_->referenceDb[10] != 0.0) {
        // Target only (no measurement yet).
        if (mode_ == SpecMode::Octave) {
            const OctaveArray ro = octaveFromThirdOct(stats_->referenceDb);
            for (std::size_t k = 0; k < kNumOctaveBands; ++k) ref[k] = ro[k];
        } else {
            for (std::size_t b = kFirstBand, i = 0; b <= kLastBand; ++b, ++i) ref[i] = stats_->referenceDb[b];
        }
    }
    view.set(mode_, ref, aligned, have);
    if (have) {
        errorLabel.setText("Average target error  " + juce::String(m.avgErrorDb, 1) + " dB", juce::dontSendNotification);
        deviationLabel.setText("Largest deviation  " + juce::String(m.maxDevDb, 1) + " dB at " + hzText(m.maxDevHz),
                               juce::dontSendNotification);
    } else {
        errorLabel.setText("Average target error  " + dash(), juce::dontSendNotification);
        deviationLabel.setText("Largest deviation  " + dash(), juce::dontSendNotification);
    }
}

void AnalysisPage::SpectrumCard::resized() {
    Card::resized();
    auto r = contentArea();
    auto top = r.removeFromTop(30);
    const int w = juce::jmin(110, top.getWidth() / 3);
    for (auto& b : modes_) {
        b.setBounds(top.removeFromLeft(w));
        top.removeFromLeft(4);
    }
    r.removeFromTop(8);
    auto metrics = r.removeFromBottom(48);
    errorLabel.setBounds(metrics.removeFromTop(24));
    deviationLabel.setBounds(metrics);
    r.removeFromBottom(6);
    view.setBounds(r);
}

// ================================================================================== Modulation

void AnalysisPage::DensityMeter::set(int level, bool have) {
    level_ = juce::jlimit(0, 2, level);
    have_ = have;
    repaint();
}

void AnalysisPage::DensityMeter::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    auto r = getLocalBounds().toFloat();
    const float third = r.getWidth() / 3.0f;
    static const char* const names[3] = {"Low", "Medium", "High"};
    g.setFont(fonts::body(15.0f));
    for (int i = 0; i < 3; ++i) {
        auto cell = juce::Rectangle<float>(r.getX() + third * static_cast<float>(i), r.getY(), third, 22.0f);
        g.setColour(have_ && i == level_ ? t.text : t.textDim);
        g.drawText(names[i], cell, juce::Justification::centred);
    }
    const float y = r.getY() + 38.0f;
    g.setColour(t.outline);
    g.fillRoundedRectangle(r.getX() + third * 0.5f, y - 2.0f, third * 2.0f, 4.0f, 2.0f);
    if (have_) {
        g.setColour(t.accentStrong);
        g.fillEllipse(juce::Rectangle<float>(16.0f, 16.0f).withCentre({r.getX() + third * (static_cast<float>(level_) + 0.5f), y}));
    }
}

void AnalysisPage::ModSpectrumView::set(const ModBandArray& m, bool have) {
    m_ = m;
    have_ = have;
    repaint();
}

void AnalysisPage::ModSpectrumView::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    auto r = getLocalBounds().toFloat();
    g.setColour(t.panelAlt);
    g.fillRoundedRectangle(r, 6.0f);
    auto plot = r.reduced(10.0f, 10.0f).withTrimmedBottom(14.0f);
    if (!have_) {
        g.setColour(t.textFaint);
        g.setFont(fonts::body(13.0f));
        g.drawText("The modulation spectrum needs about a minute of masking.", r, juce::Justification::centred);
        return;
    }
    double mx = 0.1;
    for (const double v : m_) mx = std::max(mx, v);
    const float slot = plot.getWidth() / static_cast<float>(kNumModBands);
    const auto& fc = modulationBandCentresHz();
    g.setFont(fonts::body(10.0f));
    for (std::size_t k = 0; k < kNumModBands; ++k) {
        const float h = static_cast<float>(juce::jlimit(0.0, 1.0, m_[k] / mx)) * plot.getHeight();
        g.setColour(t.accent.withAlpha(0.85f));
        g.fillRect(juce::Rectangle<float>(plot.getX() + static_cast<float>(k) * slot + 2.0f, plot.getBottom() - h, slot - 4.0f, h));
        if (k % 3 == 0) {
            g.setColour(t.textFaint);
            g.drawText(juce::String(fc[k], fc[k] < 10.0 ? 1 : 0), juce::Rectangle<float>(plot.getX() + static_cast<float>(k) * slot - 8.0f, plot.getBottom() + 1.0f, slot + 16.0f, 12.0f),
                       juce::Justification::centred);
        }
    }
}

AnalysisPage::ModulationCard::ModulationCard()
    : Card("Modulation", "How much the masking level naturally rises and falls over time. Low sounds steadier; High sounds "
                         "more like a lively conversation.") {
    const auto& t = Theme::get();
    addAndMakeVisible(meter);
    styleLabel(caption, fonts::heading(12.5f), t.textDim, juce::Justification::centredLeft);
    caption.setText("TEMPORAL DENSITY", juce::dontSendNotification);
    addAndMakeVisible(caption);
    noFocus(detailsButton);
    detailsButton.setClickingTogglesState(false);
    detailsButton.setTooltip("Technical view: how fast the masking level changes (0.5 to 16 Hz).");
    detailsButton.onClick = [this] { setDetails(!details_); };
    addAndMakeVisible(detailsButton);
    addChildComponent(spectrum);
    styleLabel(detailText, fonts::body(13.0f), t.textDim, juce::Justification::topLeft);
    addChildComponent(detailText);
}

void AnalysisPage::ModulationCard::setDetails(bool on) {
    details_ = on;
    detailsButton.setButtonText(on ? "Hide details" : "Show details");
    spectrum.setVisible(on);
    detailText.setVisible(on);
    resized();
    if (onHeightChanged) onHeightChanged();
}

int AnalysisPage::ModulationCard::preferredHeight() const { return details_ ? 396 : 176; }

void AnalysisPage::ModulationCard::setStats(const MaskStatistics* s) {
    meter.set(s ? static_cast<int>(s->temporalDensity) : 1, s && s->samples > 0);
    spectrum.set(s ? s->modulation : ModBandArray{}, s && s->haveModulation);
    if (s && s->samples > 0)
        detailText.setText("Modulation Spectrum, 0.5" + juce::String::fromUTF8("\xe2\x80\x93") + "16 Hz\nDepth " +
                               juce::String(s->modulationDepth10s, 2) + "   Level swing (L10" + juce::String::fromUTF8("\xe2\x88\x92") + "L90) " +
                               juce::String(s->crest60sDb, 1) + " dB",
                           juce::dontSendNotification);
    else detailText.setText("Modulation Spectrum, 0.5" + juce::String::fromUTF8("\xe2\x80\x93") + "16 Hz", juce::dontSendNotification);
}

void AnalysisPage::ModulationCard::resized() {
    Card::resized();
    auto r = contentArea();
    caption.setBounds(r.removeFromTop(20));
    meter.setBounds(r.removeFromTop(56));
    r.removeFromTop(6);
    detailsButton.setBounds(r.removeFromTop(30).removeFromLeft(130));
    if (details_) {
        r.removeFromTop(8);
        detailText.setBounds(r.removeFromTop(40));
        spectrum.setBounds(r.removeFromTop(150));
    }
}

// ================================================================================== Spatial

void AnalysisPage::SpeakerMap::set(std::vector<double> pct, bool have) {
    pct_ = std::move(pct);
    have_ = have;
    repaint();
}

void AnalysisPage::SpeakerMap::paint(juce::Graphics& g) {
    const auto& t = Theme::get();
    auto r = getLocalBounds().toFloat();
    const int n = static_cast<int>(pct_.size());
    if (n == 0) {
        g.setColour(t.textFaint);
        g.setFont(fonts::body(13.0f));
        g.drawText("Start masking to see speaker activity.", r, juce::Justification::centred);
        return;
    }
    const auto c = r.getCentre();
    const float rx = r.getWidth() * 0.5f - 44.0f, ry = r.getHeight() * 0.5f - 30.0f;
    for (int i = 0; i < n; ++i) {
        // 2 speakers: left / right. Otherwise clockwise from the top (4: top, right, bottom, left).
        float a = juce::MathConstants<float>::twoPi * static_cast<float>(i) / static_cast<float>(n);
        juce::Point<float> p;
        if (n == 2) p = {c.x + (i == 0 ? -rx : rx), c.y};
        else p = {c.x + rx * std::sin(a), c.y - ry * std::cos(a)};
        auto box = juce::Rectangle<float>(40.0f, 22.0f).withCentre({p.x, p.y - 12.0f});
        const double v = pct_[static_cast<std::size_t>(i)];
        g.setColour(t.panelAlt);
        g.fillRoundedRectangle(box, 5.0f);
        g.setColour(have_ ? t.accent : t.outline);
        g.drawRoundedRectangle(box, 5.0f, 1.5f);
        g.setColour(t.text);
        g.setFont(fonts::heading(13.0f));
        g.drawText("[" + juce::String(i + 1) + "]", box, juce::Justification::centred);
        g.setColour(have_ ? t.accentStrong : t.textFaint);
        g.setFont(fonts::title(16.0f));
        g.drawText(have_ ? juce::String(juce::roundToInt(v)) + "%" : dash(),
                   juce::Rectangle<float>(60.0f, 20.0f).withCentre({p.x, p.y + 12.0f}), juce::Justification::centred);
    }
}

AnalysisPage::SpatialCard::SpatialCard()
    : Card("Spatial", "Shows how evenly each speaker is used. Speakers that sound too alike (high correlation) make the "
                      "masking easier to locate.") {
    const auto& t = Theme::get();
    addAndMakeVisible(map);
    styleLabel(correlationName, fonts::body(14.5f), t.textDim, juce::Justification::centredLeft);
    correlationName.setText("Channel correlation", juce::dontSendNotification);
    styleLabel(correlationValue, fonts::title(17.0f), t.text, juce::Justification::centredRight);
    correlationValue.setText(dash(), juce::dontSendNotification);
    addAndMakeVisible(correlationName);
    addAndMakeVisible(correlationValue);
    noFocus(detailsButton);
    detailsButton.setClickingTogglesState(false);
    detailsButton.setTooltip("Shows the exact correlation between neighbouring speakers.");
    detailsButton.onClick = [this] { setDetails(!details_); };
    addAndMakeVisible(detailsButton);
    styleLabel(detailText, fonts::body(13.0f), t.textDim, juce::Justification::topLeft);
    addChildComponent(detailText);
}

void AnalysisPage::SpatialCard::setDetails(bool on) {
    details_ = on;
    detailsButton.setButtonText(on ? "Hide details" : "Show details");
    detailText.setVisible(on);
    resized();
    if (onHeightChanged) onHeightChanged();
}

int AnalysisPage::SpatialCard::preferredHeight() const { return details_ ? 380 : 330; }

void AnalysisPage::SpatialCard::setStats(const MaskStatistics* s) {
    const auto& t = Theme::get();
    std::vector<double> pct;
    const bool have = s && s->samples > 0 && !s->outputRmsChDb.empty();
    if (have) {
        double mx = -1e9;
        for (const double v : s->outputRmsChDb) mx = std::max(mx, v);
        for (const double v : s->outputRmsChDb) pct.push_back(v < -150.0 ? 0.0 : 100.0 * std::pow(10.0, (v - mx) / 10.0));
    }
    map.set(std::move(pct), have);
    if (s && s->haveCorrelation && !s->adjacentCorrelation.empty()) {
        double mx = 0.0;
        juce::String lines = "Neighbouring speakers (correlation coefficient):\n";
        for (std::size_t i = 0; i < s->adjacentCorrelation.size(); ++i) {
            mx = std::max(mx, std::fabs(s->adjacentCorrelation[i]));
            lines << "[" << juce::String(static_cast<int>(i) + 1) << "]-[" << juce::String(static_cast<int>(i) + 2) << "]  "
                  << juce::String(s->adjacentCorrelation[i], 2) << "   ";
        }
        const bool good = mx <= 0.35;
        correlationValue.setText(good ? "Low  (Good)" : "High  (speakers sound alike)", juce::dontSendNotification);
        correlationValue.setColour(juce::Label::textColourId, good ? t.ok : t.warn);
        detailText.setText(lines, juce::dontSendNotification);
    } else {
        correlationValue.setText(s && s->samples > 0 && s->numChannels < 2 ? "Single speaker" : dash(), juce::dontSendNotification);
        correlationValue.setColour(juce::Label::textColourId, t.text);
        detailText.setText("No correlation data yet.", juce::dontSendNotification);
    }
}

void AnalysisPage::SpatialCard::resized() {
    Card::resized();
    auto r = contentArea();
    auto bottom = r.removeFromBottom(details_ ? 86 : 30);
    auto row = bottom.removeFromBottom(30);
    detailsButton.setBounds(row.removeFromLeft(130));
    if (details_) detailText.setBounds(bottom.reduced(0, 4));
    else detailText.setBounds({});
    auto corr = r.removeFromBottom(26);
    correlationName.setBounds(corr.removeFromLeft(corr.getWidth() / 2));
    correlationValue.setBounds(corr);
    r.removeFromBottom(6);
    map.setBounds(r);
}

// ================================================================================== Page

AnalysisPage::AnalysisPage(PageContext& c) : Page(c) {
    for (juce::Component* k : std::initializer_list<juce::Component*>{&voice_, &spectrum_, &modulation_, &spatial_}) addAndMakeVisible(k);
    modulation_.onHeightChanged = [this] { relayout(); };
    spatial_.onHeightChanged = [this] { relayout(); };
    applyStats(nullptr);
    refreshStatus(ctx.bridge.status());
}

int AnalysisPage::layoutPage(int width) {
    auto col = column(width, 1040, 24);
    const int gap = 16;
    const int hVoice = 340, hSpec = 340;
    const int hMod = modulation_.preferredHeight(), hSpat = spatial_.preferredHeight();
    int y = col.getY();
    if (col.getWidth() >= 720) {
        const int w = (col.getWidth() - gap) / 2;
        const int h1 = juce::jmax(hVoice, hSpec);
        voice_.setBounds(col.getX(), y, w, h1);
        spectrum_.setBounds(col.getX() + w + gap, y, col.getWidth() - w - gap, h1);
        y += h1 + gap;
        const int h2 = juce::jmax(hMod, hSpat);
        modulation_.setBounds(col.getX(), y, w, h2);
        spatial_.setBounds(col.getX() + w + gap, y, col.getWidth() - w - gap, h2);
        y += h2;
    } else {
        for (const auto& p : std::initializer_list<std::pair<juce::Component*, int>>{
                 {&voice_, hVoice}, {&spectrum_, hSpec}, {&modulation_, hMod}, {&spatial_, hSpat}}) {
            p.first->setBounds(col.getX(), y, col.getWidth(), p.second);
            y += p.second + gap;
        }
        y -= gap;
    }
    return y + 24;
}

void AnalysisPage::refreshStatus(const EngineStatus& s) {
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastUpdateMs_ < 66.0) return;  // <= 15 Hz
    if (!s.stats) {
        if (hadStats_) applyStats(nullptr, false);
        return;
    }
    lastUpdateMs_ = now;
    applyStats(s.stats, false);
}

void AnalysisPage::applyStats(std::shared_ptr<const MaskStatistics> s, bool force) {
    juce::ignoreUnused(force);
    ++updates_;
    hadStats_ = s != nullptr;
    const MaskStatistics* p = s.get();
    // Per-voice strip: one column every 200 ms.
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (p && p->babbleActive) {
        if (force || now - lastColumnMs_ >= 200.0) {
            lastColumnMs_ = now;
            std::uint64_t mask = 0;
            for (std::size_t i = 0; i < p->slotActive.size() && i < 64; ++i)
                if (p->slotActive[i]) mask |= (std::uint64_t{1} << i);
            voice_.strip.push(mask);
        }
    } else if (!p) {
        voice_.strip.clear();
    }
    voice_.strip.setVisible(p && p->babbleActive);
    voice_.setStats(p);
    spectrum_.setStats(s);
    modulation_.setStats(p);
    spatial_.setStats(p);
}

}  // namespace bf::gui
