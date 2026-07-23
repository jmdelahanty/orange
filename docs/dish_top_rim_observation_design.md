# Dish Top-Rim Observation And Runtime Mask Design

Date: 2026-06-01
Last updated: 2026-07-18

Status: design plus first implementation slice. The Orange-native artifact
writer, Hough-circle proposal, visible overlays, Palette export, spatial
`dish_mask_runtime` export, Spatial Layout UI save action, and first live
stream preview plus full-resolution stream snapshot capture now exist.
Runtime detection gating and Citrus preview/accept remain future slices.

Related documents:

- `docs/calibration_image_set_schema.md`
- `docs/spatial_layout_contract.md`
- `docs/spatial_layout_schema.md`
- `docs/projected_center_alignment_todo.md`
- `docs/dish_plane_homography_calibration_todo.md`
- `docs/calibration_artifact_contract.md`

## Purpose

Define the Orange/Citrus contract for using camera-captured dish images to
produce four related but separate outputs:

- a camera-space observation of the water-side inner edge in the physical dish
  top-rim plane
- a session or daily valid-detection mask for Orange predictions
- a visible review artifact that shows how the mask was made
- a reviewable Citrus alignment suggestion based on the current Citrus
  homography and projected crosshair

The immediate rig has one dish/arena per camera view, but the artifact shape
should not assume that forever. The V0 implementation scope is one top-rim
circle and one valid circular detection region per camera. The intended
fish-occupiable boundary feature is the water-side inner rim, not the outer
edge of the top flange.

## Design Summary

Orange should produce an observation artifact in camera coordinates. Citrus
should consume that observation for preview, operator review, and optional
accepted alignment updates.

The durable source of truth is Orange-native. Palette-compatible metadata is an
export view derived from the Orange artifact, not the native storage model.

For generic calibration-image acquisition, Orange should use
`orange.calibration.image_set` with `purpose = "dish_top_rim"` and
`target_plane = "dish_top_rim"`. This top-rim observation artifact remains the
specialized load-bearing fit/review artifact for the accepted circle or
boundary. A daily capture may therefore write both:

- `orange.calibration.image_set`: source images and acquisition metadata.
- `orange.calibration.dish_top_rim_observation`: accepted camera-space
  circle/boundary, valid-detection mask, Palette export, and Citrus preview
  diagnostics.

Ownership split:

- Orange owns raw captured images, detected inner-rim geometry in the top-rim
  plane, valid-detection masks, visible review overlays, and prediction-gating
  decisions in camera pixels.
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
Orange may show Citrus-space preview values to help the operator, but those
values are diagnostic and non-authoritative. Citrus should recompute and own
any accepted Citrus-space center/radius/config change from the durable
camera-space Orange observation and Citrus-owned rig/homography/optical model.

## Physical Target

The target has three separate semantics:

```text
target_plane   = dish_top_rim
target_feature = dish_inner_rim_water_side_edge
region         = water_accessible_footprint
```

`dish_top_rim` names the observation plane. It is not specific enough to name
the fitted edge. The load-bearing feature for the fish-occupiable area is the
water-side inner edge of the dish opening. It is intentionally not the outer
edge of the top flange, the lower inner wall, dish bottom, fish plane, or
projector plane.

The canonical physical boundary should follow the operator-confirmed inner rim
without silently expanding or eroding it. Uncertainty, overcoverage, and
undercoverage belong in fit metrics and downstream policy, not in the accepted
physical evidence itself.

Recommended separate geometry names:

- `observed_boundary`: the fitted water-side inner rim in camera pixels
- `accepted_inner_rim_boundary`: schema-v2 name for the
  Orange-operator-confirmed physical boundary
- `accepted_experimental_area_boundary`: existing schema-v1 compatibility name
  for the same Orange-confirmed boundary; it does not assert Citrus acceptance
- `valid_detection_region`: a derived centroid-gating region. New Orange saves
  use a small outward offset so a fish bounding-box centroid near the wall is
  not rejected; legacy inward-eroded artifacts remain readable
- `valid_tracking_region`: optional future region, usually stricter than the
  physical boundary
- `stimulus_safe_region`: a derived region owned or accepted by Citrus

The physical inner-rim boundary is a proposal input to Citrus. Citrus owns the
decision to accept it as, or derive from it, the runtime `experimental_area`.
Orange should never encode Citrus acceptance merely because an Orange operator
accepted the camera-space fit.

The runtime containment invariant remains:

```text
water_accessible_footprint <= Citrus experimental_area / chaser boundary
```

That invariant is a Citrus runtime-policy constraint, not permission to inflate
the Orange physical observation. If a circle approximation or uncertain edge
would under-cover part of the observed footprint, Orange should report signed
residuals and require review. Any inward erosion or outward centroid
forgiveness belongs to `valid_detection_region`, post-processing masks, or
analysis/tracking diagnostics. Citrus may derive stimulus clearance
separately; neither offset is baked into the canonical inner-rim evidence.

## Circular Dish V0 Geometry

For the current circular single-arena dish, V0 should use a circle-first
workflow:

1. Capture a source frame in a declared coordinate system, preferably the full
   camera frame.
2. Run an OpenCV Hough-circle detector with logged parameters.
3. Show the detected circle as an overlay to the operator.
4. Let the operator accept or adjust center/radius.
5. Persist the accepted circle as the load-bearing Orange inner-rim boundary
   evidence.

The accepted circle, not the raw detector output, is the geometry proposed for
daily Citrus experimental-area registration. Detection gating and Palette
exports may use an offset `accepted_mask` or `valid_detection_region`, but
those derived masks are not the physical inner-rim boundary and do not carry
Citrus runtime acceptance.

### Commissioning Versus Daily Registration

The first trusted registration for a dish design is a commissioning operation:

- compare the accepted inner-rim center and full boundary against the canonical
  dish definition;
- permit an explicit center-and-radius proposal so an incorrect canonical size
  is not preserved indefinitely;
- require Citrus-side operator acceptance before any canonical or runtime
  experimental-area revision.

After a trusted inner-rim size exists, daily placement registration should
normally preserve that size and solve only placement. For a circle this is a
center update; radius deviation is a drift diagnostic. A meaningful radius
change should trigger re-commissioning or explicit operator-reviewed geometry
revision rather than being absorbed silently into every daily fit.

Orange may compute both commissioning and daily diagnostics, but they remain
proposals. Citrus owns the accepted definition/runtime revision.

Recommended V0 method string:

```text
orange_acquisition_circle_hough_operator_confirmed_v1
```

Coordinate rules:

- circle center is `[x, y]` or `{ "x": ..., "y": ... }`, never `[row, col]`
- image shape is `[height, width]` or `{ "height": ..., "width": ... }`
- saved top-rim observation artifacts must declare
  `source_array_role = "images_full"`
- do not save a top-rim observation artifact from a downsampled preview-space
  capture; Citrus homography captures are full-resolution camera images, so
  the accepted circle must be in the same coordinate space
- downsampled live-preview captures may be used only as operator/workflow
  previews until a full-resolution capture or validated coordinate-upscale path
  exists

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

### Physical units at the top-rim plane

When the selected dish definition provides a known inner diameter, Orange
copies that dimension and its source reference into the accepted boundary. For
a circular dish:

```text
physical_inner_radius_mm = physical_inner_diameter_mm / 2
top_rim_pixels_per_mm = accepted_radius_px / physical_inner_radius_mm
top_rim_mm_per_pixel = 1 / top_rim_pixels_per_mm
centroid_gate_outset_mm = centroid_gate_outset_px / top_rim_pixels_per_mm
```

The physical radius and camera-space radius describe the same water-side inner
rim at `target_plane = dish_top_rim`. This derived scale is therefore valid for
rim-local distances. A Citrus `pixels_per_mm_camera` value calibrated at
`projected_surface` may be recorded as a diagnostic comparison, but it is not
authoritative for the top-rim plane.

For the palm dishes, the known inner diameter is 80 mm, so the physical
inner-rim radius is 40 mm. The accepted Cam2010093 fit from 2026-07-18 has
`radius_px = 2152.7285`, yielding approximately `53.8182 px/mm`; a 12 px
centroid-gate outset is approximately `0.2230 mm` at that plane.

### Citrus fit-ring versus experimental-area diagnostics

The Citrus calibration fit ring and Citrus experimental area are separate
configured geometries and must be labeled separately in Orange:

- current `shadow/arena_1` experimental-area radius: `166 canvas px`, or
  `39.8145 mm` at `4.16934 canvas px/mm`;
- circular calibration fit-ring outer dot-center radius: `140 canvas px`, or
  `33.5785 mm`;
- fit-ring inset from the experimental-area boundary: `26 canvas px`, or
  `6.2360 mm`; the fit-ring radius is only `84.34%` of the experimental radius.

This explains why a projected circular fit pattern can look much smaller even
when the separately rendered experimental-area outline/crosshair is correctly
sized. The `experimental_area` mask clips the calibration pattern but does not
move the fit-ring dot centers from 140 px out to the 166 px boundary.

There is also a second, independent transform discrepancy in the 2026-07-18
Cam2010093 comparison. Orange currently loads the legacy sidecar
`calibration_artifacts/homography_arena_1_2010093.yml`, timestamped
2026-05-27. Inverse-projecting the configured 166 px experimental circle with
that matrix gives an approximately `(2253.05, 2358.99)` camera-pixel center and
`2033.68 px` radius. The new accepted rim is approximately
`(2226.53, 2237.55)` with `2152.73 px` radius. The predicted Citrus radius is
therefore `119.05 px`, or `5.53%`, smaller, and the centers differ by about
124 px. The 39.8145 mm versus 40 mm physical-radius difference is far too small
to explain that result.

Until a live capture validates which transform matches the physically rendered
outline, treat the legacy homography projection as a diagnostic rather than
ground truth. Orange now displays both the projected experimental area and the
projected 140 px fit ring with distinct labels and reports their camera-space
circle fits. The next calibration check should capture the live Citrus
experimental-area outline and fit-ring in the same camera state, then compare
each against its corresponding prediction and the plane-labeled Citrus
homography products.

## Mapping To Spatial Layout

This artifact is a concrete V0 producer for the existing spatial layout layers.
It does not introduce a fourth canonical geometry model.

Mapping:

```text
dish_top_rim_observation.observed_boundary
  -> spatial_layout.dish_mask.outer_geometry

dish_top_rim_observation.accepted_experimental_area_boundary (v1 compatibility)
  -> schema-v2 semantic: accepted_inner_rim_boundary
  -> Orange-confirmed water-side inner-rim evidence
  -> Citrus experimental_area / chaser boundary proposal
  -> spatial_layout.dish_mask.outer_geometry

dish_top_rim_observation.accepted_mask or valid_detection_region
  -> spatial_layout.dish_mask.valid_geometry

Citrus canonical dish/arena definition
  -> spatial_layout.arena_layout

Orange/Citrus accepted alignment semantics
  -> spatial_layout.view_registration

derived camera-pixel zone/mask geometry
  -> resolved runtime overlays
```

Interpretation:

- `dish_mask` is the camera-space valid-region layer. The inner-rim observation
  in the top-rim plane is one way to produce it.
- For the immediate imported single-circle slice, `arena_layout` is still the
  canonical Citrus dish/arena identity. More generally, definition authority
  follows provenance. The Hough circle must not silently replace an imported
  canonical arena layout.
- `view_registration` may use the accepted circle as evidence for a
  `translation` or `similarity` fit, but a top-rim circle is not a replacement
  homography.
- resolved runtime overlays are per-recording convenience outputs for drawing,
  gating, and H5 snapshots.

For the first circular single-arena implementation, the expected spatial-layout
runtime shape is:

```text
one dish_mask.valid_geometry circle
one arena_layout zone, usually zone_id = "z0"
zero or one view_registration suggestion/accepted fit
```

The recording snapshot and Citrus H5 output should keep both references:

- Orange source artifact and accepted mask
- Citrus layout/homography/registration state used during the experiment

## Relationship To Homography

Citrus already owns homography artifacts and mappings. V0 should not create new
Orange-owned homographies.

Reason:

- a homography is exact for one physical plane
- Citrus homography captures are usually tied to the projection/calibration
  plane
- the top rim is at a different height from the projection plane and may have
  parallax relative to the calibrated surface

Current Citrus coordinate convention:

- Citrus `homography_matrix` maps `camera_view_px` to
  `final_display_canvas_px`.
- camera points are OpenCV image pixels: origin top-left, x right, y down.
- Citrus `experimental_area_center_x_px`,
  `experimental_area_center_y_px`, and `experimental_area_radius_px` are
  arena-relative canvas coordinates, not full camera pixels and not global
  display-canvas coordinates.
- The matching camera calibration provides the global canvas placement:
  `arena_center_x_px`, `arena_center_y_px`, `arena_width_px`, and
  `arena_height_px`.

Therefore, when Orange previews the current Citrus experimental area in camera
space, it must first convert the arena-relative circle to global canvas
coordinates:

```text
arena_origin_canvas = (
  arena_center_canvas_x - arena_width_px / 2,
  arena_center_canvas_y - arena_height_px / 2
)

experimental_center_global_canvas =
  arena_origin_canvas + experimental_center_arena_relative
```

Only then should Orange apply `canvas_to_camera = inverse(camera_to_canvas)` for
the blue diagnostic overlay. Inverting the homography on the arena-relative
point directly is a coordinate-space bug and can make the Citrus overlay appear
drastically off center.

Safe V0 uses:

- map the accepted circle center and sampled circle perimeter points through
  the current Citrus homography as a preview
- report residuals and offsets
- let the operator accept a Citrus-side center correction only after review
- treat any radius correction as a separate future/explicit
  operator-reviewed behavior
- for rigs where the intended Citrus experimental area should cover every
  camera-observed location the fish can occupy, allow an explicit center+radius
  `experimental_area` adjustment mode that maps sampled accepted-boundary
  points into Citrus arena-relative coordinates and fits the Citrus
  experimental area there

Unsafe V0 uses:

- replace the Citrus homography from the top-rim fit alone
- treat an ellipse fit at the rim height as a full plane calibration
- silently adjust active stimulus geometry during an experiment

If future work needs a new homography, use the dish-plane homography workflow
with known projected points on the intended physical plane. Keep that separate
from the top-rim mask observation.

### Experimental Area Adjustment Policy

For the current chasing/stimulus use case, the desired Citrus policy is:

```text
experimental_area = camera-observed area the fish can occupy
```

This avoids a failure mode where tracking shows the fish inside the physical
dish area while Citrus logic considers it outside the configured experimental
area. In that case the chaser or other closed-loop stimulus may fail to pursue
or constrain the fish near the true boundary.

The stronger invariant is that every physically reachable fish position should
remain inside the configured Citrus experimental-area / chaser boundary. Small
positive margin is acceptable; undercoverage is not, because it creates states
where the fish is physically reachable but logically outside the closed-loop
stimulus region.

The center-only correction remains useful as a diagnostic because it separates
translation error from scale/radius error. The intended accepted correction for
this policy can be an explicit center+radius experimental-area adjustment:

1. Sample points along the accepted Orange top-rim circle in camera pixels.
2. Map each point through the existing Citrus `camera_view_px ->
   final_display_canvas_px` homography.
3. Convert each mapped point from global canvas coordinates to Citrus
   arena-relative coordinates.
4. Fit the Citrus experimental-area circle in arena-relative coordinates.
5. Present the proposed center/radius as an operator-reviewed Citrus
   experimental-area adjustment.

This still does not create a new homography. It updates or proposes Citrus
experimental-area parameters in Citrus-owned coordinates using the current
homography as the point-mapping bridge.

Important optics caveat: the projector is calibrated on a projection surface,
while the fish, dish bottom, water column, and top rim can live at different
effective optical heights. What is physically projected at the calibration
surface is therefore not guaranteed to match what the fish perceives or where
the fish can move in the camera image. If future accuracy requires modeling
that difference directly, use a separate projection-surface / dish-bottom /
fish-plane calibration workflow rather than overloading this top-rim
observation as a replacement homography.

### Projection Surface, Fish Plane, And Stimulus Size

Current rig interpretation:

- the projector image plane / projection surface is the diffusive gel
- Citrus homography is calibrated at that projection surface
- the tank bottom and fish plane are above the projection surface
- the tank bottom and fish plane are close enough that, for the current
  behavioral geometry, they can be treated as approximately the same plane
- the top rim is another visible camera boundary and is useful for fitting the
  experimental area, but it should not be confused with the projection surface

This means the current Citrus homography is exact for the projection surface,
not automatically exact for the tank-bottom/fish plane or top-rim plane. The
homography remains useful as the current point-mapping bridge, but
plane-height differences can change apparent center, radius, and local scale.

Orthographic versus perspective intuition:

- an orthographic system behaves as if imaging/projection rays are parallel
- in an orthographic system, apparent scale is nearly unchanged by small height
  differences
- the real camera/projector setup is not perfectly orthographic; it is a
  perspective optical system
- however, if the height difference between projection surface and fish plane
  is small compared with the camera/projector optical distance, the pure
  geometric scale change may be small enough for the current use case

Rule of thumb:

```text
approximate perspective scale change ~= plane_height_difference / optical_distance
```

For example, a `9 mm` plane separation over a `300-600 mm` optical distance is
on the order of `1.5-3%` before considering refraction/scattering. Whether that
is experimentally meaningful must be measured.

A physical target or scale image at dish/fish-plane height can answer a
camera-side question:

```text
camera_px at dish/fish plane -> physical mm at dish/fish plane
```

That helps measure fish position, dish size, and camera-space calibration at
the behavioral plane. It does not by itself fully answer:

```text
Citrus/projector canvas px -> physical/perceived stimulus size at fish plane
```

The current Orange calibration workflow does not collect projected-pattern
tank-bottom or fish-plane artifacts for this purpose. Do not project known dots,
bars, circles, or grids at dish/base or fish height and label them as camera-side
physical calibration. If a future experiment needs stimulus-size validation at
the behavioral plane, it should be defined as a separate projector-side
validation product with explicit projector/canvas provenance and must remain
separate from `C_base` / `C_fish` physical camera maps.

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
~/orange_data/calibrations/sessions/<session_id>/artifacts/Cam<serial>_<arena_id>/top_rim_observations/<artifact_id>/
|-- manifest.json
|-- observation.json
|-- captures/
|   |-- projector_off.png
|   |-- crosshair_on.png
|   `-- difference.png
|-- overlays/
|   |-- top_rim_fit.png
|   |-- registration_hough_overlay.png
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

### Schema-V1 Compatibility Decision

The current writer emits `schema_version = 2`. Existing schema-v1 artifacts
used `accepted_experimental_area_boundary`, including legacy interpretation
strings that can read as though Orange accepted Citrus runtime geometry.
Existing artifacts are immutable and remain readable as written.

For schema v1:

- treat `accepted_experimental_area_boundary` as Orange-operator-confirmed
  camera-space physical-boundary evidence only;
- require review of the source image/overlay before asserting that a legacy
  `top_level_visible_boundary` is specifically the water-side inner edge;
- do not infer Citrus acceptance from the field name, role, or interpretation
  string.

Schema v2 adds `accepted_inner_rim_boundary` with:

```text
target_plane   = dish_top_rim
target_feature = dish_inner_rim_water_side_edge
region         = water_accessible_footprint
```

New artifacts retain the v1 field as an explicit compatibility alias with
`alias_of = accepted_inner_rim_boundary`, `compatibility_alias = true`, and
`asserts_citrus_acceptance = false`. The alias carries the same non-eroded
geometry so v1 consumers can continue to read it. Existing v1 files are never
rewritten.

## Observation JSON Shape

The load-bearing schema-v2 boundary fields are:

```json
{
  "schema_id": "orange.calibration.dish_top_rim_observation",
  "schema_version": 2,
  "circle_detection": {
    "method": "orange_acquisition_circle_hough_operator_confirmed_inner_rim_v2"
  },
  "observed_boundary": {
    "target_plane": "dish_top_rim",
    "target_feature": "dish_inner_rim_water_side_edge",
    "region": "water_accessible_footprint",
    "coordinate_space": "camera_native_pixels",
    "geometry": {"type": "circle", "center_px": {"x": 1234, "y": 988}, "radius_px": 822}
  },
  "accepted_inner_rim_boundary": {
    "role": "orange_physical_boundary_evidence",
    "interpretation": "operator_confirmed_dish_inner_rim_water_side_edge",
    "target_plane": "dish_top_rim",
    "target_feature": "dish_inner_rim_water_side_edge",
    "region": "water_accessible_footprint",
    "boundary_inclusion_policy": "follow_observed_inner_rim_without_silent_offset",
    "coordinate_space": "camera_native_pixels",
    "geometry": {"type": "circle", "center_px": {"x": 1234, "y": 988}, "radius_px": 822},
    "physical_geometry": {
      "type": "circle",
      "coordinate_space": "dish_top_rim_mm",
      "center_mm": {"x": 0, "y": 0},
      "radius_mm": 40,
      "diameter_mm": 80,
      "dish_design_id": "palm1",
      "dimension_source": ".../shadow.json#dish_config.dimensions.diameter_mm"
    },
    "camera_scale": {
      "target_plane": "dish_top_rim",
      "pixels_per_mm": 20.55,
      "mm_per_pixel": 0.04866,
      "derivation": "accepted_camera_radius_px_divided_by_physical_inner_radius_mm"
    },
    "operator_confirmed": true
  },
  "accepted_experimental_area_boundary": {
    "compatibility_alias": true,
    "alias_of": "accepted_inner_rim_boundary",
    "asserts_citrus_acceptance": false,
    "geometry": {"type": "circle", "center_px": {"x": 1234, "y": 988}, "radius_px": 822}
  },
  "valid_detection_region": {
    "derived_from": "accepted_inner_rim_boundary",
    "erosion_px": 0,
    "centroid_gate_outset_px": 12,
    "centroid_gate_outset_mm": 0.584,
    "offset_direction": "outward",
    "purpose": "bounding_box_centroid_detection_gating",
    "geometry": {"type": "circle", "center_px": {"x": 1234, "y": 988}, "radius_px": 834}
  }
}
```

The following full schema-v1 `observation.json` example is retained as a
legacy reader reference. Its boundary role and interpretation strings do not
override the compatibility decision above:

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
    "registration_hough_overlay_path": "overlays/registration_hough_overlay.png",
    "registration_hough_overlay_sha256": "...",
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
  "accepted_experimental_area_boundary": {
    "role": "citrus_experimental_area_boundary",
    "interpretation": "operator_accepted_orange_dish_rim_equals_citrus_experimental_area_boundary",
    "physical_target": "dish_top_rim",
    "target_plane": "dish_top_rim",
    "coordinate_space": "camera_native_pixels",
    "geometry": {
      "type": "circle",
      "center_px": {"x": 1234, "y": 988},
      "radius_px": 822
    },
    "operator_boundary_target": "top_level_visible_boundary",
    "boundary_inclusion_policy": "prefer_slight_overcoverage_to_avoid_fish_escape"
  },
  "boundary_interpretation": {
    "accepted_boundary_role": "citrus_experimental_area_boundary",
    "accepted_boundary_semantics": "operator_accepted_orange_dish_rim_equals_citrus_experimental_area_boundary",
    "observed_boundary_surface": "dish_top_rim",
    "operator_boundary_target": "top_level_visible_boundary",
    "boundary_inclusion_policy": "prefer_slight_overcoverage_to_avoid_fish_escape",
    "valid_detection_region_policy": "derived_by_erosion_for_detection_gating_exports_not_primary_citrus_experimental_area_boundary"
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
    "diagnostic_only": true,
    "authority": "citrus_recomputes_before_acceptance",
    "semantics": "reviewable_center_alignment_suggestion",
    "citrus_config_ref": {
      "rig_id": "omnifin0",
      "canvas_id": "shadow",
      "arena_id": "arena_1",
      "homography_ref": "homography_arena_1_2010093.yml"
    },
    "homography_surface_assumption": "projection_plane",
    "observed_boundary_surface": "dish_top_rim",
    "arena_canvas_region": {
      "center_canvas_px": {"x": 257.0, "y": 329.0},
      "size_canvas_px": {"width": 344.0, "height": 344.0},
      "origin_canvas_px": {"x": 85.0, "y": 157.0}
    },
    "current_experimental_area": {
      "center_arena_relative_px": {"x": 172.0, "y": 172.0},
      "center_global_canvas_px": {"x": 257.0, "y": 329.0},
      "radius_canvas_px": 166.0
    },
    "observed_top_rim_center": {
      "camera_px": {"x": 2319.9, "y": 2286.7},
      "global_canvas_px": {"x": 266.3, "y": 326.4},
      "arena_relative_px": {"x": 181.3, "y": 169.4}
    },
    "proposed_center_only_correction": {
      "semantics": "center_only_runtime_registration_offset",
      "proposed_center_arena_relative_px": {"x": 181.3, "y": 169.4},
      "proposed_center_global_canvas_px": {"x": 266.3, "y": 326.4},
      "delta_arena_relative_px": {"dx": 9.3, "dy": -2.6},
      "delta_global_canvas_px": {"dx": 9.3, "dy": -2.6},
      "radius_policy": "preserve_current_citrus_radius"
    },
    "proposed_experimental_area_adjustment": {
      "available": true,
      "semantics": "center_and_radius_match_observed_experimental_area",
      "input_boundary": "accepted_top_rim_circle_sampled_in_camera_px",
      "mapping": "camera_px_to_global_canvas_px_to_arena_relative_px",
      "fit_space": "citrus_arena_relative_canvas_px",
      "proposed_center_arena_relative_px": {"x": 181.4, "y": 169.2},
      "proposed_radius_arena_relative_px": 184.6,
      "delta_arena_relative_px": {"dx": 9.4, "dy": -2.8},
      "delta_radius_px": 18.6,
      "radius_policy": "match_observed_experimental_area",
      "fit_quality": {
        "sample_count": 96,
        "residual_rms_arena_relative_px": 1.1
      }
    }
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

New recordings snapshot the exact selected daily-registration observation rather
than resolving a mutable "latest" artifact later. The compact, machine-readable
observation package is copied under
`recording_geometry_assets/cameras/Cam<serial>/daily_registration/`, while the
selection, acceptance, and candidate receipts live under
`recording_geometry_assets/daily_registration/`. The two accepted circles are
embedded directly in both the Orange recording snapshot and the immutable
recording geometry contract. Source paths and checksums remain present for
provenance.

This is intentionally separate from claiming that Orange used the region as a
live detector gate. Until Slice 3 is implemented,
`active_in_orange_live_detection_pipeline` remains `false`. Citrus records its
own exact daily-registration application state independently; Orange only marks
the selected mask as applied by Citrus after the returned camera, arena,
registration path, and checksum all match.

Representative `recording_snapshot.json` block:

```json
{
  "calibrations": {
    "2012632": {
      "dish_top_rim_observation": {
        "artifact_id": "dishrim_20260601_120000_2012632",
        "artifact_schema_id": "orange.calibration.dish_top_rim_observation",
        "artifact_schema_version": 2,
        "coordinate_space": "camera_native_pixels",
        "accepted_inner_rim_boundary": {
          "coordinate_space": "camera_native_pixels",
          "target_plane": "dish_top_rim",
          "geometry": {
            "type": "circle",
            "center_px": {"x": 2252.1, "y": 2281.7},
            "radius_px": 2146.7
          }
        },
        "valid_detection_region": {
          "coordinate_space": "camera_native_pixels",
          "purpose": "bounding_box_centroid_detection_gating",
          "offset_direction": "outward",
          "geometry": {
            "type": "circle",
            "center_px": {"x": 2252.1, "y": 2281.7},
            "radius_px": 2163.8
          }
        },
        "source": {
          "path": "/absolute/calibration/path/observation.json",
          "sha256": "sha256:..."
        },
        "available_for_downstream_detection_gating": true,
        "active_in_orange_live_detection_pipeline": false,
        "gating_semantics": "bounding_box_centroid_inside_valid_detection_region",
        "recording_local_assets": {
          "observation_relative_path": "recording_geometry_assets/cameras/Cam2012632/daily_registration/rim_observation/observation.json",
          "spatial_mask_export_relative_path": "recording_geometry_assets/cameras/Cam2012632/daily_registration/rim_observation/exports/spatial_dish_mask_runtime_v1.json",
          "palette_mask_export_relative_path": "recording_geometry_assets/cameras/Cam2012632/daily_registration/rim_observation/exports/palette_dish_mask_v2.json"
        },
        "citrus_runtime_application": {
          "capture_status": "pending_recording_start"
        }
      }
    }
  }
}
```

The exact same camera entry is included in
`recording_geometry_contract.json`. Citrus mirrors the full contract to
`/recording_geometry_contract/contract_json` and its camera-scoped view to
`/recording_geometry_contract/h5_scope_json`. This gives downstream readers a
recording-stable mask even when the original calibration-session directory or
active daily-registration pointer later changes.

## Palette Compatibility Mapping

Orange should not write Palette's H5 attributes as the native artifact format.
Instead, Orange should expose a deterministic adapter that converts an
Orange-native circular mask into Palette-compatible metadata when a Palette H5
or import package is requested.

Palette adapter load-bearing fields:

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
- current Citrus experimental center/radius in arena-relative and global
  canvas coordinates
- Orange-observed top-rim center or sampled boundary mapped through the Citrus
  homography into global canvas and arena-relative coordinates, if Citrus used
  those values during acceptance
- accepted center-only correction delta, if any, recomputed by Citrus
- accepted center+radius `experimental_area` adjustment, if explicitly
  configured for rigs where the projected/stimulus area should cover the
  camera-observed fish-occupiable area, recomputed by Citrus
- radius policy, such as `preserve_current_citrus_radius` or
  `match_observed_experimental_area`
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

The conservative V0 acceptance mode is center-only:

```text
proposed_experimental_area_center_arena_relative =
  camera_to_canvas(observed_top_rim_center_camera_px) - arena_origin_canvas
```

For the corrected-outline preview, Citrus should preserve the current
experimental-area shape and radius and move only the center. Radius adjustment
is a future or explicit operator-reviewed behavior because top-rim camera
radius, projection-plane canvas radius, and dish-height/parallax assumptions
are not interchangeable without additional modeling.

For this rig class, the intended Citrus experimental area is the
camera-observed area the fish can occupy. Use the explicit
`center_and_radius_match_observed_experimental_area` mode only after
scale-aware validation and operator review. That mode samples the accepted
Orange boundary, maps sampled points through the selected Citrus homography,
fits a circle in arena-relative coordinates, and proposes updated Citrus
experimental-area center/radius values.

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
8. Let the operator tune Hough parameters, rerun detection, and optionally
   numerically adjust the detected circle.
9. Detect the crosshair center from frame difference when crosshair capture is
   available.
10. Preview top-rim circle, valid detection region, and center offset.
11. Save the observation artifact and visible review overlay.
12. Optionally export or notify Citrus for preview.
13. For runtime use, mark the artifact active only after operator review.

Current Orange UI capture modes:

- `single_camera_direct_still`: opens/captures the selected camera directly.
  This is only useful when that camera can produce a lit frame without the
  normal multi-camera TTL-lighting path.
- `live_stream_preview_snapshot`: reads back the selected camera's latest
  uploaded GUI preview texture while normal streaming is active. This is the
  preview/workflow mode for TTL-lit rigs where another camera, such as
  `2010096`, drives the light source.
- `full_resolution_stream_snapshot`: requests the selected camera's acquisition
  thread to fan out one full-resolution `WORKER_ENTRY` to a snapshot worker.
  The snapshot worker copies the full camera frame into snapshot-owned memory,
  releases the acquisition frame promptly, then returns RGBA bytes to the
  Spatial Layout UI.
- `temporal_mean_stream_frames_v1`: requests the same acquisition fanout path
  for a bounded number of full-resolution frames, averages them in the snapshot
  worker, and returns one RGBA image. This is the preferred artifact capture
  mode for static calibration targets when the normal stream exposure is usable,
  because it improves signal/noise without temporarily changing camera exposure
  or frame rate.

Important limitation: `live_stream_preview_snapshot` captures the displayed GUI
preview. If display downsampling is active, Orange marks the capture as
`source_array_role = "images_ds"` and the circle coordinates are in preview
space, not full camera-native pixels. The Spatial Layout UI must not save a
top-rim observation artifact from that downsampled capture. Use it for operator
review and workflow validation only. To save the artifact while streaming with
downsampled preview, use `full_resolution_stream_snapshot` or
`temporal_mean_stream_frames_v1`.

The full-resolution stream snapshot path is an optional acquisition fanout
consumer, not a separate camera open/start/stop path. It follows the
`WORKER_ENTRY` retain/enqueue/release lease model documented in
`docs/threading_model_overview.md`: retain only when a snapshot request is
pending, copy each requested full-resolution frame into snapshot-owned memory,
release the acquisition frame promptly, then run Hough fitting and UI review
from the snapshot-owned copy. Averaged captures record `source_frame_count`,
the local/camera frame ranges, and
`temporal_compositing_method = "temporal_mean_stream_frames_v1"` in the saved
capture metadata.

Current Spatial Layout detection controls expose Hough `dp`, `param1`,
`param2`, minimum distance fraction, radius range fractions, radius adjustment,
median blur kernel, maximum detection dimension, and the fallback pass toggle.
After Hough detection, the detected circle center/radius can also be edited
directly before using it to seed the registration.

The Spatial Layout UI also exposes optional capture metadata fields for
`filter_state`, `runtime_filter_state`, `light_handling`, `light_state`,
`projector_state`, `projector_visible_to_camera`, structured illumination
wavelength metadata, repeatable filter reinstall requirements, and operator
notes. These fields document the calibration preflight state when an operator
saves a top-rim observation or image-set companion. They do not make daily
image-set capture mandatory for every recording.

`light_state` should remain a categorical state such as
`ttl_nir_strobe_active`, `visible_projector_only`, or `lights_off`. Wavelength
information belongs under `capture.illumination`:
`light_handling` separately records operator intent, such as
`leave_current`, `suppress_mapped_strobe`, or `keep_or_restore_mapped_pulse`.
When an exposed `nir_strobe_trigger` Rig I/O output mapping is available, the
Spatial Layout UI can explicitly apply those handling choices before capture;
artifact saving itself does not silently toggle lights.
The Spatial Layout workflow tabs group this under `Dish / Valid Area`, separate
from projection-surface homography captures and estimated-fish-plane scale
captures.

```json
{
  "spectrum": "narrowband_nir",
  "source": "custom_ttl_nir_strobe",
  "center_wavelength_nm": 855.0,
  "wavelength_confidence": "nominal"
}
```

For broadband visible light, use a wavelength range rather than a fake center
wavelength:

```json
{
  "spectrum": "broadband_visible",
  "source": "visible_projector",
  "min_wavelength_nm": 400.0,
  "max_wavelength_nm": 700.0,
  "wavelength_confidence": "approximate_range"
}
```

Initial UI presets include the current Orange/Citrus rig IR filter:

```text
installed: HOYA Creative Filter Infrared R72 67 mm (Kenko Tokina)
removed: HOYA Creative Filter Infrared R72 67 mm (Kenko Tokina)
```

The HOYA/Kenko R72 filter should be treated as filter transmission metadata,
not as the illumination wavelength. It is an IR long-pass filter that blocks
most visible light and passes near-infrared light above roughly 720 nm. A
calibration image can therefore record both an installed R72 filter and a
separate illumination source such as a nominal 855 nm TTL NIR strobe.

Light-state presets include TTL NIR strobe, visible projector, ambient room
light, external continuous visible light, external continuous IR/NIR light, and
lights-off states. Projector-state presets include off, black/idle, crosshair,
homography grid, scale pattern, and normal stimulus active states.

The Hough proposal is a separate overlay from the applied registered top-rim
mask. The proposal should remain visible on top of the registration overlay
with a labeled center marker so operators can distinguish "detected proposal"
from "applied registration".

When a Citrus template and homography are imported, the Spatial Layout UI also
shows a corrected Citrus outline preview. That preview preserves the current
Citrus experimental-area shape/radius, moves only the center to the
Hough-derived daily top-rim center mapped through the Citrus homography, and
draws the resulting outline back in camera space. It is a review overlay for
the V0 center-only correction contract, not an automatic Citrus config
mutation.

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

- [x] add schema constants and JSON writer for
  `orange.calibration.dish_top_rim_observation`
- [x] save source frames, Hough parameters, detected circle, accepted circle,
  visible overlay, and valid-region overlay
- [ ] save crosshair center and alignment offsets
- [x] no runtime detection gating yet
- [x] no automatic Citrus mutation

The implemented UI save action records a schema-v2 accepted inner-rim circle
and valid detection region from the current Spatial Layout fit. It labels the
plane, target feature, and water-accessible region explicitly, requires the
operator to confirm that the fit follows the water-side inner edge, and clears
that confirmation when the capture, Hough proposal, or accepted geometry
changes. The action does not yet capture or decode a projected crosshair
center, so crosshair alignment offsets remain future work.

### Slice 2: Operator Preview

- [x] add Orange overlay preview for Hough circle, accepted circle, and valid
  detection region
- add confidence and quality flags
- add explicit optical-state metadata in the UI
- [x] add optional Palette adapter export JSON for import testing
- [x] add spatial `dish_mask_runtime` adapter JSON for import/testing against
  the existing `recording_snapshot.json` calibration shape
- [x] add schema-v2 `accepted_inner_rim_boundary` output and an explicit
  `dish_inner_rim_water_side_edge` UI target without rewriting v1 artifacts

### Slice 2.5: Recording Persistence

- [x] resolve the exact selected daily-registration receipt at recording start
  without falling back to a mutable latest observation
- [x] validate the receipt, registration, candidate, commissioned homography,
  observation checksum, camera identity, coordinate space, and two-circle
  semantics
- [x] copy the compact observation, manifest, image-set reference, and spatial
  and Palette exports into recording-local assets
- [x] embed the full inner-rim boundary and outward centroid gate directly in
  `recording_snapshot.json` and the Orange recording geometry contract
- [x] expose the camera-scoped entry through the contract Citrus mirrors into
  H5
- [x] preserve the distinction between an available downstream mask and an
  active Orange live-detection gate
- [ ] backfill historical recordings; existing recordings remain unchanged

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

- Is the current approximately 0.223 mm outward centroid forgiveness sufficient
  for near-wall fish bounding boxes across cameras and body orientations?
- Can the rim be seen reliably in normal 850 nm runtime images, or is runtime
  verification usually `unknown`?
- Does removing and reinstalling the filter introduce measurable pixel shift?
- Should Orange request the Citrus crosshair through local control, or should
  V0 remain operator-driven?
- What minimum Hough quality metrics should be required before the UI allows
  operator confirmation?
- Should Citrus copy the Orange visible review overlay into its H5/output
  folder? V0 copies images into the Orange recording only when
  `ORANGE_RECORDING_GEOMETRY_COPY_IMAGES=1`; Citrus H5 stores the exact compact
  contract and checksums rather than image bytes.

## Resolved V0 Decisions

- V0 geometry for the current circular single-arena dish is an
  operator-confirmed circle in full-resolution camera-native pixels.
- V0 Citrus correction semantics are center-only:
  map the observed top-rim center through the existing Citrus
  camera-to-canvas homography, convert to arena-relative coordinates, and
  preserve the current Citrus experimental-area shape/radius. Any radius
  adjustment must be an explicit future/operator-reviewed behavior.
- OpenCV Hough circle detection is the initial automatic proposal mechanism.
- The accepted circle is the load-bearing physical inner-rim boundary; offset
  masks are derived detection/export views.
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
