# Spatial Layout Schema

Purpose: define the field-level schema for Orange spatial calibration payloads
used to describe `dish_mask`, canonical `arena_layout`, and the resolved
per-recording camera-view overlays consumed by Citrus.

Date anchored: 2026-04-06.
Last updated: 2026-07-18.
Status: draft schema, partially implemented in Orange code. Snapshot emission
and Citrus-side consumption are still pending.

Related documents:

- `docs/calibration_artifact_contract.md`
- `docs/calibration_image_set_schema.md`
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
- Per-arena `orange.calibration.arena_layout` artifacts are immutable saved
  measurements. A calibration session may also maintain a mutable
  `arena_layout_set.json` companion that points at the latest saved arena-layout
  artifact for each camera/canvas/arena. The set is an operator/navigation and
  import convenience. The per-arena artifact is the source of truth for the
  exact revision Orange used; `layout_authority` and provenance determine who
  owns the upstream semantic definition.
- `orange.calibration.image_set` is an acquisition/import artifact, not a
  runtime spatial-layout payload. Citrus may import image sets to fit or accept
  homography, scale, top-rim, or crosshair calibration, while `dish_mask` and
  `arena_layout` carry resolved geometry for Orange/Citrus runtime use.
- `dish_mask` names the observed usable boundary in camera space. In v1 that
  may be the inner experimental area if the outer dish rim is not visible.
- For the current circular single-arena V0, `dish_mask` may be sourced from
  `orange.calibration.dish_top_rim_observation`. In that case the
  observation's schema-v2 `accepted_inner_rim_boundary` is the
  Orange-confirmed physical boundary used for Citrus experimental-area review
  and `dish_mask.outer_geometry`. The v1
  `accepted_experimental_area_boundary` field remains an explicit compatibility
  alias and does not assert Citrus acceptance. The offset `accepted_mask`
  remains the `dish_mask.valid_geometry` / detection-export view.
- The Hough-detected circle is proposal/provenance. The operator-confirmed
  accepted circle is Orange's physical-boundary evidence for the daily
  experimental-area proposal; offset derived masks are used for detection
  gating or export.
- Palette `dish_mask` metadata is an adapter/export view from the Orange
  artifact. It is not the native spatial-layout payload.
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
  and a trivial one-zone `arena_layout` with
  `zone_id = "experimental_area"` so downstream consumers can already use the
  general runtime contract shape.
- In that single-circle slice, `arena_layout` remains the canonical
  Citrus-owned dish/arena identity, while `dish_mask` remains Orange's
  camera-space evidence about the visible and valid dish region.
- Importing a Citrus layout into an `orange.calibration.arena_layout` artifact
  does not transfer definition authority. Orange owns the immutable artifact
  and its execution provenance; Citrus remains authoritative for the imported
  `layout_id` and layout-space geometry. An Orange-authored layout instead uses
  `layout_authority.system = "orange"`.
- The current Orange UI may use an approximate camera-space circle fit to an
  inverse-projected Citrus contour as a preview/registration seed when
  homography is available.
- The current schema does not yet define an explicit `citrus_template_ref`. If
  exact Citrus provenance must survive into H5, the schema should be extended
  or that provenance should live elsewhere in recording metadata.
- Orange may include Citrus-space preview values, but they are diagnostic and
  non-authoritative. Citrus should recompute any accepted Citrus-space
  correction from the Orange camera-space observation and Citrus-owned
  homography/arena/optical-stack state.
- For Citrus single-circle daily top-rim correction, the conservative preview
  mode is center-only: map the observed top-rim center from camera pixels
  through the existing Citrus camera-to-canvas homography, convert to
  arena-relative coordinates with the Citrus arena canvas region, and preserve
  the current Citrus experimental-area shape and radius.
- A center+radius `experimental_area` adjustment is explicit
  operator-reviewed behavior. It should be recomputed by Citrus from sampled
  accepted-boundary points rather than silently inferred from only a
  camera-space Hough radius.
- The Citrus runtime containment invariant for chasing/stimulus use is
  `water_accessible_footprint <= Citrus experimental_area / chaser boundary`.
  Orange exports offset masks as `valid_detection_region` or
  `dish_mask.valid_geometry` for detection/analysis policy, but those derived
  regions must not silently replace the Citrus runtime `experimental_area`.
  Orange's accepted physical evidence should follow the observed water-side
  inner rim without silently expanding it. When uncertain, Orange should report
  signed fit residuals and uncertainty; Citrus owns any conservative runtime
  containment policy applied to its experimental area.

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
    "r": 2072.0
  },
  "edge_margin_px": 0.0,
  "centroid_gate_outset_px": 12.0
}
```

Required fields:

- `coordinate_space = "camera_native_pixels"`
- `outer_geometry`: `circle` or `oriented_rectangle`
- `valid_geometry`: same geometry type as `outer_geometry`
- `edge_margin_px`: number
- `centroid_gate_outset_px`: optional number; readers default it to zero for
  older runtime JSON

Rules:

- `edge_margin_px` must be greater than or equal to zero.
- `centroid_gate_outset_px` must be greater than or equal to zero.
- `edge_margin_px` and `centroid_gate_outset_px` are mutually exclusive.
- With a positive outset, `outer_geometry` must lie inside `valid_geometry`.
  Otherwise `valid_geometry` must lie inside `outer_geometry`.
- For circular masks, `outer_geometry` and `valid_geometry` should share the
  same center unless the operator explicitly overrides them.
- For rectangular masks, `outer_geometry` and `valid_geometry` are represented
  as `oriented_rectangle`.

Compatibility note:

- Older autofocus-only notes in this repo used the shorthand
  `cx/cy/r/rim_margin_px/r_valid`.
- New spatial-calibration work should use `outer_geometry`,
  `valid_geometry`, `edge_margin_px`, and `centroid_gate_outset_px` instead.
- UI and detection flows may describe this as the `experimental area` when that
  is the visible boundary the operator is fitting.

#### Circular top-rim V0 mapping

When a circular mask is sourced from
`orange.calibration.dish_top_rim_observation`, the runtime mapping is:

```text
observation.accepted_inner_rim_boundary
  -> dish_mask.runtime.geometry.outer_geometry

observation.accepted_experimental_area_boundary (schema-v1 fallback / schema-v2 alias)
  -> dish_mask.runtime.geometry.outer_geometry

observation.accepted_mask
  -> dish_mask.runtime.geometry.valid_geometry
```

Readers select `accepted_inner_rim_boundary` first, then the schema-v1
`accepted_experimental_area_boundary`, then a non-eroded observed rim circle.
The selected circle maps to `outer_geometry`. If the observation only has one
confirmed circle, Orange uses it as `outer_geometry` and derives
`valid_geometry` by adding `centroid_gate_outset_px` for new centroid-gating
artifacts or subtracting legacy `edge_margin_px`.

Compatibility rule: v1 consumers must continue to read
`accepted_experimental_area_boundary`. New design and UI language should call
the physical feature the water-side inner rim and must not imply that Orange
accepted a Citrus runtime configuration.

Recommended runtime source fields:

```json
{
  "source": "dish_top_rim_observation",
  "source_artifact": {
    "artifact_id": "dishrim_20260601_120000_2012632",
    "artifact_schema_id": "orange.calibration.dish_top_rim_observation",
    "artifact_schema_version": 1,
    "fingerprint": "fnv1a64:..."
  },
  "source_array_role": "images_full",
  "operator_confirmed": true,
  "review_overlay": {
    "path": "overlays/top_rim_fit.png",
    "sha256": "..."
  }
}
```

Rules:

- `source_artifact` should reference the Orange-native observation artifact,
  not a Palette export.
- `source_array_role` must match the coordinate system of the circle values.
- `operator_confirmed` should be true before using `valid_geometry` for
  detection gating or using `outer_geometry` for Citrus experimental-area
  review.
- Review overlays are for operator trust and audit; consumers should not parse
  geometry from overlay pixels.

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
- `ordering`: object, required when the layout's `ordering_rule` is
  `camera_row_major_v1`:
  - `rule`: `camera_row_major_v1`
  - `row_margin_px`: number
  - `column_margin_px`: number
  - `ordering_margin_threshold_px`: number
  - `ordering_status`: one of `resolved`, `ordering_unresolved`
  - `zone_index_to_id`: array of `zone_id` strings in index order

Rules:

- `layout_to_camera_matrix` maps canonical layout coordinates into
  camera-native pixels.
- If `camera_to_layout_matrix` is emitted, it should be numerically consistent
  with the inverse of `layout_to_camera_matrix`.
- `orientation_status` is required when `ordering_rule` is
  `row_major_from_layout_space` and the layout is symmetric; `ambiguous` or
  `unknown` blocks stable `zone_id` publication.
- Under `camera_row_major_v1`, `ordering_status` is `resolved` only when both
  margins exceed `ordering_margin_threshold_px`; otherwise it is
  `ordering_unresolved` and stable `zone_id` publication is blocked. The
  margins are centre-to-centre separations minus within-row or within-column
  scatter, as defined in the contract's "Ordering And Symmetry Rule".

Geometry consequence:

- circles remain circles after v1 registration
- canonical axis-aligned rectangles become runtime `oriented_rectangle`
  overlays when the resolved registration contains non-zero rotation

### Citrus Center-Correction Preview

For the current Citrus single-circle integration, Orange may preview a
center-only Citrus correction alongside the camera-space dish mask. This is a
review/registration suggestion, not a new homography.

Required Citrus coordinate inputs for this preview:

- `experimental_area_center_x_px`
- `experimental_area_center_y_px`
- `experimental_area_radius_px`
- matching `camera_calibrations[*].camera_id`
- matching `camera_calibrations[*].arena_center_x_px`
- matching `camera_calibrations[*].arena_center_y_px`
- matching `camera_calibrations[*].arena_width_px`
- matching `camera_calibrations[*].arena_height_px`
- matching camera-to-canvas homography sidecar

Coordinate rules:

- Citrus experimental-area center/radius are arena-relative canvas
  coordinates.
- Citrus arena center/width/height define the arena's region in global
  final-display canvas coordinates.
- The arena origin is:

```text
arena_origin_canvas = (
  arena_center_x_px - arena_width_px / 2,
  arena_center_y_px - arena_height_px / 2
)
```

- The proposed center-only correction is:

```text
proposed_center_arena_relative =
  camera_to_canvas(observed_top_rim_center_camera_px) - arena_origin_canvas
```

- A corrected Citrus outline preview should preserve the current Citrus shape
  and radius and move only the center to
  `proposed_center_arena_relative`.
- Radius changes require explicit operator-reviewed semantics and should use a
  separate field/policy from the V0 center-only correction.
- For rigs where the Citrus experimental area is intended to match the
  camera-observed area the fish can occupy, that explicit policy is a
  center+radius `experimental_area` adjustment. It should be computed by
  sampling the accepted Orange boundary in camera pixels, mapping the sampled
  points through the Citrus camera-to-canvas homography, converting them to
  arena-relative coordinates, and fitting the experimental-area circle in that
  Citrus coordinate space.
- Orange-side preview values for this adjustment should be marked
  `diagnostic_only`. Citrus owns the authoritative recomputation and accepted
  artifact/config/H5 state.

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
      "r": 2072.0
    },
    "edge_margin_px": 0.0,
    "centroid_gate_outset_px": 12.0
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

## Session Arena Layout Set

When operators save arena registrations one at a time, Orange updates a
session-scoped companion file:

```text
calibrations/sessions/<session_id>/arena_layout_set.json
```

Shape:

```json
{
  "schema_id": "orange.calibration.arena_layout_set",
  "schema_version": 1,
  "session_id": "calsess_2026_06_09T01_25_48Z_shadow",
  "selection_policy": "latest_per_camera_canvas_arena",
  "mutable_session_companion": true,
  "layout_count": 4,
  "layouts": [
    {
      "artifact_id": "arenalayout_citrus_shadow_arena_1_...",
      "artifact_schema_id": "orange.calibration.arena_layout",
      "artifact_schema_version": 1,
      "camera_serial": "2010093",
      "canvas_id": "shadow",
      "arena_id": "arena_1",
      "layout_id": "citrus_shadow_arena_1",
      "fingerprint": "fnv1a64:...",
      "relative_manifest_path": "artifacts/<artifact_id>/manifest.json",
      "relative_measurement_path": "artifacts/<artifact_id>/measurement.json",
      "relative_arena_layout_runtime_path": "artifacts/<artifact_id>/arena_layout_runtime.json",
      "relative_dish_mask_runtime_path": "artifacts/<artifact_id>/dish_mask_runtime.json"
    }
  ],
  "latest_by_camera_serial": {
    "2010093": "arenalayout_citrus_shadow_arena_1_..."
  },
  "latest_by_canvas_arena": {
    "shadow": {
      "arena_1": "arenalayout_citrus_shadow_arena_1_..."
    }
  }
}
```

Rules:

- Each click on `Save Arena Layout Artifact` writes a new per-arena artifact.
- If the same camera/canvas/arena is saved again, the cumulative set replaces
  that entry with the latest artifact ID.
- The cumulative file is intentionally mutable and session-scoped. Consumers
  that need immutable provenance should follow the listed per-arena artifact
  paths and fingerprints.

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
- `layout_authority`

### `layout_authority`

`layout_authority` separates ownership of the semantic layout definition from
custody of the immutable Orange artifact.

Required fields when present:

- `system`: one of `orange`, `citrus`
- `role = "definition_authority"`

Optional fields:

- `canonical_layout_ref`: object identifying the upstream definition
- `source_fingerprint`: string

Rules:

- New `imported_template` artifacts should include
  `layout_authority.system = "citrus"` and a stable upstream reference or
  fingerprint.
- New `manual_template` artifacts authored in Orange should include
  `layout_authority.system = "orange"`.
- `artifact_id` identifies the immutable Orange materialization and must not be
  substituted for the upstream `layout_id`.
- Existing schema-v1 artifacts without `layout_authority` remain readable. An
  imported artifact without it is authority-ambiguous and should produce a
  provenance warning rather than being assumed Orange-owned.

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
- When `provenance.ordering_rule` is `camera_row_major_v1`, `zone_index` is
  required on every zone and must be dense from 0.
- Canonical `rectangle` zones are axis-aligned in layout space.
- Zone geometry should lie inside `outer_geometry`.
- Overlapping zones are discouraged but not forbidden by schema alone.

### V0 Single-Zone Convention

For the current single-arena workflow, Orange writes exactly one zone that is
the experimental area:

```json
{
  "zone_id": "experimental_area",
  "zone_index": 0,
  "display_label": "Experimental Area",
  "geometry": {"type": "circle", "cx": 0.0, "cy": 0.0, "r": 50.0}
}
```

In this mode, `zones[0].geometry` mirrors `layout.outer_geometry`. The
Spatial Layout UI keeps this default single zone synchronized with the
experimental-area editor and offers a reset action to return to this shape.
This is intentionally not a separate zone-semantic model yet; it is the
minimum representation needed so downstream artifacts can always reference a
zone while the single-arena experimental area remains the source of truth.

Future multi-zone work may add physical partitions or behavior-specific ROIs
inside the same experimental area. Those zones should still live inside
`outer_geometry`, but their accepted semantics and Citrus runtime behavior are
deferred.

### `provenance`

Required fields:

- `source`: one of `manual_template`, `imported_template`
- `ordering_rule`: one of `camera_row_major_v1`, `row_major_from_layout_space`
  - `camera_row_major_v1`: well grid of spatially separated zones on a fixed
    plate; zones are independently fitted circles ordered row-major from the
    camera's top-left, and the materialization must record ordering margins
    (see the contract's "Ordering And Symmetry Rule")
  - `row_major_from_layout_space`: barrier-partitioned or symmetric layouts
    whose order is fixed in canonical layout space and requires
    `orientation_status`
  - legacy values written by existing producers (`row_major_top_left`,
    `single_circle_imported_from_citrus`) remain accepted for compatibility
    and carry no structural requirements; new artifacts should use one of the
    two enumerated values

Optional fields:

- `notes`: string
- `source_ref`: object containing the upstream path or stable identity and its
  fingerprint

Rules:

- `source = "imported_template"` should carry `source_ref` in newly written
  artifacts.
- `source_ref` is provenance for the definition that Orange materialized; it
  does not transfer ownership of that definition.

### `context`

Optional fields:

- `dish_design_id`: string
- `canvas_id`: string
- `arena_id`: string

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
  "layout_authority": {
    "system": "orange",
    "role": "definition_authority"
  },
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
    "canvas_id": "canvas_a",
    "arena_id": "arena_1"
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
- Under `camera_row_major_v1`, every runtime zone carries `zone_index`, the
  indices match `registration.ordering.zone_index_to_id`, and each zone's
  `geometry` is the independently fitted `circle` for that well rather than a
  transformed canonical shape.
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
