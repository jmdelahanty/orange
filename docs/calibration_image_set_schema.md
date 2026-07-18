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

## Citrus Active Projection Provenance

When Citrus is running and exposing its local-control socket, Orange can
capture the active Citrus projection state around each calibration image
capture. This is optional, read-only provenance. It does not grant Orange
authority to mutate Citrus config, accept a homography, or decide runtime
stimulus behavior.

Orange queries Citrus with:

```json
{
  "schema_id": "citrus.local_control.request",
  "schema_version": 1,
  "method": "active_projection_snapshot",
  "request_id": "projection-snapshot-pre-<capture-id>",
  "source": "orange"
}
```

Orange consumes only:

```text
effect.active_projection_snapshot
```

from the Citrus response and embeds that object directly on the corresponding
`images[]` entry:

```json
{
  "citrus_projection_snapshot_pre_capture": {
    "schema_id": "citrus.active_projection_snapshot",
    "schema_version": 1,
    "projection_epoch_id": "fnv1a64:...",
    "status": "active",
    "projection_count": 4,
    "coordinate_contract": {
      "arena_relative_coordinate_space": "arena_relative_canvas_px",
      "final_display_coordinate_space": "final_display_canvas_px",
      "point_origin": "dot_center",
      "units": "px",
      "frame_authority": "citrus_runtime"
    },
    "projections": []
  },
  "citrus_projection_snapshot_post_capture": {
    "schema_id": "citrus.active_projection_snapshot",
    "projection_epoch_id": "fnv1a64:..."
  },
  "citrus_projection_epoch_consistency": {
    "status": "same_epoch",
    "projection_epoch_id": "fnv1a64:...",
    "blocking_or_warning_reason": "",
    "policy": "warn_only_v0"
  }
}
```

`citrus_projection_epoch_consistency.status` values:

- `same_epoch`: pre/post snapshots were present and had the same
  `projection_epoch_id`.
- `changed_epoch`: Citrus projection content changed between pre-capture and
  post-capture.
- `unavailable`: Citrus was not configured/running, did not respond, reported
  inactive projection status, or did not provide enough epoch metadata.
- `metadata_mismatch`: the snapshot did not contain a projection matching the
  Orange camera/arena context.

For V0, missing, inactive, changed, or mismatched snapshots are warn-only and
must not block saving. This lets existing manual calibration workflows continue
while giving Citrus enough durable provenance to prove which final-display
points were actually active during a capture.

`target_plane` remains Orange image-set authority because it describes where
the physical target or observed feature was placed. The Citrus active projection
snapshot only records what Citrus was rendering, including expected points in
Citrus-owned coordinate spaces such as `final_display_canvas_px` and
`arena_relative_canvas_px`.

Citrus `projections[].projected_pattern.mode` V0 values are:

- `rectangular_grid`
- `circular_rings`
- `verification_dots`
- `validation_pattern`

`verification_dots` is a sparse held-out calibration/probe pattern constrained
to the configured experimental area. It is not a separate experimental-area
concept and should not replace the primary homography fitting point set. It is
intended for validation and parallax/residual diagnostics, and may later
support a separate accepted correction model if Citrus defines one. Orange
should not use or emit the retired `experimental_area_verification` name.

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

For Spatial Layout session saves, this generic package is usually the
camera/arena aggregate directory, for example
`calibrations/sessions/<session_id>/artifacts/Cam2010093_arena_1/`. Multiple
purpose-specific captures append to that aggregate instead of creating a new
top-level artifact directory for every image.

## Purpose

`purpose` describes the calibration task the image set supports. V1 values:

- `arena_projection`: camera observation of the Citrus arena/experimental
  definition as projected onto the target plane. This is not the authoritative
  Citrus arena definition; it is a captured image of how that definition lands
  in camera space.
- `homography_grid`: projected grid, dots, checkerboard, or equivalent pattern
  used by Citrus to fit camera-to-display homography.
- `verification_dots`: sparse held-out projected dots used to validate fitted
  homographies, measure residual/parallax behavior, and support future
  secondary correction models if Citrus defines one. In the current workflow,
  this purpose is projection-surface validation only and must not replace the
  primary `homography_grid` fitting point set.
- `validation_pattern`: projected validation/probe pattern capture used after
  a homography or scale image in the same stage. It records what the operator
  saw for validation and drift/debug review; Citrus still owns any accepted
  fit or correction.
- `scale_image`: ruler, dot grid, USAF target, or other known-size target used
  by Citrus to estimate pixels per millimeter on a named plane.
- `dish_top_rim`: daily or session image set used to observe the dish top rim
  in camera space. This can reference the specialized
  `orange.calibration.dish_top_rim_observation` artifact when a circle/boundary
  fit has already been accepted.
- `crosshair_alignment`: image set with projected Citrus crosshair or center
  marker used to compare projected center and observed dish/camera center.
- `dry_physical_target_height_parallax_diagnostic`: dry physical-target capture
  at a named height used to estimate camera/lens height-parallax behavior
  without the runtime water path. Current Orange physical-plane defaults use
  this purpose, `wet_or_dry = "dry"`, and `reference_only = true`. These
  captures are diagnostic/provenance products, not runtime correction-ready
  wet recording-condition maps.
- `camera_only_physical_target_calibration`: physical-target capture intended
  for a camera-only `C_z: camera_px -> physical_mm` map under explicit capture
  conditions. This is the purpose to use for future wet/runtime-condition
  physical C-plane calibration once the physical target detection and water-path
  protocol are ready.
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

## Capture Stage And Wet Matching

`capture_stage` describes the operator workflow block. It is separate from
`purpose` because each stage can contain a physical-target image,
projector-surface validation image, scale image, homography image, crosshair
image, or validation pattern.

V1 stage values:

- `projected_surface_dry_reference`: dry projection surface / gel reference.
  This is useful for historical comparison, drift checks, and debugging. It
  must use `target_plane = "projected_surface"`,
  `wet_or_dry = "dry"`, `parity_group_role = "dry_reference"`, and
  `reference_only = true`. It records the non-wetted full camera-space
  projection-surface reference for a given rig/canvas/camera configuration.
- `projected_surface_wet_runtime_stack`: projection-surface capture after the
  imaging shelf and target dish assembly are installed, water is at recording
  fill level, the open water surface is present and settled, and the projected
  calibration pattern is emitted onto the gel/projection surface. This is
  projector/render validation at the projection surface only; it is not a
  physical tank-bottom or fish-height calibration.
- `camera_physical_projected_surface`: camera-only physical-target capture for
  `C_0`, mapping `camera_px -> physical_mm` at the projection/gel surface.
- `camera_physical_dish_base_inner_surface`: camera-only physical-target
  capture for `C_base`, mapping `camera_px -> physical_mm` at the dish/base
  inner surface.
- `camera_physical_fish_height`: camera-only physical-target capture for
  `C_fish`, mapping `camera_px -> physical_mm` at approximate fish height.
  This requires the physical target to be placed or shimmed at the assumed fish
  plane.
- `projector_surface_validation`: optional explicit projector/render validation
  at the projection surface. This stage may carry projected-pattern/canvas
  metadata, but it must not be selected as a physical `C_z` map.
- `dish_top_observation`: existing dish top-rim / valid-area observation set.

Current Orange physical-target defaults are dry height-parallax diagnostics.
For those saves, use `purpose = "dry_physical_target_height_parallax_diagnostic"`,
`wet_or_dry = "dry"`, `fill_state = "dry_or_empty"` or `"not_applicable"`,
`open_water_surface_present = false`, `water_settled_status = "not_applicable"`,
and `reference_only = true`. The three dry diagnostic stages should still share
one `parity_group_id` so analysis can compare height-dependent camera/lens
behavior without mixing in projector/canvas effects.

Future wet/runtime-condition physical-target calibration should use
`purpose = "camera_only_physical_target_calibration"` only when the physical
target capture includes the intended runtime water path and the target detection
protocol is reliable. In that case, the three physical-target stages in one
settled runtime stack should be captured back-to-back with the same dish, arena
slot, water fill, camera, lens/filter/focus, and rig state. The open water
surface should be present if that matches recording conditions, and the
target/water should settle before each capture.

Projected-pattern captures at a raised diffuser/screen, dish/base height, or
fish height are legacy diagnostics only. Do not label those moved-diffuser
captures as `camera_physical_dish_base_inner_surface`,
`camera_physical_fish_height`, physical tank-bottom calibration, or measured
fish-plane calibration.

Recommended metadata for each stage includes:

- `plane_z_mm_nominal` and `plane_z_mm_uncertainty`
- `wet_or_dry`
- `imaging_shelf_installed`
- `dish_installed`
- `dish_id`, when available
- `water_fill_mm` or `fill_state`
- `open_water_surface_present`
- `water_settled_status`
- `target_method`
- `pattern_type`
- `pattern_domain`
- `capture_timestamp_utc`, matching `capture.timestamp_utc`
- `matched_parity_group_id`
- `parity_group_id`
- `parity_group_role`
- `reference_only`
- `physical_target_used`
- `projected_pattern_used_as_coordinate_target`
- `target_id`
- `target_design`
- `physical_target_grid_spacing_mm` or `physical_target.known_points_mm`
- `physical_target_origin_definition`
- `physical_target_x_orientation_marker_definition`
- `plane_id`
- `z_mm_relative_to_projection_surface`

For the current setup, wet projection-surface homography uses
`pattern_type = "circular_rings"` and remains a projection-surface
projector/render validation capture. The camera-only physical `C_z` maps must
use a physical target with known XY coordinates and must set
`projected_pattern_used_as_coordinate_target = false`. Current Orange
physical-target captures are dry diagnostics and additionally set
`reference_only = true`.

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

## Camera Raster Coordinate Convention

For Orange-acquired calibration images, the source of truth is the camera raster
as displayed by the normal Orange live stream and by the grouped-capture
thumbnails.

Coordinate convention:

- `(0, 0)` is the top-left pixel of the camera raster.
- `x` increases to the right.
- `y` increases downward.
- Pixel points are `[x, y]` or named `{ "x": ..., "y": ... }`, not
  row/column.
- `image_shape` is `{ "height": ..., "width": ... }`.

This matches OpenCV image pixel coordinates and the TIFF orientation convention
reported by ImageMagick as `TopLeft`. A 4512 x 4512 TIFF exported from the
vendor camera acquisition path was checked on 2026-06-10 by marking the literal
top-left raster pixel `(0,0)`; the marked image agreed with the Orange live
stream orientation. If an Orange preview canvas displays a calibration image
flipped relative to the live stream or grouped-capture thumbnail, the preview
canvas is wrong. Do not flip saved source images, Hough/manual fit
coordinates, or artifact geometry to compensate for a display-only inversion.

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

Projected-pattern homography captures and camera-only physical-target
homography captures use different coordinate targets and must not be mixed.

A projector/render homography is a mapping between:

```text
camera_native_pixels at the projection/gel surface -> Citrus final_display_canvas_px
```

It is valid for the exact projected pattern/canvas coordinates used at the
projection surface. Operator edits to the Orange rim/dish overlay do not affect
this homography. If the operator recenters the projected Citrus arena, changes
the pattern origin, or changes arena canvas placement, Citrus must know the
exact expected destination points for that projection-surface capture.

A camera-only physical plane map is a mapping between:

```text
camera_native_pixels on one physical target plane -> physical_mm on that target
```

These maps are the `C_0`, `C_base`, and `C_fish` products. Their coordinate
target is a printed, etched, or otherwise physical target with known XY
coordinates, origin, and +x orientation marker. Projected dot/grid/canvas
coordinates are not valid coordinate targets for these maps.

Recommended V0 rule:

- Keep the Citrus canvas, arena placement, and projected-pattern coordinate
  frame fixed while collecting projection-surface projector validation images.
- If the projected pattern is intentionally recentered or changed, record a
  stable `projected_pattern.pattern_id` plus enough Citrus-authored pattern
  metadata to recover exact expected points.
- Citrus should treat homographies from different authored coordinate frames as
  separate calibrations unless it can prove the expected points for each frame.
- `projected_pattern.expected_points[]`, when present, should contain stable
  point IDs and Citrus/canvas coordinates such as `canvas_px` and/or
  `arena_relative_canvas_px`.
- For `dry_physical_target_height_parallax_diagnostic` and
  `camera_only_physical_target_calibration`, ignore projected-pattern expected
  points and fit only against `physical_target` known coordinates.

## Spatial Layout V0 Save Workflow

The Spatial Layout UI can save generic `orange.calibration.image_set` artifacts
piecewise from the currently captured full-resolution snapshot. This is an
acquisition/provenance workflow, not a fitting workflow.

The UI groups the daily workflow into tabs:

- `Camera Physical Planes`: dry physical-target captures at the `C_0`,
  `C_base`, and `C_fish` nominal heights. The target must have known physical
  XY coordinates, origin, and +x orientation marker. Projector coordinates must
  not be used as coordinate targets. Current Orange defaults use
  `purpose = "dry_physical_target_height_parallax_diagnostic"`,
  `wet_or_dry = "dry"`, `reference_only = true`,
  `physical_target_used = true`, and
  `projected_pattern_used_as_coordinate_target = false`. These captures are for
  dry camera/lens height-parallax diagnostics and are not runtime correction
  ready.
- `Projector Surface Validation`: dry projection-surface reference captures
  before the dish assembly/imaging shelf is installed. This block supports full
  camera-space arena definition, dry projection-surface homography, and
  validation pattern captures. These saves use
  `capture_stage = "projected_surface_dry_reference"` and
  `reference_only = true`.
- `Wet Projection Surface`: wet runtime-stack projection-surface captures
  after the imaging shelf and dish assembly are installed at the target arena
  slot and water is at recording fill level. This block supports projection
  surface scale, homography with rings, experimental-area crosshair, and
  validation pattern captures. These saves use
  `capture_stage = "projected_surface_wet_runtime_stack"` and
  `parity_group_role = "wet_projected_surface"`. They are
  projection-surface projector/render validation captures, not physical
  tank-bottom or fish-height calibration maps.
- `Dish / Valid Area`: existing dish top-rim observation and valid-area/mask
  review. This tab prepares top-rim capture metadata and does not imply a
  Citrus runtime geometry update by itself.

The `Camera Physical Planes` captures share a `parity_group_id` so analysis can
fit and compare dry `C_0`, `C_base`, and `C_fish` diagnostic maps without
mixing in projector or canvas effects. Citrus must not treat the dry diagnostic
set as accepted wet runtime C-plane calibration. Future wet
`camera_only_physical_target_calibration` captures should use their own matched
physical C-plane group. The wet projection-surface projector-validation
captures may also carry a group ID for provenance, but that group is not the
physical camera-only C-plane group.

The operator selects:

- `purpose`: `dry_physical_target_height_parallax_diagnostic`,
  `camera_only_physical_target_calibration`,
  `arena_projection`, `homography_grid`, `verification_dots`,
  `validation_pattern`, `scale_image`, or `crosshair_alignment`
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

Spatial Layout must not write new artifacts to the retired top-level
`calibrations/artifacts` folder. That folder may exist on older systems, but
new image sets, top-rim observations, and arena-layout artifacts are session
members under `calibrations/sessions/<session_id>/artifacts`.

Generic calibration images are grouped by camera and Citrus arena inside the
session. Each click on `Save Calibration Image Set` appends one full-resolution
capture to the same camera/arena image-set artifact:

```text
calibrations/sessions/calsess_<timestamp>_<canvas>/
|-- session.json
|-- session_index.json
|-- arena_layout_set.json
`-- artifacts/
    `-- Cam<serial>_<arena_id>/
        |-- captures/
        |   |-- arena_projection_<timestamp>.png
        |   |-- homography_grid_<timestamp>.png
        |   |-- verification_dots_<timestamp>.png
        |   |-- validation_pattern_<timestamp>.png
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

New rim observations are stored inside the camera/arena aggregate folder, not
as direct siblings of `Cam<serial>_<arena_id>`. Repeated daily or same-session
rim fits append immutable artifacts under a typed subfolder:

```text
artifacts/
`-- Cam2010093_arena_1/
    |-- image_set.json
    |-- manifest.json
    |-- captures/
    `-- top_rim_observations/
        |-- dishrim_2026-06-09T02_15_45Z_2010093/
        `-- dishrim_2026-06-09T02_30_10Z_2010093/
```

```json
{
  "linked_observations": {
    "accepted_top_rim_observation": {
      "artifact_id": "dishrim_2026-06-09T02_15_45Z_2010093",
      "artifact_schema_id": "orange.calibration.dish_top_rim_observation",
      "artifact_schema_version": 1,
      "fingerprint": "fnv1a64:...",
      "relative_manifest_path": "top_rim_observations/dishrim_2026-06-09T02_15_45Z_2010093/manifest.json",
      "relative_observation_path": "top_rim_observations/dishrim_2026-06-09T02_15_45Z_2010093/observation.json",
      "selection_policy": "latest_saved_for_camera_arena",
      "target_plane": "dish_top_rim",
      "coordinate_space": "camera_native_pixels",
      "camera_serial": "2010093",
      "arena_id": "arena_1",
      "canvas_id": "shadow"
    },
    "top_rim_observation_history": [
      {
        "artifact_id": "dishrim_2026-06-09T02_15_45Z_2010093",
        "relative_manifest_path": "top_rim_observations/dishrim_2026-06-09T02_15_45Z_2010093/manifest.json",
        "relative_observation_path": "top_rim_observations/dishrim_2026-06-09T02_15_45Z_2010093/observation.json",
        "selection_policy": "latest_saved_for_camera_arena"
      }
    ]
  }
}
```

Arena layout saves follow the same session rule. Each `Save Arena Layout
Artifact` click writes a new immutable per-arena artifact under `artifacts/`,
then updates the session-level `arena_layout_set.json` companion so an operator
can build the full canvas one arena at a time. If an arena is saved again, the
set points at the newest saved artifact for that camera/canvas/arena while the
older artifact remains available for audit.

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

The Spatial Layout UI can reload a saved session by opening `session.json` or
`session_index.json`. The current review slice reads the session index, lists
reviewable saved images from aggregate `orange.calibration.image_set`
artifacts and linked `orange.calibration.dish_top_rim_observation`
source/overlay images, and loads the selected PNG back into the preview panel
with its saved metadata. This is a review aid only; it does not mutate the
session, refit Citrus homographies, or mark any artifact accepted.

When a session is created after importing a Citrus canvas config, `session.json`
records the full source config reference under
`context.citrus.citrus_config_ref.path`. Loading that session attempts to
auto-load the same Citrus canvas config into the Spatial Layout UI. Older
sessions may not have this session-level field; the loader also recovers the
path from per-artifact `rig_context.citrus_config_ref.path` or top-rim
`arena_context.citrus_config_ref.path` when present.

On load, Orange also builds a camera-first, plane-aware review summary from the
flat artifact list. The UI exposes these groups as review navigation metadata:
camera, then calibration level / target plane, then individual images. Each
image row shows Citrus canvas and arena identifiers when known so operators can
see, for example, `Cam2010096 -> Projection Surface -> arena_4 homography_grid`
without repeating canvas/arena in every tree label.

- `Projection Surface`: `target_plane = "projected_surface"` captures such as
  arena projection, projection homography grids, projection scale images, and
  projection-surface crosshair checks, plus `C_0` physical-target captures.
- `Tank Bottom Inner Surface`: `target_plane = "tank_bottom_inner_surface"`
  captures such as `C_base` physical-target captures or physical scale images.
  Projected-pattern tank-bottom homography grids are legacy diagnostics and
  must not be treated as physical tank-bottom calibration.
- `Estimated Fish Plane`: `target_plane = "estimated_fish_plane"` captures
  such as `C_fish` physical-target captures when the target is physically
  shimmed or placed at assumed fish height.
- `Dish / Valid Area`: specialized top-rim and accepted mask artifacts.

The loader keeps the existing image picker as the active review mechanism, but
the grouped summary makes it clear which physical plane each image belongs to.
It also surfaces warnings for missing purpose/role/target-plane metadata,
unknown target planes, legacy `estimated_fish_plane` captures, missing
homography pattern IDs, and circular tank-bottom captures without a linked
accepted top-rim observation.

If an aggregate image set has
`linked_observations.accepted_top_rim_observation`, Orange treats that linked
fit as the authoritative accepted dish/top-rim geometry for review overlays.
Any older `images[].observations.observed_domain` values remain visible as
capture-time diagnostic hints, but the linked accepted top-rim observation is
preferred for the displayed accepted rim when a tank-bottom/fish-proxy image is
loaded.

For top-rim saves from the Spatial Layout UI, `circle_detection.detected_circle`
is the already displayed Hough proposal scaled back to full-resolution
camera-native coordinates. The save path must not rerun Hough on the full image
and risk persisting a different fit from the one the operator reviewed. The
operator-confirmed/edited circle remains the load-bearing geometry in
`observed_boundary` and schema-v2 `accepted_inner_rim_boundary`; the
`accepted_experimental_area_boundary` field is a compatibility alias. Offset
`accepted_mask` and `valid_detection_region` remain detection/export views.
Top-rim artifacts also write `overlays/registration_hough_overlay.png`, a
review image showing the saved Hough proposal together with the accepted
registration/fit. That overlay is audit material only; consumers should use the
JSON circle and registration fields instead of parsing overlay pixels.

This supports daily piecewise acquisition:

1. Save the dry projection-surface block: full camera-space arena definition,
   scale image, homography image, and validation pattern. These captures are
   dry references and projector-surface validation artifacts.
2. Install the target dish assembly/imaging shelf, fill to recording level, and
   let the open water surface settle.
3. Save the wet projection-surface block: scale image, homography with rings,
   experimental-area crosshair, and validation pattern. These captures validate
   projector/render behavior at the wetted projection surface only.
4. With projector off unless it is used only as non-coordinate illumination,
   save dry physical-target height-parallax diagnostic captures at:
   `camera_physical_projected_surface`, `camera_physical_dish_base_inner_surface`,
   and `camera_physical_fish_height`. These three captures share one
   `parity_group_id`, use
   `purpose = "dry_physical_target_height_parallax_diagnostic"`, and are
   reference-only dry source images for comparing height-dependent camera/lens
   behavior. They are not wet runtime recording-condition `C_0`, `C_base`, or
   `C_fish` maps.
5. Save the existing dish top-observation set for top-rim / valid-area review.

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
do not require strict synchronized exposure. For the current physical-target
C-plane, dry/wet projection-surface validation, and dish/valid-area workflow,
"simultaneous" means one operator action requests captures from multiple
selected/open cameras. It does not yet mean ChArUco/3D-calibration-grade frame
alignment.

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
  purpose, target plane, capture stage, physical-target or projected-pattern
  descriptors, operator notes, illumination/filter/projector/light state, and
  capture timing metadata for each camera.
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
is appropriate for arena projection, homography image collection, dry and wet
projection-surface scale, camera-only physical-target C-plane capture,
projection-surface crosshair alignment, validation-pattern capture, and daily
top-rim review when exact same-frame timing is not required.

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
  - save generic `verification_dots` image sets
  - save generic `scale_image` image sets
  - save generic `crosshair_alignment` image sets
  - preserve `target_plane`, filter/illumination/projector state, rig context,
    and full-resolution camera-native coordinates
- Citrus import and preview
  - import `orange.calibration.image_set`
  - preview homography fit from `homography_grid`
  - compare held-out residual/parallax probes from `verification_dots`
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
The durable normal-runtime filter identity belongs in the camera config
`optics.filter_stack`, for example a HOYA/Kenko Tokina R72 67 mm infrared
filter marked `state = "installed"`. The calibration capture fields then record
whether that filter stack was actually installed, removed, or otherwise changed
for this specific image.

`light_handling` records operator intent for the capture:

- `leave_current`: do not intentionally change mapped lighting state.
- `suppress_mapped_strobe`: suppress an exposure-synchronized mapped strobe for
  the capture.
- `keep_or_restore_mapped_pulse`: keep or restore the normal mapped
  exposure-pulse state before capture.
- `force_manual_active`: intentionally force a mapped output active.
- `operator_manual`: operator handled lighting outside Orange.

The Spatial Layout UI sets purpose/plane-specific defaults but lets the
operator override them. `arena_projection`, `homography_grid`,
`verification_dots`, and `crosshair_alignment` default to
`suppress_mapped_strobe`; projection-surface scale images and
tank-bottom/fish-plane proxy scale images default to
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
- `validation_pattern_on`
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

Do not use projected-pattern homography captures at
`tank_bottom_inner_surface` or `estimated_fish_plane` as camera-side physical
calibration. A moved diffuser/screen with projected dots is a
projector/moved-diffuser diagnostic only, even when its height approximates the
dish base or fish plane.

### `dry_physical_target_height_parallax_diagnostic`

Current Orange physical-target defaults use this purpose.

Required interpretation:

- dry camera/lens height-dependence diagnostic only
- no open-water surface or water-path refraction included
- not runtime correction ready
- `reference_only = true`
- `physical_target_used = true`
- `projected_pattern_used_as_coordinate_target = false`

Recommended:

- `target_id`
- `target_design`
- `physical_target.grid_spacing_mm` or `physical_target.known_points_mm`
- `physical_target.origin_definition`
- `physical_target.x_orientation_marker_definition`
- `plane_id`
- `z_mm_relative_to_projection_surface`
- `parity_group_id`
- `wet_or_dry = "dry"`
- `fill_state = "dry_or_empty"` or `"not_applicable"`
- `open_water_surface_present = false`
- `water_settled_status = "not_applicable"`
- `capture.projector_state = "off"` unless the projector is being used only as
  non-coordinate illumination

Example for the dish/base inner-surface diagnostic height:

```json
{
  "purpose": "dry_physical_target_height_parallax_diagnostic",
  "target_plane": "tank_bottom_inner_surface",
  "capture_stage": "camera_physical_dish_base_inner_surface",
  "plane_id": "dish_base_inner_surface_physical",
  "z_mm_relative_to_projection_surface": 8.0,
  "wet_or_dry": "dry",
  "fill_state": "dry_or_empty",
  "open_water_surface_present": false,
  "water_settled_status": "not_applicable",
  "reference_only": true,
  "parity_group_id": "camera_physical_planes_20260612T143000Z_shadow_arena_1",
  "parity_group_role": "physical_dish_base",
  "physical_target_used": true,
  "projected_pattern_used_as_coordinate_target": false,
  "target_id": "acrylic_hole_target_78mm_pitch5_margin3_v002",
  "target_design": "opaque_acrylic_hole_mask_78mm_pitch5_margin3_v002",
  "capture": {
    "projector_state": "off",
    "projector_visible_to_camera": false
  }
}
```

### `camera_only_physical_target_calibration`

Use this purpose for physical-target camera maps captured under explicit
runtime-condition or otherwise accepted calibration conditions. Do not use it for
the current dry diagnostics unless the operator intentionally promotes a dry,
reference-only experiment into a separate analysis path. Wet/runtime-condition
captures should include water state and settled-stack metadata sufficient for
Citrus to decide whether the maps are eligible for import or acceptance.

Recommended:

- `physical_target_used = true`
- `projected_pattern_used_as_coordinate_target = false`
- `target_id`
- `target_design`
- `physical_target.grid_spacing_mm` or `physical_target.known_points_mm`
- `physical_target.origin_definition`
- `physical_target.x_orientation_marker_definition`
- `plane_id`
- `z_mm_relative_to_projection_surface`
- `parity_group_id`
- `wet_or_dry`
- `water_fill_mm` or `fill_state`
- `open_water_surface_present`
- `water_settled_status`
- `capture.filter_state`
- `capture.projector_state = "off"` unless the projector is being used only as
  non-coordinate illumination

Example for the dish/base inner surface:

```json
{
  "purpose": "camera_only_physical_target_calibration",
  "target_plane": "tank_bottom_inner_surface",
  "capture_stage": "camera_physical_dish_base_inner_surface",
  "plane_id": "dish_base_inner_surface_physical",
  "z_mm_relative_to_projection_surface": 8.0,
  "wet_or_dry": "wet",
  "parity_group_id": "camera_physical_planes_20260612T143000Z_shadow_arena_1",
  "parity_group_role": "physical_dish_base",
  "physical_target_used": true,
  "projected_pattern_used_as_coordinate_target": false,
  "target_id": "physical_xy_target_v1",
  "target_design": "printed_or_etched_grid_known_xy",
  "physical_target": {
    "target_id": "physical_xy_target_v1",
    "target_design": "printed_or_etched_grid_known_xy",
    "grid_spacing_mm": 5.0,
    "origin_definition": "target_marked_origin",
    "x_orientation_marker_definition": "target_marked_positive_x_axis"
  },
  "capture": {
    "projector_state": "off",
    "projector_visible_to_camera": false
  }
}
```

Citrus can fit one `camera_px -> physical_mm` map per plane from these
artifacts. The three-plane set should include:

- `camera_physical_projected_surface` / `projected_surface_physical` for `C_0`
- `camera_physical_dish_base_inner_surface` /
  `dish_base_inner_surface_physical` for `C_base`
- `camera_physical_fish_height` / `fish_height_physical_assumed` for `C_fish`

### `verification_dots`

Recommended:

- `projected_pattern.pattern_id`
- `projected_pattern.type = "verification_dots"`
- `projected_pattern.mode = "verification_dots"` when copied from Citrus
  active projection snapshots
- `projected_pattern.target_plane`
- optional `projected_pattern.expected_points[]`, copied from Citrus-rendered
  metadata or `citrus_projection_snapshot_*`. Expected points should be in
  Citrus/canvas coordinates with stable point IDs.
- `citrus_projection_snapshot_pre_capture` and
  `citrus_projection_snapshot_post_capture` when Citrus local control is
  available.
- `observations.observed_domain` for the camera-space domain where the probes
  were captured.

`verification_dots` is a first-class acquisition artifact at
`projected_surface`. It is a sparse held-out calibration/probe pattern
constrained to the configured experimental area. It should be used to validate
projection-surface fitted transforms, measure projector/render residuals, and
support later secondary diagnostic models if Citrus defines those products.

Do not use `verification_dots` as the primary homography fitting point set.
Primary plane homographies should be fit from `homography_grid` captures, using
`rectangular_grid`, `circular_rings`, or another explicitly fitting-oriented
pattern. A `verification_dots` capture may be compared against those accepted
projection-surface homographies to quantify residuals, center shift, local
warping, or projector/render drift. Projected dots at dish/base or fish height
are moved-diffuser diagnostics, not camera-only physical calibration.

### `validation_pattern`

Recommended:

- `projected_pattern.pattern_id`
- `projected_pattern.type = "validation_pattern"`
- `projected_pattern.target_plane`
- `capture_stage`
- `matched_parity_group_id` / `parity_group_id` for grouped
  projection-surface validation captures
- `pattern_domain`
- `citrus_projection_snapshot_pre_capture` and
  `citrus_projection_snapshot_post_capture` when Citrus local control is
  available.

`validation_pattern` captures are operator-visible checks in the same stage as
the scale or homography capture. They are useful for drift/debug review and for
future residual analysis, but they do not activate correction by themselves.
When saved in the wet projection-surface stage, they validate the
projection-surface projector/render stack only. They must not be selected as
`C_base` or `C_fish` physical calibration.

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
- `observations.dish_top_rim.observed_boundary`
- `observations.dish_top_rim.boundary_interpretation`
- `observations.dish_top_rim.sampled_boundary_points_camera_px`
- `observations.dish_top_rim.fit_quality`
- `derived_artifacts[]` reference to
  `orange.calibration.dish_top_rim_observation`, if the specialized top-rim
  artifact has been written.

This image-set purpose is the generic acquisition envelope. The specialized
top-rim observation remains the load-bearing accepted-boundary fit artifact.
For Orange/Citrus daily rim registration, `accepted_boundary` should mirror the
specialized artifact's `accepted_inner_rim_boundary`, not the offset
Palette-compatible `accepted_mask`. The accepted physical circle follows the
observed water-side inner rim without a silent offset. A distinct outward
centroid-gate outset may be exported for bounding-box-centroid forgiveness;
legacy inward valid-detection margins remain readable.

Consumers should preserve this runtime invariant:

```text
physically_reachable_fish_area <= Citrus experimental_area / chaser boundary
```

Do not use an offset `accepted_mask`, `valid_detection_region`, or analysis ROI
as the Citrus runtime experimental-area/chaser boundary unless that stricter
policy is explicitly operator accepted. The default daily circular-dish import
should keep the accepted top-rim boundary at least as large as the area the
fish can physically occupy.

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

Camera-only physical map selection:

- To fit wet/runtime-condition `C_0`, select `purpose =
  "camera_only_physical_target_calibration"` with
  `capture_stage = "camera_physical_projected_surface"` and
  `plane_id = "projected_surface_physical"`.
- To fit `C_base`, select the same purpose with
  `capture_stage = "camera_physical_dish_base_inner_surface"` and
  `plane_id = "dish_base_inner_surface_physical"`.
- To fit `C_fish`, select the same purpose with
  `capture_stage = "camera_physical_fish_height"` and
  `plane_id = "fish_height_physical_assumed"`.
- Prefer captures sharing the same `parity_group_id`, target ID/design, dish
  ID or arena slot, water fill, camera/lens/filter/focus state, and settled
  runtime stack.
- Require `physical_target_used = true` and
  `projected_pattern_used_as_coordinate_target = false`.
- Reject physical-map fitting when the artifact carries projected-pattern
  coordinate targets or projected/canvas expected points as the coordinate
  authority.
- Treat `purpose = "dry_physical_target_height_parallax_diagnostic"` as
  reference-only diagnostic input. It may be fit for wrong-plane localization
  analysis, dry camera/lens parallax estimates, and provenance review, but it is
  not a wet recording-condition map and must not be accepted as runtime-ready
  `C_0`, `C_base`, or `C_fish`.

Projector-surface validation selection:

- Use `projected_surface_dry_reference`, `projected_surface_wet_runtime_stack`,
  or `projector_surface_validation` only for projection-surface
  projector/render validation, drift checks, and previewing
  camera-to-Citrus-surface homography.
- Do not select projected-pattern captures at moved diffuser, dish/base height,
  or fish height as physical tank-bottom or fish-plane calibration.
- Legacy moved-diffuser/projected-dot artifacts should be labeled and imported
  only as diagnostics. They are not `C_base` or `C_fish`.

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
      "path": "captures/grid_on.png",
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
