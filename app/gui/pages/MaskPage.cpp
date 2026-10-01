#include "pages/MaskPage.h"

#include <cmath>

#include "LookAndFeel.h"
#include "core/spectrum/SpectrumTarget.h"

namespace bf::gui {

static PageRegistrar maskPageRegistration({"mask", "MASK", 20, PageInfo::Sidebar,
                                           [](PageContext& c) { return std::make_unique<MaskPage>(c); }});

namespace {

std::vector<juce::String> namesOf(const std::vector<Choice>& v) {
    std::vector<juce::String> out;
    for (const auto& c : v) out.push_back(c.name);
    return out;
}

const char* const kSpectrumIds[] = {"speech_matched", "ltass_universal", "slope_-5", "slope_-7", "slope_-9", "custom"};
const char* const kVariety[] = {"low", "balanced", "high"};

}  // namespace

MaskPage::MaskPage(PageContext& c)
    : Page(c), styles_(maskStyleChoices(c.state.data())), style_("Masking Style", namesOf(styles_)) {
    const auto& t = Theme::get();

    // ---- Simple ---------------------------------------------------------------------------
    for (int i = 0; i < style_.size(); ++i) style_.setOptionTooltip(i, styles_[static_cast<std::size_t>(i)].description);
    style_.onSelect = [this](int i) { state().setStrategy(styles_[static_cast<std::size_t>(i)].id); };
    addAndMakeVisible(style_);
    styleDesc_.setFont(fonts::body(14.5f));
    styleDesc_.setColour(juce::Label::textColourId, t.textDim);
    styleDesc_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(styleDesc_);

    voiceAmount_.setTooltip("How many voices talk at the same time.");
    voiceAmount_.valueText = [this](double) {
        if (!advanced() || !state().plan().ok || !state().plan().plan.babbleEnabled) return juce::String();
        return juce::String::fromUTF8("\xe2\x89\x88 ") + juce::String(state().plan().plan.talkers.targetMean(), 1) + " talkers";
    };
    voiceAmount_.onGestureStart = [this] { state().beginGesture("Voice Amount"); };
    voiceAmount_.onGestureEnd = [this] { state().endGesture(); };
    voiceAmount_.onChange = [this](double v) { state().setVoiceAmount(v); };

    variety_.setRange(0.0, 2.0, 1.0);
    variety_.setTooltip("Controls how different the voices in the babble sound from one another.");
    variety_.valueText = [](double v) {
        static const char* const n[] = {"Low", "Medium", "High"};
        return juce::String(n[juce::jlimit(0, 2, juce::roundToInt(v))]);
    };
    variety_.onChange = [this](double v) { state().setVoiceVariety(kVariety[juce::jlimit(0, 2, juce::roundToInt(v))]); };

    cvr_.setTooltip("Reduces the chance that a single phrase stands out and becomes understandable.");
    cvr_.onGestureStart = [this] { state().beginGesture("Clear Voice Reduction"); };
    cvr_.onGestureEnd = [this] { state().endGesture(); };
    cvr_.onChange = [this](double v) { state().setClearVoiceReduction(v); };

    mix_.setTooltip("Left: more steady speech-shaped masking. Right: more voices.");
    mix_.valueText = [this](double v) {
        return advanced() ? juce::String(juce::roundToInt(v * 100.0)) + "% voices" : juce::String();
    };
    mix_.onGestureStart = [this] { state().beginGesture("Mask Mix"); };
    mix_.onGestureEnd = [this] { state().endGesture(); };
    mix_.onChange = [this](double v) { state().setBabbleFraction(v); };
    for (auto* m : {&voiceAmount_, &variety_, &cvr_, &mix_}) addAndMakeVisible(*m);

    // ---- Advanced -------------------------------------------------------------------------
    addAndMakeVisible(advancedCaption_);
    buildTalkerPanel();
    buildTimingPanel();
    buildStationaryPanel();
    buildSpectrumPanel();
    for (auto* p : {&talkerPanel_, &timingPanel_, &stationaryPanel_, &spectrumPanel_}) {
        p->onToggle = [this] { relayout(); };
        addAndMakeVisible(*p);
    }
    refreshFromState();
}

MaskPage::~MaskPage() {
    for (auto* p : {&talkerPanel_, &timingPanel_, &stationaryPanel_, &spectrumPanel_}) p->onToggle = nullptr;
}

void MaskPage::wireRow(ParamRow& r, const juce::String& name, std::function<void(double)> set) {
    r.onGestureStart = [this, name] { state().beginGesture(name); };
    r.onGestureEnd = [this] { state().endGesture(); };
    r.onChange = std::move(set);
}

void MaskPage::buildTalkerPanel() {
    wireRow(pool_, "Talker Pool", [this](double v) { state().setTalkerValue(TalkerField::Pool, v); });
    wireRow(avg_, "Average Active", [this](double v) { state().setTalkerValue(TalkerField::Average, v); });
    wireRow(min_, "Minimum Active", [this](double v) { state().setTalkerValue(TalkerField::Minimum, v); });
    wireRow(max_, "Maximum Active", [this](double v) { state().setTalkerValue(TalkerField::Maximum, v); });
    pool_.setTooltip("How many different recorded voices the babble draws from.");
    avg_.setTooltip("Average number of voices talking at the same time.");
    min_.setTooltip("The fewest voices that talk at once. Always at most the average.");
    max_.setTooltip("The most voices that talk at once. Always at least the average.");
    talkerInfo_.setFont(fonts::body(14.0f));
    talkerInfo_.setColour(juce::Label::textColourId, Theme::get().textDim);
    for (auto* r : {&pool_, &avg_, &min_, &max_}) talkerBody_.add(*r, [](int) { return 30; }, 4);
    talkerBody_.add(talkerInfo_, [](int) { return 22; }, 0);
    talkerPanel_.setContent(&talkerBody_, [this](int w) { return talkerBody_.preferredHeight(w); });
}

void MaskPage::buildTimingPanel() {
    wireRow(gap_, "Maximum internal gap", [this](double v) { state().setMaxInternalGapMs(v); });
    wireRow(segMin_, "Minimum segment length", [this](double v) { state().setSegmentMinS(v); });
    wireRow(segMax_, "Maximum segment length", [this](double v) { state().setSegmentMaxS(v); });
    wireRow(gainVar_, "Talker gain variation", [this](double v) { state().setGainVariationDb(v); });
    wireRow(fade_, "Fade time", [this](double v) { state().setFadeMs(v); });
    gap_.setTooltip("The longest pause inside one voice's speech before it counts as a gap.");
    segMin_.setTooltip("Shortest stretch of speech taken from one recording.");
    segMax_.setTooltip("Longest stretch of speech taken from one recording.");
    gainVar_.setTooltip("How much the loudness of individual voices varies.");
    fade_.setTooltip("How smoothly voices start and stop.");
    for (auto* r : {&gap_, &segMin_, &segMax_, &gainVar_, &fade_}) timingBody_.add(*r, [](int) { return 30; }, 4);
    timingPanel_.setContent(&timingBody_, [this](int w) { return timingBody_.preferredHeight(w); });
}

void MaskPage::buildStationaryPanel() {
    noFocus(stationaryOn_);
    stationaryOn_.setTooltip("Steady speech-shaped noise underneath the voices.");
    stationaryOn_.onClick = [this] { state().setStationaryEnabled(stationaryOn_.getToggleState()); };
    wireRow(energy_, "Noise contribution", [this](double v) { state().setBabbleFraction(1.0 - v / 100.0); });
    energy_.setTooltip("Share of the masking energy that comes from steady noise.");
    stationarySpectrum_.setOptionEnabled(0, false, "Needs a speech-matched profile for the voice library (not available).");
    stationarySpectrum_.setOptionTooltip(1, "Average spectrum of speech (Universal LTASS).");
    stationarySpectrum_.setOptionTooltip(2, "Falls by 5 dB per octave; quieter high frequencies.");
    stationarySpectrum_.setOptionTooltip(3, "Your own curve (Spectrum panel).");
    stationarySpectrum_.onSelect = [this](int i) {
        static const char* const ids[] = {"speech_matched", "ltass_universal", "slope_-5", "custom"};
        state().setSpectrumTarget(ids[i]);
    };
    seed_.setOptionTooltip(0, "A new noise pattern every session.");
    seed_.setOptionTooltip(1, "The same noise pattern every time (reproducible).");
    seed_.onSelect = [this](int i) { state().setSeedMode(i == 0 ? "session" : "fixed"); };
    stationaryBody_.add(stationaryOn_, [](int) { return 28; }, 4);
    stationaryBody_.add(energy_, [](int) { return 30; }, 6);
    stationaryBody_.add(stationarySpectrum_, [this](int) { return stationarySpectrum_.preferredHeight(); }, 6);
    stationaryBody_.add(seed_, [this](int) { return seed_.preferredHeight(); }, 0);
    stationaryPanel_.setContent(&stationaryBody_, [this](int w) { return stationaryBody_.preferredHeight(w); });
}

void MaskPage::buildSpectrumPanel() {
    spectrumMode_.setOptionEnabled(0, false, "Needs a speech-matched profile for the voice library (not available).");
    spectrumMode_.setOptionTooltip(1, "Average long-term spectrum of speech. Recommended.");
    for (int i = 2; i <= 4; ++i) spectrumMode_.setOptionTooltip(i, "Privacy curve: high frequencies fall off with this slope.");
    spectrumMode_.setOptionTooltip(5, "Shape the spectrum with a 7-band equalizer.");
    spectrumMode_.onSelect = [this](int i) { state().setSpectrumTarget(kSpectrumIds[i]); };

    for (int b = 0; b < 7; ++b) {
        auto& s = eq_[static_cast<std::size_t>(b)];
        s.setSliderStyle(juce::Slider::LinearVertical);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 22);
        s.setRange(-kEqDefaultRangeDb, kEqDefaultRangeDb, 0.5);
        s.setTextValueSuffix(" dB");
        s.setWantsKeyboardFocus(false);
        s.setDoubleClickReturnValue(true, 0.0);
        s.setTooltip("Raise or lower this frequency band of the masking sound.");
        s.onDragStart = [this, b] { state().beginGesture("Custom spectrum " + eqBandLabel(static_cast<std::size_t>(b))); };
        s.onDragEnd = [this] { state().endGesture(); };
        s.onValueChange = [this, b] {
            if (!eqSilent_) state().setCustomEqDb(b, eq_[static_cast<std::size_t>(b)].getValue());
        };
        auto& l = eqLabels_[static_cast<std::size_t>(b)];
        l.setText(eqBandLabel(static_cast<std::size_t>(b)), juce::dontSendNotification);
        l.setJustificationType(juce::Justification::centred);
        l.setFont(fonts::body(13.0f));
        l.setColour(juce::Label::textColourId, Theme::get().textDim);
        eqBox_.addAndMakeVisible(s);
        eqBox_.addAndMakeVisible(l);
    }
    eqBox_.layout = [this] {
        const int w = eqBox_.getWidth() / 7, h = eqBox_.getHeight();
        for (int b = 0; b < 7; ++b) {
            eq_[static_cast<std::size_t>(b)].setBounds(b * w, 0, w, h - 18);
            eqLabels_[static_cast<std::size_t>(b)].setBounds(b * w, h - 18, w, 18);
        }
    };
    noFocus(eqWide_);
    eqWide_.setTooltip(juce::String::fromUTF8("The recommended range is \xc2\xb1" "6 dB. Larger corrections can sound unnatural."));
    eqWide_.onClick = [this] {
        const double r = eqWide_.getToggleState() ? kEqMaxDb : kEqDefaultRangeDb;
        eqSilent_ = true;
        for (auto& s : eq_) s.setRange(-r, r, 0.5);
        eqSilent_ = false;
        refreshFromState();
    };
    noFocus(correction_);
    correction_.setTooltip("Gradually corrects long-term spectral drift without changing the natural short-term movement of speech.");
    correction_.onClick = [this] { state().setCorrectionEnabled(correction_.getToggleState()); };
    speed_.setOptionTooltip(0, "Very gradual correction.");
    speed_.setOptionTooltip(2, "Faster correction of drift.");
    speed_.onSelect = [this](int i) {
        static const char* const ids[] = {"slow", "normal", "fast"};
        state().setCorrectionSpeed(ids[i]);
    };
    spectrumBody_.add(spectrumMode_, [this](int) { return spectrumMode_.preferredHeight(); }, 8);
    spectrumBody_.add(graph_, [](int) { return 150; }, 8);
    spectrumBody_.add(eqBox_, [](int) { return 170; }, 4);
    spectrumBody_.add(eqWide_, [](int) { return 26; }, 8);
    spectrumBody_.add(correction_, [](int) { return 28; }, 4);
    spectrumBody_.add(speed_, [this](int) { return speed_.preferredHeight(); }, 0);
    spectrumPanel_.setContent(&spectrumBody_, [this](int w) { return spectrumBody_.preferredHeight(w); });
}

int MaskPage::spectrumIndex(const std::string& target) {
    for (int i = 0; i < 6; ++i)
        if (target == kSpectrumIds[i]) return i;
    return 1;
}

void MaskPage::refreshSpectrumGraph() {
    const auto& p = state().plan();
    if (!p.ok) return;
    const auto db = p.plan.target.effectiveThirdOctDb();
    const auto& hz = thirdOctNominalHz();
    std::vector<std::pair<double, double>> curve;
    for (std::size_t i = 0; i < kNumThirdOctBands; ++i) curve.emplace_back(hz[i], db[i]);
    graph_.setCurve(std::move(curve));
}

void MaskPage::refreshFromState() {
    AppState& s = state();
    const std::string strat = s.preset().strategy;
    const bool adv = advanced();
    int idx = -1;
    for (int i = 0; i < static_cast<int>(styles_.size()); ++i)
        if (styles_[static_cast<std::size_t>(i)].id == strat) idx = i;
    style_.setSelected(idx);
    styleDesc_.setText(maskTypeDescription(s.data(), strat), juce::dontSendNotification);

    const bool babble = strat != "speech_noise";
    voiceAmount_.setValueSilently(s.voiceAmount());
    voiceAmount_.setEnabled(babble);
    const std::string v = s.voiceVariety();
    variety_.setValueSilently(v == "low" ? 0.0 : (v == "high" ? 2.0 : 1.0));
    variety_.setEnabled(babble);
    cvr_.setValueSilently(s.clearVoiceReduction());
    cvr_.setEnabled(babble);
    mix_.setVisible(strat == "hybrid");  // GUI §13: only for Hybrid
    mix_.setValueSilently(s.babbleFraction());

    for (auto* c : std::initializer_list<juce::Component*>{&advancedCaption_, &talkerPanel_, &timingPanel_,
                                                           &stationaryPanel_, &spectrumPanel_})
        c->setVisible(adv);

    // Talker engine.
    const TalkerCounts tc = s.talkerCounts();
    pool_.setValueSilently(tc.pool);
    avg_.setValueSilently(tc.average);
    min_.setValueSilently(tc.minimum);
    max_.setValueSilently(tc.maximum);
    for (auto* r : {&pool_, &avg_, &min_, &max_}) r->setEnabled(babble);
    const auto& pl = s.plan();
    juce::String info = "Average simultaneous talkers: ";
    info << (pl.ok && pl.plan.babbleEnabled ? juce::String(pl.plan.talkers.targetMean(), 1) : juce::String("-"));
    if (strat == "multi_voice") info << juce::String::fromUTF8("  \xc2\xb7  Multi-Voice uses a fixed number of voices");
    talkerInfo_.setText(info, juce::dontSendNotification);

    // Timing.
    const auto tm = s.timing();
    gap_.setValueSilently(tm.maxInternalGapMs);
    segMin_.setValueSilently(tm.segmentMinS);
    segMax_.setValueSilently(tm.segmentMaxS);
    gainVar_.setValueSilently(tm.gainVariationDb);
    fade_.setValueSilently(tm.fadeMs);
    for (auto* r : {&gap_, &segMin_, &segMax_, &gainVar_, &fade_}) r->setEnabled(babble);

    // Stationary (dependent controls grey out when disabled, GUI §29).
    const bool statOn = s.stationaryEnabled();
    stationaryOn_.setToggleState(statOn, juce::dontSendNotification);
    stationaryOn_.setEnabled(strat != "speech_noise");
    energy_.setValueSilently((1.0 - s.babbleFraction()) * 100.0);
    energy_.setEnabled(statOn && babble);
    const std::string target = s.spectrumTarget();
    int si = 1;
    if (target == "custom") si = 3;
    else if (target.rfind("slope_", 0) == 0) si = 2;
    stationarySpectrum_.setSelected(si);
    stationarySpectrum_.setEnabled(statOn);
    seed_.setSelected(s.seedMode() == "fixed" ? 1 : 0);
    seed_.setEnabled(statOn);

    // Spectrum.
    spectrumMode_.setSelected(spectrumIndex(target));
    const bool custom = target == "custom";
    const auto eq = s.customEqDb();
    bool beyond = false;
    for (double e : eq) beyond = beyond || std::abs(e) > kEqDefaultRangeDb + 1e-9;
    if (beyond && !eqWide_.getToggleState()) {
        eqWide_.setToggleState(true, juce::dontSendNotification);
        eqSilent_ = true;
        for (auto& sl : eq_) sl.setRange(-kEqMaxDb, kEqMaxDb, 0.5);
        eqSilent_ = false;
    }
    eqSilent_ = true;
    for (std::size_t b = 0; b < 7; ++b)
        if (!eq_[b].isMouseButtonDown()) eq_[b].setValue(eq[b], juce::dontSendNotification);
    eqSilent_ = false;
    eqBox_.setVisible(custom);
    eqWide_.setVisible(custom);
    correction_.setToggleState(s.correctionEnabled(), juce::dontSendNotification);
    const std::string sp = s.correctionSpeed();
    speed_.setSelected(sp == "slow" ? 0 : (sp == "fast" ? 2 : 1));
    speed_.setEnabled(s.correctionEnabled());
    refreshSpectrumGraph();
    relayout();
}

void MaskPage::refreshStatus(const EngineStatus& st) {
    if (!advanced() || !st.haveStats || st.voicesActive <= 0.0) return;
    const auto& pl = state().plan();
    juce::String info = "Average simultaneous talkers: " + juce::String(st.voicesActive, 1);
    if (pl.ok && pl.plan.babbleEnabled) info << "  (target " << juce::String(pl.plan.talkers.targetMean(), 1) << ")";
    talkerInfo_.setText(info, juce::dontSendNotification);
}

int MaskPage::layoutPage(int width) {
    auto col = column(width, 680, 20);
    int y = col.getY();
    const int x = col.getX(), w = col.getWidth();
    auto place = [&](juce::Component& c, int h, int gap = 10) {
        c.setBounds(x, y, w, h);
        y += h + gap;
    };
    place(style_, style_.preferredHeight(), 4);
    place(styleDesc_, 22, 14);
    place(voiceAmount_, voiceAmount_.preferredHeight(), 12);
    place(variety_, variety_.preferredHeight(), 12);
    place(cvr_, cvr_.preferredHeight(), 12);
    if (mix_.isVisible()) place(mix_, mix_.preferredHeight(), 12);
    if (advanced()) {
        y += 8;
        place(advancedCaption_, 20, 8);
        for (auto* p : {&talkerPanel_, &timingPanel_, &stationaryPanel_, &spectrumPanel_})
            place(*p, p->preferredHeight(w), 10);
    }
    return y + 20;
}

}  // namespace bf::gui
