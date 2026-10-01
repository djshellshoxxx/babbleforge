#include "pages/Page.h"

#include <algorithm>

#include "LookAndFeel.h"

namespace bf::gui {

Page::Page(PageContext& c) : ctx(c) {
    ctx.state.addListener(this);
    ctx.bridge.addListener(this);
}

Page::~Page() {
    ctx.bridge.removeListener(this);
    ctx.state.removeListener(this);
}

void Page::appStateChanged(unsigned changes) {
    refreshFromState();
    if (changes & AppState::kModeChanged) relayout();
}

void Page::relayout() {
    const int h = layoutPage(getWidth());
    if (h != lastHeight_) {
        lastHeight_ = h;
        if (onHeightChanged) onHeightChanged();
    }
    repaint();
}

void Page::resized() { lastHeight_ = layoutPage(getWidth()); }

void Page::paint(juce::Graphics& g) { g.fillAll(Theme::get().bg); }

juce::Rectangle<int> Page::column(int width, int maxWidth, int top) {
    const int w = std::min(maxWidth, std::max(0, width - 48));
    return {std::max(0, (width - w) / 2), top, w, 100000};
}

std::vector<PageInfo>& PageRegistry::storage() {
    static std::vector<PageInfo> pages;  // function-local: safe during static registration
    return pages;
}

void PageRegistry::add(PageInfo info) {
    auto& s = storage();
    s.erase(std::remove_if(s.begin(), s.end(), [&](const PageInfo& p) { return p.id == info.id; }), s.end());
    s.push_back(std::move(info));
}

std::vector<PageInfo> PageRegistry::all() {
    auto v = storage();
    std::stable_sort(v.begin(), v.end(), [](const PageInfo& a, const PageInfo& b) { return a.order < b.order; });
    return v;
}

const PageInfo* PageRegistry::find(const std::string& id) {
    for (const auto& p : storage())
        if (p.id == id) return &p;
    return nullptr;
}

std::vector<PageInfo> PageRegistry::sidebar(bool advanced) {
    std::vector<PageInfo> out;
    for (auto& p : all())
        if (p.placement == PageInfo::Sidebar && (advanced || !p.advancedOnly)) out.push_back(p);
    return out;
}

}  // namespace bf::gui
