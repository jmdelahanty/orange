# GUI Modularization Plan

## Purpose

This document proposes a practical refactor plan for Orange's GUI so new
windows, diagnostics, and calibration tools can be added without continuing to
grow `src/orange.cpp` as a monolith.

This is a planning document only. It does **not** commit us to a full rewrite
or to immediate code movement.

## Current State

Orange's GUI is mixed between:

- a very large main app shell in `src/orange.cpp`
- older helper-style UI in `src/gui.h`
- newer feature-specific windows such as:
  - `src/usaf_resolution_ui.cpp`
  - aperture characterization logic/rendering still in `src/orange.cpp`
  - shared image rendering in `src/image_canvas.h`

The newer calibration UI work shows a better pattern:

- explicit UI state structs
- dedicated render functions
- dedicated worker lifecycle helpers
- shared canvas utilities

The main app shell still owns too much inline behavior:

- camera open/close
- streaming start/stop
- recording start/stop
- worker creation and destruction
- host PTP controls
- frame IPC diagnostics
- camera configuration editing
- picture save controls
- per-camera display windows

## Immediate Mode vs MVC

Orange is an immediate-mode GUI application because it uses Dear ImGui.

In immediate mode, the UI is rebuilt every frame from current state. The code
typically looks like:

```cpp
if (ImGui::Button("Start streaming")) {
    // perform an action
}
```

That is different from a more classical MVC-style GUI where the system is
organized around longer-lived UI objects and a more explicit split between:

- model: durable application data/state
- view: rendering/presentation
- controller: input handling and actions

Immediate mode does **not** mean "no architecture." It just means the
architecture should fit a frame-by-frame UI model.

For Orange, the right target is:

- explicit durable state
- modular render functions
- narrow action/orchestration functions

The goal is **not** to force a heavyweight MVC framework onto Dear ImGui.
The goal is to make the immediate-mode code modular and easier to reason about.

## Tradeoffs Of Immediate Mode

Immediate mode has real strengths for a tool like Orange:

- fast to build operator tools and diagnostics
- UI code is usually direct and easy to trace
- fewer hidden widget lifecycle rules
- a natural fit for debug panels, calibration tools, and visualization-heavy
  workflows

But it also has real risks:

- UI and operational logic can collapse into the same function too easily
- large windows can become monolithic quickly
- persistent interaction patterns require explicit state discipline
- if state is not structured well, the code turns into a large set of locals,
  flags, and inline actions

By contrast, more classical retained-mode / MVC-style architectures often give:

- stronger long-term separation between state, rendering, and actions
- better fit for large product-style interfaces
- clearer ownership of persistent view state

But those approaches also cost more in:

- boilerplate
- indirection
- implementation overhead for small tool workflows

For Orange, immediate mode remains the right fit. The design problem is not the
GUI paradigm itself. The design problem is insufficient modularity inside the
current immediate-mode codebase.

## Main Problems

### 1. `src/orange.cpp` is overloaded

It is currently responsible for:

- application startup/shutdown
- long-lived runtime state
- most main-window layout
- calibration tool state ownership
- streaming session orchestration
- recording orchestration
- worker cleanup
- ENet lifecycle

This makes changes risky because one file mixes UI rendering and operational
side effects.

### 2. UI actions directly perform orchestration

Several button handlers do too much inline work:

- allocate resources
- create worker objects
- start threads
- open/close streams
- flush encoders
- destroy GPU/display resources

This is hard to review and hard to extend safely.

### 3. Some UI state is still hidden in static locals

`src/gui.h` currently keeps panel behavior partly in `static` locals inside
render helpers. That makes the state:

- implicit
- harder to reset
- harder to reuse
- harder to test mentally

### 4. Resource ownership is spread across the render loop

Some runtime resources are still managed through raw pointers and arrays:

- worker arrays
- camera arrays
- texture arrays
- IPC manager vectors

The lifecycle is understandable, but spread across multiple render-time button
handlers and cleanup blocks.

## Design Goals

The goal is **not** to force a classical MVC rewrite. Orange is an immediate
mode GUI, and the refactor should stay aligned with that style.

The target design is:

- UI rendering functions stay simple and local
- UI state is explicit
- operational actions move behind narrow service-style functions
- runtime resources have clearer ownership
- new tools/windows follow one consistent pattern

## Proposed Design Rules

### Rule 1: Every major window gets explicit UI state

Pattern:

- `struct XxxUiState`
- `void render_xxx_window(...)`
- `void join_xxx_worker_if_finished(...)`
- `void stop_xxx_worker(...)` when needed

This is already working well for USAF and should become the standard.

### Rule 2: Button handlers should request actions, not perform everything inline

Buttons should mostly do one of:

- mutate UI state
- call a narrow app/service function
- queue a worker action

They should not directly contain large blocks of streaming/session teardown or
resource orchestration logic.

### Rule 3: Shared rendering primitives should live outside feature windows

Examples:

- image canvas
- plot helpers
- reusable status summaries
- per-camera selectors

These should become shared utilities instead of being reimplemented per window.

### Rule 4: Runtime resources need one clear owner

Streaming resources should be grouped under one app/session owner instead of
being scattered across local variables in the main loop.

## Proposed Module Boundaries

### A. App Shell

Suggested responsibility:

- startup/shutdown
- top-level window ordering
- top-level state ownership
- dispatch of render functions

Likely file:

- `src/orange.cpp` remains the shell, but gets thinner over time

Recommended directory-level split:

- `src/gui/`
  - ImGui rendering
  - panel/window UI state
  - reusable GUI widgets and helpers
- `src/session/` (or `src/app/`)
  - camera/stream/recording orchestration
  - runtime resource lifecycle
  - action helpers invoked by UI controls

This split is important.

If Orange only introduces a `src/gui/` directory but keeps large operational
actions embedded inside GUI files, the current monolith is only moved, not
reduced.

Practical rule of thumb:

- if a file is mostly `ImGui::...`, it probably belongs in `src/gui/`
- if a file is mostly worker creation, stream/record lifecycle, camera
  open/close, or resource setup/teardown, it probably belongs in `src/session/`

### B. Main Window Panels

Extract from the current main window into panel-style renderers:

- camera selection panel
- camera property panel
- model/output selection panel
- stream/session control panel
- recording control panel
- frame IPC status panel
- picture save panel
- host PTP panel

Possible naming:

- `src/main_window_panels.h/.cpp`
- or one file per larger panel if needed

Recommended directory direction:

- `src/gui/main_window_panels/`
  - `camera_panel.cpp`
  - `recording_panel.cpp`
  - `stream_panel.cpp`
  - `ptp_panel.cpp`
  - `ipc_panel.cpp`

### C. Session / Runtime Control

Move orchestration code behind explicit functions:

- `open_selected_cameras(...)`
- `close_open_cameras(...)`
- `start_streaming_session(...)`
- `stop_streaming_session(...)`
- `start_recording(...)`
- `stop_recording(...)`

Possible naming:

- `src/stream_session.h/.cpp`
- `src/recording_session.h/.cpp`

Recommended directory direction:

- `src/session/`
  - `camera_session.cpp`
  - `stream_session.cpp`
  - `recording_session.cpp`

The exact filenames are less important than the separation itself:

- GUI files decide what to render and what action to request
- session/app files decide how to perform runtime lifecycle work

### D. Camera Property Editing

Current `src/gui.h` should likely be split so camera property editing becomes:

- explicit UI state
- explicit render function
- explicit save-to-config feedback state

Possible naming:

- `src/camera_properties_ui.h/.cpp`

### E. Diagnostics Panels

These should be their own modules rather than inline blocks:

- host PTP stack panel
- frame IPC status panel
- realtime plots panel

Possible naming:

- `src/ptp_ui.h/.cpp`
- `src/frame_ipc_ui.h/.cpp`
- `src/realtime_plots_ui.h/.cpp`

### F. Calibration Tools

These should all follow the same pattern as USAF:

- dedicated `UiState`
- dedicated worker lifecycle
- dedicated artifact writer/service layer

Current/future examples:

- aperture characterization
- USAF resolution calibration
- future DOF calibration
- future target/FOV assistants

## Proposed Shared App State

Introduce an app-level owner struct for runtime resources.

Example shape:

```cpp
struct AppRuntimeState {
    CameraControl* camera_control;
    std::vector<CameraResources> camera_resources;
    std::vector<std::unique_ptr<FrameIPCManager>> frame_ipc_managers;
    std::vector<std::string> frame_ipc_init_errors;
    std::vector<std::thread> camera_threads;
    std::vector<YOLOv8Worker*> yolo_workers;
    COpenGLDisplay** openGLDisplayWorkers;
    EncoderPreprocessWorker** encoderPreprocessWorkers;
    EncoderHwWorker** encoderHWWorkers;
    CropAndEncodeWorker** cropAndEncodeWorkers;
    GL_Texture* tex;
    GL_Texture* crop_tex;
};
```

This does not need to be perfect immediately. The goal is simply to reduce the
number of unrelated locals spread across the main render loop.

## Migration Strategy

### Phase 1: Stabilize Structure Without Behavioral Change

Low-risk extractions only:

- extract panel render functions from the main window
- move `gui.h` camera property logic into a dedicated UI module
- keep all behavior the same

Suggested filesystem move during this phase:

- begin a real `src/gui/` directory for extracted panel modules
- keep `src/orange.cpp` as the app shell
- do not move heavy orchestration into `src/gui/`

Target outcome:

- smaller `src/orange.cpp`
- same runtime behavior

### Phase 2: Pull Orchestration Out Of Button Bodies

Move large inline actions behind helper functions:

- stream start/stop
- recording start/stop
- camera open/close

Target outcome:

- easier reasoning about side effects
- safer future edits

### Phase 3: Introduce Shared Runtime Owner

Replace some scattered local ownership with an app/session state object.

Target outcome:

- clearer resource lifetime
- cleaner cleanup paths

### Phase 4: Normalize Future Tool Windows

Require new windows/features to follow the newer pattern:

- dedicated UI state
- dedicated render function
- no major orchestration inline in the main window

## Recommended First Refactor Slice

The best first code slice is:

1. extract camera properties UI from `src/gui.h`
2. extract host PTP panel from `src/orange.cpp`
3. extract frame IPC status panel from `src/orange.cpp`

Why this slice:

- visible structural improvement
- low coupling to streaming/recording teardown logic
- preserves behavior
- gives a repeatable panel pattern for future additions

If recording UI becomes the next priority after that, the recommended sequence
is:

1. `src/gui/recording_panel.*`
2. `src/session/recording_session.*`
3. wire the existing recording section in `src/orange.cpp` through those modules

That gives a meaningful improvement without requiring a full GUI rewrite.

## Good Practices To Keep

The current code already has patterns worth preserving:

- immediate mode UI is fine for this app
- worker join helpers are good
- feature-specific UI state structs are good
- shared canvas helpers are good
- artifact writing separated from UI for newer calibration features is good

The goal is to extend those good patterns to the rest of the GUI, not replace
them with a heavier framework.

## Open Design Questions For Review

These should be answered before major refactoring:

1. Which future windows do we expect to add soon?
   Examples:
   - DOF calibration
   - resolution/target assist tools
   - sync diagnostics
   - illumination diagnostics
   - IPC control/inspection

2. Should the main `"Orange"` window remain one large operator dashboard, or do
   we want more dedicated top-level windows for operations?

3. Should per-camera diagnostics stay embedded in tables, or should some become
   per-camera popouts/panels?

4. Do we want a persistent left-nav / tab model eventually, or keep the current
   free-form multi-window style?

5. Which parts must stay very close to the current operator workflow to avoid
   disruption?

## Non-Goals

This plan is not proposing:

- a full UI redesign
- a switch away from ImGui
- a strict MVC framework
- behavior changes to streaming/recording/calibration by default

## Review Outcome We Want Before Coding

Before starting the refactor, we should agree on:

- the list of major GUI modules we want to exist
- the first 1-2 extraction slices
- any upcoming windows/functions that should influence the shape now
