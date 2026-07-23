# Spatial Layout Contract (Draft)

Purpose: define a shared Orange/Citrus contract for stable zone identity across
recordings while also exposing resolved camera-pixel overlays for each
recording.

Date anchored: 2026-04-06.
Last updated: 2026-07-19.
Status: draft design, partially implemented in Orange UI/schema code. The
recording-snapshot writer and Citrus/H5 consumer path are still pending.

Field-level schema details now live in `docs/spatial_layout_schema.md`.

For generic Orange-acquired calibration image sets used by Citrus homography,
scale, top-rim, and crosshair workflows, see
`docs/calibration_image_set_schema.md`.

For the proposed single-window Orange/Citrus workflow that combines a stable
commissioning calibration with a translation-only daily dish placement, see
`docs/orange_citrus_guided_daily_registration_contract.md`.

For the implemented dry projected-surface disk workflow, including Orange and
Citrus ownership, the 3 mm target-plane contract, scale QC, and explicit
candidate promotion, see `docs/projected_surface_scale_commissioning.md`.

Camera-raster coordinate convention is shared with the calibration image-set
contract: `(0,0)` is the top-left camera pixel, `x` increases right, and `y`
increases downward. The Orange live stream and grouped-capture thumbnails are
the visual reference for this orientation. Spatial Layout preview canvases must
render images to match that convention; if a preview appears flipped relative
to the live stream, fix the preview renderer rather than flipping saved images
or artifact geometry.

For the current circular single-arena dish-mask workflow, including
Hough-circle/operator confirmation, Orange-native mask artifacts, Palette
export mapping, and Citrus H5 snapshot expectations, see
`docs/dish_top_rim_observation_design.md`.

## Problem

We want all of these at once:

- stable identity for sub-regions across recordings
- support for one camera seeing multiple sub-regions
- support for multiple cameras covering one large region
- robustness to small dish-placement changes between recordings
- direct camera-pixel overlays that Citrus can draw or log without doing extra
  geometry work

A fixed set of pixel ROIs is not enough for this. If the dish shifts or rotates
slightly between recordings, camera-native coordinates drift even when the
underlying experimental layout is the same.

## Design Summary

Use three layers:

- `dish_mask`: observed camera-view boundary for usable arena-bearing space
- `arena_layout`: authority-owned rig-level zone layout, materialized in an
  Orange artifact when Orange needs to register or snapshot it
- `view_registration`: per-camera, per-recording mapping from canonical layout
  space into camera-native pixels

Then also emit:

- `resolved runtime overlays`: the per-recording zone geometry already mapped
  into `camera_native_pixels`

Design rule:

- `layout_id` and `zone_id` are authoritative identity
- camera-pixel overlays are derived runtime outputs for convenience
- Orange should recompute overlays from the canonical layout plus the current
  recording's registration instead of treating the overlays as the source of
  truth

Recommended naming split:

- canonical artifact name: `arena_layout`
- per-recording resolved snapshot block: `calibrations[serial].arena_layout`
- per-recording transform payload inside that block: `registration`

### Definition Authority Versus Artifact Custody

The component that serializes or stores an `arena_layout` artifact is not
automatically the authority that may redefine the layout.

- Citrus is the definition authority for a layout imported from a Citrus
  rig/canvas/arena configuration.
- Orange is the definition authority for an Orange-authored standalone layout.
- Orange owns every Orange-native immutable artifact it writes, including an
  imported execution snapshot, the camera-space observation, the
  per-recording registration, and the resolved overlays.
- An imported Orange artifact must preserve the upstream layout identity and
  source fingerprint. Importing must not mint a second semantic identity or
  transfer definition authority to Orange.

This yields two different, compatible source-of-truth statements:

- the definition authority is the source of truth for layout identity and
  layout-space geometry;
- the immutable Orange artifact is the source of truth for the exact layout
  revision and registration Orange used for a calibration or recording.

## Immediate V1 Slice

Although the long-term contract supports multi-zone layouts, the immediate
Orange/Citrus integration slice should be narrower:

- single circular experimental area only
- Citrus remains the canonical owner of the selected canvas/dish definition
- Orange imports that Citrus definition for the selected camera
- Orange fits the current recording against that imported definition
- Orange writes the accepted per-recording fit into `recording_snapshot.json`
- Citrus mirrors that snapshot into H5

Practical consequence:

- for this slice, Orange may materialize a trivial `arena_layout` containing a
  single circular zone `z0` so downstream consumers can exercise the same
  runtime contract shape that later multi-zone layouts will use
- `dish_mask` remains the observed usable circular boundary in camera pixels
- the single-circle slice should validate ownership and snapshot flow before we
  expand authoring around multi-zone layouts

## Mapping To Dish Top-Rim Observation

The current circular-dish implementation path is documented in
`docs/dish_top_rim_observation_design.md`. It maps onto the spatial layout
contract as follows:

```text
orange.calibration.dish_top_rim_observation
  -> dish_mask
  -> optional view_registration input or preview
  -> resolved runtime overlays
  -> recording_snapshot.json and Citrus H5 calibration snapshot
```

Layer mapping:

- `dish_top_rim_observation.observed_boundary` is physical boundary evidence in
  `camera_native_pixels`. For the fish-occupiable circular-dish workflow, the
  intended feature is the water-side inner edge in the `dish_top_rim` plane,
  not the outer rim.
- `dish_top_rim_observation.circle_detection.detected_circle` is the Hough
  proposal shown to the operator, already scaled to full-resolution
  `camera_native_pixels`; top-rim save should persist this proposal instead of
  rerunning Hough at save time.
- `dish_top_rim_observation.accepted_inner_rim_boundary` is the schema-v2 name
  for the Orange-operator-confirmed physical boundary. It is evidence for a
  Citrus `experimental_area` proposal; it does not mean Citrus has accepted or
  applied it. `accepted_experimental_area_boundary` remains a schema-v1 reader
  fallback and an explicit compatibility alias in schema-v2 artifacts.
- `dish_top_rim_observation.accepted_mask` is the offset
  Palette-compatible/detection-gating view of that accepted boundary.
- `dish_mask.outer_geometry` should represent the accepted or observed
  water-side inner-rim circle when that feature bounds the water-accessible
  footprint.
- `dish_mask.valid_geometry` represents the derived detection-centroid gate.
  New Orange saves use an outward `centroid_gate_outset_px`; legacy artifacts
  may use an inward `edge_margin_px`.
- `arena_layout` remains the Citrus-owned canonical dish/arena definition and
  should not be replaced by the Orange Hough result.
- `view_registration` may use the accepted circle as evidence for
  `translation` or `similarity`, or may record
  `existing_homography_preview` semantics when Citrus homography is used only
  for review.
- resolved runtime overlays are derived per-recording camera-pixel geometry
  and remain convenience outputs.

The Orange top-rim artifact is therefore camera-space evidence for `dish_mask`
and Citrus experimental-area review, not a replacement for `arena_layout`.

The v2 field, with the v1 field as fallback/alias, produces a Citrus proposal
and the camera-space `dish_mask.outer_geometry`:

```text
dish_top_rim_observation.accepted_inner_rim_boundary.circle
  -> v1 fallback: accepted_experimental_area_boundary.circle
  -> Citrus experimental_area / chaser boundary proposal, pending Citrus acceptance
  -> dish_mask.outer_geometry.circle
```

The offset detection/export view is separate:

```text
dish_top_rim_observation.accepted_mask.circle
  -> dish_mask.valid_geometry.circle
  -> optional valid_detection_region / Palette export
```

If only one confirmed circle is available, Orange uses it as `outer_geometry`
and derives `valid_geometry` using exactly one policy: legacy inward
`edge_margin_px` or new outward `centroid_gate_outset_px`. Both values must not
be positive simultaneously. This offset changes centroid gating only; it does
not change the accepted physical rim.

Citrus H5 snapshots both sides for recording-bound selected daily
registrations:

- the Orange artifact reference, accepted circle, source coordinate system, and
  visible review overlay checksum
- the Citrus canvas/arena/homography identity and accepted transform semantics
  used during the experiment

Palette compatibility remains an export mapping from the Orange artifact. It
is not the native spatial-layout storage model.

## V1 Geometry Scope

Canonical layout geometry types in v1:

- `circle`
- `rectangle`

Notes:

- a square is represented as `rectangle` with `width == height`
- do not add a separate `square` type

Resolved runtime overlay geometry types in v1:

- `circle`
- `oriented_rectangle`

Notes:

- runtime squares/rectangles use `oriented_rectangle` in camera space so small
  placement rotation remains representable
- do not add `polygon` or raster masks in v1

Reason:

- runtime overlays are the canonical shapes after applying the per-recording
  registration transform into camera pixels
- under v1 `similarity` registration, a canonical rectangle stays a rectangle
  but may be rotated relative to the image axes
- circles stay circles because image-plane rotation does not change them

Canonical geometry field rules:

- `circle` fields: `cx`, `cy`, `r`
- `rectangle` fields: `x`, `y`, `width`, `height`
- all canonical geometry values are expressed in the declared
  `layout.coordinate_space`
- all runtime overlay geometry values are expressed in
  `camera_native_pixels`
- `rectangle.x` and `rectangle.y` refer to the top-left corner

## Canonical Identity

Primary identity fields:

- `layout_id`: stable id for the rig/canvas layout template
- `zone_id`: stable id for a sub-region inside that layout

Important distinction:

- `zone_id` is the identity
- `zone_index` is convenience metadata only

`zone_index` may be assigned row-major during layout authoring, but it must not
be treated as a cross-recording source of truth on its own.

## Dish Mask

`dish_mask` remains the observed valid camera-view region.

In many rigs, the camera sees the inner experimental boundary fully while the
outer dish rim is out of frame. In that case, the observed `dish_mask`
geometry may correspond to the visible experimental area rather than the full
physical dish.

It answers:

- what part of the frame contains usable arena-bearing space at all
- what inner experimental boundary Citrus can safely consume when that is the
  only fully visible circular reference

It does not answer:

- which inner sub-region is which

For v1, `dish_mask` should stay in camera-native pixel coordinates because it is
inherently a view-specific calibration.

## Arena Layout

`arena_layout` is the canonical zone template.

It should be treated as:

- rig-level or canvas-level layout metadata
- independent of one specific recording's dish placement
- the source of stable `layout_id` and `zone_id`

It should not be treated as:

- a one-off set of camera-pixel ROIs for a single recording

Recommended canonical coordinate space:

- preferred: `layout_mm`
- allowed if millimeters are not yet available: `layout_units`

V1 contract rule:

- canonical layout space must be declared explicitly
- runtime resolved overlays must always be emitted in
  `camera_native_pixels`

## View Registration

`view_registration` is per camera and per recording.

It answers:

- how this recording's observed camera view maps to the canonical layout

It is the piece that makes the system robust to small dish-placement changes.

Recommended initial registration types:

- `identity`
- `translation`
- `similarity`

Recommended v1 default:

- start with `similarity` when that is sufficient
- leave `homography` for a later schema version when runtime overlay geometry is
  rich enough to represent projective transforms cleanly

Recommended registration payload fields:

- `type`
- `layout_coordinate_space`
- `source`
- `layout_to_camera_matrix`
- optional `camera_to_layout_matrix`
- `fit_point_count`
- `residual_px`
- optional `orientation_status`

Rules:

- matrices are 3x3 row-major arrays with 9 numeric entries
- `layout_to_camera_matrix` maps homogeneous points in canonical layout space
  into camera pixels
- `camera_to_layout_matrix` is optional convenience output and, if emitted,
  should be numerically consistent with the inverse of
  `layout_to_camera_matrix`
- `residual_px` is the fit residual in camera pixels after registration

## Citrus Homography Note

For rigs that already have a Citrus camera-to-canvas homography, Orange should
use that mapping as a planar point transform.

What the homography is good for:

- mapping boundary points on the calibrated dish plane between camera pixels and
  Citrus canvas/projector pixels
- comparing Orange-detected boundaries against Citrus canonical experimental
  area definitions
- converting projector-space millimeter geometry into projector pixels using
  Citrus `pixels_per_mm_projector`

For example, a projector-space circle authored in millimeters should still be
treated as physically correct on the plane:

```text
radius_projector_px = radius_mm * pixels_per_mm_projector
```

Important limitation:

- a general homography preserves planar point correspondence, not circle
  parameterization
- a circle in Citrus projector/canvas space will generally appear as an ellipse
  in raw camera pixels
- therefore Orange should not assume that a fitted camera-space circle
  `(cx, cy, r)` can be transformed into a projector-space circle by directly
  transforming only those three parameters

Recommended single-circle workflow:

1. Citrus provides the canonical circular experimental area in
   projector/canvas space.
2. Orange detects boundary points in camera space.
3. Orange maps those boundary points through the Citrus homography into
   projector/canvas space.
4. Orange fits or compares the result against the Citrus canonical circle there.
5. Orange writes the per-recording resolved result into
   `recording_snapshot.json`.

If Orange needs to draw the expected projector-space circle back onto the camera
image, it should:

1. sample boundary points along the projector-space circle
2. map those sampled points through the inverse homography into camera space
3. draw that projected contour as the camera overlay

For current single-circle rigs, a camera-space circle approximation may still be
acceptable when perspective distortion is small, but the mathematically correct
cross-space model is boundary-point mapping on the calibrated plane.

Current Orange UI note:

- the current importer samples the Citrus projector-space circle, inverse-projects
  those points through the homography, and draws the resulting camera-space
  sampled outline as the current Citrus overlay
- Orange also fits an approximate camera-space circle to those sampled points
  for center/radius/RMS diagnostics and similarity-seed purposes
- that fitted-circle approximation is a registration aid only; it is not a
  claim that projector-space circles remain circles in raw camera pixels

## Citrus Import Surface For Single-Circle V1

For the current v1 slice, Orange should import a selected Citrus arena config
and match it to the currently selected camera serial.

Required Citrus fields:

- `config_name`
- `selected_dish_type_name`
- `experimental_area_shape`
- `experimental_area_center_x_px`
- `experimental_area_center_y_px`
- `experimental_area_radius_px`
- matching `camera_calibrations[*].camera_id`
- matching `camera_calibrations[*].arena_center_x_px`
- matching `camera_calibrations[*].arena_center_y_px`
- matching `camera_calibrations[*].arena_width_px`
- matching `camera_calibrations[*].arena_height_px`

Optional Citrus fields:

- `experimental_area_radius_mm`
- `camera_calibrations[*].pixels_per_mm_projector`

Optional Citrus sidecar:

- `<canvas>/calibration_artifacts/homography_<config_name>_<camera_id>.yml`
- the saved `homography_matrix` is `camera_view_px ->
  final_display_canvas_px`
- Orange must explicitly invert that matrix before projecting Citrus
  final-display canvas geometry into camera pixels for the preview overlay

V1 rejection rules:

- reject the import if no `camera_calibrations[*].camera_id` matches the
  selected Orange camera serial
- reject the import if `experimental_area_shape != CIRCLE`
- allow the import without a homography sidecar, but in that case skip the
  homography-seed assist

Coordinate rules for Citrus imports:

- Citrus `experimental_area_center_x_px`,
  `experimental_area_center_y_px`, and `experimental_area_radius_px` are
  arena-relative canvas coordinates.
- Citrus `arena_center_x_px`, `arena_center_y_px`, `arena_width_px`, and
  `arena_height_px` define that arena's placement in final display-canvas
  coordinates.
- To preview Citrus's current experimental area in camera pixels, Orange must
  first convert the arena-relative circle into global final-display canvas
  coordinates:

```text
arena_origin_canvas = (
  arena_center_x_px - arena_width_px / 2,
  arena_center_y_px - arena_height_px / 2
)

current_center_canvas =
  arena_origin_canvas + experimental_area_center_arena_relative
```

- Only then should Orange apply the inverse Citrus homography to draw the
  current Citrus area in camera pixels.
- Mapping `experimental_area_center_x_px/y_px` directly through the inverse
  homography is invalid because those values are not global canvas
  coordinates.

Daily top-rim correction semantics:

- Orange's fitted top-rim center is camera-native evidence about today's dish
  placement.
- Orange may map that center through the imported Citrus camera-to-canvas
  homography and subtract `arena_origin_canvas` to show a diagnostic
  arena-relative preview.
- Citrus must recompute this mapping from the Orange camera-space observation
  and Citrus-owned config/homography before accepting any Citrus-space
  correction.
- The conservative preview mode is **center-only**:

```text
proposed_experimental_area_center_arena_relative =
  camera_to_canvas(observed_top_rim_center_camera_px) - arena_origin_canvas
```

- In that preview, the current Citrus experimental-area radius and shape are
  preserved.
- Radius adjustment is future or explicit operator-reviewed behavior, not an
  implicit consequence of a daily top-rim center fit.
- For rigs where the intended Citrus experimental area should match the
  camera-observed area the fish can occupy, the explicit operator-reviewed
  behavior is a center+radius `experimental_area` adjustment. Compute it by
  sampling the accepted Orange boundary in camera pixels, mapping those points
  through the Citrus camera-to-canvas homography, converting them to
  arena-relative coordinates, and fitting the Citrus experimental-area circle
  there.
- This center+radius adjustment proposes Citrus experimental-area parameters in
  Citrus-owned coordinates. Citrus owns the authoritative recomputation,
  acceptance, config/runtime artifact, and H5 snapshot. It does not replace or
  rewrite the Citrus homography.

Next implementation step:

- thread the accepted single-circle fit into `recording_snapshot.json` using the
  existing `dish_mask.runtime` and `arena_layout.runtime` shapes
- add an explicit `citrus_template_ref` or equivalent stable provenance field
  before Citrus H5 consumers depend on exact config/homography identity

## Runtime Mapping Rule

Orange should be able to map camera-derived coordinates into canonical zone
space using the inverse of the resolved registration.

Conceptually:

```text
camera_native_pixels -> canonical_layout_space -> zone membership
```

For example:

```text
p_layout ~ H_camera_to_layout * p_camera
```

Then:

- zone membership is computed in canonical layout space
- camera overlays are derived by applying the forward registration back onto the
  canonical zone geometry

This is the intended mechanism for robustness to mm-scale placement changes.

## Ordering And Symmetry Rule

Top-left ordering is acceptable only during canonical layout authoring.

Recommended rule:

- define `zone_index` and initial `zone_id` order once in canonical layout
  space, for example row-major from top-left to bottom-right
- persist that mapping in the layout artifact
- do not re-infer canonical `zone_id` from raw camera-space top-left ordering on
  every recording

Reason:

- symmetric layouts can silently rename zones if the dish rotates, mirrors, or
  shifts enough to change row/column ordering

If the layout is symmetric, there must be a symmetry-breaking cue somewhere in
the setup or registration flow, for example:

- fixed holder orientation
- fiducial mark
- asymmetric chamber spacing
- manual orientation confirmation

## Canonical Arena-Layout Artifact

Recommended artifact schema id:

```text
orange.calibration.arena_layout
```

Recommended payload shape:

```json
{
  "schema_id": "orange.calibration.arena_layout",
  "schema_version": 1,
  "artifact_id": "arenalayout_2026_04_06_...",
  "created_utc": "2026-04-06T12:00:00Z",
  "calibration_ref": {
    "artifact_id": "arenalayout_2026_04_06_...",
    "artifact_schema_id": "orange.calibration.arena_layout",
    "artifact_schema_version": 1,
    "fingerprint": "fnv1a64:..."
  },
  "layout_id": "bank4_circle_v1",
  "layout": {
    "coordinate_space": "layout_mm",
    "outer_geometry": {
      "type": "rectangle",
      "x": 0.0,
      "y": 0.0,
      "width": 80.0,
      "height": 80.0
    },
    "zones": [
      {
        "zone_id": "z0",
        "zone_index": 0,
        "display_label": "top_left",
        "geometry": {"type": "circle", "cx": 20.0, "cy": 20.0, "r": 8.0}
      },
      {
        "zone_id": "z1",
        "zone_index": 1,
        "display_label": "top_right",
        "geometry": {"type": "circle", "cx": 60.0, "cy": 20.0, "r": 8.0}
      },
      {
        "zone_id": "z2",
        "zone_index": 2,
        "display_label": "bottom_left",
        "geometry": {"type": "circle", "cx": 20.0, "cy": 60.0, "r": 8.0}
      },
      {
        "zone_id": "z3",
        "zone_index": 3,
        "display_label": "bottom_right",
        "geometry": {"type": "circle", "cx": 60.0, "cy": 60.0, "r": 8.0}
      }
    ]
  },
  "context": {
    "dish_design_id": "dish4_circle_v1",
    "canvas_id": "canvas_a"
  },
  "provenance": {
    "source": "manual_template",
    "ordering_rule": "row_major_from_layout_space"
  }
}
```

Notes:

- `layout_id` is the stable template identity
- `zone_id` is the stable zone identity
- `zone_index` is only ordering convenience
- `outer_geometry` is required in v1 and is not a replacement for the
  per-camera `dish_mask`
- canonical artifact payload should not contain per-recording camera-pixel
  overlays

Recommended artifact invariants:

- `layout_id` must be unique within the producer's calibration namespace
- `zone_id` values must be unique within one `layout_id`
- `zone_index`, if present, should be unique within one `layout_id`
- all zone geometry must lie inside `outer_geometry`

## Recording Snapshot Integration

Orange should emit both identity and resolved overlay geometry into
`recording_snapshot.json`.

Recommended top-level shape:

```json
{
  "calibrations": {
    "2010093": {
      "dish_mask": {
        "calibration_ref": {
          "artifact_id": "dishmask_...",
          "artifact_schema_id": "orange.calibration.dish_mask",
          "artifact_schema_version": 1,
          "fingerprint": "fnv1a64:..."
        },
        "runtime": {
          "enabled": true,
          "schema_version": 1,
          "geometry": {
            "coordinate_space": "camera_native_pixels",
            "outer_geometry": {
              "type": "circle",
              "cx": 2254.0,
              "cy": 2256.0,
              "r": 2060.0
            },
            "valid_geometry": {
              "type": "circle",
              "cx": 2254.0,
              "cy": 2256.0,
              "r": 2072.0
            },
            "edge_margin_px": 0.0,
            "centroid_gate_outset_px": 12.0
          },
          "source": "manual"
        }
      },
      "arena_layout": {
        "calibration_ref": {
          "artifact_id": "arenalayout_...",
          "artifact_schema_id": "orange.calibration.arena_layout",
          "artifact_schema_version": 1,
          "fingerprint": "fnv1a64:..."
        },
        "runtime": {
          "enabled": true,
          "schema_version": 1,
          "layout_id": "bank4_circle_v1",
          "coordinate_space": "camera_native_pixels",
          "registration": {
            "type": "similarity",
            "layout_coordinate_space": "layout_mm",
            "source": "manual_fit",
            "layout_to_camera_matrix": [
              52.0, 0.0, 100.0,
              0.0, 52.0, 120.0,
              0.0, 0.0, 1.0
            ],
            "camera_to_layout_matrix": [
              0.0192307692, 0.0, -1.9230769231,
              0.0, 0.0192307692, -2.3076923077,
              0.0, 0.0, 1.0
            ],
            "fit_point_count": 8,
            "residual_px": 1.8
          },
          "visible_zone_ids": ["z0", "z1", "z2", "z3"],
          "zones": [
            {
              "zone_id": "z0",
              "zone_index": 0,
              "visibility_status": "full",
              "geometry": {"type": "circle", "cx": 1120.0, "cy": 1120.0, "r": 420.0}
            },
            {
              "zone_id": "z1",
              "zone_index": 1,
              "visibility_status": "full",
              "geometry": {"type": "circle", "cx": 3392.0, "cy": 1120.0, "r": 420.0}
            },
            {
              "zone_id": "z2",
              "zone_index": 2,
              "visibility_status": "full",
              "geometry": {"type": "circle", "cx": 1120.0, "cy": 3392.0, "r": 420.0}
            },
            {
              "zone_id": "z3",
              "zone_index": 3,
              "visibility_status": "full",
              "geometry": {"type": "circle", "cx": 3392.0, "cy": 3392.0, "r": 420.0}
            }
          ]
        }
      }
    }
  }
}
```

Rules:

- `calibration_ref` remains the identity of the canonical layout artifact
- `runtime.layout_id` repeats the canonical `layout_id` for convenience
- `runtime.schema_version` versions the resolved runtime payload shape rather
  than the canonical artifact schema
- `runtime.registration` records how the canonical layout was aligned to this
  recording
- `runtime.zones[*].geometry` is the resolved camera-pixel overlay Citrus can
  draw immediately
- `runtime.zones[*].zone_id` must match a zone defined by the referenced
  canonical layout artifact
- runtime overlays are convenience outputs, not canonical identity

Recommended `visibility_status` enum:

- `full`
- `partial`
- `occluded`

Recommended `source` enum for `runtime.registration.source`:

- `identity`
- `manual`
- `manual_fit`
- `detected_fit`
- `imported`

## Multi-Camera And Multi-Zone Behavior

### One Camera, Multiple Zones

One camera's `runtime.zones` list may contain many entries.

This is the standard multi-chamber-in-one-view case.

### Multiple Cameras, One Large Zone

The same `layout_id` and `zone_id` may appear in more than one camera block.

In that case:

- each camera has its own `registration`
- each camera emits its own resolved camera-pixel overlay
- `visibility_status` may differ by camera, for example `full` vs `partial`

This is the intended path for one large arena covered by multiple cameras.

## Citrus Consumer Rule

Citrus should consume:

- `layout_id`
- `zone_id`
- optional `visible_zone_ids`
- resolved camera-pixel overlays from the Orange recording snapshot

Citrus may mirror those into H5 or its own runtime metadata for convenience.

Citrus should not:

- mint a new semantic `layout_id` for a layout whose definition authority
  already supplied one; an Orange `calibration_ref` may identify only the
  immutable execution snapshot
- treat raw camera-pixel overlays as the source of truth
- require `experimental_area_*` to be the shared cross-tool contract

Current Citrus `experimental_area_*` fields may still be used as a derived
single-zone convenience view when a protocol is scoped to one zone, but the
shared Orange/Citrus contract should be based on `layout_id`, `zone_id`, and
resolved overlays.

## Ownership

Definition authority follows provenance:

- Citrus owns the canonical identity and layout-space geometry for layouts
  imported from Citrus.
- Orange owns the canonical identity and layout-space geometry for layouts
  authored as standalone Orange layouts.

Orange owns artifact custody for:

- Orange-authored `arena_layout` definitions
- immutable Orange materializations of imported layouts
- Orange-native `dish_mask` artifact packages and camera-space evidence
- per-recording `view_registration`
- resolved runtime overlays in `recording_snapshot.json`

Citrus owns runtime interpretation for:

- its rig/canvas/arena configuration and imported-layout definition revisions
- acceptance or rejection of Orange boundary/registration proposals
- optional copy-through into H5/session metadata
- protocol- and stimulus-specific use of one or more resolved zones

An Orange execution snapshot may be authoritative for what Orange used without
becoming authoritative for the upstream Citrus definition. Neither side may
silently rewrite the other side's artifact or semantic identity.

## V1 Implementation Guidance

Current implemented slice:

1. Orange can save immutable `arena_layout` artifact directories containing
   `circle` and `rectangle` zones plus resolved runtime sidecars. Such an
   artifact is either an Orange-authored definition or an imported execution
   snapshot; provenance determines which.
2. Saved artifact directories contain `measurement.json`,
   `arena_layout_runtime.json`, and `dish_mask_runtime.json`.
3. Recording startup can load those saved files through
   `ORANGE_SPATIAL_CALIBRATION_ARTIFACT_<serial>` and upsert
   `recording_snapshot.json["calibrations"][serial]`.
4. The emitted snapshot carries `arena_layout.registration`, resolved
   camera-pixel zone overlays, and the resolved `dish_mask.runtime`.
5. Independently of that legacy assignment hook, recording startup now resolves
   the exact selected Citrus daily registration and writes its accepted
   schema-v2 Orange rim and outward gate to
   `calibrations[serial].dish_top_rim_observation`. Exact compact sources are
   copied under `recording_geometry_assets`, and Citrus mirrors the contract
   into H5.
6. Until a standalone canonical `dish_mask` artifact file exists, the emitted
   `dish_mask.calibration_ref` is a runtime-derived ref tied to the saved arena
   artifact id.

Remaining recommended slice:

1. Add standalone canonical `dish_mask` artifact packages when the dish mask is
   no longer just the saved runtime sidecar.
2. Add a first-class UI/session assignment surface for non-daily standalone
   spatial artifacts instead of relying on the per-camera environment
   variables. Selected daily observations no longer require that hook.
3. Let Citrus consume the emitted `calibrations[serial]` overlays directly.

Recommended first fitting strategy:

- manual or semi-manual registration from an empty-dish frame
- use `similarity` first
- keep v1 registration to `identity` / `translation` / `similarity`

Recommended v1 scope boundary:

- allow `dish_mask` without `arena_layout`
- allow `arena_layout` runtime only when a valid `calibration_ref` exists
- do not require projector-space mapping in this contract
- do not require physical-world millimeter calibration for Citrus consumption
  of runtime overlays

## Open Questions

- Should canonical layout space require real millimeters in v1, or only
  recommend them?
- Should Citrus mirror `registration` into H5 as structured fields, or only
  preserve the JSON snapshot blob plus refs?
