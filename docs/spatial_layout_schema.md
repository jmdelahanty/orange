# Spatial Layout Schema

Purpose: define the field-level schema for Orange spatial calibration payloads
used to describe `dish_mask`, canonical `arena_layout`, and the resolved
per-recording camera-view overlays consumed by Citrus.

Date anchored: 2026-04-06.
Status: draft schema, partially implemented in Orange code. Snapshot emission
and Citrus-side consumption are still pending.

Related documents:

- `docs/calibration_artifact_contract.md`
- `docs/dish_top_rim_observation_design.md`
- `docs/spatial_layout_contract.md`
- `docs/recording_metadata.md`

## Identifiers

Artifact payload schemas:

- `schema_id = "orange.calibration.dish_mask"`
- `schema_id = "orange.calibration.arena_layout"`

Artifact payload schema versions:

- `schema_version = 1` for both artifact payloads above

Embedded runtime payload versions:

- `recording_snapshot.calibrations[serial].dish_mask.runtime.schema_version = 1`
- `recording_snapshot.calibrations[serial].arena_layout.runtime.schema_version = 1`

## Compatibility

- `dish_mask` and `arena_layout` are independent. A camera may emit either one
  or both.
- `dish_mask` names the observed usable boundary in camera space. In v1 that
  may be the inner experimental area if the outer dish rim is not visible.
- `layout_id` and `zone_id` are the authoritative stable identifiers. Runtime
  camera-pixel overlays are convenience outputs for a specific recording.
- Canonical `arena_layout` geometry is authored in layout space. Resolved
  runtime overlays are emitted in camera-native pixels.
- Canonical v1 layout geometry supports only `circle` and axis-aligned
  `rectangle`.
- Runtime v1 overlay geometry supports only `circle` and
  `oriented_rectangle`.
- Runtime v1 registration supports only `identity`, `translation`, and
  `similarity`.
- `homography` is intentionally left for a future schema version because a full
  projective transform would require richer runtime overlay geometry than v1
  defines.
- Existing Citrus homography is still useful in v1 as a point-mapping tool for
  single-circle rigs: it can accurately move boundary points between camera
  space and projector/canvas space on the calibrated dish plane.
- That does not mean circle parameters are preserved across spaces. A
  projector-space circle will generally appear as an ellipse in raw camera
  pixels under a general homography, so cross-space fitting should operate on
  sampled or detected boundary points rather than only on `(cx, cy, r)`.
- The immediate Orange/Citrus integration slice is a single circular
  experimental area imported from Citrus for the selected camera.
- Orange may represent that single-circle slice as both a circular `dish_mask`
  and a trivial one-zone `arena_layout` with `zone_id = "z0"` so downstream
  consumers can already use the general runtime contract shape.
- The current Orange UI may use an approximate camera-space circle fit to an
  inverse-projected Citrus contour as a preview/registration seed when
  homography is available.
- The current schema does not yet define an explicit `citrus_template_ref`. If
  exact Citrus provenance must survive into H5, the schema should be extended
  or that provenance should live elsewhere in recording metadata.

## Common Types

### `calibration_ref`

Required fields:

- `artifact_id`: string
- `artifact_schema_id`: string
- `artifact_schema_version`: integer
- `fingerprint`: string

Rules:

- `artifact_schema_id` must match the schema of the referenced artifact payload.
- `artifact_schema_version` must match the schema version of the referenced
  artifact payload.

### Coordinate-space enum

Supported values in v1:

- `camera_native_pixels`
- `layout_mm`
- `layout_units`

Rules:

- `camera_native_pixels` means raw pixel coordinates in the producer camera's
  native stream resolution.
- `layout_mm` is preferred for canonical layout authoring when physical
  measurements are available.
- `layout_units` is allowed when a stable rig-level template exists but the
  layout has not yet been measured in real millimeters.

### Geometry: `circle`

```json
{
  "type": "circle",
  "cx": 2254.0,
  "cy": 2256.0,
  "r": 1920.0
}
```

Required fields:

- `type = "circle"`
- `cx`: number
- `cy`: number
- `r`: number

Rules:

- `r` must be strictly positive.

Cross-space note:

- if Citrus authors a circular experimental area in projector/canvas space,
  that circle remains physically meaningful on the dish plane
- projector-space millimeter radii should still use
  `radius_projector_px = radius_mm * pixels_per_mm_projector`
- if Orange needs a camera overlay for that projector-space circle, it should
  project sampled boundary points through the inverse homography instead of
  assuming the camera overlay is exactly a circle

### Geometry: `rectangle`

Canonical layout rectangles are axis-aligned in canonical layout space.

```json
{
  "type": "rectangle",
  "x": 0.0,
  "y": 0.0,
  "width": 80.0,
  "height": 80.0
}
```

Required fields:

- `type = "rectangle"`
- `x`: number
- `y`: number
- `width`: number
- `height`: number

Rules:

- `x` and `y` refer to the top-left corner in the declared coordinate space.
- `width` and `height` must be strictly positive.
- A square is represented as `rectangle` with `width == height`.

### Geometry: `oriented_rectangle`

Runtime square/rectangular overlays are expressed as oriented rectangles in
camera space so the schema remains valid even when the layout is slightly
rotated during placement.

Why this is necessary:

- canonical `rectangle` geometry is axis-aligned in canonical layout space
- runtime overlay geometry is the canonical shape after applying the resolved
  per-recording registration transform
- under v1 `similarity` registration, a rectangle remains a rectangle, but it
  may be rotated relative to the camera pixel axes
- forcing the runtime overlay back into an axis-aligned `rectangle` would either
  mis-draw the true zone boundary or require a padded bounding box that covers
  extra background
- circles do not have this problem because rotation leaves a circle unchanged

```json
{
  "type": "oriented_rectangle",
  "cx": 2254.0,
  "cy": 2256.0,
  "width": 1800.0,
  "height": 1800.0,
  "rotation_deg_clockwise": 2.5
}
```

Required fields:

- `type = "oriented_rectangle"`
- `cx`: number
- `cy`: number
- `width`: number
- `height`: number
- `rotation_deg_clockwise`: number

Rules:

- `cx` and `cy` are the rectangle center in camera-native pixels.
- `width` and `height` must be strictly positive.
- `rotation_deg_clockwise` is measured in image space, where positive rotation
  turns clockwise with `+x` to the right and `+y` downward.

Example intuition:

- if a canonical square zone is authored upright in layout space
- and the dish is placed with a `3 deg` in-plane rotation during one recording
- the correct camera overlay for that zone is the same square rotated by
  `3 deg`

### `dish_mask_geometry`

`dish_mask` carries both an outer observed boundary and a valid inner boundary
that downstream consumers can use directly.

For current Orange rigs, this often means the detected circle is the inner
experimental area because the camera FOV is intentionally cropped tighter than
the full dish rim.

```json
{
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
}
```

Required fields:

- `coordinate_space = "camera_native_pixels"`
- `outer_geometry`: `circle` or `oriented_rectangle`
- `valid_geometry`: same geometry type as `outer_geometry`
- `edge_margin_px`: number

Rules:

- `edge_margin_px` must be greater than or equal to zero.
- `valid_geometry` must lie inside `outer_geometry`.
- For circular masks, `outer_geometry` and `valid_geometry` should share the
  same center unless the operator explicitly overrides them.
- For rectangular masks, `outer_geometry` and `valid_geometry` are represented
  as `oriented_rectangle`.

Compatibility note:

- Older autofocus-only notes in this repo used the shorthand
  `cx/cy/r/rim_margin_px/r_valid`.
- New spatial-calibration work should use `outer_geometry`,
  `valid_geometry`, and `edge_margin_px` instead.
- UI and detection flows may describe this as the `experimental area` when that
  is the visible boundary the operator is fitting.

### `registration`

```json
{
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
  "residual_px": 1.8,
  "orientation_status": "manual_confirmed"
}
```

Required fields:

- `type`: one of `identity`, `translation`, `similarity`
- `layout_coordinate_space`: one of `layout_mm`, `layout_units`
- `source`: one of `identity`, `manual`, `manual_fit`, `detected_fit`, `imported`
- `layout_to_camera_matrix`: array of 9 numbers, row-major 3x3
- `fit_point_count`: integer greater than or equal to 0
- `residual_px`: number greater than or equal to 0

Optional fields:

- `camera_to_layout_matrix`: array of 9 numbers, row-major 3x3
- `orientation_status`: one of `trusted`, `manual_confirmed`, `ambiguous`, `unknown`

Rules:

- `layout_to_camera_matrix` maps canonical layout coordinates into
  camera-native pixels.
- If `camera_to_layout_matrix` is emitted, it should be numerically consistent
  with the inverse of `layout_to_camera_matrix`.
- `orientation_status` is recommended whenever the layout is symmetric.

Geometry consequence:

- circles remain circles after v1 registration
- canonical axis-aligned rectangles become runtime `oriented_rectangle`
  overlays when the resolved registration contains non-zero rotation

## Artifact Schema: `orange.calibration.dish_mask`

### Top-level fields

Required fields:

- `schema_id = "orange.calibration.dish_mask"`
- `schema_version = 1`
- `artifact_id`
- `created_utc`
- `calibration_ref`
- `camera`
- `geometry`
- `provenance`

Optional fields:

- `context`

### `camera`

Required fields:

- `serial`: string
- `width`: integer
- `height`: integer

Optional fields:

- `pixel_format`: string

Rules:

- `width` and `height` must match the native camera stream dimensions used to
  author the mask artifact.

### `geometry`

Required shape:

- `dish_mask_geometry`

### `provenance`

Required fields:

- `source`: one of `manual`, `manual_fit`, `detected_fit`, `imported`

Optional fields:

- `source_image_kind`: one of `empty_dish_frame`, `calibration_capture`, `synthetic_template`
- `notes`: string

### `context`

Optional fields:

- `dish_design_id`: string
- `canvas_id`: string
- `shelf_id`: string

### Example

```json
{
  "schema_id": "orange.calibration.dish_mask",
  "schema_version": 1,
  "artifact_id": "dishmask_2026_04_06_...",
  "created_utc": "2026-04-06T12:00:00Z",
  "calibration_ref": {
    "artifact_id": "dishmask_2026_04_06_...",
    "artifact_schema_id": "orange.calibration.dish_mask",
    "artifact_schema_version": 1,
    "fingerprint": "fnv1a64:..."
  },
  "camera": {
    "serial": "2010093",
    "width": 4512,
    "height": 4512,
    "pixel_format": "Mono8"
  },
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
  "provenance": {
    "source": "manual",
    "source_image_kind": "empty_dish_frame"
  },
  "context": {
    "dish_design_id": "dish_circle_v1",
    "canvas_id": "canvas_a"
  }
}
```

## Artifact Schema: `orange.calibration.arena_layout`

### Top-level fields

Required fields:

- `schema_id = "orange.calibration.arena_layout"`
- `schema_version = 1`
- `artifact_id`
- `created_utc`
- `calibration_ref`
- `layout_id`
- `layout`
- `provenance`

Optional fields:

- `context`

### `layout`

Required fields:

- `coordinate_space`: one of `layout_mm`, `layout_units`
- `outer_geometry`: `circle` or `rectangle`
- `zones`: array of one or more zone objects

Zone object required fields:

- `zone_id`: string
- `geometry`: `circle` or `rectangle`

Zone object optional fields:

- `zone_index`: integer greater than or equal to 0
- `display_label`: string

Rules:

- `outer_geometry` is required in v1.
- `zone_id` values must be unique within one `layout_id`.
- `zone_index`, if present, should be unique within one `layout_id`.
- Canonical `rectangle` zones are axis-aligned in layout space.
- Zone geometry should lie inside `outer_geometry`.
- Overlapping zones are discouraged but not forbidden by schema alone.

### `provenance`

Required fields:

- `source`: one of `manual_template`, `imported_template`
- `ordering_rule`: string

Optional fields:

- `notes`: string

### `context`

Optional fields:

- `dish_design_id`: string
- `canvas_id`: string

### Example

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
        "geometry": {
          "type": "circle",
          "cx": 20.0,
          "cy": 20.0,
          "r": 8.0
        }
      },
      {
        "zone_id": "z1",
        "zone_index": 1,
        "display_label": "top_right",
        "geometry": {
          "type": "circle",
          "cx": 60.0,
          "cy": 20.0,
          "r": 8.0
        }
      },
      {
        "zone_id": "z2",
        "zone_index": 2,
        "display_label": "bottom_left",
        "geometry": {
          "type": "circle",
          "cx": 20.0,
          "cy": 60.0,
          "r": 8.0
        }
      },
      {
        "zone_id": "z3",
        "zone_index": 3,
        "display_label": "bottom_right",
        "geometry": {
          "type": "circle",
          "cx": 60.0,
          "cy": 60.0,
          "r": 8.0
        }
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

## Embedded Runtime Schema: `calibrations[serial].dish_mask.runtime`

Required fields:

- `schema_version = 1`
- `enabled`: boolean
- `geometry`: `dish_mask_geometry`
- `source`: one of `manual`, `manual_fit`, `detected_fit`, `imported`

Rules:

- `geometry.coordinate_space` must be `camera_native_pixels`.
- If `enabled = false`, the producer may omit `geometry`.
- When present, runtime geometry is the resolved mask for that recording and
  camera view, not the canonical artifact identity.

## Embedded Runtime Schema: `calibrations[serial].arena_layout.runtime`

Required fields:

- `schema_version = 1`
- `enabled`: boolean
- `layout_id`: string
- `coordinate_space = "camera_native_pixels"`
- `registration`
- `zones`: array

Optional fields:

- `visible_zone_ids`: array of strings

Zone entry required fields:

- `zone_id`: string
- `visibility_status`: one of `full`, `partial`, `occluded`
- `geometry`: `circle` or `oriented_rectangle`

Zone entry optional fields:

- `zone_index`: integer greater than or equal to 0

Rules:

- `runtime.zones` contains only zones for which Orange emitted a concrete
  camera-pixel overlay.
- If `visible_zone_ids` is emitted, it must exactly match the set of `zone_id`
  values in `runtime.zones`.
- `runtime.zones[*].zone_id` must exist in the referenced canonical
  `arena_layout` artifact.

## Recording Snapshot Integration

Recommended enclosing shape in `recording_snapshot.json`:

```json
{
  "calibrations": {
    "02010093": {
      "dish_mask": {
        "calibration_ref": {
          "artifact_id": "dishmask_...",
          "artifact_schema_id": "orange.calibration.dish_mask",
          "artifact_schema_version": 1,
          "fingerprint": "fnv1a64:..."
        },
        "runtime": {
          "schema_version": 1,
          "enabled": true,
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
          "schema_version": 1,
          "enabled": true,
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
            "residual_px": 1.8,
            "orientation_status": "manual_confirmed"
          },
          "visible_zone_ids": ["z0", "z1", "z2", "z3"],
          "zones": [
            {
              "zone_id": "z0",
              "zone_index": 0,
              "visibility_status": "full",
              "geometry": {
                "type": "circle",
                "cx": 1120.0,
                "cy": 1120.0,
                "r": 420.0
              }
            }
          ]
        }
      }
    }
  }
}
```
