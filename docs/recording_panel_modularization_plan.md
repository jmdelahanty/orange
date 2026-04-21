# Recording Panel Modularization Plan

## Purpose

This note defines the next modularization step for Orange's GUI and runtime:
separating the current monolithic recording-related logic in `src/orange.cpp`
into:

- `src/gui/recording_panel.*`
- `src/session/recording_session.*`

The goal is to preserve validated behavior while making the recording flow
easier to extend, especially now that the branch supports:

- schema 3 camera recording config
- app-level storage config
- resolved runtime recording config
- split-GOP recording
- latest-recording pointer handling

Related follow-up:

- `docs/gui_display_recording_buffer_ownership_plan.md`

## Why This Needs A Design Pass

Unlike the earlier Host PTP and frame IPC extractions, the current "recording"
behavior is not a single self-contained panel.

It is split across three different responsibilities in `src/orange.cpp`:

1. Recording configuration UI
2. Stream-time recording pipeline lifecycle
3. Record start/stop toggle and metadata handling

If we treat all of that as "the recording panel" and move it wholesale into
`src/gui/`, we will just relocate the monolith.

## Current State

### 1. Recording Config UI

The main recording controls live inline in `src/orange.cpp` and include:

- save folder selection
- codec / preset / tuning / rate control
- quality value
- GOP length
- recording resize / downsample
- estimated bitrate summary
- per-camera recording resize overrides
- record button and active path display

This is operator-facing UI and belongs in `src/gui/`.

### 2. Recording Pipelines Are Created At Stream Start

When streaming starts, `orange.cpp`:

- allocates `CameraResources`
- resolves per-camera recording output
- builds `ResolvedRecordingConfig`
- constructs `ModernRecordingPipeline`
- starts the recording pipelines alongside display / YOLO / crop workers

When streaming stops, it:

- requests recording pipeline stop
- shuts the pipelines down during reverse-order cleanup

This is session/runtime orchestration and belongs in `src/session/`, not
`src/gui/`.

### 3. The Record Button Does Not Create Pipelines

The current record button does **not** create or destroy the recording workers.

Instead, it:

- toggles `camera_control->record_video`
- initializes the recording folder and snapshot metadata on record start
- sets `stop_record` / `recording_draining` on record stop
- exposes the active recording path in the GUI

So the correct mental model is:

- stream start/stop owns recorder pipeline lifetime
- record start/stop owns whether those pipelines are actively writing

That distinction is important and should be preserved by the modularization.

## Design Goals

1. Keep validated split-GOP recording behavior unchanged.
2. Move rendering code out of `src/orange.cpp`.
3. Move session/runtime orchestration out of the GUI layer.
4. Preserve the current immediate-mode style.
5. Introduce explicit recording UI/session boundaries without forcing a large
   rewrite.

## Proposed Module Boundary

### `src/gui/recording_panel.*`

Owns:

- rendering the recording controls
- rendering bitrate and output summaries
- rendering per-camera recording override tables
- rendering record button / active path UI
- producing narrow action requests

Does **not** own:

- creating `ModernRecordingPipeline`
- starting/stopping recording pipelines
- writing snapshots directly
- joining/shutting down worker objects

### `src/session/recording_session.*`

Owns:

- building runtime recording overrides from UI state
- creating per-camera `ModernRecordingPipeline` instances at stream start
- starting and shutting down recording pipelines
- initializing recording folders and run metadata on record start
- handling record stop / drain state transitions

Does **not** render ImGui directly.

## Recommended Transitional State Model

We do not need to redesign the whole recording config model in this step.

A pragmatic transitional shape is:

### UI Editing State

Keep the existing operator-facing editing model, but group it explicitly:

- save folder / recording root
- `EncoderConfig`
- current per-camera override state already stored on `CameraEachSelect`

Suggested wrapper:

```cpp
struct RecordingPanelState {
    std::string input_folder;
    EncoderConfig encoder_config;
};
```

This remains a GUI/session editing model, not the resolved runtime model.

### Session Runtime State

Group the current pipeline/runtime pieces explicitly:

```cpp
struct RecordingSessionState {
    std::vector<std::unique_ptr<ModernRecordingPipeline>> recording_pipelines;
};
```

That can expand later if needed, but it is enough for the first extraction.

## Recommended Action Boundary

The recording panel should not directly mutate large runtime structures beyond
its local UI state. Instead, it should return small action requests such as:

```cpp
struct RecordingPanelActions {
    bool choose_folder_requested = false;
    bool toggle_record_requested = false;
};
```

The app shell can still dispatch those actions initially, but the long-term
goal is for `src/session/recording_session.*` helpers to handle them.

## Phased Migration Plan

### Phase 1: Extract The Recording Config UI

Create `src/gui/recording_panel.*` and move only:

- save folder controls
- codec / preset / tuning / rate control controls
- GOP controls
- resize controls
- bitrate estimates
- per-camera recording resize overrides
- active recording path display

For the first slice, the panel may still directly toggle small UI-owned values
like `EncoderConfig` and `input_folder`.

Goal:

- remove the large recording UI block from `orange.cpp`
- keep behavior identical

### Phase 2: Introduce Recording Session Helpers

Create `src/session/recording_session.*` and move:

- per-camera recording output resolution for stream start
- `ResolvedRecordingConfigOverrides` population
- `ModernRecordingPipeline` construction
- recording pipeline startup/shutdown helpers

Suggested helper directions:

- `start_recording_pipelines_for_stream(...)`
- `shutdown_recording_pipelines_for_stream(...)`
- `build_gui_recording_overrides(...)`

Goal:

- make streaming lifecycle stop owning recorder details inline

### Phase 3: Move Record Toggle And Metadata Flow

Move the record start/stop behavior into session helpers:

- recording folder initialization
- snapshot creation
- PTP sync summary initialization
- `record_video` / `stop_record` / `recording_draining` transitions

Suggested helper directions:

- `begin_recording_capture(...)`
- `request_recording_stop(...)`

Goal:

- make the record button request an action instead of performing inline
  metadata/session work

### Phase 4: Tighten The UI/Session Contract

After the above slices:

- keep `src/gui/recording_panel.*` mostly ImGui and summaries
- keep `src/session/recording_session.*` mostly orchestration
- reduce remaining recording-specific glue in `orange.cpp`

Goal:

- `orange.cpp` becomes a shell that owns state and dispatches actions

## Suggested First Implementation Slice

The safest next code step is:

1. Create `src/gui/recording_panel.*`
2. Move the recording config/summary UI there
3. Keep the stream start/stop and record toggle logic in `orange.cpp`
4. Do **not** change runtime behavior yet

Why this first:

- it removes a large chunk of UI code
- it avoids mixing UI extraction with lifecycle refactoring
- it preserves the validated split-GOP path

## Explicit Non-Goals For This Step

This modularization step should **not**:

- change how `ModernRecordingPipeline` works
- change validated recording defaults
- change the resolved runtime config model
- redesign the recording state machine
- merge stream start/stop with record start/stop semantics

## Follow-Up

Once this plan is accepted, the next concrete patch should be:

- `gui: extract recording panel`

And only after that should we do:

- `session: extract recording session helpers`
