# Spatial Layout UI Refactor Plan

## Status

The spatial layout UI started as one large `src/spatial_layout_ui.cpp` file that mixed
state, geometry, Citrus import, camera preflight, capture panels, Hough tuning,
metadata editing, persistence controls, and artifact writing.

The first refactor pass keeps behavior intact while carving out stable module
boundaries under `src/gui/spatial_layout/`:

- `state.h`: shared UI state and small data structs.
- `geometry.{h,cpp}`: camera/canvas coordinate helpers and circle fitting helpers.
- `citrus_import.{h,cpp}`: Citrus template discovery/import and homography sidecar loading.
- `preflight.{h,cpp}`: calibration capture light/GPIO/timing prepare and restore logic.
- `session_io.{h,cpp}`: calibration session paths, manifests, JSON/image IO, and artifact IDs.
- `hough_panel.{h,cpp}`: dish/valid-area Hough tuning controls and proposal overlay state updates.
- `capture_panel.{h,cpp}`: grouped capture preview panels.
- `metadata_panel.{h,cpp}`: calibration capture metadata and generic image-set controls.
- `persistence_panel.{h,cpp}`: persistence buttons, JSON preview, and save/load event selection.

`src/spatial_layout_ui.cpp` remains the coordinator for now. It still owns the
artifact save workers, schema assembly, load/save side effects, and the top-level
window orchestration.

## Refactor Rules

- Keep each extraction behavior-preserving unless there is a separate operator-facing
  change request.
- Move ImGui panel rendering into GUI modules.
- Move non-ImGui IO and camera-control work into service modules.
- Keep artifact mutation and worker submission in one owner until the save pipeline
  has its own service boundary.
- Prefer explicit state/event structs over hidden globals or callbacks with unclear
  ownership.

## Next Targets

1. Extract artifact save preparation and save-worker coordination from
   `spatial_layout_ui.cpp` into a persistence service.
2. Extract schema/runtime preview assembly into a layout preview service.
3. Extract top-level workflow tab rendering into a small workflow panel.
4. Add focused unit tests for path/session helpers and pure geometry helpers where
   they can run without a GUI or camera.

The desired end state is a thin `render_spatial_layout_window(...)` coordinator that
selects the camera/template, polls background jobs, delegates panels, and dispatches
high-level events.
