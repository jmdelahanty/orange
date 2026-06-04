# Dish Top-Rim Observation And Runtime Mask Design

Date: 2026-06-01

Status: design proposal. No runtime implementation yet.

Related documents:

- `docs/spatial_layout_contract.md`
- `docs/spatial_layout_schema.md`
- `docs/projected_center_alignment_todo.md`
- `docs/dish_plane_homography_calibration_todo.md`
- `docs/calibration_artifact_contract.md`

## Purpose

Define the Orange/Citrus contract for using camera-captured dish images to
produce four related but separate outputs:

- a camera-space observation of the physical dish top rim
- a session or daily valid-detection mask for Orange predictions
- a visible review artifact that shows how the mask was made
- a reviewable Citrus alignment suggestion based on the current Citrus
  homography and projected crosshair

The immediate rig has one dish/arena per camera view, but the artifact shape
should not assume that forever. The V0 scope is one top-rim boundary and one
valid circular detection region per camera.

## Design Summary

Orange should produce an observation artifact in camera coordinates. Citrus
should consume that observation for preview, operator review, and optional
accepted alignment updates.

The durable source of truth is Orange-native. Palette-compatible metadata is an
export view derived from the Orange artifact, not the native storage model.

Ownership split:

- Orange owns raw captured images, detected top-rim geometry, valid-detection
  masks, visible review overlays, and prediction-gating decisions in camera
  pixels.
- Citrus owns canonical canvas, arena layout, homography artifacts, stimulus
  geometry, and any accepted changes to Citrus-side spatial configuration.
- Neither side should silently mutate the other side's geometry.
- Palette consumers may import an Orange artifact through a deterministic
  adapter that maps the Orange circle mask into Palette's `dish_mask` metadata
  shape.

The top-rim observation is not a replacement homography. It is evidence about
where the dish is in the current camera view. Citrus may use its existing
homography to map that evidence into canvas or arena space for preview, but any
accepted Citrus change should preserve the Orange observation provenance.

## Physical Target

The observed boundary target is:

```text
dish_top_rim
```

This is intentionally not the inner wall, dish bottom, fish plane, or projector
plane. The top rim is a practical and repeatable physical boundary for the
operator to detect and review.

The valid detection area should usually be derived by eroding inward from the
top-rim boundary. The rim itself is the observed physical boundary; the eroded
region is the safe area where fish detections should be accepted.

Recommended separate geometry names:

- `observed_boundary`: the fitted top rim in camera pixels
- `valid_detection_region`: an eroded region used to gate YOLO outputs
- `valid_tracking_region`: optional future region, usually stricter than the
  observed boundary
- `stimulus_safe_region`: optional future region, owned or accepted by Citrus

## Circular Dish V0 Geometry

For the current circular single-arena dish, V0 should use a circle-first
workflow:

1. Capture a source frame in a declared coordinate system, preferably the full
   camera frame.
2. Run an OpenCV Hough-circle detector with logged parameters.
3. Show the detected circle as an overlay to the operator.
4. Let the operator accept or adjust center/radius.
5. Persist the accepted circle as the load-bearing mask geometry.

The accepted circle, not the raw detector output, is the geometry used for
runtime gating and downstream export.

Recommended V0 method string:

```text
orange_acquisition_circle_hough_operator_confirmed_v1
```

Coordinate rules:

- circle center is `[x, y]` or `{ "x": ..., "y": ... }`, never `[row, col]`
- image shape is `[height, width]` or `{ "height": ..., "width": ... }`
- full-frame detections must declare `source_array_role = "images_full"`
- downsampled detections must declare `source_array_role = "images_ds"` and
  use downsampled dimensions
- do not store full-frame pixel values while claiming the mask was tuned on a
  downsampled array

Ellipse fits and sampled boundary points remain useful future extensions for
oblique views, non-circular masks, or richer provenance. They are not required
for the first circular-dish implementation.

## Optical Constraint

In normal experimental operation, cameras have 850 nm bandpass filters. The
projector cannot display a visible pattern at that wavelength for the cameras.

Therefore the projected-crosshair alignment workflow is a daily or occasional
visible-light calibration mode, not a normal live-session runtime measurement.

The artifact must explicitly record the optical state:

```json
{
  "capture_mode": "daily_visible_projector_alignment",
  "filter_state": "removed",
  "runtime_filter_state": "850nm_bandpass_installed",
  "projector_visible_to_camera": true,
  "exposure_us": 500000,
  "frame_rate_hz": 1.0
}
```

Important caveat:

- removing and reinstalling the filter can slightly change focus, alignment, or
  optical path
- daily artifacts are only valid while the camera mount, dish holder, projector
  geometry, and repeatable filter seating remain stable
- if Orange can see any rim or fiducial evidence under normal 850 nm runtime
  conditions, a quick runtime verification should compare the current image
  against the daily mask before trusting it

## Coordinate Spaces

Orange emits primary geometry in:

```text
camera_native_pixels
```

This is the raw camera frame coordinate system for the configured stream
resolution.

Citrus may map points into:

- Citrus canvas/display pixels
- arena-relative units
- layout millimeters, if available

Those mapped values are derived previews. The authoritative Orange observation
remains the camera-space source geometry plus source images.

## Relationship To Homography

Citrus already owns homography artifacts and mappings. V0 should not create new
Orange-owned homographies.

Reason:

- a homography is exact for one physical plane
- Citrus homography captures are usually tied to the projection/calibration
  plane
- the top rim is at a different height from the projection plane and may have
  parallax relative to the calibrated surface

Safe V0 uses:

- map the accepted circle center and sampled circle perimeter points through
  the current Citrus homography as a preview
- report residuals and offsets
- let the operator accept a Citrus-side center or radius correction only after
  review

Unsafe V0 uses:

- replace the Citrus homography from the top-rim fit alone
- treat an ellipse fit at the rim height as a full plane calibration
- silently adjust active stimulus geometry during an experiment

If future work needs a new homography, use the dish-plane homography workflow
with known projected points on the intended physical plane. Keep that separate
from the top-rim mask observation.

## Capture Modes

### Daily Visible Projector Alignment

Purpose:

- fit the top rim in camera pixels
- detect the projected Citrus dish-center crosshair in the same camera view
- compute camera-space center offset and Citrus preview correction

Expected setup:

- operator removes the 850 nm filter
- Orange switches to long exposure and low frame rate
- Citrus displays a calibration crosshair at the current dish/arena center
- Orange captures projector-off and crosshair-on still frames
- operator reinstalls the filter before normal experiment runtime

Minimum frame set:

- `projector_off`: visible-light still used for top-rim fit and background
- `crosshair_on`: visible-light still with Citrus calibration crosshair
- optional `difference`: generated image used to isolate the crosshair

### Runtime 850 nm Verification

Purpose:

- verify that the daily mask is still plausible after reinstalling the filter
- detect gross camera or dish shifts before applying prediction gating

Expected setup:

- filter installed
- normal or near-normal runtime illumination
- projector not visible to the camera

This mode may not be possible if the top rim is not visible at 850 nm. If it is
not possible, the session should record:

```text
mask_runtime_verification = "unknown"
```

rather than pretending the daily mask was revalidated.

### Session-Local Operator Still

Purpose:

- produce a session-local mask from a still image when the operator explicitly
  runs the workflow before a recording
- useful when dish placement may shift during the day

This may use visible-light or 850 nm capture depending on what the camera can
actually see. The capture metadata must say which optical state was used.

## Proposed Artifact Package

The durable package should follow the calibration artifact pattern:

```text
~/orange_data/calibrations/artifacts/<artifact_id>/
|-- manifest.json
|-- observation.json
|-- captures/
|   |-- projector_off.png
|   |-- crosshair_on.png
|   `-- difference.png
|-- overlays/
|   |-- top_rim_fit.png
|   `-- valid_detection_region.png
|-- exports/
|   `-- palette_dish_mask_v2.json
`-- masks/
    `-- valid_detection_region.png
```

The first implementation may write only `observation.json` plus source frames,
but the contract should leave room for overlays and masks because they are
important for operator review.

The overlay image is load-bearing for review but not for computation. Citrus
and Palette should consume `observation.json` or a deterministic export file,
not parse pixels out of the review overlay.

Recommended schema id:

```text
orange.calibration.dish_top_rim_observation
```

## Observation JSON Shape

Candidate `observation.json`:

```json
{
  "schema_id": "orange.calibration.dish_top_rim_observation",
  "schema_version": 1,
  "artifact_id": "dishrim_20260601_120000_2012632",
  "created_utc": "2026-06-01T16:00:00Z",
  "camera": {
    "serial": "2012632",
    "name": "Cam2012632",
    "width": 2464,
    "height": 2064,
    "pixel_format": "BayerRG8"
  },
  "rig_context": {
    "rig_id": "omnifin0",
    "canvas_id": "shadow",
    "arena_id": "arena0",
    "citrus_homography_ref": {
      "available": true,
      "source": "citrus_homography_sidecar"
    }
  },
  "capture": {
    "operation_id": "dishrim_daily_20260601",
    "capture_mode": "daily_visible_projector_alignment",
    "filter_state": "removed",
    "runtime_filter_state": "850nm_bandpass_installed",
    "projector_visible_to_camera": true,
    "exposure_us": 500000,
    "frame_rate_hz": 1.0,
    "requires_camera_mount_unchanged": true,
    "requires_filter_reinstalled_repeatably": true
  },
  "source_frames": [
    {
      "role": "projector_off",
      "path": "captures/projector_off.png",
      "sha256": "..."
    },
    {
      "role": "crosshair_on",
      "path": "captures/crosshair_on.png",
      "sha256": "..."
    }
  ],
  "review_artifacts": {
    "top_rim_overlay_path": "overlays/top_rim_fit.png",
    "top_rim_overlay_sha256": "...",
    "valid_detection_overlay_path": "overlays/valid_detection_region.png",
    "valid_detection_overlay_sha256": "..."
  },
  "circle_detection": {
    "method": "orange_acquisition_circle_hough_operator_confirmed_v1",
    "source_array_role": "images_full",
    "source_frame_index": 0,
    "image_shape": {"height": 2064, "width": 2464},
    "detected_circle": {
      "center_px": {"x": 1232, "y": 989},
      "radius_px": 824
    },
    "hough_params": {
      "dp": 1.2,
      "min_dist_px": 400,
      "param1": 100,
      "param2": 35,
      "min_radius_px": 700,
      "max_radius_px": 900,
      "radius_adjustment_px": 0
    }
  },
  "observed_boundary": {
    "surface": "dish_top_rim",
    "coordinate_space": "camera_native_pixels",
    "geometry": {
      "type": "circle",
      "center_px": {"x": 1234, "y": 988},
      "radius_px": 822
    },
    "fit_quality": {
      "confidence": 0.94,
      "residual_px_rms": 1.8,
      "edge_coverage_fraction": 0.87,
      "quality_flags": []
    }
  },
  "valid_detection_region": {
    "coordinate_space": "camera_native_pixels",
    "derived_from": "observed_boundary",
    "erosion_px": 20.0,
    "geometry": {
      "type": "circle",
      "center_px": {"x": 1234, "y": 988},
      "radius_px": 802
    },
    "mask_path": "masks/valid_detection_region.png",
    "mask_sha256": "..."
  },
  "accepted_mask": {
    "shape": "circle",
    "coordinate_space": "camera_native_pixels",
    "source_array_role": "images_full",
    "source_frame_index": 0,
    "image_shape": {"height": 2064, "width": 2464},
    "center_px": {"x": 1234, "y": 988},
    "radius_px": 802,
    "operator_confirmed": true,
    "operator_adjustment_px": {
      "center_dx": 2,
      "center_dy": -1,
      "radius_delta": -2
    }
  },
  "projected_crosshair": {
    "available": true,
    "source": "citrus_dish_calibration_crosshair",
    "coordinate_space": "camera_native_pixels",
    "detected_center": {"x": 1238.1, "y": 982.4},
    "fit_quality": {
      "confidence": 0.98,
      "residual_px_rms": 0.9
    }
  },
  "alignment": {
    "crosshair_to_rim_center_offset_px": {"dx": 3.6, "dy": -5.2},
    "suggested_center_delta_camera_px": {"dx": -3.6, "dy": 5.2},
    "sign_convention": "suggested_center_delta = rim_center - crosshair_center"
  },
  "citrus_preview": {
    "available": true,
    "semantics": "reviewable_center_alignment_suggestion",
    "homography_surface_assumption": "projection_plane",
    "observed_boundary_surface": "dish_top_rim",
    "suggested_center_delta_canvas_px": {"dx": -2.9, "dy": 4.1},
    "suggested_radius_delta_canvas_px": 0.0
  },
  "runtime_verification": {
    "status": "unknown",
    "reason": "runtime_850nm_rim_not_verified"
  },
  "operator_review": {
    "status": "orange_operator_confirmed",
    "accepted": true
  },
  "compatibility_exports": {
    "palette_dish_mask_v2": {
      "available": true,
      "path": "exports/palette_dish_mask_v2.json",
      "mapping": "orange_circle_mask_to_palette_dish_mask_v2",
      "target": "analysis_metadata.attrs.dish_mask"
    }
  }
}
```

## Recording Snapshot Reference

Normal recordings should not embed the full observation. They should reference
the accepted or active artifact and record the gating state.

Candidate `recording_snapshot.json` block:

```json
{
  "calibrations": {
    "2012632": {
      "dish_top_rim_observation": {
        "artifact_id": "dishrim_20260601_120000_2012632",
        "artifact_schema_id": "orange.calibration.dish_top_rim_observation",
        "artifact_schema_version": 1,
        "fingerprint": "fnv1a64:...",
        "runtime_verification": {
          "status": "pass",
          "checked_at_utc": "2026-06-01T16:10:00Z"
        }
      },
      "valid_detection_region": {
        "enabled": true,
        "source_artifact_id": "dishrim_20260601_120000_2012632",
        "coordinate_space": "camera_native_pixels",
        "gating_policy": "reject_outside_region_and_log"
      }
    }
  }
}
```

## Palette Compatibility Mapping

Orange should not write Palette's H5 attributes as the native artifact format.
Instead, Orange should expose a deterministic adapter that converts an
Orange-native circular mask into Palette-compatible metadata when a Palette H5
or import package is requested.

Native Orange load-bearing fields:

- `accepted_mask.shape`
- `accepted_mask.source_array_role`
- `accepted_mask.source_frame_index`
- `accepted_mask.image_shape`
- `accepted_mask.center_px`
- `accepted_mask.radius_px`
- `circle_detection.method`
- `circle_detection.hough_params`, if Hough was used

Palette adapter output:

```json
{
  "shape": "circle",
  "version": "2.0",
  "method": "orange_acquisition_circle_hough_operator_confirmed_v1",
  "tuned_timestamp": "2026-06-01T16:00:00Z",
  "source": {
    "array": "images_full",
    "frame": 0
  },
  "tuned_on_array": "images_full",
  "tuned_on_frame": 0,
  "detected_circle": {
    "center": [1234, 988],
    "radius": 802
  },
  "metrics": {
    "image_shape": [2064, 2464],
    "center_px": [1234, 988],
    "center_norm": [0.5008116883, 0.4786821705],
    "radius_px": 802,
    "radius_norm": 0.3885658915,
    "area_px": 2021184.0743,
    "area_fraction": 0.3977537561
  },
  "hough_params": {
    "param1": 100,
    "param2": 35,
    "radius_adjustment": -2
  },
  "orange_artifact_id": "dishrim_20260601_120000_2012632",
  "orange_artifact_schema_id": "orange.calibration.dish_top_rim_observation",
  "orange_artifact_schema_version": 1,
  "orange_artifact_fingerprint": "fnv1a64:..."
}
```

Mapping rules:

- `accepted_mask.shape` maps to Palette `shape`.
- `accepted_mask.center_px` maps to Palette `detected_circle.center`.
- `accepted_mask.radius_px` maps to Palette `detected_circle.radius`.
- `accepted_mask.source_array_role` maps to Palette `source.array` and
  `tuned_on_array`.
- `accepted_mask.source_frame_index` maps to Palette `source.frame` and
  `tuned_on_frame`.
- `accepted_mask.image_shape.height,width` maps to Palette
  `metrics.image_shape` as `[height, width]`.
- `circle_detection.method` maps to Palette `method`.
- Orange-specific provenance can be copied into extra JSON-compatible keys, but
  Palette's load-bearing fields must remain present and type-stable.

For rolling clipped recordings, the adapter may also write Palette policy
metadata indicating that one fixed camera mask applies to all clips for that
camera. That policy is an export view, not the native Orange artifact.

## Citrus H5 Snapshot

Citrus should log two different things when it runs an experiment with an
Orange-derived mask:

1. The Orange source artifact reference and mask used for gating or review.
2. The Citrus-owned transform or registration state used to interpret that
   mask in Citrus canvas/arena coordinates.

Recommended H5 group shape:

```text
/calibration_snapshot/dish_mask/
/calibration_snapshot/orange_source_artifact/
/calibration_snapshot/citrus_spatial_registration/
```

Minimum Citrus-side fields to persist:

- Orange artifact id, schema id, schema version, and fingerprint
- camera serial and source coordinate space
- accepted mask shape, center, radius, image shape, and source array role
- path or copied checksum for the visible review overlay
- Citrus rig/canvas/arena identity
- Citrus homography artifact id/fingerprint used for preview, if any
- transform semantics, such as `existing_homography_preview` or
  `accepted_center_delta`
- derived center/radius in Citrus canvas or arena coordinates, if accepted
- operator decision and timestamp

The important rule is that H5 should snapshot what was actually used during the
experiment. It should not rely on re-reading current Orange or Citrus config
files later, because those may have changed.

## Prediction Gating

The mask should be used to gate predictions in camera pixels before downstream
crop, pose, tracking, or Citrus handoff.

Do not silently discard evidence. Persist enough information to debug bad masks
or false rejections.

Recommended per-detection fields:

- `inside_valid_region`: boolean
- `valid_region_id`: artifact or runtime id
- `distance_to_valid_region_boundary_px`: signed number if cheap to compute
- `rejected_reason`: `outside_valid_detection_region` when rejected
- original raw detection box/keypoints/confidence

Recommended policy:

- raw YOLO rows remain auditable
- accepted downstream rows exclude outside-mask detections
- validation tools can report counts of outside-mask rejections

This gives the runtime a clean valid area without losing forensic visibility
when a mask is stale or too tight.

## Citrus Consumption

Citrus should consume the artifact as a reviewable observation:

1. Validate schema, fingerprint, camera serial, rig/canvas/arena hints, and
   source frame checksums.
2. Load or display the Orange visible review overlay for operator context.
3. Render the current Citrus arena/dish geometry.
4. Overlay the Orange accepted circle mapped through the current Citrus
   homography.
5. Overlay the projected crosshair center, if available.
6. Show center/radius deltas and surface caveats.
7. Let the operator accept, reject, or ignore the suggestion.
8. If accepted, write a Citrus-owned calibration/config revision with a
   reference back to the Orange artifact.
9. During an experiment, snapshot the Orange artifact reference and Citrus
   transform state into H5.

Citrus should not treat the Orange artifact as a direct replacement for its
homography. It may use the artifact to adjust center/radius or to create a
session-local registration only after operator review.

## Orange UI Workflow

Proposed operator flow:

1. Select camera and arena.
2. Choose capture mode:
   `daily_visible_projector_alignment`, `runtime_850nm_verification`, or
   `session_local_operator_still`.
3. For daily visible mode, show instructions to remove the 850 nm filter and
   use long exposure/low frame rate.
4. Capture `projector_off`.
5. Ask Citrus to show the dish calibration crosshair, or have the operator
   trigger it manually.
6. Capture `crosshair_on`.
7. Run Hough-circle detection on the declared source frame.
8. Let the operator confirm or adjust the circle.
9. Detect the crosshair center from frame difference when crosshair capture is
   available.
10. Preview top-rim circle, valid detection region, and center offset.
11. Save the observation artifact and visible review overlay.
12. Optionally export or notify Citrus for preview.
13. For runtime use, mark the artifact active only after operator review.

## Validation Plan

### Repeatability

- capture the daily visible artifact three to five times without moving the
  camera, dish, or projector
- measure top-rim center standard deviation
- measure crosshair center standard deviation
- fail or warn if variability exceeds the mask margin

### Filter Reinstall Stability

- remove filter, capture daily artifact
- reinstall filter
- remove and reinstall again, then recapture daily artifact
- quantify center and scale shift caused by filter handling

This determines whether a once-per-day artifact is reliable enough or whether
each session needs a new capture.

### Runtime Verification

- if rim or fiducials are visible at 850 nm, capture a short runtime still
- compare observed runtime evidence to the daily mask
- record `pass`, `fail`, or `unknown`

### Detection Gating Audit

- run a normal recording with gating enabled
- verify raw YOLO rows and accepted detection rows are both auditable
- verify outside-mask rejection counts are recorded
- intentionally use an offset mask and confirm validation catches excessive
  rejections or boundary-distance anomalies

### Citrus Preview

- import the Orange artifact into Citrus
- verify Citrus displays current geometry and Orange suggestion side by side
- verify accepting a suggestion creates a new Citrus-owned artifact/config
  revision rather than mutating the original silently

## Implementation Slices

### Slice 1: Artifact-Only Capture

- add schema constants and JSON writer for
  `orange.calibration.dish_top_rim_observation`
- save source frames, Hough parameters, detected circle, accepted circle,
  visible overlay, crosshair center, and alignment offsets
- no runtime detection gating yet
- no automatic Citrus mutation

### Slice 2: Operator Preview

- add Orange overlay preview for Hough circle, accepted circle, and valid
  detection region
- add confidence and quality flags
- add explicit optical-state metadata in the UI
- add optional Palette adapter export JSON for import testing

### Slice 3: Runtime Gating

- load an active observation artifact per camera
- gate YOLO detections against `valid_detection_region`
- log rejected detections with reasons and boundary distance
- add validator checks for gating counters

### Slice 4: Citrus Preview/Accept

- add Citrus import/preview path
- map Orange circle/crosshair evidence through current Citrus homography
- allow explicit accept/reject
- write a Citrus-owned accepted registration/config artifact
- snapshot the Orange artifact reference and Citrus transform state into H5

### Slice 5: Runtime Verification

- add optional 850 nm still capture
- compare against active mask when possible
- write runtime verification result into `recording_snapshot.json`

## Open Questions

- How much inward erosion should be the default for
  `valid_detection_region`?
- Can the rim be seen reliably in normal 850 nm runtime images, or is runtime
  verification usually `unknown`?
- Does removing and reinstalling the filter introduce measurable pixel shift?
- Should an accepted Citrus correction adjust center only in V0, or center plus
  radius?
- Should Orange request the Citrus crosshair through local control, or should
  V0 remain operator-driven?
- What minimum Hough quality metrics should be required before the UI allows
  operator confirmation?
- Should Citrus copy the Orange visible review overlay into its H5/output
  folder, or store only the Orange artifact reference plus checksum?

## Resolved V0 Decisions

- V0 geometry for the current circular single-arena dish is an
  operator-confirmed circle.
- OpenCV Hough circle detection is the initial automatic proposal mechanism.
- The accepted circle is the load-bearing mask geometry.
- The native durable artifact is Orange-specific; Palette metadata is an
  adapter/export view.
- Citrus H5 should snapshot the Orange artifact reference and Citrus transform
  state used during the experiment.
- Ellipse fits and sampled boundary points are future extensions, not V0
  requirements.

## Recommended V0 Decision

Build this first as a daily or session-local Orange observation artifact with
Hough-circle proposal, operator confirmation, and visible review overlay. Use
it for detection-mask design, Palette adapter testing, and Citrus preview, but
do not enable automatic Citrus geometry mutation or default runtime detection
gating until repeatability and filter-reinstall stability are measured.
