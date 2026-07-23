# Holder-Installed Fixture Validation

This commissioning checkpoint records the installed, flattened diffusive gels
that are the real projection surface during experiments. It preserves the
holder-removed rectangular homography as broad commissioning reference, fits a
separate holder-installed operational candidate through the visible aperture,
and independently validates that candidate. Capture and fitting never promote
the candidate automatically.

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
   a rectangular holder — primary support against the active transform;
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

## Run tomorrow

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

The default foreground is the holder-plane validated gray value `72`, at
`5 fps` and `100000 us`. Gray `76` retained good geometry but saturated dot
cores on cameras 2010094 and 2010095; gray `64` was below the circular
detector's reliable range. Use `--foreground-gray-u8` only when deliberately
testing a different intensity.

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
represented as a fully measured aperture. The analyzer then uses the existing
active Citrus homography as a read-only input and measures ring/grid and
verification-dot residuals.

Default gates require:

- at least four expected visible points in each validation pattern;
- at least 95% of expected visible points detected;
- RMS residual no greater than `0.75` canvas pixels;
- maximum point residual no greater than `1.5` canvas pixels;
- a plausible aperture area between 5% and 98% of the sensor.

Expected points outside the observed holder aperture are classified as
occluded, not as calibration failures. The report gives two distinct outcomes:
active dry-reference agreement and operational-candidate assessment. The first
may fail when the installed gels change the practical mapping, while the second
can pass on the primary rings and independent verification dots. The report and
runner use the operational assessment as their pass/fail result; dry-reference
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
