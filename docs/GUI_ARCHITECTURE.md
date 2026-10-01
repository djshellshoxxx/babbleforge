# BabbleForge GUI architecture

The GUI behavior is specified in `docs/GUI.md`. This note covers how the JUCE app in `app/gui/` is organized.

## Classes

| Layer | Class | File | Role |
|---|---|---|---|
| Model | `AppSettings` | `model/AppSettings.*` | `settings.json` (PRESETS §9): Simple/Advanced, whether the Advanced explanation has been shown, device, last page, UI scale, voice library root. Writes are atomic and debounced by 2 s. Unknown keys are preserved. |
| Model | `AppState` | `model/AppState.*` | The editable model: the current `bf::Preset` and the UI mode. Each edit is an undoable whole-preset snapshot in a `juce::UndoManager`. Typed setters for every page control. Enforces min ≤ average ≤ max ≤ pool, segment min ≤ max, and the EQ ±12 dB limit. After each change it recomputes `compose()` and `composePlan()` (about 0.1 ms), so pages always show effective values. |
| Model | `PresetSession` | `model/PresetSession.*` | Factory / Modified / User Preset state (GUI §46), the change list for "ADVANCED SETTINGS MODIFIED" (§57), "differs significantly" (§48), Reset to Recommended, Save As / load `user_presets/*.bfpreset`. |
| Model | `EngineBridge` | `model/EngineBridge.*` | Owns the `EngineController` and the `JuceAudioBackend` (or an injected backend). It is the only GUI code that talks to the engine. |
| Model | `Catalog` | `model/Catalog.*` | User-facing names, order and descriptions for areas and mask types (§5, §6, §10), and Strength labels (from `engine_defaults.json`). |
| View | `MainComponent` | `MainComponent.*` | Header (SIMPLE \| ADVANCED, gear), preset bar (state, ADVANCED SETTINGS MODIFIED, Reset, Save As), sidebar built from the registry, a scrolling page viewport, the status bar, keyboard shortcuts (§62) and in-window dialogs (§25, §47, §57, F1 help). |
| View | `Page`, `PageRegistry` | `pages/Page.*` | Page base class and self-registration. |
| View | `RunPage`, `MaskPage` | `pages/*` | GUI §3–§13 and §26–§32. |
| View | `AnalysisPage` | `pages/AnalysisPage.*` | GUI §41-§45, Advanced only. Four cards fed by `EngineStatus::stats` (a `MaskStatistics` snapshot, which also carries the analysis view: target/measured 1/3-octave spectra, occupancy, density, per-slot activity, adjacent-channel correlation). Updates are limited to 15 Hz. |
| View | `SettingsPage` | `pages/SettingsPage.*` | GUI §50-§53 (gear). Preferences live in `settings.json` under `prefs` (`model/Prefs.*`). The fallback policy is a preset field. |
| View | dialogs | `dialogs/*` | `LibraryDialogs` (Manage Library, import wizard on a background `CorpusImportJob` with cancel; imports into a staging folder and installs on Add), `FirstRunWizard` (§60, shown when `settings.json` is absent), `SafetyBanner` (§48, non-blocking), `HelpParts` (`?` buttons, cards). |
| View | `StatusBar` | `StatusBar.*` | §61: a Simple and an Advanced variant, plus "Limiter disabled" (§40). |
| View | `BfLookAndFeel`, `Theme` | `LookAndFeel.*` | Dark, high-contrast theme. Component properties `bf.primary`, `bf.segment` and `bf.card` give the emphasis of §63. Scaling uses `Desktop::setGlobalScaleFactor(settings.uiScale)`. |
| View | widgets | `Widgets.*` | `MacroSlider` (caption, end labels, value text, ticks, drag = gesture), `ParamRow`, `ChoiceGroup`, `CollapsiblePanel`, `VStack`, `ActivityBar`, `SpectrumGraph`, `OverlayDialog`. |

## Data flow

```text
widget ──setter──▶ AppState ──UndoManager.perform(PresetEditAction)──▶ preset_ + compose/composePlan
                      │ listeners (message thread)
       ┌──────────────┼───────────────────────────┬────────────────────────┐
       ▼              ▼                           ▼                        ▼
  pages refresh   MainComponent header      StatusBar           EngineBridge: debounce 150 ms
                  (PresetSession)                                 ──▶ worker "bf.gui.engine":
                                                                     composePlan (validate)
                                                                     EngineController::setPreset (blocking)
EngineController ◀── status thread "bf.gui.status" (pollStatus, state, metrics, statistics)
        snapshot ──▶ 30 Hz juce::Timer ──▶ EngineBridge::Listener (pages, status bar)
```

- **No blocking on the message thread.** All engine commands (`setPreset`, `start`, `stop`, `reconnect`, `reset`, device changes and device enumeration) run on the worker. Only the newest pending preset is pushed ("latest wins"). A push is skipped when the `AppState` revision has not changed. `start()` flushes a pending push first.
- **Undo.** `AppState::beginGesture()` / `endGesture()` wrap a slider drag. All edits in a gesture that have the same coalesce key merge into a single undo step. Every other edit starts its own transaction. The shortcuts are Ctrl+Z, Ctrl+Shift+Z and Ctrl+Y.
- **Modes.** Switching between Simple and Advanced changes only the UI and `settings.json`. It never touches audio. The first switch to Advanced shows the §25 explanation. Settings has a button that shows it again.
- **Area / mask type.**
  - Selecting an area loads its recommendation: macros and advanced fields go back to inherit. Strength and outputs are kept.
  - Changing the mask type resets the strategy-dependent defaults: Character, Mix and Multi-Voice K.
  - Strength and the output configuration do not count toward "Modified", and Reset keeps them.
- **Device.** The bridge never changes devices on its own. If no device is configured, the first start picks the first device that has outputs and stores it in `settings.json` on exit.

## Adding a page

1. Create `pages/FooPage.{h,cpp}` with `class FooPage : public Page`.
   - Build the controls in the constructor and call `refreshFromState()` at the end.
   - Implement `int layoutPage(int width)`: position the children and return the content height. The window scrolls when the content is taller than the view.
   - Override `refreshFromState()` (called on preset or mode changes) and, for live data, `refreshStatus(const EngineStatus&)` (30 Hz).
   - Call `relayout()` when the content height changes, for example after a `CollapsiblePanel` toggles.
2. Register it in the `.cpp`:
   ```cpp
   static PageRegistrar reg({"area", "AREA", 30, PageInfo::Sidebar,
                             [](PageContext& c) { return std::make_unique<AreaPage>(c); }});
   ```
   - `order` sets the sidebar position: RUN 10, MASK 20, AREA 30, OUTPUT 40, ANALYSIS 50. Ctrl+1…5 follow the sidebar order.
   - Pass `PageInfo::Gear` for the Settings page.
   - Pass `advancedOnly = true` (the 6th field) to hide the page in Simple mode.
   - Use the same id as the placeholder, then remove the placeholder's registration.
3. Add the sources to `app/gui/CMakeLists.txt`. `MainComponent` does not need any change.
4. Read and write through `ctx.state` (add typed setters to `AppState` so that edits are undoable), `ctx.session`, `ctx.bridge` and `ctx.settings`. `ctx.showPage("output")` navigates to another page.
5. Call `noFocus(component)` on clickable controls so that Space stays the global Start/Stop key. Text fields keep their focus, and Space is never taken from them.

## Tests

`BabbleForge --run-ui-tests` (`tests/UiTests.cpp`, JUCE `UnitTest`, category `BabbleForgeUI`) builds the whole main component offscreen on a `NullBackend` and checks:

- the page registry and layout;
- Simple/Advanced with the one-time dialog, and the Ctrl+N rules;
- the Strength labels; one undo step per drag; Character undo/redo via the keyboard;
- that the 150 ms push debounce coalesces several edits into a single push received by the controller;
- min ≤ avg ≤ max ≤ pool, from the widgets and from a 300-step fuzz;
- the MASK advanced controls: mix, stationary, custom EQ ±6/±12, and correction;
- Reset, Save As and load;
- shortcuts: Space is ignored in text fields and dialogs; Space starts and stops the engine; masking survives a mode switch.

Ways to run the tests:

- The ctest test is `gui_ui_tests`. It is registered even when `BF_BUILD_TESTS=OFF`: `ctest --test-dir build-app/app/gui`. On Linux the test runs under `xvfb-run -a` when that is installed (`apt-get install xvfb`).
- To run it directly: `xvfb-run -a build-app/app/gui/BabbleForge_artefacts/Release/BabbleForge --run-ui-tests`.
- `BF_UI_SNAPSHOT_DIR=<dir>` also writes PNG snapshots of the RUN and MASK pages for visual review.
