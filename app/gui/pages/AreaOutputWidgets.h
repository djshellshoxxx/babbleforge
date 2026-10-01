#pragma once
// Widgets private to the AREA and OUTPUT pages: a per-channel level meter and the top-down
// speaker map (docs/GUI.md §19 style room view with live per-output activity).
#include <algorithm>
#include <cmath>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "LookAndFeel.h"
#include "core/engine/MaskEngine.h"
#include "core/spatial/OutputLayout.h"

namespace bf::gui {

// Output activity 0..1 from an RMS-fast value (dBFS): -60 dB = 0, -10 dB = 1.
inline float meterFraction(double db) { return static_cast<float>(juce::jlimit(0.0, 1.0, (db + 60.0) / 50.0)); }

// One output channel: name, segmented bar, optional value text ("-23.4 dBFS").
class ChannelMeter final : public juce::Component {
public:
    void setName(const juce::String& n) {
        if (n != name_) {
            name_ = n;
            repaint();
        }
    }
    void setLevelDb(double db, bool showValue) {
        const float f = meterFraction(db);
        const juce::String v = showValue ? (db > -150.0 ? juce::String(db, 1) + " dBFS" : juce::String(juce::CharPointer_UTF8("-\xe2\x88\x9e dBFS"))) : juce::String();
        if (f != level_ || v != value_) {
            level_ = f;
            value_ = v;
            repaint();
        }
    }
    float level() const { return level_; }
    const juce::String& name() const { return name_; }

    void paint(juce::Graphics& g) override {
        const auto& t = Theme::get();
        auto r = getLocalBounds();
        g.setFont(fonts::body(14.0f));
        g.setColour(t.text);
        g.drawText(name_, r.removeFromLeft(96), juce::Justification::centredLeft);
        if (value_.isNotEmpty()) {
            g.setColour(t.textDim);
            g.drawText(value_, r.removeFromRight(92), juce::Justification::centredRight);
        }
        auto bar = r.reduced(0, 4).toFloat();
        g.setColour(t.panelAlt);
        g.fillRoundedRectangle(bar, 3.0f);
        const int segs = 28;
        const float gap = 2.0f, w = (bar.getWidth() - gap * (segs - 1)) / segs;
        const int lit = juce::roundToInt(level_ * segs);
        for (int i = 0; i < segs; ++i) {
            juce::Colour c = i < lit ? (i > segs * 0.9f ? t.danger : i > segs * 0.75f ? t.warn : t.accent) : t.outline.withAlpha(0.6f);
            g.setColour(c);
            g.fillRoundedRectangle(bar.getX() + i * (w + gap), bar.getY() + 2.0f, w, bar.getHeight() - 4.0f, 1.5f);
        }
    }

private:
    juce::String name_, value_;
    float level_ = 0.0f;
};

// Top-down view: the room outline, the listener at the centre, one marker per output with its
// enabled state, zone colour and live activity (%).
class SpeakerMap final : public juce::Component {
public:
    struct Speaker {
        juce::String label;
        float x = 0.0f, y = 0.0f;  // -1..1, x right, y down (front = up)
        bool enabled = true;
        int zone = 0;
        bool zoned = false;
        float activity = 0.0f;  // 0..1
    };

    void setLayout(const OutputLayout& L, const std::vector<int>& zones, bool zoned) {
        speakers_.clear();
        float maxR = 1.0f;
        std::vector<Vec2> pos;
        const bool grid = !L.outputs.empty() && L.outputs.front().posM.has_value();
        for (const OutputDef& d : L.outputs) {
            Vec2 p;
            if (grid && d.posM) {
                p = *d.posM;
            } else {
                const float a = juce::degreesToRadians(d.azimuthDeg);
                p = {-std::sin(a), -std::cos(a)};  // + azimuth = left, 0 = front (up)
            }
            pos.push_back(p);
        }
        if (grid) {
            float cx = 0, cy = 0;
            for (auto& p : pos) cx += p.x, cy += p.y;
            cx /= static_cast<float>(pos.size());
            cy /= static_cast<float>(pos.size());
            for (auto& p : pos) maxR = std::max({maxR, std::abs(p.x - cx), std::abs(p.y - cy)}), p = {p.x - cx, p.y - cy};
        }
        for (std::size_t i = 0; i < L.outputs.size(); ++i) {
            Speaker s;
            s.label = juce::String(static_cast<int>(i) + 1);
            s.x = pos[i].x / maxR;
            s.y = pos[i].y / maxR;
            s.enabled = L.outputs[i].enabled;
            s.zoned = zoned;
            s.zone = i < zones.size() ? zones[i] : 0;
            s.activity = i < activity_.size() ? activity_[i] : 0.0f;
            speakers_.push_back(s);
        }
        repaint();
    }
    void setActivity(const std::vector<float>& a) {
        activity_ = a;
        for (std::size_t i = 0; i < speakers_.size(); ++i) speakers_[i].activity = i < a.size() ? a[i] : 0.0f;
        repaint();
    }
    // Virtual talkers (engine MaskStatistics::talkerDots): one dot per active talker at its virtual
    // position. Stereo: pan between the two speakers; ring / small multichannel: azimuth on the
    // circle; distributed: next to the home output. Null / no active talker clears the dots.
    struct Dot {
        float x = 0.0f, y = 0.0f;  // map coordinates (-1..1)
        float gain = 0.0f;         // 0..1
        int slot = 0;
    };
    void setTalkers(const MaskStatistics* st) {
        dots_.clear();
        if (st && st->spatialAlgorithm >= 0 && !speakers_.empty()) {
            const auto algo = static_cast<SpatialAlgorithm>(st->spatialAlgorithm);
            for (std::size_t i = 0; i < st->talkerDots.size(); ++i) {
                const auto& d = st->talkerDots[i];
                if (!d.active) continue;
                Dot o;
                o.slot = static_cast<int>(i);
                o.gain = juce::jlimit(0.0f, 1.0f, d.gain);
                const auto& a = speakers_.front();
                if (algo == SpatialAlgorithm::DistributedStereo && speakers_.size() >= 2) {
                    const float u = juce::jlimit(0.0f, 1.0f, (d.pan + 1.0f) * 0.5f);  // -1 = first (left) output
                    o.x = a.x + (speakers_[1].x - a.x) * u;
                    o.y = a.y + (speakers_[1].y - a.y) * u;
                } else if (algo == SpatialAlgorithm::SmallMultichannel) {
                    const float r = juce::degreesToRadians(d.azimuthDeg);
                    o.x = -std::sin(r) * 0.72f;
                    o.y = -std::cos(r) * 0.72f;
                } else if (algo == SpatialAlgorithm::LargeDistributed && d.home >= 0 && d.home < static_cast<int>(speakers_.size())) {
                    const auto& h = speakers_[static_cast<std::size_t>(d.home)];
                    const float jitter = 0.10f * std::sin(static_cast<float>(i) * 2.399f);  // spread several talkers of one home
                    o.x = h.x * 0.85f + jitter;
                    o.y = h.y * 0.85f + 0.10f * std::cos(static_cast<float>(i) * 2.399f);
                } else {
                    o.x = a.x * 0.6f;
                    o.y = a.y * 0.6f;
                }
                dots_.push_back(o);
            }
        }
        repaint();
    }
    int talkerDotCount() const { return static_cast<int>(dots_.size()); }
    const Dot& talkerDot(int i) const { return dots_[static_cast<std::size_t>(i)]; }
    int speakerCount() const { return static_cast<int>(speakers_.size()); }
    float activityAt(int i) const { return i >= 0 && i < speakerCount() ? speakers_[static_cast<std::size_t>(i)].activity : 0.0f; }
    // "37%" text shown beside speaker i.
    static juce::String percent(float a) { return juce::String(juce::roundToInt(a * 100.0f)) + "%"; }

    void paint(juce::Graphics& g) override {
        const auto& t = Theme::get();
        auto r = getLocalBounds().toFloat();
        g.setColour(t.panel);
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(t.outline);
        g.drawRoundedRectangle(r.reduced(0.5f), 8.0f, 1.0f);
        auto room = r.reduced(34.0f, 26.0f);
        g.setColour(t.panelAlt);
        g.fillRoundedRectangle(room, 6.0f);
        g.setColour(t.outline);
        g.drawRoundedRectangle(room, 6.0f, 1.5f);
        g.setFont(fonts::body(11.5f));
        g.setColour(t.textFaint);
        g.drawText("FRONT", room.removeFromTop(14.0f).toNearestInt(), juce::Justification::centred);
        const auto c = room.getCentre();
        // Listener / talker area.
        g.setColour(t.accent.withAlpha(0.25f));
        g.fillEllipse(c.x - 16, c.y - 16, 32, 32);
        g.setColour(t.accent);
        g.drawEllipse(c.x - 16, c.y - 16, 32, 32, 1.5f);
        g.setColour(t.textDim);
        g.drawText("room", juce::Rectangle<float>(c.x - 24, c.y - 7, 48, 14).toNearestInt(), juce::Justification::centred);
        const float rx = room.getWidth() * 0.5f - 22.0f, ry = room.getHeight() * 0.5f - 18.0f;
        static const juce::uint32 zoneColours[] = {0xff2fb7a4, 0xff5b8def, 0xffd18bf0, 0xffe8b13a, 0xffe0603a, 0xff46c37b, 0xfff07ab0, 0xff9aa5b1};
        for (const Speaker& s : speakers_) {
            const float px = c.x + s.x * rx, py = c.y + s.y * ry;
            const float rad = 14.0f;
            g.setColour(s.enabled ? t.panel : t.bg);
            g.fillEllipse(px - rad, py - rad, rad * 2, rad * 2);
            if (s.enabled && s.activity > 0.0f) {
                g.setColour(t.accentStrong.withAlpha(0.15f + 0.6f * s.activity));
                const float rr = rad + 10.0f * s.activity;
                g.fillEllipse(px - rr, py - rr, rr * 2, rr * 2);
            }
            g.setColour(!s.enabled ? t.textFaint : s.zoned ? juce::Colour(zoneColours[s.zone & 7]) : t.accent);
            g.drawEllipse(px - rad, py - rad, rad * 2, rad * 2, 2.0f);
            g.setColour(s.enabled ? t.text : t.textFaint);
            g.setFont(fonts::heading(13.0f));
            g.drawText(s.label, juce::Rectangle<float>(px - rad, py - 8, rad * 2, 16).toNearestInt(), juce::Justification::centred);
            g.setFont(fonts::body(11.5f));
            g.setColour(s.enabled ? t.textDim : t.textFaint);
            g.drawText(s.enabled ? percent(s.activity) : juce::String("off"),
                       juce::Rectangle<float>(px - 24, py + rad + 1, 48, 14).toNearestInt(), juce::Justification::centred);
        }
        for (const Dot& d : dots_) {
            const float px = c.x + d.x * rx, py = c.y + d.y * ry;
            const float rad = 3.0f + 3.0f * d.gain;
            g.setColour(t.warn.withAlpha(0.35f + 0.55f * d.gain));
            g.fillEllipse(px - rad, py - rad, rad * 2, rad * 2);
            g.setColour(t.warn);
            g.drawEllipse(px - rad, py - rad, rad * 2, rad * 2, 1.0f);
        }
    }

private:
    std::vector<Speaker> speakers_;
    std::vector<float> activity_;
    std::vector<Dot> dots_;
};

}  // namespace bf::gui
