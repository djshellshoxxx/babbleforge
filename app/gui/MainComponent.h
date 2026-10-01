#pragma once
// Main window content (docs/GUI.md §2): header (preset state, ADVANCED SETTINGS MODIFIED,
// SIMPLE | ADVANCED, gear), sidebar built from the PageRegistry, scrollable page area,
// status bar; keyboard shortcuts (§62) and in-window dialogs (§25, §47, §48, §57).
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "StatusBar.h"
#include "Widgets.h"
#include "pages/Page.h"

namespace bf::gui {

class MainComponent final : public juce::Component, private AppState::Listener {
public:
    MainComponent(AppState& state, PresetSession& session, EngineBridge& bridge, AppSettings& settings);
    ~MainComponent() override;

    void showPage(const std::string& id);
    const std::string& currentPageId() const noexcept { return current_; }
    Page* page(const std::string& id);  // created on demand
    std::vector<juce::TextButton*> sidebarButtons();

    // SIMPLE / ADVANCED (GUI §25): the first switch to Advanced shows the explanation.
    void requestMode(UiMode m);
    // Keyboard shortcuts (GUI §62). Returns true when handled.
    bool handleKey(const juce::KeyPress& k);

    void openSaveDialog();
    void openChangesDialog();
    void openHelp();
    OverlayDialog* dialog() noexcept { return dialog_.get(); }
    void closeDialog();

    juce::TextButton& advancedButton() { return advancedBtn_; }
    juce::TextButton& simpleButton() { return simpleBtn_; }
    juce::TextButton& modifiedButton() { return modifiedBtn_; }
    juce::TextButton& resetButton() { return resetBtn_; }
    StatusBar& statusBar() { return status_; }
    juce::Label& presetLabel() { return presetLabel_; }

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& k) override { return handleKey(k); }
    void parentHierarchyChanged() override;

private:
    struct ShortcutListener final : juce::KeyListener {
        explicit ShortcutListener(MainComponent& m) : owner(m) {}
        bool keyPressed(const juce::KeyPress& k, juce::Component*) override { return owner.handleKey(k); }
        MainComponent& owner;
    };
    class GearButton final : public juce::Button {
    public:
        GearButton() : juce::Button("Settings") {}
        void paintButton(juce::Graphics&, bool highlighted, bool down) override;
    };

    void appStateChanged(unsigned changes) override;
    void rebuildSidebar();
    void updateHeader();
    void sizePage();
    void showDialog(std::unique_ptr<OverlayDialog> d);

    AppState& state_;
    PresetSession& session_;
    EngineBridge& bridge_;
    AppSettings& settings_;
    PageContext ctx_;

    juce::Label title_, presetLabel_;
    juce::TextButton resetBtn_{"Reset to Recommended"}, saveBtn_{"Save As Preset"};
    juce::TextButton modifiedBtn_{"ADVANCED SETTINGS MODIFIED"};
    juce::TextButton simpleBtn_{"SIMPLE"}, advancedBtn_{"ADVANCED"};
    GearButton gear_;
    std::vector<std::unique_ptr<juce::TextButton>> sidebar_;
    std::vector<std::string> sidebarIds_;
    juce::Viewport viewport_;
    std::map<std::string, std::unique_ptr<Page>> pages_;
    std::string current_;
    StatusBar status_;
    std::unique_ptr<OverlayDialog> dialog_;
    juce::TooltipWindow tooltips_{this, 600};
    ShortcutListener shortcuts_{*this};
    juce::Component* keyTarget_ = nullptr;
};

}  // namespace bf::gui
