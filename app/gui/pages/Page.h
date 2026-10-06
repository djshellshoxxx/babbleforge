#pragma once
// Page base class and self-registration (docs/GUI_ARCHITECTURE.md "Adding a page").
//
// A page is a juce::Component created by the main window from the PageRegistry. To add one:
//   1. derive from bf::gui::Page, build controls in the constructor, lay them out in
//      layoutPage(width) (returns the content height; the window scrolls if needed);
//   2. override refreshFromState() (AppState changed: preset / mode) and, if it shows live
//      data, refreshStatus() (EngineBridge, 30 Hz);
//   3. register it in its .cpp:
//        static bf::gui::PageRegistrar reg({"area", "AREA", 30, PageInfo::Sidebar,
//            [](bf::gui::PageContext& c) { return std::make_unique<AreaPage>(c); }});
//   4. add the .cpp to app/gui/CMakeLists.txt.
// The main window builds the sidebar (ordered by `order`, Ctrl+1..5 = sidebar position) and
// the gear button (Placement::Gear) from the registry; it needs no changes.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/AppSettings.h"
#include "model/AppState.h"
#include "model/EngineBridge.h"
#include "model/PresetSession.h"

namespace bf::gui {

struct PageContext {
    AppState& state;
    PresetSession& session;
    EngineBridge& bridge;
    AppSettings& settings;
    std::function<void(const std::string& pageId)> showPage;  // navigate (set by the main window)
    std::function<void(bool)> setTooltipsEnabled;
};

class Page : public juce::Component, private AppState::Listener, private EngineBridge::Listener {
public:
    explicit Page(PageContext& ctx);
    ~Page() override;

    // Lays out children for the given width and returns the content height.
    virtual int layoutPage(int width) = 0;
    virtual void refreshFromState() {}
    virtual void refreshStatus(const EngineStatus&) {}

    // Recomputes the layout (e.g. after a collapsible panel toggled); the main window resizes
    // the page to the new height.
    void relayout();
    std::function<void()> onHeightChanged;

    void resized() override;
    void paint(juce::Graphics&) override;

protected:
    PageContext& ctx;
    AppState& state() { return ctx.state; }
    bool advanced() const { return ctx.state.advanced(); }
    // Content column: centred, at most maxWidth wide.
    static juce::Rectangle<int> column(int width, int maxWidth, int top = 24);

private:
    void appStateChanged(unsigned changes) override;
    void engineStatusChanged(const EngineStatus& s) override { refreshStatus(s); }
    int lastHeight_ = 0;
};

struct PageInfo {
    enum Placement { Sidebar, Gear };
    std::string id;       // "run", "mask", "area", "output", "analysis", "settings"
    juce::String title;   // sidebar text
    int order = 0;        // sidebar order (RUN 10, MASK 20, AREA 30, OUTPUT 40, ANALYSIS 50)
    Placement placement = Sidebar;
    std::function<std::unique_ptr<Page>(PageContext&)> create;
    bool advancedOnly = false;  // hidden in Simple mode (ANALYSIS)
};

class PageRegistry {
public:
    static void add(PageInfo info);
    static std::vector<PageInfo> all();  // sorted by order
    static const PageInfo* find(const std::string& id);
    // Sidebar pages visible in the mode, in order.
    static std::vector<PageInfo> sidebar(bool advanced);

private:
    static std::vector<PageInfo>& storage();
};

struct PageRegistrar {
    explicit PageRegistrar(PageInfo info) { PageRegistry::add(std::move(info)); }
};

}  // namespace bf::gui
