# Calibration Image Set Schema

Purpose: define a generic Orange acquisition artifact for calibration images
that Citrus can import, preview, fit, and accept into Citrus-owned calibration
state.

Date anchored: 2026-06-08.
Status: draft schema. No runtime writer is required by this document yet. The
current implemented top-rim writer remains
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
|-- images/
|   |-- <role>.png
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

Do not encode the plane into `purpose`. Use `target_plane` for physical plane
semantics.

## Target Plane

`target_plane` names the physical plane where the calibration target or
observed feature lives. V1 uses Citrus-style names:

- `projected_surface`
- `tank_bottom_outer_surface`
- `tank_bottom_inner_surface`
- `estimated_fish_plane`
- `dish_top_rim`
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

For V0, `scale_image` at `estimated_fish_plane` is diagnostic only. Citrus can
persist and report the measured scale and compare it to projection-surface
scale, but it should not change chaser/stimulus runtime behavior until
plane-specific runtime transforms are designed and accepted.

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
- `timestamp_utc`
- `frame_id`
- `recording_frame_id`
- `capture_mode`
- `exposure_us`
- `frame_rate_hz`
- `gain`
- `filter_state`
- `runtime_filter_state`
- `light_state`
- `projector_state`
- `projector_visible_to_camera`
- `requires_camera_mount_unchanged`
- `requires_filter_reinstalled_repeatably`

Use explicit `filter_state` and `light_state` values because some Orange rigs
normally run with 850 nm bandpass filters and TTL-driven infrared strobes. A
visible-light calibration image may require filter removal, different exposure,
and a nonstandard illumination/preflight state.

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

Citrus uses these fields to preview and fit homography. Orange may provide
detected camera points as diagnostic observations, but Citrus owns accepted
point correspondences and homography sidecars.

### `scale_image`

Recommended:

- `scale_target.type`
- `scale_target.known_spacing_mm`
- `scale_target.known_distance_mm`
- `scale_target.units`
- `scale_target.target_plane`
- `scale_target.target_plane_material`

For `target_plane = "estimated_fish_plane"`, Citrus should treat the
measurement as diagnostic until plane-specific runtime transforms exist.

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
- `observations.crosshair.detected_center_camera_px`
- `observations.crosshair.fit_quality`

Crosshair alignment is useful for daily center checks, but Citrus still owns
accepted center correction semantics.

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
