#include "pages/AreaPage.h"

#include <algorithm>

#include "LookAndFeel.h"

namespace bf::gui {

static PageRegistrar areaPageRegistration({"area", "AREA", 30, PageInfo::Sidebar,
                                           [](PageContext& c) { return std::make_unique<AreaPage>(c); }});

using namespace outputs;

namespace {

void styleLabel(juce::Label& l, const juce::Font& f, juce::Colour c, juce::Justification j) {
    l.setFont(f);
    l.setColour(juce::Label::textColourId, c);
    l.setJustificationType(j);
    l.setInterceptsMouseClicks(false, false);
}

void styleSlider(juce::Slider& s, double lo, double hi, double step, const juce::String& suffix) {
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 78, 24);
    s.setRange(lo, hi, step);
    s.setTextValueSuffix(suffix);
    s.setWantsKeyboardFocus(false);
}

}  // namespace

// One speaker: "Speaker 3  Rear Left", Enabled, Level (dB), Delay (ms), Zone.
class AreaPage::SpeakerRow final : public juce::Component {
public:
    SpeakerRow(AreaPage& owner, int index) : owner_(owner), index_(index) {
        name_.setFont(fonts::body(14.0f));
        name_.setColour(juce::Label::textColourId, Theme::get().text);
        addAndMakeVisible(name_);
        enabled_.setButtonText("Enabled");
        noFocus(enabled_);
        enabled_.onClick = [this] { setChannelEnabled(owner_.state(), index_, enabled_.getToggleState()); };
        addAndMakeVisible(enabled_);
        styleSlider(level_, -40.0, 6.0, 0.1, " dB");
        styleSlider(delay_, 0.0, 100.0, 0.1, " ms");
        level_.setDoubleClickReturnValue(true, 0.0);
        delay_.setDoubleClickReturnValue(true, 0.0);
        level_.setTooltip("Level of this speaker relative to the others.");
        delay_.setTooltip("Optional delay for this speaker (0 to 100 ms).");
        level_.onDragStart = [this] { owner_.state().beginGesture("Speaker level"); };
        level_.onDragEnd = [this] { owner_.state().endGesture(); };
        delay_.onDragStart = [this] { owner_.state().beginGesture("Speaker delay"); };
        delay_.onDragEnd = [this] { owner_.state().endGesture(); };
        level_.onValueChange = [this] {
            if (!silent_) setChannelGainDb(owner_.state(), index_, level_.getValue());
        };
        delay_.onValueChange = [this] {
            if (!silent_) setChannelDelayMs(owner_.state(), index_, delay_.getValue());
        };
        addAndMakeVisible(level_);
        addAndMakeVisible(delay_);
        noFocus(zone_);
        zone_.setTooltip("Zone this speaker belongs to.");
        zone_.onChange = [this] {
            if (!silent_) setChannelZone(owner_.state(), index_, zone_.getSelectedId() - 1);
        };
        addChildComponent(zone_);
    }

    void update(const Preset& p, const OutputLayout& L) {
        silent_ = true;
        const OutputChannel* c = channelAt(p, index_);
        const OutputDef& d = L.outputs[static_cast<std::size_t>(index_)];
        juce::String label = c && !c->label.empty() ? juce::String::fromUTF8(c->label.c_str()) : juce::String::fromUTF8(d.label.c_str());
        name_.setText("Speaker " + juce::String(index_ + 1) + (label.isNotEmpty() ? "   " + label : juce::String()), juce::dontSendNotification);
        enabled_.setToggleState(c ? c->enabled : d.enabled, juce::dontSendNotification);
        if (!level_.isMouseButtonDown()) level_.setValue(c ? c->gainDb : 0.0, juce::dontSendNotification);
        if (!delay_.isMouseButtonDown()) delay_.setValue(c ? c->delayMs : 0.0, juce::dontSendNotification);
        const bool zoned = !p.outputs.zones.empty();
        if (zoned) {
            zone_.clear(juce::dontSendNotification);
            for (const auto& z : p.outputs.zones) zone_.addItem(zoneName(z.id), z.id + 1);
            zone_.setSelectedId((c ? c->zone : d.zone) + 1, juce::dontSendNotification);
        }
        zone_.setVisible(zoned);
        silent_ = false;
    }

    void resized() override {
        auto r = getLocalBounds();
        name_.setBounds(r.removeFromLeft(170));
        enabled_.setBounds(r.removeFromLeft(92));
        if (zone_.isVisible()) zone_.setBounds(r.removeFromRight(92).reduced(0, 3));
        const int w = r.getWidth() / 2;
        level_.setBounds(r.removeFromLeft(w).reduced(4, 0));
        delay_.setBounds(r.reduced(4, 0));
    }

    juce::ToggleButton enabled_;
    juce::Slider level_, delay_;

private:
    AreaPage& owner_;
    int index_;
    bool silent_ = false;
    juce::Label name_;
    juce::ComboBox zone_;
};

// One zone: name, Enable, Relative Level, Mix offset.
class AreaPage::ZoneRow final : public juce::Component {
public:
    ZoneRow(AreaPage& owner, int zoneId) : owner_(owner), id_(zoneId) {
        name_.setFont(fonts::heading(14.0f));
        name_.setColour(juce::Label::textColourId, Theme::get().text);
        name_.setText(zoneName(zoneId).toUpperCase(), juce::dontSendNotification);
        addAndMakeVisible(name_);
        enabled_.setButtonText("Enable");
        noFocus(enabled_);
        enabled_.onClick = [this] { setZoneEnabled(owner_.state(), id_, enabled_.getToggleState()); };
        addAndMakeVisible(enabled_);
        styleSlider(level_, -24.0, 6.0, 0.1, " dB");
        styleSlider(mix_, -50.0, 50.0, 1.0, " %");
        level_.setDoubleClickReturnValue(true, 0.0);
        mix_.setDoubleClickReturnValue(true, 0.0);
        level_.setTooltip("Relative level of this zone.");
        mix_.setTooltip("Shifts the noise / voices balance of this zone.");
        level_.onDragStart = [this] { owner_.state().beginGesture("Zone level"); };
        level_.onDragEnd = [this] { owner_.state().endGesture(); };
        mix_.onDragStart = [this] { owner_.state().beginGesture("Zone mix offset"); };
        mix_.onDragEnd = [this] { owner_.state().endGesture(); };
        level_.onValueChange = [this] {
            if (!silent_) setZoneLevelDb(owner_.state(), id_, level_.getValue());
        };
        mix_.onValueChange = [this] {
            if (!silent_) setZoneMixOffset(owner_.state(), id_, mix_.getValue() / 100.0);
        };
        addAndMakeVisible(level_);
        addAndMakeVisible(mix_);
        for (auto* l : {&levelCap_, &mixCap_}) {
            l->setFont(fonts::body(12.5f));
            l->setColour(juce::Label::textColourId, Theme::get().textDim);
            l->setInterceptsMouseClicks(false, false);
            addAndMakeVisible(l);
        }
        levelCap_.setText("Relative level", juce::dontSendNotification);
        mixCap_.setText("Mix offset", juce::dontSendNotification);
    }

    void update(const Preset& p) {
        silent_ = true;
        for (const auto& z : p.outputs.zones)
            if (z.id == id_) {
                enabled_.setToggleState(z.enabled, juce::dontSendNotification);
                if (!level_.isMouseButtonDown()) level_.setValue(z.levelDb, juce::dontSendNotification);
                if (!mix_.isMouseButtonDown()) mix_.setValue(z.babbleFractionOffset * 100.0, juce::dontSendNotification);
            }
        std::vector<int> chans;
        for (std::size_t i = 0; i < p.outputs.channels.size(); ++i)
            if (p.outputs.channels[i].zone == id_) chans.push_back(static_cast<int>(i) + 1);
        juce::String t = zoneName(id_).toUpperCase();
        if (!chans.empty()) {
            t << "   Ch " << chans.front();
            if (chans.size() > 1) t << juce::String::fromUTF8("\xe2\x80\x93") << chans.back();
        }
        name_.setText(t, juce::dontSendNotification);
        silent_ = false;
    }

    void resized() override {
        auto r = getLocalBounds();
        auto top = r.removeFromTop(26);
        name_.setBounds(top.removeFromLeft(220));
        enabled_.setBounds(top.removeFromLeft(100));
        const int w = r.getWidth() / 2;
        auto a = r.removeFromLeft(w).reduced(2, 0), b = r.reduced(2, 0);
        levelCap_.setBounds(a.removeFromTop(16));
        mixCap_.setBounds(b.removeFromTop(16));
        level_.setBounds(a);
        mix_.setBounds(b);
    }

    int id() const { return id_; }
    juce::ToggleButton enabled_;
    juce::Slider level_, mix_;

private:
    AreaPage& owner_;
    int id_;
    bool silent_ = false;
    juce::Label name_, levelCap_, mixCap_;
};

AreaPage::AreaPage(PageContext& c) : Page(c) {
    const auto& t = Theme::get();
    areas_ = areaChoices(state().data());

    area_.getProperties().set("bf.primary", true);
    noFocus(area_);
    for (int i = 0; i < static_cast<int>(areas_.size()); ++i) area_.addItem(areas_[static_cast<std::size_t>(i)].name, i + 1);
    area_.setTooltip("Choose the kind of space. Recommended masking settings are loaded automatically.");
    area_.onChange = [this] {
        const int i = area_.getSelectedItemIndex();
        if (i >= 0 && i < static_cast<int>(areas_.size())) state().setArea(areas_[static_cast<std::size_t>(i)].id);
    };
    styleLabel(areaDesc_, fonts::body(15.0f), t.textDim, juce::Justification::topLeft);
    styleLabel(areaWarning_, fonts::body(14.0f), t.warn, juce::Justification::topLeft);
    styleLabel(recommendation_, fonts::body(14.0f), t.warn, juce::Justification::topLeft);
    for (auto* l : {&areaCaption_, &mapCaption_}) addAndMakeVisible(*l);
    addAndMakeVisible(area_);
    for (auto* l : {&areaDesc_, &areaWarning_, &recommendation_}) addAndMakeVisible(*l);
    noFocus(dismiss_);
    dismiss_.onClick = [this] {
        dismissed_ = recommendation_.getText();
        refreshFromState();
        relayout();
    };
    addChildComponent(dismiss_);

    size_.setOptionTooltip(0, "Compact space: speakers close together, narrower spread.");
    size_.setOptionTooltip(2, "Wide space: voices spread across all speakers.");
    size_.onSelect = [this](int i) { setAreaSize(state(), static_cast<AreaSize>(i)); };
    addAndMakeVisible(size_);
    speakers_.onSelect = [this](int i) { setSpeakerSetup(state(), static_cast<SpeakerSetup>(i)); };
    outdoorSpeakers_.onSelect = [this](int i) { setOutdoorSpeakers(state(), i == 0 ? 2 : i == 1 ? 4 : i == 2 ? 6 : 8); };
    addAndMakeVisible(speakers_);
    addChildComponent(outdoorSpeakers_);
    addAndMakeVisible(map_);

    // Advanced.
    for (auto* l : {&spatialCaption_, &speakersListCaption_, &zonesCaption_}) addChildComponent(*l);
    spatial_.onSelect = [this](int i) {
        const auto mode = static_cast<SpatialMode>(i);
        setSpatialMode(state(), mode, juce::roundToInt(customCount_.slider.getValue()));
    };
    addChildComponent(spatial_);
    customCount_.setTooltip("Number of speakers in a custom ring (3 to 16).");
    customCount_.onChange = [this](double v) {
        if (spatialMode(state().preset()) == SpatialMode::Custom) setSpatialMode(state(), SpatialMode::Custom, juce::roundToInt(v));
    };
    addChildComponent(customCount_);
    spread_.setTooltip("How widely voices are distributed across the physical speakers.");
    spread_.valueText = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
    spread_.setRange(0.0, 1.0, 0.0);
    spread_.onGestureStart = [this] { state().beginGesture("Spatial spread"); };
    spread_.onGestureEnd = [this] { state().endGesture(); };
    spread_.onChange = [this](double v) { setSpread(state(), v); };
    addChildComponent(spread_);
    variation_.setOptionTooltip(1, "Medium or High is recommended for distributed installations.");
    variation_.onSelect = [this](int i) { setSpeakerVariation(state(), i == 0 ? "low" : i == 1 ? "medium" : "high"); };
    addChildComponent(variation_);
    styleLabel(variationTech_, fonts::body(12.5f), t.textFaint, juce::Justification::centredLeft);
    variationTech_.setText("Channel decorrelation", juce::dontSendNotification);
    addChildComponent(variationTech_);
    styleLabel(zonesHelp_, fonts::body(13.0f), t.textDim, juce::Justification::topLeft);
    zonesHelp_.setText("Zones group speakers for level and mix balance only (for example Zone A = channels 1-4, "
                       "Zone B = channels 5-8).", juce::dontSendNotification);
    addChildComponent(zonesHelp_);
    for (auto* b : {&addZone_, &removeZone_}) {
        noFocus(*b);
        addChildComponent(*b);
    }
    addZone_.onClick = [this] { addZone(state()); };
    removeZone_.onClick = [this] { removeZone(state()); };

    refreshFromState();
}

AreaPage::~AreaPage() = default;

bool AreaPage::outdoor() { return state().preset().area == "free_field"; }

juce::ToggleButton& AreaPage::speakerEnabled(int i) { return rows_[static_cast<std::size_t>(i)]->enabled_; }
juce::Slider& AreaPage::speakerLevel(int i) { return rows_[static_cast<std::size_t>(i)]->level_; }
juce::Slider& AreaPage::speakerDelay(int i) { return rows_[static_cast<std::size_t>(i)]->delay_; }
juce::ToggleButton& AreaPage::zoneEnabled(int i) { return zoneRows_[static_cast<std::size_t>(i)]->enabled_; }
juce::Slider& AreaPage::zoneLevel(int i) { return zoneRows_[static_cast<std::size_t>(i)]->level_; }
juce::Slider& AreaPage::zoneMix(int i) { return zoneRows_[static_cast<std::size_t>(i)]->mix_; }

void AreaPage::rebuildRows() {
    const Preset& p = state().preset();
    const OutputLayout L = currentLayout(p);
    if (static_cast<int>(rows_.size()) != L.size()) {
        rows_.clear();
        for (int i = 0; i < L.size(); ++i) {
            rows_.push_back(std::make_unique<SpeakerRow>(*this, i));
            addChildComponent(*rows_.back());
        }
    }
    for (int i = 0; i < L.size(); ++i) rows_[static_cast<std::size_t>(i)]->update(p, L);
    if (zoneRows_.size() != p.outputs.zones.size() ||
        !std::equal(zoneRows_.begin(), zoneRows_.end(), p.outputs.zones.begin(),
                    [](const std::unique_ptr<ZoneRow>& r, const OutputZone& z) { return r->id() == z.id; })) {
        zoneRows_.clear();
        for (const auto& z : p.outputs.zones) {
            zoneRows_.push_back(std::make_unique<ZoneRow>(*this, z.id));
            addChildComponent(*zoneRows_.back());
        }
    }
    for (auto& z : zoneRows_) z->update(p);
}

void AreaPage::refreshFromState() {
    const Preset& p = state().preset();
    const bool adv = advanced();
    for (int i = 0; i < static_cast<int>(areas_.size()); ++i)
        if (areas_[static_cast<std::size_t>(i)].id == p.area) area_.setSelectedItemIndex(i, juce::dontSendNotification);
    juce::String desc;
    for (const auto& a : areas_)
        if (a.id == p.area) desc = a.description;
    areaDesc_.setText(desc, juce::dontSendNotification);
    areaWarning_.setText(areaAdvisory(state().data(), p.area), juce::dontSendNotification);

    const bool out = outdoor();
    size_.setCaption(out ? "Coverage Area" : "Approximate Area Size");
    speakers_.setVisible(!out);
    outdoorSpeakers_.setVisible(out);
    size_.setSelected(static_cast<int>(areaSize(state())));
    {
        const int n = outputCount(p);
        speakers_.setSelected(n <= 2 ? 0 : n == 4 ? 1 : 2);
        const int o = outdoorSpeakers(state());
        outdoorSpeakers_.setSelected(o == 2 ? 0 : o == 4 ? 1 : o == 6 ? 2 : 3);
    }

    // Contextual recommendation: informational, never modal (GUI §56).
    const juce::String rec = recommendation(state());
    recommendation_.setText(rec == dismissed_ ? juce::String() : rec, juce::dontSendNotification);
    dismiss_.setVisible(recommendation_.getText().isNotEmpty());

    // Advanced controls.
    for (auto* c : std::initializer_list<juce::Component*>{&spatialCaption_, &speakersListCaption_, &zonesCaption_, &spatial_,
                                                           &spread_, &variation_, &variationTech_, &zonesHelp_, &addZone_})
        c->setVisible(adv);
    const SpatialMode mode = spatialMode(p);
    spatial_.setSelected(static_cast<int>(mode));
    customCount_.setVisible(adv && mode == SpatialMode::Custom);
    if (mode == SpatialMode::Custom) customCount_.setValueSilently(outputCount(p));
    spread_.setValueSilently(spread(state()));
    {
        const std::string v = speakerVariation(state());
        variation_.setSelected(v == "low" ? 0 : v == "high" ? 2 : 1);
    }
    rebuildRows();
    for (auto& r : rows_) r->setVisible(adv);
    for (auto& z : zoneRows_) z->setVisible(adv);
    removeZone_.setVisible(adv && !p.outputs.zones.empty());
    addZone_.setEnabled(static_cast<int>(p.outputs.zones.size()) < kMaxZones);
    addZone_.setButtonText(p.outputs.zones.empty() ? "Add zones (A / B)" : "Add zone");

    // Map.
    std::vector<int> zones;
    for (const auto& c : p.outputs.channels) zones.push_back(c.zone);
    const OutputLayout L = currentLayout(p);
    map_.setLayout(L, zones, !p.outputs.zones.empty());
    map_.setActivity(activity_);
    relayout();
}

void AreaPage::refreshStatus(const EngineStatus& s) {
    // Live per-output activity (RMS-fast of each output channel).
    std::vector<float> a;
    if (s.state == rt::EngineState::Running || s.state == rt::EngineState::Degraded)
        for (double db : s.out.rmsFastDb) a.push_back(meterFraction(db));
    if (a != activity_) {
        activity_ = a;
        map_.setActivity(activity_);
    }
    // Virtual talker dots (live updates <= 15 Hz; cleared at once when masking is not running).
    const bool live = s.state == rt::EngineState::Running || s.state == rt::EngineState::Degraded;
    if (!live) {
        if (map_.talkerDotCount() > 0) map_.setTalkers(nullptr);
        return;
    }
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastTalkerMs_ >= 1000.0 / 15.0 - 1.0) {
        lastTalkerMs_ = now;
        map_.setTalkers(s.stats.get());
    }
}

int AreaPage::layoutPage(int width) {
    const bool adv = advanced();
    auto col = column(width, adv ? 780 : 640, 20);
    int y = col.getY();
    const int x = col.getX(), w = col.getWidth();
    auto place = [&](juce::Component& c, int h, int gapAfter = 8) {
        c.setBounds(x, y, w, h);
        y += h + gapAfter;
    };
    auto hide = [](juce::Component& c) { c.setBounds(0, 0, 0, 0); };
    place(areaCaption_, 22, 4);
    place(area_, 48, 6);
    place(areaDesc_, 44, 4);
    if (areaWarning_.getText().isNotEmpty()) place(areaWarning_, 40, 6);
    else hide(areaWarning_);
    y += 8;
    place(size_, size_.preferredHeight(), 12);
    if (outdoor()) {
        place(outdoorSpeakers_, outdoorSpeakers_.preferredHeight(), 10);
        hide(speakers_);
    } else {
        place(speakers_, speakers_.preferredHeight(), 10);
        hide(outdoorSpeakers_);
    }
    if (recommendation_.getText().isNotEmpty()) {
        place(recommendation_, 40, 4);
        dismiss_.setBounds(x, y, 110, 30);
        y += 38;
    } else {
        hide(recommendation_);
        hide(dismiss_);
    }
    y += 6;
    place(mapCaption_, 20, 4);
    place(map_, juce::jlimit(220, 300, w * 2 / 5), 18);

    if (adv) {
        place(spatialCaption_, 24, 4);
        place(spatial_, spatial_.preferredHeight(), 4);
        if (customCount_.isVisible()) place(customCount_, 30, 6);
        else hide(customCount_);
        y += 6;
        place(spread_, spread_.preferredHeight(), 12);
        place(variation_, variation_.preferredHeight(), 0);
        place(variationTech_, 18, 14);
        place(speakersListCaption_, 22, 4);
        for (auto& r : rows_) {
            r->setBounds(x, y, w, 34);
            y += 36;
        }
        y += 14;
        place(zonesCaption_, 22, 2);
        place(zonesHelp_, 36, 4);
        for (auto& z : zoneRows_) {
            z->setBounds(x, y, w, 62);
            y += 66;
        }
        addZone_.setBounds(x, y, 170, 32);
        removeZone_.setBounds(x + 180, y, 150, 32);
        y += 32 + 24;
    } else {
        for (auto* c : std::initializer_list<juce::Component*>{&spatialCaption_, &spatial_, &customCount_, &spread_, &variation_,
                                                               &variationTech_, &speakersListCaption_, &zonesCaption_, &zonesHelp_,
                                                               &addZone_, &removeZone_})
            hide(*c);
        for (auto& r : rows_) hide(*r);
        for (auto& z : zoneRows_) hide(*z);
        y += 16;
    }
    return y;
}

}  // namespace bf::gui
