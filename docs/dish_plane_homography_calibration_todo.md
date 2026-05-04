# Dish-Plane Homography Calibration TODO

Date: 2026-05-04

Status: design/TODO. No runtime implementation yet.

Related docs:

- `docs/projected_center_alignment_todo.md`
- `docs/spatial_layout_contract.md`
- `docs/spatial_layout_schema.md`
- `docs/camera_level_measurement.md`
- Citrus `docs/tank_plane_calibration_todo.md`
- Citrus `docs/spatial_overlay_contract.md`

## Purpose

Define the calibration workflow for fitting a camera-to-Citrus homography on
the physical plane that matters for the experiment, even when the actual
experimental dish is circular.

The central idea:

```text
Use a large square/rectangular calibration target at the correct dish/fish
plane height to fit the homography. Then use the circular dish as the runtime
usable region or mask inside that calibrated plane.
```

The calibration target shape and the runtime dish shape do not need to match.
The homography only requires known point correspondences on a single physical
plane.

## Key Principle

A homography maps points between two 2D coordinate systems for one plane:

```text
camera_view_px <-> Citrus canvas/display px
```

The calibration object can be square, rectangular, circular, or irregular. What
matters is:

- the calibration points are known in Citrus canvas/display coordinates
- the same points are detected in camera pixels
- all points lie on the same physical plane
- that plane matches the plane where the mapping will later be used

For this rig, the desired plane may be:

- `dish_bottom_outer_surface`
- `dish_bottom_inner_surface`
- `estimated_fish_plane`
- another explicitly named projection/observation plane

The correct plane should be declared in metadata.

## Why A Square Target Helps

A square or rectangular calibration surface is useful because it can cover more
of the camera sensor than a circular dish.

Benefits:

- better point coverage across the field of view
- better homography conditioning
- better visibility of edge/corner distortion
- easier detection with projected dot grids, checkerboards, ChArUco, or ArUco
  boards
- reusable calibration for circular dishes placed within the calibrated region

This does not mean the animal container must be square. The square target is a
calibration tool. The circular dish remains the experimental usable region.

Runtime interpretation:

```text
full-field dish-plane homography
  + circular dish mask / experimental-area layout
  = calibrated circular experiment region
```

## Plane Height Requirement

The square target only solves the desired problem if its calibration surface is
coplanar with the plane used during experiments.

Examples:

- If fish positions should be interpreted at the dish bottom inner surface, the
  calibration target should represent that inner surface.
- If fish positions should be interpreted at an estimated swim plane above the
  bottom, the calibration target should be shimmed or modeled to that height.
- If the projected pattern is measured on a diffuser below the dish, the
  homography is for that diffuser plane, not automatically for the fish plane.

Bad calibration:

```text
fit homography on shelf plane
use it for fish positions several millimeters above shelf
```

Better calibration:

```text
fit homography on a target whose surface matches dish-bottom/fish-plane height
use circular dish mask as the valid runtime region
```

## Optical Stack Requirement

The calibration should preserve the optical stack whenever possible.

If the experiment sees/projectors through:

- acrylic
- dish bottom material
- water
- diffuser or shelf material

then the calibration target should include the same materials or the metadata
should explicitly state what differs.

Important limitation:

- water and acrylic can introduce refractive effects that a simple homography
  does not fully model
- a planar homography can still be a useful empirical mapping, but only for the
  measured optical condition

Recommended metadata:

```json
{
  "plane_id": "dish_bottom_inner_surface",
  "optical_stack": {
    "dish_present": true,
    "water_present": true,
    "water_depth_mm": 4.0,
    "dish_bottom_thickness_mm": 1.59,
    "refraction_model": "empirical_plane_homography_v0"
  }
}
```

## Relationship To Circular Dishes

Circular dishes are not a problem for homography calibration.

Two valid workflows:

### Circular-Dish-Only Calibration

Project a grid over the circular dish area and use only the visible points
inside the dish.

Pros:

- exactly matches the runtime container placement
- simple physical setup

Cons:

- calibration points cover only a circular subset
- fewer edge/corner points for homography conditioning
- harder to distinguish full-field distortion from missing coverage

### Full-Field Target Calibration

Use a larger flat target at the same height/optical condition as the dish plane,
fit the homography from points spread across the camera view, then place the
circular dish for experiments.

Pros:

- stronger full-field calibration
- better point distribution
- circular dish can move within the calibrated area

Cons:

- target must accurately reproduce the runtime plane height and optical stack
- if the real dish/water changes the optical path, the full-field calibration
  may be biased

Recommended first approach:

- use the full-field target for homography quality
- separately capture the circular dish to measure the usable mask/center
- compare projected-center alignment with the dish installed to quantify any
  remaining offset

## Recommended V0 Capture Set

For each camera and target plane:

1. Place the calibration target in the dish holder/on the shelf at the intended
   physical plane height.
2. Configure the camera so projected visible light is observable.
3. Capture a projector-off reference frame.
4. Project a high-contrast dot grid or checker pattern.
5. Capture one or more projector-on calibration frames.
6. Detect point centers in camera pixels.
7. Pair detected points with their known Citrus canvas/display coordinates.
8. Fit a homography.
9. Report residuals and coverage.
10. Install the circular dish and capture projected-center alignment frames.
11. Save both the homography calibration and the circular dish mask/alignment
    observation.

Minimum saved artifacts:

- raw projector-off frame
- raw projector-on grid frame
- detected point list
- known Citrus point list
- fitted homography matrix
- fit residuals
- target plane metadata
- optical stack metadata
- circular dish mask/center observation, if captured

## Calibration Pattern Options

Start with projected dots or a high-contrast checker/grid.

Good V0 choices:

- projected dot grid with known canvas coordinates
- checkerboard or ChArUco board if printed/physical target is easier
- ArUco/AprilTag board for robust point identity

Projected dot grid advantages:

- the known coordinates are already Citrus canvas/display coordinates
- no printed target fabrication required
- can cover arbitrary display/camera regions

Printed/physical board advantages:

- can be placed exactly on a physical target surface
- marker identity can be robust if using ChArUco/ArUco
- useful for camera intrinsics and camera-to-plane pose

The best eventual workflow may use both:

```text
printed/physical board -> camera intrinsics and plane pose
projected grid -> camera-to-Citrus display homography on that plane
```

## Fit Quality Metrics

Every fitted homography should report:

- point count
- coverage bounding box in camera pixels
- coverage bounding box in Citrus canvas/display pixels
- RMS reprojection error in camera pixels
- max reprojection error in camera pixels
- per-point residuals
- whether points cover the runtime circular dish region
- plane ID and optical stack summary

Suggested acceptance thresholds should be empirical. Do not hard-code strict
values before collecting data.

Useful starting labels:

```text
excellent: RMS < 1 px
usable: RMS < 3 px
review: RMS 3-8 px
reject: RMS > 8 px or structured residuals
```

These thresholds are placeholders and should be revised after real captures.

## Runtime Use

The plane homography should answer:

```text
camera point on calibrated plane -> Citrus canvas/display point
Citrus canvas/display point -> expected camera point on calibrated plane
```

The circular dish should be represented separately as:

- `dish_mask` in camera-native pixels
- canonical experimental/arena layout in Citrus/arena/layout coordinates
- per-recording center-alignment observation, when available

Do not encode the circular dish boundary into the homography itself. The
homography maps the plane; the mask/layout describes the usable region on that
plane.

## Metadata Shape

Candidate artifact:

```json
{
  "schema_id": "orange.calibration.dish_plane_homography",
  "schema_version": 1,
  "camera_serial": "2010096",
  "citrus_canvas_name": "default",
  "citrus_arena_config_name": "100_cam4",
  "source_frame": "camera_view_px",
  "dest_frame": "final_display_canvas_px",
  "plane": {
    "plane_id": "dish_bottom_inner_surface",
    "surface_description": "calibration target top surface shimmed to dish inner bottom",
    "height_reference": "dish_holder_shelf",
    "height_offset_mm": 6.59
  },
  "optical_stack": {
    "dish_present": true,
    "water_present": true,
    "water_depth_mm": 4.0,
    "dish_bottom_thickness_mm": 1.59,
    "refraction_model": "empirical_plane_homography_v0"
  },
  "calibration_target": {
    "type": "projected_dot_grid",
    "shape": "rectangular_full_field",
    "point_count": 80
  },
  "homography_matrix": [
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    0.0, 0.0, 1.0
  ],
  "fit_quality": {
    "rms_reprojection_error_px": 1.4,
    "max_reprojection_error_px": 3.2,
    "point_count": 80
  },
  "coverage": {
    "camera_bbox_px": {"x": 120.0, "y": 80.0, "width": 5080.0, "height": 2860.0},
    "covers_runtime_dish_mask": true
  },
  "operator_action": {
    "status": "suggested",
    "accepted": false
  }
}
```

## Validation Plan

### Repeatability

- capture the same target without moving anything three to five times
- fit homography for each capture
- compare matrix stability and reprojection residuals
- compare mapped center point stability

### Target Movement

- move the target or dish by a known small amount
- confirm mapped center/dish-mask deltas move in the expected direction

### Circular Dish Check

- after full-field calibration, install the real circular dish
- project a crosshair at the Citrus experimental center
- capture projected-center alignment
- compare projected center, camera center, and observed dish center
- record whether the full-field homography remains valid with the dish/water
  optical stack installed

### Plane Height Check

- repeat the calibration or center check at two known target heights
- measure how much the mapped center shifts per millimeter
- use this to decide whether height metadata is enough or multi-plane
  calibration is required

## Implementation TODO

### Orange

- [ ] Define `orange.calibration.dish_plane_homography` artifact schema.
- [ ] Add capture workflow for projector-off and projector-on grid frames.
- [ ] Add projected dot/grid detection and point pairing.
- [ ] Fit and validate homography for a named target plane.
- [ ] Persist raw frames, detected points, known points, matrix, residuals, and
  plane/optical-stack metadata.
- [ ] Link the dish-plane homography artifact from `recording_snapshot.json`.
- [ ] Keep circular `dish_mask` artifacts separate from homography artifacts.
- [ ] Add a review UI showing residual vectors and coverage over the runtime
  dish mask.

### Citrus

- [ ] Provide a calibration pattern mode that renders a known dot/checker grid
  in final display canvas coordinates.
- [ ] Export the exact rendered point coordinates for Orange pairing.
- [ ] Accept or import Orange dish-plane homography artifacts as reviewable
  calibration evidence.
- [ ] Mirror accepted artifact references into H5 calibration snapshots.
- [ ] Keep projected-plane, dish-bottom-plane, and fish-plane mappings named
  separately.

### Shared

- [ ] Agree on plane IDs and optical stack metadata names.
- [ ] Agree on whether accepted dish-plane homographies replace, augment, or sit
  alongside existing Citrus homography.
- [ ] Define how multi-camera coverage references the same canonical
  experimental layout with different per-camera homographies/masks.

## Recommended Rollout

1. Build a manual capture workflow and save raw frames plus JSON sidecars.
2. Fit homographies offline from projected grid captures.
3. Validate repeatability and height sensitivity.
4. Add Orange artifact emission.
5. Add Citrus pattern export/import support.
6. Only after measurement, decide whether chaser/detection mapping should use
   the dish-plane homography instead of the existing projected-plane
   homography.

## Summary

You do not need a square dish. You need a well-characterized calibration plane.
A large square/rectangular target at the correct dish/fish-plane height is a
good way to get full-field point coverage. The circular dish then becomes a
mask or experimental-area layout on top of that calibrated plane, not the object
that defines the homography.
