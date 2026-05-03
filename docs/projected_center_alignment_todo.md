# Projected Center Alignment Diagnostic

Date: 2026-05-03

Status: design/TODO. No runtime implementation yet.

Related docs:

- `docs/spatial_layout_contract.md`
- `docs/spatial_layout_schema.md`
- Citrus `docs/coordinate_systems.md`
- Citrus `docs/spatial_overlay_contract.md`

## Purpose

Build the smallest practical bridge between what Orange sees in the camera and
what Citrus believes the experimental area center is.

The immediate goal is not full dish geometry recovery. The immediate goal is a
center-alignment diagnostic:

```text
Where does Orange see the projected experimental-area center/crosshair,
relative to the observed dish or experimental-area center?
```

This gives the operator and future automation a clear answer to:

- is Citrus projecting the experimental center where the camera sees the dish
  center?
- is Citrus projecting the experimental center near the camera frame center?
- how large is the center offset in camera pixels?
- what Citrus arena/canvas center correction would be suggested if the current
  homography is trusted?

This project becomes feasible because the camera can be configured, by removing
the IR filter, to see the visible projector output directly.

## Non-Goals

This V0 should not attempt to solve:

- full camera/projector recalibration
- full homography replacement
- lens distortion modeling
- 3D reconstruction of dish/fish/projector surfaces
- automatic canonical Citrus config mutation without review
- multi-zone layout authoring
- precise correction of scale, rotation, or projective distortion

A center-only diagnostic is intentionally low degree-of-freedom. It can detect
translation-like misalignment. It cannot prove that the entire arena geometry is
correct.

## Alignment Targets

There are two useful center measurements in the current single-camera rig:

```text
projected Citrus experimental center -> camera frame center
projected Citrus experimental center -> observed dish/experimental-area center
```

The camera-frame-center measurement is a rig setup diagnostic. It answers:

```text
Is the projected experimental center centered in this camera's image?
```

This is useful while the camera always sees the full experimental area and the
operator expects the dish to be mechanically centered in the camera view.

The observed-dish-center measurement is the stronger experimental alignment
target. It answers:

```text
Is Citrus projecting the experimental center where Orange sees the usable
experimental area center?
```

If the dish is also centered in the camera, these three points should all be
near each other:

```text
camera frame center
observed dish/experimental-area center
projected Citrus experimental center
```

V0 should measure and persist all three. It should not make the camera frame
center the only source of truth.

## Current Single-Camera Interpretation

For the current rig, where one camera sees the full experimental area, a
near-zero projected-to-camera-center offset is a useful setup check:

```text
camera_frame_center = (width / 2, height / 2)
projected_to_camera_center_offset =
    projected_center_camera_px - camera_frame_center
```

However, if the dish is placed slightly off-center in the camera view, the
camera center is no longer the physically meaningful target. In that case the
projected center should be compared against the observed dish/experimental-area
center.

Recommended V0 interpretation:

- use `projected_to_camera_center_offset` as a mechanical/optical setup metric
- use `projected_to_observed_offset` as the experimental center alignment metric
- only suggest Citrus center corrections from the observed experimental-area
  center unless the operator explicitly wants to center the projection in the
  camera crop

## Future Multi-Camera Interpretation

In a future layout where one experimental area is covered by multiple cameras,
no single camera frame center should define the canonical experimental center.

The long-term model should be:

```text
canonical experimental/arena layout
  -> camera A registration and coverage geometry
  -> camera B registration and coverage geometry
  -> camera C registration and coverage geometry
```

Each camera may see the same experimental area with a different center, scale,
rotation, or partial coverage. Camera center is then only a per-camera coverage
quality metric, not a shared geometry authority.

For that future case:

- the canonical experimental center should live in layout/arena/canvas space
- each camera should emit its own observed center and coverage geometry
- Citrus should consume per-camera registration/coverage metadata instead of
  assuming every camera is centered on the same physical point
- a camera whose frame center is far from the experimental center may still be
  valid if its declared coverage geometry is correct

## Geometry Caveat

There are multiple physical surfaces in the rig:

- the surface the projector is focused onto
- the dish bottom or projected-through surface
- the fish plane in water
- the visible top rim or inner wall of the dish

A homography is only exact for one planar surface. If Citrus homography was
calibrated on the projected plane but Orange detects the dish rim or fish
plane, the measured offset includes parallax and surface-height mismatch.

Therefore this diagnostic must persist the surface assumptions instead of
pretending the result is an exact physical correction.

Recommended metadata fields:

```json
{
  "surface_model": "projected_crosshair_visible_to_camera",
  "observed_boundary_surface": "inner_experimental_area_or_dish_rim",
  "homography_surface_assumption": "existing_citrus_calibrated_plane",
  "correction_semantics": "center_translation_suggestion_only"
}
```

## V0 Workflow

1. Citrus displays a simple calibration stimulus.
2. The stimulus is a high-contrast crosshair centered at the current Citrus
   experimental-area center.
3. Orange captures a still frame from the selected camera while the projector is
   visible to that camera.
4. Orange detects the projected crosshair center in `camera_view_px`.
5. Orange detects or loads the observed dish/experimental-area center in
   `camera_view_px`.
6. Orange computes center offsets in camera pixels.
7. If Citrus homography is available, Orange maps points into Citrus canvas or
   arena-relative coordinates and computes a suggested Citrus center correction.
8. Orange writes the observation and suggestion into the recording snapshot or
   a calibration artifact.
9. Citrus imports the result as a reviewable suggestion, not an automatic
   replacement.

## Recommended Calibration Stimulus

Start with a crosshair rather than a full projected circle.

Crosshair advantages:

- robust center detection
- low ambiguity
- cheap to render
- readable even if only part of the projected pattern is visible
- avoids treating a projected circle as if it remains circular in camera view

Recommended V0 pattern:

```text
background: black
crosshair: white or high-contrast visible color
center dot: optional filled disk
line width: configurable
line length: configurable
blink cadence: optional, off by default
```

Recommended capture strategy:

- capture one frame with crosshair on
- optionally capture one frame with crosshair off
- use frame difference to isolate the projected pattern from dish texture,
  glare, labels, and fish/background features

## Measurements

Minimum camera-space outputs:

```json
{
  "camera_serial": "2010096",
  "camera_frame_size_px": {"width": 5320, "height": 3032},
  "camera_frame_center_px": {"x": 2660.0, "y": 1516.0},
  "projected_center_camera_px": {"x": 2680.5, "y": 1514.0},
  "observed_boundary_center_camera_px": {"x": 2667.0, "y": 1526.5},
  "projected_to_camera_center_offset_camera_px": {"dx": 20.5, "dy": -2.0},
  "projected_to_observed_offset_camera_px": {"dx": 13.5, "dy": -12.5},
  "camera_frame_to_observed_offset_camera_px": {"dx": 7.0, "dy": 10.5}
}
```

If Citrus homography is available:

```json
{
  "projected_center_canvas_px": {"x": 1920.0, "y": 1080.0},
  "observed_center_canvas_px": {"x": 1912.4, "y": 1091.8},
  "suggested_center_delta_canvas_px": {"dx": -7.6, "dy": 11.8},
  "suggested_center_delta_arena_px": {"dx": -7.6, "dy": 11.8}
}
```

The sign convention should be documented explicitly:

```text
suggested_center_delta = observed_center - projected_center
projected_to_camera_center_offset = projected_center - camera_frame_center
projected_to_observed_offset = projected_center - observed_center
```

Meaning: add this delta to the Citrus experimental-area center if the operator
decides to align the projected center to the observed boundary center.

## Detection Strategy

### Projected Crosshair Center

Preferred V0:

1. Capture projector-off frame.
2. Capture projector-on frame.
3. Convert both to grayscale or luminance.
4. Difference the frames.
5. Threshold the difference image.
6. Detect horizontal and vertical line components.
7. Fit line centers or use connected components/moments.
8. Intersect fitted horizontal and vertical center lines.

Fallback V0:

- find the largest high-contrast connected component near the expected center
- compute its centroid
- report lower confidence because centroid can be biased by glare or partial
  visibility

### Observed Dish/Experimental-Area Center

Use the existing Orange spatial-layout detection path when available:

- Hough-circle detection from a frozen camera frame
- imported Citrus single-circle template as a prior
- manual operator adjustment in the spatial layout UI

The detected/accepted boundary center should be stored separately from the
projected crosshair center.

## Artifact Shape

Add a new observation block rather than overloading `dish_mask`.

Candidate recording snapshot path:

```text
recording_snapshot.calibrations[serial].projected_center_alignment
```

Candidate payload:

```json
{
  "schema_id": "orange.calibration.projected_center_alignment",
  "schema_version": 1,
  "enabled": true,
  "capture_timestamp_ns": 1777766400000000000,
  "camera_serial": "2010096",
  "coordinate_frame": "camera_view_px",
  "projected_pattern": {
    "type": "crosshair",
    "citrus_source": "experimental_area_center",
    "citrus_canvas_name": "default",
    "citrus_arena_config_name": "100_cam4"
  },
  "projected_center_camera_px": {"x": 2680.5, "y": 1514.0},
  "observed_boundary_center_camera_px": {"x": 2667.0, "y": 1526.5},
  "projected_to_observed_offset_camera_px": {"dx": 13.5, "dy": -12.5},
  "fit_quality": {
    "crosshair_confidence": 0.97,
    "boundary_confidence": 0.91,
    "residual_px": 2.4
  },
  "surface_assumptions": {
    "surface_model": "projected_crosshair_visible_to_camera",
    "observed_boundary_surface": "inner_experimental_area_or_dish_rim",
    "homography_surface_assumption": "existing_citrus_calibrated_plane",
    "correction_semantics": "center_translation_suggestion_only"
  },
  "homography_projection": {
    "available": true,
    "source": "citrus_homography_sidecar",
    "suggested_center_delta_canvas_px": {"dx": -7.6, "dy": 11.8},
    "suggested_center_delta_arena_px": {"dx": -7.6, "dy": 11.8}
  },
  "operator_action": {
    "status": "suggested",
    "accepted": false
  }
}
```

## Citrus Consumption

Citrus should treat this as a proposed correction.

Recommended Citrus behavior:

- display current experimental-area center
- display Orange-observed projected-center offset
- show suggested arena-relative center correction
- allow operator to accept, reject, or ignore
- if accepted, write a new amended config or calibration artifact
- preserve the original Citrus config and Orange observation provenance

Citrus should not silently modify the active canonical experimental-area center
based only on a single V0 center measurement.

## UI Sketch

Orange spatial calibration panel:

```text
Projected Center Alignment

[Capture projector-off reference]
[Ask Citrus/show crosshair, then capture projector-on frame]
[Detect projected crosshair center]
[Detect/load observed dish center]

Projected center: (x, y)
Observed dish center: (x, y)
Offset camera px: dx, dy
Suggested Citrus center delta: dx, dy

[Save observation]
[Export suggestion for Citrus]
```

Citrus arena calibration panel:

```text
Orange center-alignment suggestion

Camera: 2010096
Offset camera px: dx, dy
Suggested center delta arena px: dx, dy
Surface caveat: center translation only

[Preview correction]
[Accept as new config revision]
[Reject]
```

## Validation Plan

### Bench Test

- remove IR filter so the camera sees the projector
- display a crosshair at the current Citrus experimental-area center
- capture projector-off and projector-on frames
- verify the crosshair center detector returns a stable center within a few
  pixels across repeated captures

### Repeatability Test

- run the capture three to five times without moving the dish
- expected result: offset standard deviation is small relative to the correction
  magnitude

### Translation Test

- intentionally move the dish or Citrus experimental-area center by a known
  small amount
- expected result: measured offset changes in the expected direction

### Homography Sanity Test

- compare the Citrus-known crosshair center mapped through inverse homography
  with the camera-detected crosshair center
- large error means the active homography does not match the actual visible
  projection/camera state and suggested Citrus correction should be flagged as
  low confidence

### Surface Caveat Test

- capture with the projected pattern visible on the projection plane
- capture with the observed boundary being the dish rim or fish plane
- record whether the measured offset changes when surface height/placement
  changes
- use this to decide whether V1 needs explicit multi-surface calibration

## Implementation TODO

### Orange

- [ ] Add `projected_center_alignment` schema structs and JSON serializer.
- [ ] Add projector-off/projector-on frozen-frame capture support in the
  spatial layout UI.
- [ ] Add frame-difference crosshair detector.
- [ ] Reuse existing experimental-area Hough-circle/manual-fit state as the
  observed boundary center.
- [ ] Compute camera-space center offsets with documented sign convention.
- [ ] If Citrus homography is loaded, map center points into canvas/arena space
  and compute suggested center correction.
- [ ] Persist the observation into `recording_snapshot.json` or a linked
  calibration artifact.
- [ ] Add artifact provenance: Citrus config path, homography source, camera
  serial, frame size, timestamps, operator acceptance state.

### Citrus

- [ ] Add a simple crosshair calibration stimulus at experimental-area center.
- [ ] Add a command/API path for Orange or the operator to request the crosshair
  state.
- [ ] Add importer for Orange `projected_center_alignment` observations.
- [ ] Show suggested center delta in the arena calibration UI.
- [ ] Support previewing the corrected center without mutating the active
  config.
- [ ] If accepted, write a new config revision or calibration artifact with
  provenance.
- [ ] Mirror accepted/alignment metadata into H5 under `/calibration_snapshot`.

### Shared Contract

- [ ] Define `orange.calibration.projected_center_alignment` schema.
- [ ] Define coordinate frames for all fields.
- [ ] Define sign convention for correction deltas.
- [ ] Define confidence/error fields.
- [ ] Define accepted/rejected/operator-review states.
- [ ] Decide whether accepted corrections become part of `arena_layout`,
  Citrus arena config, or a separate per-recording calibration artifact.

## Recommended Rollout

1. Implement manual two-frame capture and crosshair detection in Orange.
2. Save a JSON artifact outside the recording snapshot first.
3. Validate repeatability with the IR-filter-removed camera.
4. Add recording snapshot emission.
5. Add Citrus import/preview.
6. Only then allow accepted corrections to produce amended Citrus config.

## Open Questions

- Can Orange directly command Citrus to show/hide the crosshair, or should V0 be
  operator-driven?
- Should the crosshair be shown in final-display canvas coordinates or
  arena-relative coordinates?
- Which camera exposure/gain settings make projector-visible capture reliable
  without disturbing normal NIR tracking settings?
- Should accepted corrections be stored as a new Citrus config revision, a
  calibration artifact, or per-recording metadata only?
- How much offset is acceptable before the operator must re-run full
  homography calibration?
- Do we need an RGB/visible-light calibration mode distinct from normal NIR
  recording mode?

## Summary

The IR-filter-removed camera makes a useful V0 possible: detect where the
projected experimental-area center appears in the same camera frame that sees
the dish. This should be implemented as a center-offset diagnostic and
reviewable correction suggestion. It should not be treated as full homography
calibration or automatic dish-layout authority until repeatability and surface
height effects are measured.
