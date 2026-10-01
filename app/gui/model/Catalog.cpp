#include "model/Catalog.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace bf::gui {

namespace {

struct Known {
    const char* id;
    const char* name;
    const char* description;
};

// GUI §5 order and names.
const Known kAreas[] = {
    {"small_room", "Small Room", "For private offices, small meeting rooms and other compact enclosed spaces."},
    {"office", "Office", "For 1\xe2\x80\x93" "4 person offices."},
    {"conference", "Conference Room", "Even coverage around a table, with moderate voice density."},
    {"open_office", "Open Office", "Distributed, uniform masking with low recognition of individual voices."},
    {"large_room", "Large Room", "Halls and large boardrooms: several speakers and a dense masker."},
    {"common_area", "Common Area", "Natural-sounding distant background activity."},
    {"reception", "Reception", "Natural-sounding distant background activity."},
    {"free_field", "Outdoor / Free Field", "Open spaces without walls."},
    {"custom", "Custom", "Your own configuration."},
};

// GUI §6 order, names and Simple descriptions.
const Known kMaskTypes[] = {
    {"balanced", "Balanced", "General-purpose speech masking with a mix of voices and stable background masking."},
    {"natural", "Natural", "Sounds more like distant conversation."},
    {"dense", "Maximum Density", "Denser masking with fewer quiet gaps."},
    {"speech_noise", "Speech Noise", "Steady speech-shaped noise without recognizable voices."},
    {"multi_voice", "Multi-Voice", "Uses several distinct voices as the main masker."},
    {"hybrid", "Hybrid", "Combines multi-speaker babble and steady masking."},
};

// GUI §10 order (MASK page); Maximum Density is kept so every RUN choice is visible here too.
const char* const kMaskStyleOrder[] = {"balanced", "speech_noise", "multi_voice", "hybrid", "natural", "dense"};

const Known* findKnown(const Known* begin, const Known* end, const std::string& id) {
    for (auto* k = begin; k != end; ++k)
        if (id == k->id) return k;
    return nullptr;
}

juce::String utf8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

}  // namespace

std::vector<Choice> areaChoices(const DataSet& ds) {
    std::vector<Choice> out;
    for (const auto& k : kAreas)
        if (ds.areas.count(k.id)) out.push_back({k.id, juce::String::fromUTF8(k.name), juce::String::fromUTF8(k.description)});
    for (const auto& [id, a] : ds.areas)
        if (!findKnown(std::begin(kAreas), std::end(kAreas), id)) out.push_back({id, utf8(a.displayName), utf8(a.description)});
    return out;
}

std::vector<Choice> maskTypeChoices(const DataSet& ds) {
    std::vector<Choice> out;
    for (const auto& k : kMaskTypes)
        if (ds.strategies.count(k.id)) out.push_back({k.id, k.name, k.description});
    return out;
}

std::vector<Choice> maskStyleChoices(const DataSet& ds) {
    std::vector<Choice> out;
    for (const char* id : kMaskStyleOrder) {
        if (!ds.strategies.count(id)) continue;
        const Known* k = findKnown(std::begin(kMaskTypes), std::end(kMaskTypes), id);
        juce::String name = k->name;
        if (std::string(id) == "natural") name = "Natural Babble";
        out.push_back({id, name, k->description});
    }
    return out;
}

juce::String areaName(const DataSet& ds, const std::string& id) {
    if (auto* k = findKnown(std::begin(kAreas), std::end(kAreas), id)) return juce::String::fromUTF8(k->name);
    if (auto it = ds.areas.find(id); it != ds.areas.end()) return utf8(it->second.displayName);
    return utf8(id);
}

juce::String maskTypeName(const DataSet& ds, const std::string& id) {
    if (auto* k = findKnown(std::begin(kMaskTypes), std::end(kMaskTypes), id)) return k->name;
    if (auto it = ds.strategies.find(id); it != ds.strategies.end()) return utf8(it->second.displayName);
    return utf8(id);
}

juce::String maskTypeDescription(const DataSet& ds, const std::string& id) {
    if (auto* k = findKnown(std::begin(kMaskTypes), std::end(kMaskTypes), id)) return k->description;
    if (auto it = ds.strategies.find(id); it != ds.strategies.end()) return utf8(it->second.descriptionSimple);
    return {};
}

juce::String areaAdvisory(const DataSet&, const std::string& areaId) {
    if (areaId == "free_field") return "Outdoor environments usually require multiple speakers for effective coverage.";
    return {};
}

std::vector<std::pair<double, juce::String>> strengthLabels(const DataSet& ds) {
    std::vector<std::pair<double, juce::String>> out;
    for (const auto& [key, db] : ds.engineDefaults.strengthLabels) {
        juce::String name;
        for (std::size_t i = 0; i < key.size(); ++i) {
            if (i > 0 && std::isupper(static_cast<unsigned char>(key[i]))) name << " ";
            name << juce::String::charToString(static_cast<juce::juce_wchar>(key[i]));
        }
        out.emplace_back(db, name);
    }
    if (out.empty()) out = {{-12.0, "Gentle"}, {-6.0, "Low"}, {0.0, "Normal"}, {5.0, "Strong"}, {9.0, "Very Strong"}};
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

juce::String strengthLabelFor(const DataSet& ds, double db) {
    const auto labels = strengthLabels(ds);
    juce::String best;
    double bestD = 1e9;
    for (const auto& [v, n] : labels)
        if (std::abs(v - db) < bestD) {
            bestD = std::abs(v - db);
            best = n;
        }
    return best;
}

juce::String formatDb(double db) {
    const double r = std::round(db * 10.0) / 10.0;
    return (r > 0 ? "+" : "") + juce::String(r, 1) + " dB";
}

}  // namespace bf::gui
