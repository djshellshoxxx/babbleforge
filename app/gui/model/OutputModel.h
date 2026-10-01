#pragma once
// AREA / OUTPUT page edits (docs/GUI.md §14-§21, §33-§40): speaker setups, spatial output
// mode, speaker assignment (enabled / level / delay), zones and the limiter, expressed as
// undoable AppState::edit() calls on the preset's `spatial` and `outputs` blocks.
// Layouts come from src/core/spatial OutputLayout factories.
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "core/spatial/OutputLayout.h"
#include "model/AppState.h"

namespace bf::gui::outputs {

enum class SpatialMode { Mono, Stereo, Ring4, Ring6, Ring8, Custom };
enum class AreaSize { Small, Medium, Large };
enum class SpeakerSetup { Two, Four, MoreThanFour };

inline constexpr int kMaxZones = 8;
inline constexpr int kCustomMin = 3, kCustomMax = 16;

SpatialMode spatialMode(const Preset& p);
OutputLayout layoutFor(SpatialMode m, int customCount = 4);
// The layout the engine will use for the current preset (explicit channels win).
OutputLayout currentLayout(const Preset& p);
int outputCount(const Preset& p);
juce::String spatialModeName(SpatialMode m);

// Simple AREA page: area size (sets the spatial spread) and speaker setup (layout).
AreaSize areaSize(const AppState& s);
void setAreaSize(AppState& s, AreaSize a);
SpeakerSetup speakerSetup(const AppState& s);
void setSpeakerSetup(AppState& s, SpeakerSetup v);
// Outdoor layout choice 2 / 4 / 6 / 8+.
void setOutdoorSpeakers(AppState& s, int n);
int outdoorSpeakers(const AppState& s);

// Advanced.
void setSpatialMode(AppState& s, SpatialMode m, int customCount = 4);
double spread(const AppState& s);
void setSpread(AppState& s, double v);                 // 0..1
std::string speakerVariation(const AppState& s);       // low | medium | high
void setSpeakerVariation(AppState& s, const std::string& v);

void setChannelEnabled(AppState& s, int ch, bool on);
void setChannelGainDb(AppState& s, int ch, double db);   // -40..+6
void setChannelDelayMs(AppState& s, int ch, double ms);  // 0..100
void setChannelZone(AppState& s, int ch, int zone);

const OutputChannel* channelAt(const Preset& p, int ch);  // null when the preset has no explicit channel entries

// Zones A, B, ... (levels only, no separate DSP).
void addZone(AppState& s);     // first call creates zones A and B and splits the channels
void removeZone(AppState& s);  // removes the last zone; the last two collapse to none
void setZoneEnabled(AppState& s, int zoneId, bool on);
void setZoneLevelDb(AppState& s, int zoneId, double db);           // -24..+6
void setZoneMixOffset(AppState& s, int zoneId, double offset);     // -0.5..+0.5 of the babble fraction
juce::String zoneName(int zoneId);                                 // "Zone A"

// Output limiter (GUI §40).
bool limiterEnabled(const AppState& s);
void setLimiterEnabled(AppState& s, bool on);
double limiterCeilingDbtp(const AppState& s);
void setLimiterCeilingDbtp(AppState& s, double db);  // -6..-0.1

// Contextual recommendation (GUI §56): empty when there is nothing to say. Never modal.
juce::String recommendation(const AppState& s);

// ENGINE.md §5 Simple mapping on RMS-Leq60 relative to L_ref (-26 dBFS): LOW < -12 dB,
// GOOD -12..+6 dB, HIGH > +6 dB or the limiter "sustained" flag.
enum class LevelBand { None, Low, Good, High };
LevelBand levelBand(double leq60Db, double lRefDbfs, bool limiterSustained);
juce::String levelBandName(LevelBand b);

}  // namespace bf::gui::outputs
