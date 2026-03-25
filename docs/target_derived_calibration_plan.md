# Target-Derived Calibration Plan

Date: 2026-03-23
Scope: add target-based calibration artifacts in Orange for optical resolution
and depth-of-field characterization, starting with a USAF 1951 negative target
and a dedicated DOF target.

## Why This Exists

The current Orange calibration set already covers:

- field of view and approximate magnification
- iris vs relative irradiance
- field uniformity heatmaps

Those are necessary, but not sufficient, for statements like:

- "Can this rig reliably resolve a 1 mm larva?"
- "Can we see a 0.25 mm fin at the dish edge?"
- "How much z error can we tolerate before those structures are no longer
  resolved?"

Target-derived calibration is the next layer:

- USAF target -> in-plane spatial resolution
- DOF target -> resolution vs z

## Design Goals

- Keep Orange as the producer of immutable calibration artifacts.
- Reuse the current calibration artifact package model.
- Prefer quantitative outputs over subjective screenshots.
- Make user-facing claims conservative and auditable.
- Separate high-contrast optical resolution from biological feature visibility.

## Core Principle

A USAF target tells us a high-contrast resolution bound.

It does not directly prove that a translucent or low-contrast fish feature is
similarly visible. So Orange should report:

- what the optics resolve on the target
- what the object-space size of that target feature is
- what the field variation looks like

And only derive fish-facing claims conservatively from that.

## Target Types

### USAF 1951 Negative Target

Primary use:

- characterize in-plane spatial resolution at the dish plane
- compare center vs edge/corner performance
- compare focus / iris / exposure operating points

For a USAF 1951 target:

- each element contains horizontal and vertical three-bar patterns
- the spatial frequency is:

```text
lp_per_mm = 2^(group + (element - 1) / 6)
```

Useful object-space conversions:

```text
line_pair_period_um = 1000 / lp_per_mm
single_bar_width_um = 500 / lp_per_mm
```

Interpretation:

- `single_bar_width_um` is the most useful number for "smallest resolved
  high-contrast feature size"

### DOF Target

Primary use:

- characterize how usable resolution changes through z
- define a depth-of-field window using a resolution criterion rather than a
  generic sharpness score

The DOF target should be evaluated using the same spatial-resolution metric as
the USAF workflow wherever possible.

## Planned Artifact Types

Planned schema IDs:

- `orange.calibration.usaf1951_resolution`
- `orange.calibration.dof_resolution`
- `orange.calibration.imaging_capability_summary`

Notes:

- `usaf1951_resolution` is target-specific and explicit. Do not hide the target
  identity in a generic opaque artifact.
- `dof_resolution` is deliberately named around resolution, not just "focus",
  because the output should answer "what remains resolvable through z?"
- `imaging_capability_summary` is derived from other artifacts and is the place
  for conservative user-facing claims.

## Package Layout

Planned USAF package layout:

```text
<artifact_id>/
├── manifest.json
├── measurement.json
├── positions.csv
├── elements.csv
├── camera_config_snapshot.json
├── target_reference_frames/
├── analysis_overlays/
└── representative_frames/
```

Planned DOF package layout:

```text
<artifact_id>/
├── manifest.json
├── measurement.json
├── z_samples.csv
├── camera_config_snapshot.json
├── target_reference_frames/
├── analysis_overlays/
└── representative_frames/
```

## Planned Provenance Requirements

Each target-derived artifact should record:

- camera serial
- exact camera config snapshot
- lens name
- focus
- iris
- exposure
- gain
- pixel format
- field width / height and magnification inputs
- working distance
- linked upstream calibration artifact refs when used
- target identity and orientation
- capture timestamp and artifact fingerprint

Target-specific provenance should also include:

- target type
- target polarity (`negative` / `positive`)
- illumination mode (`transmission`, `reflection`, etc.)
- operator notes
- capture positions or z positions

## Target References

Target-derived artifacts should be able to reference prior Orange artifacts:

- FOV artifact or embedded FOV metadata for object-space conversion
- aperture artifact for the optical operating point

Minimum ref shape:

```json
{
  "artifact_id": "aperturecal_...",
  "artifact_schema_id": "orange.calibration.aperture",
  "artifact_schema_version": 1,
  "fingerprint": "fnv1a64:..."
}
```

## USAF Workflow

### Acquisition Mode

Orange should provide a dedicated calibration tool window for USAF capture.

It should reuse the current preview / capture infrastructure already built for:

- aperture characterization
- ruler-based FOV capture

Planned v1 acquisition behavior:

1. User selects one camera.
2. User chooses a fixed operating point:
   - focus
   - iris
   - exposure
   - gain
3. User enters or references field-size calibration.
4. User captures one or more target positions:
   - center
   - north
   - south
   - east
   - west
   - optional corners
5. Orange stores:
   - raw representative frame
   - optional overlay frame
   - operator label for that position

### Position Model

Use explicit named positions first, not arbitrary continuous coordinates.

Recommended labels:

- `center`
- `north`
- `south`
- `east`
- `west`
- `north_east`
- `north_west`
- `south_east`
- `south_west`

Reason:

- easier for operators
- easier to compare rigs
- enough to characterize field falloff before adding finer maps

### USAF Analysis Strategy

Do not jump straight to fully automatic target reading.

Planned phases:

#### Phase 1

- user captures the frame
- user provides or confirms a target ROI
- Orange evaluates candidate group/element patches inside that ROI

#### Phase 2

- automatic target finder
- automatic group/element localization
- automatic orientation handling

#### Phase 3

- field-wide position grid
- auto-generated capability maps

### Resolution Metric

For USAF, Orange should primarily use a contrast-transfer style metric, not
pretend to produce a slanted-edge MTF50 if no slanted edge is present.

Per element and axis (`horizontal` / `vertical`), compute:

- local bright mean
- local dark mean
- modulation / contrast score
- confidence score for bar spacing consistency
- resolved / not resolved decision

Recommended v1 resolved criterion:

- element accepted if:
  - modulation is above threshold
  - bar spacing estimate is self-consistent
  - pattern confidence exceeds threshold

Orange should store the full scored ladder, not just the "best resolved"
element.

### USAF Per-Position Outputs

For each captured position, Orange should emit:

- target label
- image path
- ROI
- orientation
- best resolved group/element for horizontal bars
- best resolved group/element for vertical bars
- corresponding `lp/mm`
- corresponding `single_bar_width_um`
- `pixels_per_line_pair`
- `pixels_per_bar`
- scored candidate list for all tested elements

Why both axes matter:

- field performance can be anisotropic
- one axis may degrade earlier near the edge

### USAF Summary Outputs

At artifact summary level, Orange should emit:

- center best resolved feature size
- worst-case resolved feature size across recorded field positions
- best-case resolved feature size across recorded field positions
- horizontal vs vertical asymmetry summary
- field variation summary

Recommended summary fields:

- `center_single_bar_width_um`
- `worst_field_single_bar_width_um`
- `best_field_single_bar_width_um`
- `worst_field_lp_per_mm`
- `field_resolution_cv`
- `field_resolution_range`

## DOF Workflow

DOF calibration should use a target at multiple z offsets near the imaging
plane.

Planned v1 behavior:

1. User fixes:
   - focus
   - iris
   - exposure
   - gain
2. User captures a sequence of z-labeled samples.
3. For each z sample, Orange computes the same resolution metric used for the
   USAF target or another target-specific resolution score.
4. Orange defines the usable DOF window from a threshold.

### DOF Criterion

Do not define DOF from an arbitrary focus score alone.

Preferred criterion:

- DOF is the z range where the chosen resolution metric remains above a stated
  threshold

Examples:

- "best resolved single bar width remains <= 250 um"
- "selected target element remains resolved"
- "contrast-transfer score remains above threshold for the reference element"

### Planned DOF Outputs

Per z sample:

- z offset
- captured frame path
- resolution metric
- resolved feature size
- pass/fail for selected criterion

Summary:

- lower z bound
- upper z bound
- total usable DOF span
- criterion used

## Imaging Capability Summary

This should be a derived artifact, not a direct capture artifact.

It should combine:

- FOV calibration
- aperture calibration
- USAF resolution calibration
- DOF calibration

And emit conservative statements like:

- "At the dish center, high-contrast features down to X um were resolved."
- "Across the recorded field positions, worst-case resolved high-contrast
  feature size was Y um."
- "At the chosen operating point, the usable DOF span for the selected
  criterion was Z mm."
- "A 0.25 mm feature corresponds to N pixels at this field size."

## Fish-Facing Claims

Orange should avoid claiming:

- "0.25 mm fins are guaranteed visible"

Orange should prefer:

- "High-contrast features down to X um are resolved under this operating
  condition."
- "A 0.25 mm structure spans N pixels at this field size."
- "This feature size is above / near / below the measured high-contrast
  resolution bound."

If a fish-facing claim is added later, it should be explicitly labeled as a
conservative interpretation layer, not a direct optical measurement.

## Recommended Conservative Rule

For user-facing biological guidance, consider requiring both:

- sufficient object-space sampling, for example `>= 4-6 px` across the feature
- a target-derived high-contrast resolution bound meaningfully smaller than the
  feature width

This margin matters because fish anatomy is not a high-contrast chrome-on-glass
pattern.

## Non-Goals For V1

Do not do these in the first implementation:

- fully automatic target reading from any arbitrary image
- generic MTF certification language
- arbitrary freeform field maps
- cross-rig statistical aggregation
- turning USAF results directly into biological truth statements

## Implementation Phases

### Phase 1: Docs And Contracts

- [ ] Add schema IDs to the calibration artifact contract.
- [ ] Define `measurement.json` shapes for USAF and DOF artifacts.
- [ ] Define minimum provenance fields and upstream artifact refs.

### Phase 2: Shared Target Capture Infrastructure

- [ ] Reuse current calibration preview and capture plumbing.
- [ ] Add target capture package writer.
- [ ] Add position labels and operator notes.
- [ ] Persist target reference frames and overlays.

### Phase 3: USAF V1 Analysis

- [ ] Manual or semi-manual ROI selection.
- [ ] Per-position horizontal/vertical element scoring.
- [ ] Best resolved element selection.
- [ ] Object-space conversion using FOV metadata.
- [ ] Summary metrics across positions.

### Phase 4: DOF V1 Analysis

- [ ] Z-labeled capture sequence.
- [ ] Resolution scoring per z sample.
- [ ] Threshold-defined DOF window.

### Phase 5: Derived Capability Summary

- [ ] Combine FOV, aperture, USAF, and DOF artifacts.
- [ ] Emit conservative, user-facing interpretation fields.

## Immediate Next Step

Before writing code, define the concrete JSON schema for:

- `orange.calibration.usaf1951_resolution`
- `orange.calibration.dof_resolution`

and decide whether USAF v1 will use:

- manual ROI only
- or manual ROI plus automatic element ladder scoring inside that ROI

Recommendation: manual ROI plus automatic ladder scoring.

## Concrete Schema Drafts

### Proposed USAF Measurement Shape

```json
{
  "schema_id": "orange.calibration.usaf1951_resolution",
  "schema_version": 1,
  "artifact_id": "usafcal_...",
  "created_utc": "2026-03-23T12:00:00Z",
  "calibration_ref": {
    "artifact_id": "usafcal_...",
    "artifact_schema_id": "orange.calibration.usaf1951_resolution",
    "artifact_schema_version": 1,
    "fingerprint": "fnv1a64:..."
  },
  "camera": {
    "serial": "2010096",
    "lens_name": "EF100mm f/2.8L Macro IS USM",
    "focus": 3100,
    "iris": 12,
    "exposure": 60,
    "gain": 256,
    "pixel_format": "Mono8",
    "width": 4512,
    "height": 4512
  },
  "request": {
    "target_type": "usaf1951",
    "target_polarity": "negative",
    "illumination_mode": "transmission",
    "field_calibration_ref": {
      "artifact_id": "aperturecal_...",
      "artifact_schema_id": "orange.calibration.aperture",
      "artifact_schema_version": 1,
      "fingerprint": "fnv1a64:..."
    },
    "camera_config_snapshot": {
      "has_snapshot": true,
      "snapshot_path": "camera_config_snapshot.json"
    },
    "operator_notes": "...",
    "positions": [
      "center",
      "north",
      "south",
      "east",
      "west"
    ],
    "resolution_thresholds": {
      "contrast_min": 0.2,
      "confidence_min": 0.8
    }
  },
  "summary": {
    "center_single_bar_width_um": 22.1,
    "worst_field_single_bar_width_um": 31.3,
    "best_field_single_bar_width_um": 22.1,
    "worst_field_lp_per_mm": 15.97,
    "field_resolution_cv": 0.11,
    "field_resolution_range_um": 9.2
  },
  "positions": [
    {
      "label": "center",
      "frame_path": "target_reference_frames/center.pgm",
      "overlay_path": "analysis_overlays/center_overlay.png",
      "roi": {
        "x": 1024,
        "y": 960,
        "width": 1800,
        "height": 1500
      },
      "horizontal": {
        "best_group": 4,
        "best_element": 2,
        "lp_per_mm": 17.959,
        "line_pair_period_um": 55.68,
        "single_bar_width_um": 27.84,
        "pixels_per_line_pair": 2.97,
        "pixels_per_bar": 1.48
      },
      "vertical": {
        "best_group": 4,
        "best_element": 3,
        "lp_per_mm": 20.159,
        "line_pair_period_um": 49.61,
        "single_bar_width_um": 24.80,
        "pixels_per_line_pair": 2.65,
        "pixels_per_bar": 1.32
      },
      "scored_elements": [
        {
          "group": 4,
          "element": 1,
          "axis": "horizontal",
          "lp_per_mm": 16.0,
          "contrast": 0.41,
          "confidence": 0.97,
          "resolved": true
        }
      ]
    }
  ]
}
```

### Proposed DOF Measurement Shape

```json
{
  "schema_id": "orange.calibration.dof_resolution",
  "schema_version": 1,
  "artifact_id": "dofcal_...",
  "created_utc": "2026-03-23T12:30:00Z",
  "calibration_ref": {
    "artifact_id": "dofcal_...",
    "artifact_schema_id": "orange.calibration.dof_resolution",
    "artifact_schema_version": 1,
    "fingerprint": "fnv1a64:..."
  },
  "camera": {
    "serial": "2010096",
    "lens_name": "EF100mm f/2.8L Macro IS USM",
    "focus": 3100,
    "iris": 12,
    "exposure": 60,
    "gain": 256,
    "pixel_format": "Mono8",
    "width": 4512,
    "height": 4512
  },
  "request": {
    "target_type": "dof_resolution_target",
    "criterion_type": "resolved_feature_width_um",
    "criterion_threshold_um": 250.0,
    "z_axis_units": "mm",
    "camera_config_snapshot": {
      "has_snapshot": true,
      "snapshot_path": "camera_config_snapshot.json"
    },
    "operator_notes": "..."
  },
  "summary": {
    "criterion_type": "resolved_feature_width_um",
    "criterion_threshold_um": 250.0,
    "lower_z_mm": -1.5,
    "upper_z_mm": 1.0,
    "usable_span_mm": 2.5,
    "reference_z_mm": 0.0
  },
  "z_samples": [
    {
      "z_mm": -2.0,
      "frame_path": "target_reference_frames/z_-2_0.pgm",
      "overlay_path": "analysis_overlays/z_-2_0_overlay.png",
      "resolved_feature_width_um": 310.0,
      "criterion_pass": false
    },
    {
      "z_mm": 0.0,
      "frame_path": "target_reference_frames/z_0_0.pgm",
      "overlay_path": "analysis_overlays/z_0_0_overlay.png",
      "resolved_feature_width_um": 180.0,
      "criterion_pass": true
    }
  ]
}
```
