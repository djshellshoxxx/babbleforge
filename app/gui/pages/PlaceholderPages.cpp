// AREA / OUTPUT / ANALYSIS / Settings placeholders (docs/GUI.md §14-§24, §33-§45, §50-§54).
// TODO(gui): replace each with its real page. Keep the registration (id, order, placement):
// the main window, sidebar and Ctrl+1..5 shortcuts are driven by the PageRegistry.
#include "LookAndFeel.h"
#include "Widgets.h"
#include "pages/Page.h"

namespace bf::gui {

namespace {

class PlaceholderPage : public Page {
public:
    PlaceholderPage(PageContext& c, const juce::String& title, const char* todoUtf8) : Page(c), title_(title.toUpperCase(), true) {
        addAndMakeVisible(title_);
        todo_.setText("TODO: " + juce::String::fromUTF8(todoUtf8), juce::dontSendNotification);
        todo_.setFont(fonts::body(15.0f));
        todo_.setColour(juce::Label::textColourId, Theme::get().textDim);
        todo_.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(todo_);
    }
    int layoutPage(int width) override {
        auto col = column(width, 640, 24);
        title_.setBounds(col.getX(), col.getY(), col.getWidth(), 26);
        todo_.setBounds(col.getX(), col.getY() + 34, col.getWidth(), 80);
        return layoutExtra(col.getX(), col.getY() + 124, col.getWidth());
    }

protected:
    virtual int layoutExtra(int, int y, int) { return y; }

private:
    SectionLabel title_;
    juce::Label todo_;
};

class SettingsPage final : public PlaceholderPage {
public:
    explicit SettingsPage(PageContext& c)
        : PlaceholderPage(c, "Settings",
                          "Audio, Startup, Voice Library, Appearance, Logging and Diagnostics sections (GUI \xc2\xa7" "50-\xc2\xa7" "54).") {
        noFocus(showIntro_);
        showIntro_.setTooltip("Show the explanation the next time Advanced mode is switched on.");
        showIntro_.onClick = [this] { ctx.settings.update([](AppSettingsData& d) { d.advancedIntroShown = false; }); };
        addAndMakeVisible(showIntro_);
    }

protected:
    int layoutExtra(int x, int y, int w) override {
        showIntro_.setBounds(x, y, juce::jmin(w, 360), 36);
        return y + 60;
    }

private:
    juce::TextButton showIntro_{"Show Advanced mode explanation again"};
};

PageRegistrar analysisReg({"analysis", "ANALYSIS", 50, PageInfo::Sidebar,
                           [](PageContext& c) {
                               return std::make_unique<PlaceholderPage>(
                                   c, "Analysis", "Voice Activity, Spectrum, Modulation and Spatial cards (GUI \xc2\xa7" "41-\xc2\xa7" "45).");
                           },
                           true});
PageRegistrar settingsReg({"settings", "SETTINGS", 100, PageInfo::Gear,
                           [](PageContext& c) { return std::make_unique<SettingsPage>(c); }});

}  // namespace

}  // namespace bf::gui
