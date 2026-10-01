#pragma once
// User-facing names, order and descriptions for areas, mask types and Strength labels
// (docs/GUI.md §5, §6, §7; ENGINE.md §3.1). Ids are the data-set ids; unknown ids found in
// the data set are appended with their data display names.
#include <string>
#include <utility>
#include <vector>

#include <juce_core/juce_core.h>

#include "core/config/DataSet.h"

namespace bf::gui {

struct Choice {
    std::string id;
    juce::String name;
    juce::String description;
};

std::vector<Choice> areaChoices(const DataSet& ds);
std::vector<Choice> maskTypeChoices(const DataSet& ds);  // RUN page order (GUI §6)
std::vector<Choice> maskStyleChoices(const DataSet& ds); // MASK page order (GUI §10)
juce::String areaName(const DataSet& ds, const std::string& id);
juce::String maskTypeName(const DataSet& ds, const std::string& id);
juce::String maskTypeDescription(const DataSet& ds, const std::string& id);
juce::String areaAdvisory(const DataSet& ds, const std::string& areaId);  // e.g. Outdoor (GUI §21)

// Strength labels (data: engine_defaults.json strength.simple.labels), sorted by dB.
std::vector<std::pair<double, juce::String>> strengthLabels(const DataSet& ds);
juce::String strengthLabelFor(const DataSet& ds, double db);  // nearest label
juce::String formatDb(double db);                              // "+2.0 dB"

}  // namespace bf::gui
