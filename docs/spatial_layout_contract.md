# Spatial Layout Contract (Draft)

Purpose: define a shared Orange/Citrus contract for stable zone identity across
recordings while also exposing resolved camera-pixel overlays for each
recording.

Date anchored: 2026-04-06.
Status: draft design, not implemented.

Field-level schema details now live in `docs/spatial_layout_schema.md`.

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
- `arena_layout`: canonical rig-level zone layout
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
              "r": 1920.0
            },
            "edge_margin_px": 140.0
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

- mint a new identity for the same layout when Orange already emitted a
  `calibration_ref`
- treat raw camera-pixel overlays as the source of truth
- require `experimental_area_*` to be the shared cross-tool contract

Current Citrus `experimental_area_*` fields may still be used as a derived
single-zone convenience view when a protocol is scoped to one zone, but the
shared Orange/Citrus contract should be based on `layout_id`, `zone_id`, and
resolved overlays.

## Ownership

Orange owns:

- canonical `arena_layout` artifact packages
- canonical `dish_mask` artifact packages
- per-recording `view_registration`
- resolved runtime overlays in `recording_snapshot.json`

Citrus owns:

- consumption of Orange-emitted refs and runtime overlays
- optional copy-through into H5/session metadata
- protocol- and stimulus-specific use of one or more resolved zones

## V1 Implementation Guidance

Recommended first implementation slice:

1. Add canonical `arena_layout` artifact support with `circle` and `rectangle`
   zones only.
2. Keep `dish_mask` as a separate per-camera artifact.
3. Add a per-recording `registration` block in `recording_snapshot.json`.
4. Emit resolved camera-pixel overlays for all visible zones.
5. Let Citrus consume those overlays directly.

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
