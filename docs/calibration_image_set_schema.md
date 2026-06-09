# Calibration Image Set Schema

Purpose: define a generic Orange acquisition artifact for calibration images
that Citrus can import, preview, fit, and accept into Citrus-owned calibration
state.

Date anchored: 2026-06-08.
Status: implemented for Spatial Layout generic image-set saves and still draft
for Citrus import/acceptance. The specialized top-rim writer remains
`orange.calibration.dish_top_rim_observation`.

Related documents:

- `docs/calibration_artifact_contract.md`
- `docs/dish_top_rim_observation_design.md`
- `docs/spatial_layout_contract.md`
- `docs/spatial_layout_schema.md`
- `docs/schemas/orange_calibration_image_set.schema.json`

## Boundary

Orange owns acquisition:

- source image bytes and checksums
- camera serial, image shape, and camera-native coordinate metadata
- capture settings such as exposure, frame rate, filter state, and light state
- projected-pattern metadata when Orange knows what was displayed
- review overlays and operator notes

Citrus owns interpretation:

- homography fitting
- scale fitting
- plane assumptions and optical-stack interpretation
- accepted Citrus config/runtime/H5 calibration state
- any runtime use of accepted calibration

Orange may include Citrus-space preview values, but they must be marked
diagnostic and non-authoritative. Citrus should recompute any accepted
Citrus-space transform, scale, center, or radius from the Orange camera-space
artifact plus Citrus-owned rig, homography, arena, and optical-stack state.

Orange should not silently write into Citrus calibration/config folders as the
durable contract. A compatibility export may copy files into a Citrus staging
layout, but that export is not the Orange artifact identity.

Image sets are optional calibration artifacts. They are not required for every
recording, every day, or every user workflow. A recording may reference the
latest accepted calibration state, a previous accepted image set, or no image
set at all depending on the rig policy. Validators should treat missing image
sets as normal unless a specific experiment/run profile explicitly requires a
fresh calibration capture.

## Identifiers

Payload schema:

```text
schema_id = "orange.calibration.image_set"
schema_version = 1
```

Recommended payload filename:

```text
image_set.json
```

Recommended package layout:

```text
<artifact_id>/
|-- manifest.json
|-- image_set.json
|-- captures/
|   |-- <purpose>_<timestamp>.png
|   `-- ...
|-- overlays/
|   `-- ...
`-- exports/
    `-- citrus_staging_manifest.json
```

`manifest.json` follows `docs/calibration_artifact_contract.md` with
`artifact_schema_id = "orange.calibration.image_set"`.

## Purpose

`purpose` describes the calibration task the image set supports. V1 values:

- `arena_projection`: camera observation of the Citrus arena/experimental
  definition as projected onto the target plane. This is not the authoritative
  Citrus arena definition; it is a captured image of how that definition lands
  in camera space.
- `homography_grid`: projected grid, dots, checkerboard, or equivalent pattern
  used by Citrus to fit camera-to-display homography.
- `scale_image`: ruler, dot grid, USAF target, or other known-size target used
  by Citrus to estimate pixels per millimeter on a named plane.
- `dish_top_rim`: daily or session image set used to observe the dish top rim
  in camera space. This can reference the specialized
  `orange.calibration.dish_top_rim_observation` artifact when a circle/boundary
  fit has already been accepted.
- `crosshair_alignment`: image set with projected Citrus crosshair or center
  marker used to compare projected center and observed dish/camera center.
- `camera_arena_calibration_set`: aggregate Orange artifact used by the
  Spatial Layout UI to collect multiple purpose-specific captures for one
  camera/arena during a calibration session. Individual entries in `images[]`
  carry their own `purpose`, `target_plane`, and `capture` metadata.

Do not encode the plane into `purpose`. Use `target_plane` for physical plane
semantics. Use `target_plane = "multiple"` only for aggregate containers whose
individual `images[]` entries carry their own target planes.

## Target Plane

`target_plane` names the physical plane where the calibration target or
observed feature lives. V1 uses Citrus-style names:

- `projected_surface`
- `tank_bottom_outer_surface`
- `tank_bottom_inner_surface`
- `estimated_fish_plane`
- `dish_top_rim`
- `multiple`
- `unknown`

Plane material or rig-specific construction should be metadata, not a new plane
identity. For example, use `target_plane = "projected_surface"` plus:

```json
{
  "target_plane_material": "frosted_acrylic"
}
```

or point to a Citrus/Orange tank-design reference that carries the optical stack
and material details.

`scale_image` must name the physical plane where the ruler/scale target really
was during capture. A projection-surface scale image is the normal scale
companion for homography/projector calibration, but it can still be captured
with the normal NIR strobe and a clear ruler sitting at the projection surface.

For the current circular dish workflow, a ruler sitting on the tank/dish bottom
should use:

```json
{
  "purpose": "scale_image",
  "target_plane": "tank_bottom_inner_surface",
  "runtime_role": {
    "role": "behavior_plane_proxy",
    "behavior_plane_id": "estimated_fish_plane",
    "source": "fallback_to_tank_bottom_inner_surface",
    "authority": "citrus_decides_runtime_application"
  }
}
```

Use `target_plane = "estimated_fish_plane"` only when the scale target is
actually placed or shimmed at the estimated fish body plane. For V0, any
fish-plane proxy scale is diagnostic only. Citrus can persist and report the
measured scale and compare it to projection-surface scale, but it should not
change chaser/stimulus runtime behavior until plane-specific runtime transforms
are designed and accepted.

## Required Core Fields

All `orange.calibration.image_set` payloads require:

- `schema_id`
- `schema_version`
- `artifact_id`
- `created_utc`
- `purpose`
- `target_plane`
- `coordinate_space`
- `camera`
- `capture`
- `images`

`coordinate_space` should normally be `camera_native_pixels`. `camera_view_px`
is allowed for compatibility, but artifacts meant to seed Citrus homography or
scale work should use full-resolution camera-native images whenever possible.

## Calibration Domains

`target_plane` names the physical plane where the calibration image was
captured. `observations.observed_domain` describes the camera-space domain that
Orange observed for that image set. Domain shape is per image set and per target
plane; the projection surface and tank-bottom/fish proxy plane do not need to
share the same shape.

For example, one rig may use:

- `target_plane = "projected_surface"` with a rectangular/square projector or
  shelf calibration domain.
- `target_plane = "tank_bottom_inner_surface"` with a circular dish domain.
- `runtime_role.role = "behavior_plane_proxy"` when Citrus may use the
  `tank_bottom_inner_surface` calibration as a fallback proxy for
  `estimated_fish_plane`.

For projection-surface captures, Orange writes a shape-only authored-domain
hint rather than inverse-projecting Citrus bounds:

```json
"observations": {
  "authored_domain": {
    "shape": "oriented_rectangle",
    "source": "operator_selected_projection_surface_default",
    "target_plane": "projected_surface",
    "coordinate_space": "final_display_canvas_px",
    "geometry_available": false,
    "authority": "citrus_provides_geometry"
  }
}
```

This says the projection-surface workflow is rectangular/square by default, but
Citrus remains responsible for exact canvas-space geometry and authored point
coordinates.

Orange writes observed camera-space evidence only:

```json
"observations": {
  "observed_domain": {
    "shape": "circle",
    "source": "orange_spatial_layout_runtime:manual_fit",
    "target_plane": "tank_bottom_inner_surface",
    "coordinate_space": "camera_native_pixels",
    "center_px": [2319.9, 2286.7],
    "radius_px": 2169.8,
    "outer_geometry": {"type": "circle", "cx": 2319.9, "cy": 2286.7, "r": 2169.8}
  },
  "calibration_domain": {
    "shape": "circle",
    "source": "orange_spatial_layout_runtime:manual_fit",
    "target_plane": "tank_bottom_inner_surface",
    "coordinate_space": "camera_native_pixels",
    "center_px": [2319.9, 2286.7],
    "radius_px": 2169.8,
    "outer_geometry": {"type": "circle", "cx": 2319.9, "cy": 2286.7, "r": 2169.8}
  }
}
```

`observations.authored_domain` is reserved for Citrus-authored/rendered domain
metadata such as a canvas-space circle or rectangle. Orange may write a
shape-only projection-surface hint with `geometry_available = false`, but should
not invent exact authored geometry from camera observations. If Citrus exports
expected projected points, Orange may preserve them under
`projected_pattern.expected_points`, but expected points should remain in
Citrus/canvas coordinates, not only camera coordinates.

## Homography Coordinate-Frame Invariant

Within one calibration session, homography captures that will be compared across
planes should use the same Citrus-authored coordinate frame unless the
difference is explicitly recorded in pattern metadata.

A homography is a mapping between:

```text
camera_native_pixels on one physical plane -> Citrus final_display_canvas_px
```

It is valid for the exact projected pattern/canvas coordinates used during that
capture. Operator edits to the Orange rim/dish overlay do not affect the
homography. But if the operator recenters the projected Citrus arena, changes
the pattern origin, changes the arena canvas placement, or otherwise shifts the
projected grid between projection-surface and tank-bottom captures, Citrus must
know the exact expected destination points for each capture. Otherwise a later
comparison between `H_projected_surface` and `H_tank_bottom_inner_surface` will
mix true plane/parallax effects with operator-induced coordinate-frame changes.

Recommended V0 rule:

- Keep the Citrus canvas, arena placement, and projected-pattern coordinate
  frame fixed while collecting projection-surface and tank-bottom homography
  images for one comparison.
- If the projected pattern is intentionally recentered or changed, record a
  stable `projected_pattern.pattern_id` plus enough Citrus-authored pattern
  metadata to recover exact expected points.
- Citrus should treat homographies from different authored coordinate frames as
  separate calibrations unless it can prove the expected points for each frame.
- `projected_pattern.expected_points[]`, when present, should contain stable
  point IDs and Citrus/canvas coordinates such as `canvas_px` and/or
  `arena_relative_canvas_px`.

## Spatial Layout V0 Save Workflow

The Spatial Layout UI can save generic `orange.calibration.image_set` artifacts
piecewise from the currently captured full-resolution snapshot. This is an
acquisition/provenance workflow, not a fitting workflow.

The UI groups the daily workflow into tabs:

- `Projection Surface`: arena projection, homography-grid, and scale-image
  captures at the projector/diffuser plane. Arena projection and homography
  grid captures default to visible-projector metadata and
  `suppress_mapped_strobe`. Projection-surface scale captures default to the
  HOYA R72 filter installed and `keep_or_restore_mapped_pulse` because they are
  captured with the normal NIR strobe and a clear ruler/scale target at the
  projection surface.
- `Estimated Fish Plane`: tank-bottom/fish-plane proxy captures, including
  tank-bottom homography, scale, and crosshair images. For the current circular
  dish workflow, the physical calibration target plane is
  `tank_bottom_inner_surface`, not `estimated_fish_plane`. Orange annotates
  these captures with `runtime_role.role = "behavior_plane_proxy"` when they
  may serve as Citrus' `estimated_fish_plane` fallback. Scale captures default
  to the mapped TTL NIR pulse path so ruler/target images remain visible to the
  camera; scale captures default to the HOYA R72 filter installed.
- `Dish / Valid Area`: daily dish top-rim and valid-area/mask review. This tab
  prepares top-rim capture metadata and does not imply a Citrus runtime geometry
  update by itself.

The operator selects:

- `purpose`: `arena_projection`, `homography_grid`, `scale_image`, or
  `crosshair_alignment`
- `target_plane`: for example `projected_surface`,
  `tank_bottom_inner_surface`, or `estimated_fish_plane`
- image role: for example `grid_on`, `scale_target`, or `crosshair_on`
- optional projected-pattern or scale-target descriptors
- the same capture metadata used by the top-rim save path, including filter
  state, illumination wavelength metadata, projector state, and operator notes

Each Spatial Layout save belongs to a calibration session. The first save in the
UI creates a session automatically; `Start New Calibration Session` starts a new
container for later saves. The session is scoped to the operator's current
calibration workflow, normally one rig/canvas/day, rather than one camera or one
single capture.

Generic calibration images are grouped by camera and Citrus arena inside the
session. Each click on `Save Calibration Image Set` appends one full-resolution
capture to the same camera/arena image-set artifact:

```text
calibrations/sessions/calsess_<timestamp>_<canvas>/
|-- session.json
|-- session_index.json
`-- artifacts/
    `-- Cam<serial>_<arena_id>/
        |-- captures/
        |   |-- arena_projection_<timestamp>.png
        |   |-- homography_grid_<timestamp>.png
        |   |-- scale_image_<timestamp>.png
        |   `-- crosshair_alignment_<timestamp>.png
        |-- image_set.json
        `-- manifest.json
```

The folder name is intentionally descriptive rather than sequence-based:
`Cam2010096_arena_4`, not `001`. Capture filenames are purpose-first and
timestamped so they remain understandable when copied out of the session. The
top-level `image_set.json` uses:

```text
purpose = "camera_arena_calibration_set"
target_plane = "multiple"
```

and each `images[]` entry carries the specific capture `purpose`,
`target_plane`, `capture`, checksum, coordinate space, and relative image path.

If a session contains multiple captures with the same `purpose` and
`target_plane`, Orange preserves all of them. V0 does not silently choose a
generic winner for Citrus import. Citrus should either ask the operator to pick
the capture, or use an explicit policy such as latest-by-capture-timestamp and
record that policy in its accepted Citrus artifact. Specialized links, such as
the accepted top-rim observation below, are the exception because their
`selection_policy` is written explicitly.

Specialized observations are not folded into `images[]` unless they are plain
source-image captures. For example, a manually accepted
`orange.calibration.dish_top_rim_observation` remains a first-class artifact
with its own source frame, overlays, exports, manifest, and fingerprint. The
camera/arena aggregate records the accepted rim through
`linked_observations.accepted_top_rim_observation`:

```json
{
  "linked_observations": {
    "accepted_top_rim_observation": {
      "artifact_id": "dishrim_2026-06-09T02_15_45Z_2010093",
      "artifact_schema_id": "orange.calibration.dish_top_rim_observation",
      "artifact_schema_version": 1,
      "fingerprint": "fnv1a64:...",
      "relative_manifest_path": "../dishrim_2026-06-09T02_15_45Z_2010093/manifest.json",
      "relative_observation_path": "../dishrim_2026-06-09T02_15_45Z_2010093/observation.json",
      "selection_policy": "latest_saved_for_camera_arena",
      "target_plane": "dish_top_rim",
      "coordinate_space": "camera_native_pixels",
      "camera_serial": "2010093",
      "arena_id": "arena_1",
      "canvas_id": "shadow"
    }
  }
}
```

The linked rim observation is the authoritative accepted top-rim/valid-area
fit for that camera/arena. Any older `images[].observations.observed_domain`
values inside the aggregate are capture-time hints copied from the runtime
state when those images were saved. They are useful provenance for that image,
but they should not override the latest linked top-rim observation.

The save also updates the session artifact registry in `artifacts/index.json`
and the session-level `session_index.json`. The source image is written in
full-resolution camera-native coordinates. If the current capture came from the
downsampled live preview, the UI must reject the save; use `Capture
Full-Resolution Stream Snapshot` first.

For top-rim saves from the Spatial Layout UI, `circle_detection.detected_circle`
is the already displayed Hough proposal scaled back to full-resolution
camera-native coordinates. The save path must not rerun Hough on the full image
and risk persisting a different fit from the one the operator reviewed. The
operator-confirmed/edited circle remains the load-bearing geometry in
`observed_boundary`, `accepted_mask`, and `valid_detection_region`.

This supports daily piecewise acquisition:

1. Save an `arena_projection` image showing the projected Citrus arena extent
   at the projection surface.
2. Save a specialized `dish_top_rim` observation when the rim is visible.
3. Save a `homography_grid` image at the projection surface, or an explicitly
   labeled alternate plane if the setup supports it.
4. Save a `scale_image` at the desired physical plane.
5. Save a `crosshair_alignment` image for center checks.

Citrus should import these image sets, preview/focus the relevant fit, and own
any accepted homography, scale, or runtime experimental-area correction.
Orange's generic image-set artifact does not silently mutate Citrus config.

`session_index.json` also carries latest-top-rim convenience maps when rim
observations are saved with arena context:

```json
{
  "latest_top_rim_observation_by_camera_serial": {
    "2010093": "dishrim_2026-06-09T02_15_45Z_2010093"
  },
  "latest_top_rim_observation_by_arena_artifact_id": {
    "Cam2010093_arena_1": "dishrim_2026-06-09T02_15_45Z_2010093"
  }
}
```

Consumers should prefer the per-arena aggregate link when they are importing a
camera/arena calibration set, and use the session-level maps for discovery or
repair workflows.

## Group Capture Plan

Some calibration steps should be ergonomic across the whole rig even when they
do not require strict synchronized exposure. For the current projection-surface,
estimated-fish-plane, and dish/valid-area workflow, "simultaneous" means one
operator action requests captures from multiple selected/open cameras. It does
not yet mean ChArUco/3D-calibration-grade frame alignment.

Implemented V0 group capture behavior:

- The operator prepares the calibration capture state, normally with
  `Prepare All Cameras`, so the selected light handling is applied first and
  each camera uses the intended capture timing.
- The operator clicks a group snapshot action.
- Orange sends one full-resolution snapshot request to each selected/open
  camera's `SpatialSnapshotWorker`.
- Each worker captures its next usable full-resolution frame or temporal-mean
  frame, using the same requested purpose/target-plane metadata.
- Orange records a shared `capture_group_id` and a V0 capture mode such as
  `operator_group_next_frame` or `operator_group_temporal_mean`.
- Orange freezes the image-set metadata at group-capture request time:
  purpose, target plane, image role, projected-pattern/scale-target fields,
  operator notes, illumination/filter/projector/light state, and capture timing
  metadata for each camera.
- The UI reports per-camera pending/completed/failed state.
- Completed grouped captures are shown as dynamic per-camera preview panels.
  Selecting a panel promotes that camera image into the detailed fit/editor
  preview.
- Saving writes per-camera image-set artifacts under the same calibration
  session, preserving each camera's native image geometry and Citrus
  camera/arena mapping. Grouped save uses the frozen capture-time metadata
  even if the operator changes the active tab/defaults before saving. Grouped
  saves are queued through the existing background image-set writer so the UI
  remains responsive.

This V0 mode is intended for operator convenience and consistent metadata. It
is appropriate for arena projection, homography image collection, projection
surface scale, tank-bottom/fish-plane proxy scale, crosshair alignment, and
daily top-rim review when exact same-frame timing is not required.

Deferred strict synchronized capture:

- Add an explicit capture mode such as `ptp_frame_aligned`.
- Arm all participating workers before accepting frames.
- Require PTP-enabled cameras and persist embedded camera timestamps.
- Validate timestamp/frame skew against a configured tolerance.
- Reject, retry, or warn on cameras whose captures fall outside tolerance.
- Persist per-camera skew metrics and the synchronization policy in the
  image-set metadata.
- Use this mode for future ChArUco-board or 3D calibration workflows where
  multi-camera frame alignment is part of the calibration contract.

## Plane-Specific Runtime Transform Roadmap

The current Orange implementation captures and labels calibration observations;
it does not make Citrus use a new plane-specific transform during experiments.
This distinction matters because the rig can have multiple physical planes:

- `projected_surface`: diffuser/frosted acrylic plane where the projector image
  is formed and where the current Citrus homography is usually calibrated.
- `tank_bottom_inner_surface`: physical bottom of the tank/dish.
- `estimated_fish_plane`: approximate fish body plane just above the tank
  bottom, used for perceived size and position reasoning.
- `dish_top_rim`: rim plane used for daily valid-area/detection-mask review.

The long-term plane-specific runtime model would let Citrus store and select
plane-labeled mappings, for example:

```text
H_camera_to_canvas_at_projected_surface
H_camera_to_canvas_at_estimated_fish_plane
px_per_mm_at_projected_surface
px_per_mm_at_estimated_fish_plane
dish_top_rim_valid_detection_mask_camera_px
```

Then Citrus could explicitly choose the right mapping for each runtime job:

- projector drawing and projector-surface calibration:
  `projected_surface`
- fish position interpretation and perceived size checks:
  `estimated_fish_plane`
- detection gating / invalid prediction rejection:
  `dish_top_rim` or the accepted camera-space valid mask

For V0, Orange should only produce immutable acquisition artifacts with
`purpose`, `target_plane`, full-resolution image paths, checksums, and capture
metadata. Citrus can import those artifacts, preview the relevant fit, and
persist diagnostic measurements. Citrus should not change stimulus sizing or
runtime containment based on an `estimated_fish_plane` scale image until a
plane-specific runtime-transform contract has been designed and accepted.

Implementation checklist:

- Orange V0 acquisition
  - save generic `arena_projection` image sets
  - save `dish_top_rim` observation and image-set companion
  - save generic `homography_grid` image sets
  - save generic `scale_image` image sets
  - save generic `crosshair_alignment` image sets
  - preserve `target_plane`, filter/illumination/projector state, rig context,
    and full-resolution camera-native coordinates
- Citrus import and preview
  - import `orange.calibration.image_set`
  - preview homography fit from `homography_grid`
  - preview scale fit from `scale_image`
  - preview center/crosshair correction from `crosshair_alignment`
  - recompute everything from Orange camera-space artifacts plus Citrus-owned
    rig/homography/tank-design state
- Citrus acceptance
  - write accepted Citrus-owned homography/scale/center-correction artifacts
  - reference source Orange artifact IDs/checksums
  - include target-plane assumptions and operator acceptance
  - persist accepted state to Citrus config/session/H5 as appropriate
- Runtime use
  - select plane-specific transform/scale by runtime task
  - keep diagnostic fish-plane scale separate from active stimulus behavior
    until explicitly enabled
  - record which plane transform and mask were active in each experiment

## Camera

Required fields:

- `serial`
- `image_shape.height`
- `image_shape.width`

Recommended fields:

- `name`
- `pixel_format`
- `configured_width`
- `configured_height`
- `camera_config_ref`
- `gpu_direct`

Coordinate convention:

- Pixel points are `[x, y]` or named `{ "x": ..., "y": ... }`, not row/column.
- `image_shape` is `{ "height": ..., "width": ... }`.
- Origin is top-left, `x` right, `y` down, matching OpenCV image pixels.

## Capture

Recommended fields:

- `operation_id`
- `capture_group_id`
- `timestamp_utc`
- `frame_id`
- `recording_frame_id`
- `capture_mode`
- `exposure_us`
- `frame_rate_hz`
- `gain`
- `filter_state`
- `runtime_filter_state`
- `light_handling`
- `light_state`
- `projector_state`
- `projector_visible_to_camera`
- `requires_camera_mount_unchanged`
- `requires_filter_reinstalled_repeatably`

Use explicit `filter_state`, `light_handling`, and `light_state` values because
some Orange rigs normally run with 850 nm bandpass filters and TTL-driven
infrared strobes. A visible-light calibration image may require filter removal,
different exposure, and a nonstandard illumination/preflight state, while a
`scale_image` may intentionally keep the IR strobe active so a ruler or scale
target is visible to the camera.

`light_handling` records operator intent for the capture:

- `leave_current`: do not intentionally change mapped lighting state.
- `suppress_mapped_strobe`: suppress an exposure-synchronized mapped strobe for
  the capture.
- `keep_or_restore_mapped_pulse`: keep or restore the normal mapped
  exposure-pulse state before capture.
- `force_manual_active`: intentionally force a mapped output active.
- `operator_manual`: operator handled lighting outside Orange.

The Spatial Layout UI sets purpose/plane-specific defaults but lets the
operator override them. `arena_projection`, `homography_grid`, and
`crosshair_alignment` default to `suppress_mapped_strobe`; projection-surface
scale images and tank-bottom/fish-plane proxy scale images default to
`keep_or_restore_mapped_pulse`.

When a camera has an exposed `nir_strobe_trigger` Rig I/O output mapping, the
Spatial Layout UI can apply the selected light handling before capture. The
light-control camera does not have to be the camera being calibrated; for
example, one rig camera may own the TTL strobe output while another camera is
capturing the calibration image.

- `suppress_mapped_strobe` captures the current mapped GPO state and forces the
  mapped output inactive.
- `keep_or_restore_mapped_pulse` restores the captured state when available, or
  restores the mapping's normal output mode.
- `force_manual_active` captures the current state and forces the mapped output
  active.

These actions are explicit operator clicks, not automatic side effects of
saving an artifact. They are disabled while recording/finalization is active and
when `gpio_pinout_access = "not_exposed"`.

The normal operator path is a paired preflight/restore action:

- Prefer `Capture Averaged Full-Resolution Snapshot` for static calibration
  targets when the existing stream exposure is usable. This collects a bounded
  number of full-resolution stream frames without changing camera exposure or
  frame rate, computes a temporal mean, and records
  `capture_mode = "temporal_mean_stream_frames_v1"`,
  `source_frame_count`, and the source frame range in the artifact metadata.
- `Prepare Calibration Capture` applies the selected light handling and
  temporarily sets the selected image-capture camera to `FrameRate = 10 fps`
  and `Exposure = 10000 us` where the camera permits those values. This is
  useful when normal-exposure averaging is still too dim or quantized near
  black. Orange applies this as an app-level calibration capture profile:
  it snapshots the current camera/light state first, writes camera control
  nodes sequentially, verifies/readbacks through the camera API, and rolls back
  already-applied state if a later step fails. The underlying camera API writes
  are not hardware-atomic. V0 targets the selected capture camera and the
  selected light-control camera; a future all-camera profile should snapshot all
  target states before applying settings camera by camera.
- `Restore Camera Config State` restores the selected camera's captured
  exposure/frame-rate and restores the mapped strobe output to its config-defined
  normal pulse mode.

This temporary preflight state is not saved to the camera config. If the
operator saves an image set after preparing the capture, the artifact records
the actual current exposure and frame rate in its capture metadata.

## Images

Each item in `images[]` should include:

- `role`
- `path`
- `checksum_algorithm`
- `checksum`
- `coordinate_space`
- `image_shape`

Recommended roles:

- `source`
- `projector_off`
- `projector_on`
- `crosshair_on`
- `grid_on`
- `scale_target`
- `difference`
- `overlay`

`path` is relative to the artifact package unless explicitly marked otherwise.

## Rig Context

When known, include:

- `rig_id`
- `canvas_id` or `canvas_name`
- `arena_id`
- `camera_id`
- `citrus_config_ref`
- `citrus_homography_ref`
- `tank_design_ref`
- `plane_stack_ref`

These fields are provenance and import hints. They do not grant Orange
authority to mutate Citrus config. Citrus should validate and resolve them
during import.

## Purpose-Specific Fields

### `arena_projection`

Recommended:

- `projected_pattern.pattern_id`
- `projected_pattern.type = "arena_fill"` or equivalent
- `projected_pattern.source = "citrus_arena_definition"`
- `projected_pattern.target_plane`
- `rig_context.arena_id`
- `rig_context.citrus_config_ref`

This purpose captures the projected Citrus arena/experimental definition as a
camera image. It is not the authoritative arena definition itself. Citrus owns
the arena definition and may use this image as a visual/provenance artifact
when previewing the projection surface and camera/arena association.

### `homography_grid`

Recommended:

- `projected_pattern.pattern_id`
- `projected_pattern.type`
- `projected_pattern.rows`
- `projected_pattern.cols`
- `projected_pattern.spacing_canvas_px`
- `projected_pattern.dot_radius_canvas_px`
- `projected_pattern.canvas_coordinate_space`
- `projected_pattern.projected_surface_ref`
- optional `projected_pattern.expected_points[]`, copied from Citrus-rendered
  metadata when available. Expected points should be in Citrus/canvas
  coordinates, such as `canvas_px` and/or `arena_relative_canvas_px`, with
  stable point IDs.
- optional `projected_pattern.pattern_snapshot`, copied from Citrus-rendered
  metadata when available. This should preserve exact pattern parameters needed
  to recover expected points, such as grid rows/columns, ring counts, dot
  counts, radii, arena-relative center, canvas size/placement, mask/domain
  policy, and a Citrus pattern version/hash.
- `observations.observed_domain` for the camera-space domain where the
  calibration is valid. This can be circular, square, or rectangular depending
  on the physical target plane and rig.

Citrus uses these fields to preview and fit homography. Orange may provide
detected camera points as diagnostic observations, but Citrus owns accepted
point correspondences and homography sidecars. When Orange only records
`projected_pattern.pattern_id` and `projected_pattern.type`, Citrus should
resolve the exact expected-point set from its own pattern registry/config at
capture time rather than asking Orange to infer it later.

For the current circular dish/tank-bottom workflow, use:

```json
{
  "purpose": "homography_grid",
  "target_plane": "tank_bottom_inner_surface",
  "runtime_role": {
    "role": "behavior_plane_proxy",
    "behavior_plane_id": "estimated_fish_plane",
    "source": "fallback_to_tank_bottom_inner_surface"
  },
  "projected_pattern": {
    "pattern_id": "citrus_tank_bottom_circular_grid_v1",
    "type": "circular_dot_grid",
    "target_plane": "tank_bottom_inner_surface"
  }
}
```

For a rectangular or square tank-bottom domain, keep the same
`target_plane = "tank_bottom_inner_surface"` and write
`observations.observed_domain.shape = "oriented_rectangle"` with the observed
camera-space geometry. The physical domain shape and the projected pattern shape
do not need to match the projection-surface calibration.

### `scale_image`

Recommended:

- `scale_target.type`
- `scale_target.known_spacing_mm`
- `scale_target.known_distance_mm`
- `scale_target.units`
- `scale_target.target_plane`
- `scale_target.target_plane_material`

The `target_plane` must name where the scale target physically was. For the
current circular dish workflow, a ruler on the tank bottom should use
`target_plane = "tank_bottom_inner_surface"` and a `runtime_role` marking it as
a behavior-plane proxy for `estimated_fish_plane`. Use
`target_plane = "estimated_fish_plane"` only when the scale target is actually
raised to the estimated fish body plane. Citrus should treat fish-plane proxy
measurements as diagnostic until plane-specific runtime transforms exist.

### `dish_top_rim`

Recommended:

- `observations.dish_top_rim.accepted_boundary`
- `observations.dish_top_rim.sampled_boundary_points_camera_px`
- `observations.dish_top_rim.fit_quality`
- `derived_artifacts[]` reference to
  `orange.calibration.dish_top_rim_observation`, if the specialized top-rim
  artifact has been written.

This image-set purpose is the generic acquisition envelope. The specialized
top-rim observation remains the load-bearing circle/boundary fit artifact.

### `crosshair_alignment`

Recommended:

- `projected_pattern.pattern_id`
- `projected_pattern.type = "crosshair"`
- `projected_pattern.expected_center_canvas_px`
- `projected_pattern.coordinate_frames[]` naming
  `camera_view_px`, `final_display_canvas_px`, and
  `arena_relative_canvas_px`
- `projected_pattern.homography.direction =
  "camera_view_px_to_final_display_canvas_px"` when a Citrus sidecar matrix is
  referenced
- `projected_pattern.arena_region` with Citrus arena center, size, and derived
  origin in final-display canvas pixels
- `projected_pattern.experimental_area` with center/radius/shape in
  arena-relative canvas pixels
- `projected_pattern.sampled_outline_points[]` when available, with stable
  point ids and coordinates in arena-relative canvas, final-display canvas, and
  camera pixels. Camera pixels should be computed by applying
  `inverse(H_camera_to_display)` to final-display canvas points.
- `projected_pattern.crosshair_segments[]` with line endpoints in the same
  coordinate frames when available.
- `observations.crosshair.detected_center_camera_px`
- `observations.crosshair.fit_quality`
- `observations.top_rim_or_experimental_area` reference or copy of the Orange
  Hough/top-rim center/radius/boundary evidence used for mismatch comparison
- `observations.mismatch_metrics` such as center delta, radial residuals, and
  outline-to-Hough distances when Orange computes them

Crosshair alignment is useful for daily center checks, but Citrus still owns
accepted center/radius correction semantics. Orange may include the
camera-space sampled Citrus outline as diagnostic preview evidence, but Citrus
should recompute the outline and any accepted correction from its own arena,
homography, and tank/plane state.

## Citrus Import

Recommended long-term Citrus import methods:

- `import_calibration_image_set`
- `preview_homography_calibration`
- `accept_homography_calibration`
- `preview_scale_calibration`
- `accept_scale_calibration`

For V0, a manual Citrus GUI import from an Orange artifact path is acceptable.
If Orange provides a Citrus-compatible staging export, it should write an
explicit export manifest and keep the Orange artifact as the durable source of
truth.

Current Citrus compatibility paths may look like:

```text
targets/rigs/<rig>/<canvas>/calibration_artifacts/
```

with names such as:

```text
Cam2010093_arena1_homography.png
Cam2010093_arena1_scale.png
homography_<arena_config_name>_<camera_id>.yml
```

Those names are compatibility details, not the canonical Orange contract.

If `observations.authored_domain.geometry_available = false`, Orange is saying
that the Citrus-authored shape exists conceptually but was not serialized as
authoritative geometry in the Orange artifact. Citrus should resolve that
geometry from `rig_context`, `projected_pattern.pattern_id`, its own canvas and
arena config, or a future Citrus-exported `projected_pattern.pattern_snapshot`.
Orange should not invent exact Citrus authored bounds from camera-space images.

## Example

```json
{
  "schema_id": "orange.calibration.image_set",
  "schema_version": 1,
  "artifact_id": "calimg_20260608_154500_2010093_homography_grid",
  "created_utc": "2026-06-08T19:45:00Z",
  "purpose": "homography_grid",
  "target_plane": "projected_surface",
  "coordinate_space": "camera_native_pixels",
  "camera": {
    "serial": "2010093",
    "name": "Cam2010093",
    "image_shape": {"height": 4512, "width": 4512},
    "pixel_format": "Mono8"
  },
  "rig_context": {
    "rig_id": "omnifin0",
    "canvas_id": "shadow",
    "arena_id": "arena_1",
    "citrus_config_ref": {
      "source": "manual_import",
      "config_name": "good-cop-bad-cop-shadow"
    }
  },
  "capture": {
    "operation_id": "daily_homography_20260608",
    "timestamp_utc": "2026-06-08T19:45:00Z",
    "capture_mode": "visible_projected_grid",
    "filter_state": "removed",
    "runtime_filter_state": "850nm_bandpass_installed",
    "exposure_us": 500000,
    "frame_rate_hz": 1.0,
    "light_handling": "suppress_mapped_strobe",
    "light_state": "visible_projector_only",
    "projector_state": "grid_on",
    "projector_visible_to_camera": true,
    "requires_camera_mount_unchanged": true,
    "requires_filter_reinstalled_repeatably": true
  },
  "images": [
    {
      "role": "grid_on",
      "path": "images/grid_on.png",
      "checksum_algorithm": "fnv1a64",
      "checksum": "fnv1a64:...",
      "coordinate_space": "camera_native_pixels",
      "image_shape": {"height": 4512, "width": 4512}
    }
  ],
  "projected_pattern": {
    "pattern_id": "citrus_homography_grid_v1",
    "type": "dot_grid",
    "rows": 11,
    "cols": 11,
    "spacing_canvas_px": 32.0,
    "dot_radius_canvas_px": 3.0,
    "canvas_coordinate_space": "final_display_canvas_px"
  },
  "review_artifacts": {
    "overlay_path": "overlays/grid_detection_preview.png",
    "overlay_checksum_algorithm": "fnv1a64",
    "overlay_checksum": "fnv1a64:..."
  },
  "citrus_preview": {
    "available": true,
    "diagnostic_only": true,
    "authority": "citrus_recomputes_before_acceptance"
  }
}
```
