# Holder-Installed Fixture Validation

This commissioning checkpoint records the installed, flattened diffusive gels
that are the real projection surface during experiments. It preserves the
holder-removed rectangular homography as broad commissioning reference, fits a
separate holder-installed operational candidate through the visible aperture,
and independently validates that candidate. Capture and fitting never promote
the candidate automatically.

## Required predecessor: accept the dry-shelf reference

The holder-installed workflow requires an **accepted, current**
`commissioning_reference` homography for every selected arena/camera before
capture. The required order is:

1. With the holder removed, fit the unobstructed rectangular-grid reference,
   review its image overlays and quality gates, and explicitly promote it as
   `commissioning_reference` for the current canvas.
2. Install the holder and flatten the operational gels; leave disks, dishes,
   and water out. Capture the circular ring support and independent holder
   validation evidence, then review and explicitly promote the separate
   `operational_candidate` homographies.
3. Only after those holder-plane homographies are active, place one known-size
   acrylic disk on each gel and run projected-surface physical-scale capture.

A passing fit alone does not satisfy step 1. A fit-only centering run ends with
`rejection_receipt.json` when `--accept-homographies` was not armed. Citrus can
reload that immutable set with `load_homography_candidate_set_for_review` after
a restart, revalidate its exact canvas/configuration/source images and quality,
and then accept it with a separate explicit promotion arm. This can be done
without removing an already installed holder; if revalidation fails, restore
the unobstructed physical state and recapture rather than bypassing the gate.

The holder runner resolves its projector-intensity authority through the
accepted reference pointers. Before using it, also verify each pointer is
compatible with the **current canvas checksum and rig geometry revision**;
an old accepted pointer with a matching schema is not current authority. The
runner's intensity-resolution check alone is not a full canvas-compatibility
preflight.

### Earlier 2026-09-14 handoff state (superseded by capture below)

The new holder-removed reference fit is
`homography_set_homography_fit_2026-09-14T14_55_17Z` under the Shadow
`calibration_artifacts/homography_candidates/` root. All four 100-point fits
passed, but the fit-only runner finalized the live transaction with reason
`quality_passed_but_accept_homographies_not_armed`. The set remains reloadable
for guarded review. The Shadow canvas checksum at that time was
`sha256:09e5269acb9c6854a124dd278c477adf72c923c922de15a65a0466c638683b2b`;
the July reference pointers were accepted against an older canvas and must
not be treated as current. The operator has reported the holder and gels
installed. Confirm disks, dishes, and water are absent before holder capture.
Alternatively, to exercise the full fresh capture-to-accept workflow, remove
the holder and gels without moving the cameras, lenses, projector, or canvas;
restore the unobstructed dry shelf, recapture the reference, review all four
overlays and quality results, and explicitly accept the new reference before
reinstalling the holder. The 2026-09-14 rejected fit remains diagnostic
evidence in either path.

### 2026-09-14 holder capture and marker-aware reanalysis

After accepting the new dry-shelf reference and reinstalling the holder with
flattened gels (no dishes, water, or scale disks), the four-camera capture at
`/home/jeremy/orange_data/calibrations/commissioning/holder_fixture_20260914T211305Z/`
completed all five PTP-grouped scenes. A first independent analysis failed
because its dot search used the older active operational homographies from
before the camera move. That failed `validation_report.json` and its images
remain in place as audit evidence.

The corrected analyzer checks the accepted dry-reference pointer and SHA-256
recorded before capture, uses its matrix only to locate the holder pattern,
and retains the older active operational matrix as a read-only drift
comparison. The first reference-seeded analysis found 79/81 ring points on
each camera because its generic dot-area cap rejected the deliberately enlarged
primary-axis and secondary-chiral markers. That generic checker has been
removed. Its replacement reads marker roles and radius scales from the captured
projection snapshot and explicitly requires both visible markers. Reanalysis
of the **same immutable images** passes for all four cameras at
`marker_aware_reanalysis_v2/validation_report.json` below that run directory.
Each camera has 81/81 ring points, including both markers, and 13/13
verification points; held-out RMS is 0.059–0.126 canvas px. The earlier
reports are not superseded in place, and no analysis promoted a homography.
The review command rejects the older packages because they lack marker-aware
detector-v2 evidence; only the checksummed v2 package below is eligible.

For review, use the passing package at
`/home/jeremy/orange_data/calibrations/sessions/calsess_2026_09_14T21_13_33Z_shadow/derived/holder_fixture/marker_aware_reanalysis_v2/manifest.json`
with Citrus candidate set
`homography_set_holder_operational_calgrp_2026_09_14T21_14_03Z_shadow_homography_grid_projected_surface_g4`.
Promotion still requires explicit operator review and the guarded acceptance
command below. The holder and gels must remain fixed until the subsequent
physical-scale capture.

The fixture aperture is its own geometry:

- it describes what the camera can see through the installed holder;
- it is not the Citrus experimental area;
- it is not the dish inner rim or the water-side usable boundary;
- it may be `circle`, `rectangle`, `rounded_rectangle`, or `polygon`.

The current Shadow holder uses `circle`. A future rectangular holder selects a
rectangular projected-grid support automatically. Rounded-rectangle and polygon
holders use an inscribed circular support plus the observed aperture boundary.

## Physical state

Before capture:

- install the holder in its normal fixed position;
- flatten and seat each diffusive gel exactly as it will be used during an
  experiment;
- remove every dish and all water;
- leave the dry projected shelf beneath the openings;
- remove the camera IR filters;
- do not move the cameras, lenses, projector, canvas, or shelf.

## One-session capture

The permanent runner keeps one Orange process and one Citrus process alive and
appends five PTP-grouped captures to one Orange calibration session:

1. `black_reference` — dark/reference image for subtraction;
2. `uniform_gray` — a filled configured-arena rectangle used to measure the
   illuminated support visible through the holder;
3. `arena_outline` — full arena rectangle with its center fiducial;
4. `homography_rings` for the current circular holder, or `homography_grid` for
   a rectangular holder — primary support for the holder-plane candidate;
5. `verification_dots` — an independent validation point set.

All persisted image sets use
`capture_stage=projected_surface_holder_installed`,
`fixture_state=holder_installed_dish_absent`. The ring/grid image alone uses
`homography_role=operational_candidate`; black, gray, outline, and independent
verification images use `homography_role=validation_only`. Full-surface
reference scenes and circular support scenes retain their truthful, different
pattern domains.

While Orange and Citrus are still running, Citrus fits the operational
candidate and writes its normal immutable candidate JSON/YAML and detection,
reprojection, and coordinate-frame overlays. Orange then releases the live
transaction with a rejection receipt whose reason is
`persisted_for_external_holder_evidence_review`. This closes the live
transaction without deleting the reloadable candidate set and without changing
runtime authority.

## Run capture

After rebuilding Orange, reinstall the narrow privileged wrapper because this
workflow adds two whitelisted environment fields:

```bash
scripts/install_orange_gui_validation_wrapper.sh --install-sudoers
```

With the physical state above confirmed:

```bash
scripts/run_holder_fixture_validation.py \
  --execute \
  --confirm-holder-installed-dish-absent
```

The default foreground is not a compiled or script-level literal. Both the
manual Orange holder-homography capture and the permanent runner resolve it
from the immutable projector-intensity commissioning report referenced by the
selected cameras' commissioning-reference homographies. They verify the
report checksum, passing status, all-camera gate, and each selected camera's
quality result before asking Citrus to render. Missing or contradictory
authority fails closed. The current qualified Shadow report used for the
2026-09-14 holder capture resolves to gray `84`. An earlier report resolved to
gray `72`; gray `76` retained good geometry but saturated dot cores on cameras
2010094 and 2010095, while gray `64` was below the circular detector's reliable
range. Use `--foreground-gray-u8` only when deliberately testing a different
intensity; the override and the commissioned reference are both recorded.

Grouped image-set evidence records the actual `foreground_gray_u8`, immutable
report path/checksum, selected-camera validation scope, and the
commissioning-reference pointer checksums under both capture-group membership
and projected-pattern photometry.

The runner writes beneath:

```text
/home/jeremy/orange_data/calibrations/commissioning/holder_fixture_<UTC>/
```

The directory contains:

- `run_manifest.json` — physical-state, sequence, synchronization, and
  no-mutation contract;
- `guided_capture_result.json` — every grouped capture and the shared session;
- the guided result's `homography` object — persisted Citrus candidate-set ID,
  directory, target identities, fit status, and explicit no-promotion receipt;
- `validation_report.json` — observed aperture geometry, active artifact
  identity/checksum, point residuals, gates, and mutation summary;
- `validation_report.md` — short operator report;
- `overlays/Cam<serial>.png` — aperture, predicted points, detected points, and
  per-pattern residuals.
- `homography_qc/Cam<serial>_active_primary_reprojection.png` — active-transform
  residual vectors on the primary ring/grid capture;
- `homography_qc/Cam<serial>_active_heldout_reprojection.png` — the same check
  on the independent verification-dot capture.

Those top-level files are convenient commissioning-workspace outputs, not the
only copy. The analyzer also persists a checksummed evidence package into the
Orange calibration session that owns the source frames:

```text
calsess_<...>/
  derived/holder_fixture/holder_fixture_<UTC>/
    manifest.json
    validation_report.json
    validation_report.md
    guided_capture_result.json
  artifacts/Cam<serial>_arena_<n>/
    derived/holder_fixture_observations/holder_fixture_<UTC>/
      observation.json
      holder_aperture_overlay.png
      active_primary_reprojection.png
      active_heldout_reprojection.png
```

Each camera observation records the source capture paths, capture-group IDs,
camera/local frame IDs, PTP timestamp, declared image checksum, independent
SHA-256, observed holder arcs, arena/sensor clipping classification, the exact
homography identity evaluated, and every overlay/QC checksum. The session
manifest joins the four observations and report into one commissioning
checkpoint. Derived evidence is additive: the analyzer never rewrites
`image_set.json`, `manifest.json`, or their calibration fingerprints.

When invoked through sudo, the runner only hands ownership back to
`SUDO_UID:SUDO_GID` for output trees beneath
`/home/jeremy/orange_data/calibrations`; it rejects symlinks and paths outside
that root. The ordinary non-root workflow does not perform a chown.

The report also links the accepted dry candidate's immutable Citrus
`detection_overlay.png`, `reprojection_overlay.png`, and
`coordinate_frame_evidence.png` and the newly fitted operational candidate's
immutable Citrus artifacts. Its independent QC images also show Orange's
separate in-memory comparison refit in green. That Orange refit remains
diagnostic-only; only the persisted Citrus candidate can enter review and
promotion.

If the normal Python environment lacks OpenCV, the runner automatically invokes
the analyzer through the local `juicebox` Conda environment.

## Quality and authority

For each camera, analysis subtracts black from uniform gray and segments the
largest illuminated support. That support is explicitly modeled as the
intersection of the configured arena rectangle, fixture aperture, and camera
sensor. For example, Shadow's circular holder blocks the corners of the arena
but extends beyond its flat sides. The capture therefore observes only the
holder's curved corner arcs; it does not measure the unseen remainder of the
circle.

Overlays draw the complete illuminated-support contour in gray, boundary
samples explained by the configured arena in gold, and only directly observed
holder-aperture arcs in green. The JSON preserves both the complete support
boundary and the partial holder-arc evidence in camera-native and final-display
canvas pixels. A shape fit from partial arcs is diagnostic-only and is never
represented as a fully measured aperture. The analyzer uses the accepted,
pre-capture-checksummed dry commissioning reference to locate the ring/grid
and verification dots. It separately measures residuals against the previous
active operational homography as a read-only drift comparison.

Default gates require:

- at least four expected visible points in each validation pattern;
- at least 95% of expected visible points detected;
- RMS residual no greater than `0.75` canvas pixels;
- maximum point residual no greater than `1.5` canvas pixels;
- a plausible aperture area between 5% and 98% of the sensor.

Expected points outside the observed holder aperture are classified as
occluded, not as calibration failures. The report gives two distinct outcomes:
previous-active agreement and operational-candidate assessment. The first
may fail when the installed gels change the practical mapping, while the second
can pass on the primary rings and independent verification dots. The report and
runner use the operational assessment as their pass/fail result; previous-active
disagreement is retained under `commissioning_reference_comparison` as a
diagnostic finding. Neither outcome authorizes an automatic transform rewrite.

Promotion is a later, explicit operation. Citrus first revalidates the
candidate set and checksummed holder evidence package, preserves the dry
rectangle under a commissioning-reference pointer, then atomically selects the
holder candidate as runtime operational authority. Because projected-surface
scale artifacts are bound to an exact homography candidate, changing the
operational homography deliberately makes the current dry-reference scale
stale. Re-measure/revalidate scale on the installed flattened gel before an
experiment is allowed to start; do not silently reuse the dry binding.

The permanent review command is dry-run by default:

```bash
scripts/review_holder_operational_homography_candidates.py \
  --candidate-set-dir /path/to/homography_set_<id> \
  --expected-candidate-set-id homography_set_<id> \
  --expected-canvas-sha256 sha256:<digest> \
  --holder-evidence-manifest /path/to/session/derived/holder_fixture/<run>/manifest.json
```

With Citrus running, promotion additionally requires both `--execute` and
`--accept-operational-homographies`. The command reloads/revalidates the
persisted set first and sends the checksummed evidence package in the promotion
verification. It never promotes from the dry-run form.

Use an existing capture without controlling hardware with:

```bash
scripts/run_holder_fixture_validation.py --analyze-only \
  /path/to/run_manifest.json
```
