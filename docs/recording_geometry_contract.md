# Recording Geometry Contract

## Purpose

Every new Orange GUI or headless recording writes:

```text
<recording_folder>/recording_geometry_contract.json
```

The file is an immutable, self-contained snapshot of the geometry that can be
resolved at recording start. It lets a consumer interpret the recording later
without requiring Citrus to be running or the original Citrus files to remain
unchanged. Geometry metadata is optional: missing or invalid geometry is
recorded explicitly and never prevents recording.

The schema identity is `orange.recording.geometry_contract`, version 1.
`recording_snapshot.json` and `recording_session.json` contain a path and exact
SHA-256 reference to the contract.

## Selecting a Citrus rig and canvas

In the Orange Spatial Layout UI, choose **Import Citrus Canvas Config...** and
select the canvas JSON beneath the Citrus rig tree, for example:

```text
/home/jeremy/citrus/targets/rigs/omnifin0/blindfish/blindfish.json
```

Selection is valid for recording metadata even when the legacy spatial-preview
adapter cannot display that canvas shape. The UI reports the selected
`Recording metadata canvas` separately from preview availability.

GUI and headless runs may instead set:

```bash
ORANGE_CITRUS_RECORDING_CANVAS_CONFIG_PATH=/absolute/path/to/canvas.json
```

For the GUI, the explicit recording environment variable takes precedence over
guided-capture environment selections and the current UI selection. Headless
recording uses the explicit recording environment variable. If no canvas is
selected, Orange still writes a contract with `status = "not_configured"`.

The selected canvas folder determines its rig folder. Orange reads these files
directly; a live Citrus process and Citrus socket are not required.

## Ownership and inheritance

The selected canvas always owns experiment-specific semantics:

- experimental-area shape, center, and physical dimensions;
- selected tank/dish design;
- the camera-to-arena association expected by that canvas.

A canvas may declare `projection_geometry_authority` schema version 1 with:

```json
{
  "mode": "inherit_active_commissioning",
  "source_canvas_name": "shadow",
  "geometry_scope": "arena_placement_homography_and_projected_surface_scale",
  "experimental_area_owner": "selected_canvas"
}
```

In that case the authority canvas supplies only the stable commissioned
projection geometry:

- global canvas arena placement;
- accepted camera-native-pixel to final-display-canvas-pixel homography;
- accepted projected-surface physical scale.

This supports two canvases that use the same bolted rig, cameras, projector,
and holder geometry while retaining different experimental-area shapes. It
does not copy one canvas's experimental area into another.

Orange verifies matching canvas dimensions, arena/camera mappings, and camera
native rasters before accepting inherited geometry. It also verifies the active
commissioning release and the exact checksums, identities, coordinate
direction, target plane, and homography binding of its active homography and
scale members.

## Self-contained provenance

The contract snapshots the exact JSON and SHA-256 for all available sources:

- selected canvas configuration;
- projection-authority canvas configuration;
- rig configuration;
- active commissioning pointer and immutable release manifest;
- accepted per-camera homography and projected-surface scale pointers;
- the selected canvas's exact daily-registration runtime selection, accepted
  registration, candidate, and accepted schema-v2 rim observations;
- selected tank/dish definitions.

Per-camera records contain the selected experimental area, authority-owned
arena placement, homography matrix and provenance, scale values and provenance,
and the coordinate/plane declarations needed to interpret them. GUI recordings
also embed a configured Orange spatial artifact, including its resolved dish
mask, when `ORANGE_SPATIAL_CALIBRATION_ARTIFACT_<serial>` is set.

Only cameras participating in the recording are requested. The contract does
not indiscriminately copy every configured arena into each recording.

## Recording-local geometry assets

Every new contract-writing attempt also publishes:

```text
<recording_folder>/recording_geometry_assets/
  manifest.json
  daily_registration/
    runtime_selection.json
    registration.json
    candidate.json
  tank_designs/<tank-design-id>.json
  cameras/Cam<serial>/
    daily_registration/rim_observation/
      observation.json
      manifest.json
      image_set.json
      exports/spatial_dish_mask_runtime_v1.json
      exports/palette_dish_mask_v2.json
    projection/
      homography_active.json
      homography/candidate.json
      homography/homography.yml
      projected_surface_scale_active.json
      scale/candidate.json
      scale/observation.json
    spatial/
      manifest.json
      measurement.json
      arena_layout_runtime.json
      dish_mask_runtime.json
```

`recording_geometry_assets/manifest.json` uses schema
`orange.recording.geometry_assets`, version 1. Every listed file is an exact
source-byte copy with its SHA-256, byte count, semantic role, and camera/arena/
plane context. Mutable active pointers are copied together with their
checksum-bound immutable candidates and source observations. Only tank designs
and camera products referenced by participating cameras are materialized;
there is no cross-arena fallback or recursive copy of a Citrus rig tree.

The bundle is built in a staging directory, files are flushed before the
directory is atomically published, and an identical retry reuses it only after
the request identity and every listed checksum verify. A conflicting existing
bundle is never overwritten. `recording_geometry_contract.json`,
`recording_snapshot.json`, and `recording_session.json` carry the relative path
and exact manifest checksum.

Compact JSON/YAML evidence is always attempted. Large calibration captures and
PNG/TIFF/JPEG overlays are excluded by default. Set:

```bash
ORANGE_RECORDING_GEOMETRY_COPY_IMAGES=1
```

before starting Orange to copy the referenced homography and scale captures,
their review overlays, direct image evidence in the Orange spatial artifact,
and the daily rim source frame plus its Hough, accepted-rim, and valid-gate
overlays. The daily images retain and verify their declared FNV-1a checksums;
every copied file also receives a recording-local SHA-256. Image-copy failures
are reported separately and do not downgrade a complete compact bundle.

Asset statuses are `complete`, `partial`, `empty`, or `unavailable`. A partial
or unavailable copy never changes the numerical contract's status and never
blocks recording: the numerical geometry remains embedded in
`recording_geometry_contract.json`.

## Status and runtime participation

Top-level status values are:

- `resolved`: every requested camera passed Citrus commissioning validation;
- `partial`: the selection is valid but some optional commissioned products
  are unavailable;
- `invalid`: configured files or identities contradict the contract;
- `orange_only`: no Citrus selection exists, but a GUI Orange spatial artifact
  was embedded;
- `not_configured`: no optional geometry source was selected.

All statuses are non-blocking. Consumers must inspect the status before using a
transform or scale; they must not silently treat `partial`, `invalid`, or
`not_configured` as calibrated.

Static geometry and live participation are intentionally separate. Whether
Citrus was reachable and which daily registration it applied is captured under
`recording_snapshot.json.citrus_runtime_geometry`. A valid static contract does
not claim that Citrus rendered the experiment, and Citrus being unavailable
does not erase the recorded rig geometry.

## Daily registered dish masks

When the selected canvas has an accepted
`selected_daily_registration`, Orange resolves its checksum-bound registration
for only the cameras participating in the recording. For each camera it
validates the schema-v2 Orange observation identity, camera native raster,
camera/arena mapping, operator acceptance, and dish-top-rim coordinate plane.
It also verifies that the selection and registration agree on the validity
deadline and that the registration has not expired at recording-contract
capture time.
It also requires the registration's measured center/radius to agree with the
accepted physical inner-rim circle and requires the outward valid-detection
circle to be concentric and no smaller than that physical boundary.

The full numerical product is embedded at:

```text
recording_geometry_contract.json
  .daily_registration_geometry.cameras[serial].recording_snapshot_entry

recording_snapshot.json
  .calibrations[serial].dish_top_rim_observation
```

The entry intentionally keeps two circles:

- `accepted_inner_rim_boundary.geometry` is the operator-confirmed physical
  water-side inner rim in camera-native pixels;
- `valid_detection_region.geometry` (and the equivalent `accepted_mask`) is
  the outward-forgiving gate for bounding-box centroids.

The normative downstream gate is floating-point containment in the latter:

```text
(x - cx)^2 + (y - cy)^2 <= radius_px^2
```

Persistence alone does not enable this gate in Orange's live detector. The
source snapshot therefore starts with
`active_in_orange_live_detection_pipeline = false`. When Orange explicitly
arms a YOLO spatial-mask mode, the recording-specific contract records the
applied policy at:

```text
.analytics_runtime.yolo_spatial_mask
.cameras[serial].yolo_spatial_mask_runtime
```

and updates the recording copy of the compact entry with the actual live gate
and neural-input-mask state. `off` remains the default. Explicit non-off modes
must resolve this exact selected artifact and be acknowledged by the YOLO
worker before recording begins; they fail arm rather than silently falling
back to unmasked inference.

After Orange queries Citrus at recording start, it compares the live per-camera
registration path and SHA-256 with the static accepted registration and records
`selected_daily_registration_applied_by_citrus` only for an exact identity
match whose runtime target reports `applied = true`.

`base_only`, absent, partial, and invalid daily states remain explicit and
non-blocking. Orange never substitutes a timestamp-selected or unrelated rim
observation.

## Plane boundary

The commissioned scale and homography in this first contract apply to the
`projected_surface` plane. A dish top rim, water boundary, or fish-observation
plane must remain a separately identified product. Consumers must not relabel a
projected-surface transform as a fish-plane transform.

## Citrus H5 mirror

Citrus mirrors the contract into each experimental H5 after reading it through
the checksummed `recording_snapshot.json` reference:

```text
/recording_geometry_contract/contract_json
/recording_geometry_contract/h5_scope_json
```

`contract_json` contains the exact Orange source-file bytes. Its SHA-256 must
match both the Orange recording-snapshot reference and the H5 dataset
attribute. `h5_scope_json` is a derived Citrus view containing only the H5's
associated camera/arena entries and their referenced tank definitions. It
links back to the exact contract checksum and never substitutes another
arena's geometry.

Consequently, the camera-scoped schema-v2 mask is present in Citrus H5 through
`/recording_geometry_contract/contract_json`. It is also repeated inside the
matching camera's `daily_registration_geometry` entry in
`/recording_geometry_contract/h5_scope_json`, so an arena H5 does not need to
select a mask from another camera. Citrus separately records the
translation and registration it actually applied under
`/runtime_geometry_contract/daily_registration_json`; the two records serve
different purposes and should be checked together.

The mirror is metadata, not a Citrus runtime override. Citrus continues to
record the geometry it actually resolved and used under
`/runtime_geometry_contract`; that contract links to the verified Orange
mirror with `runtime_adoption = "metadata_only_not_applied_by_citrus"`.
Missing, malformed, out-of-folder, or checksum-mismatched sources are recorded
as explicit non-authoritative statuses and do not block an experiment.
