# Projector Intensity Commissioning

This utility selects an auditable projector command intensity for geometric
calibration patterns. It is a commissioning diagnostic, not a homography
candidate writer or active-calibration promoter.

## Physical state

The live command requires explicit confirmation of `unobstructed_dry_shelf`:

- tank holder removed;
- dishes absent;
- water absent;
- camera filters removed;
- the dry projection shelf remains installed.

Stop and use a holder-installed recipe if the rectangular arena corners are
occluded. The utility intentionally requests Citrus `homography_grid` over the
full rectangular arena support.

## Capture method

The complete sequence runs in one Orange process and one Citrus process. It
first captures the configured rectangular arena outline with Citrus's
ring-with-center-dot arena-center fiducial, then performs the grayscale grid
sweep. Each reference/level/repeat is a separate guided grouped capture, but all
images append to one calibration session. Citrus displays an opaque grayscale pattern
(`R=G=B=gray`, `A=255`), waits for its presentation fence, and Orange saves one
full-resolution frame per camera.

The launcher derives a temporary camera config with 100 ms exposure at 5 FPS
by default before Orange opens the cameras. The cameras therefore establish
their common `ptp_gate` cadence at commissioning timing. The preflight verifies
those live readbacks without rewriting values that already match, suppresses
the mapped NIR strobe, and transactionally restores the in-memory startup
settings. The normal persistent camera config is never modified.

Every grouped result records each camera's embedded PTP timestamp. Validation
requires `sync_mode=ptp_gate`, all requested timestamps to be present, and a
maximum within-group timestamp span of 100,000 ns (0.1 ms). Thus “grouped” is
not treated as evidence of simultaneous exposure unless the PTP timestamps
also pass.

Repeats remain individual frames. The utility does not record video or compute
a temporal mean because either would obscure frame-to-frame centroid movement
and intermittent detector failures.

The default sweep is 64, 128, 192, and 255 with five repeats at each level:

```bash
conda run -n juicebox python \
  scripts/run_projector_intensity_commissioning.py \
  --execute \
  --confirm-unobstructed-dry-shelf
```

Run without `--execute` to print the plan. A completed sweep can be re-analyzed
with `--resume`; a failed partial single-process sweep is rerun as one atomic
sweep so the one-session and PTP-timing contract remains clear:

```bash
conda run -n juicebox python \
  scripts/run_projector_intensity_commissioning.py \
  --execute \
  --confirm-unobstructed-dry-shelf \
  --resume \
  --output-dir /home/jeremy/orange_data/calibrations/commissioning/projector_intensity_<stamp>
```

## Products and selection rule

The output directory contains:

- `run_manifest.json`: fixture, projection, timing, camera scope, and every
  guided result path;
- `commissioning_report.json`: complete frame and camera/level metrics;
- `frame_metrics.csv`: scalar per-frame metrics;
- `commissioning_report.md`: compact operator-facing comparison.
- `images/gray_<level>/repeat_<n>/Cam<serial>.png`: convenient symlinks for
  browsing; the authoritative PNGs remain in the single calibration session;
- `images/arena_outline_with_center_fiducial/Cam<serial>.png`: the commissioning
  arena boundary and center reference for every camera;
- `image_index.csv`: convenience path, authoritative session path, capture
  group, and checksum for every image.

The manifest's `calibration_session.session_dir` points directly to the one
authoritative session under `/home/jeremy/orange_data/calibrations/sessions`.
The older process-per-sample behavior remains available only as the diagnostic
`--legacy-one-shot` option.

The analyzer runs the rectangular circle-grid detector, fits a diagnostic
camera-to-arena homography, samples dot/background photometry, and compares
individual detected centers across repeats. The default quality gates require:

- every repeat detected for every camera;
- dot-core saturation fraction at or below 0.5% for pixels >= 250;
- median dot/background contrast of at least 20 Mono8 levels;
- worst repeat homography reprojection RMS at or below 0.5 canvas pixels;
- centroid jitter RMS at or below 0.5 camera pixels.

The recommendation is the lowest tested opaque grayscale level that passes all
gates on all requested cameras. Thresholds are recorded in the report and can
be overridden when running the analyzer directly. A recommendation does not
replace operator review of images or authorize homography promotion.

## Qualified Shadow result and downstream enforcement

The authoritative Shadow sweep at
`projector_intensity_20260719T014235Z/commissioning_report.json` recommends
foreground gray `72`. Gray `76` detected reliably, but it is not qualified:
Cam2010094 and Cam2010095 had worst dot-core saturation fractions of about
`4.13%` and `3.65%`, above the `0.5%` limit. This distinction is why detection
success alone is not an image-quality acceptance criterion.

`scripts/run_gui_arena_centering_commissioning.sh` consumes that report. When
the operator does not pass `--foreground-gray-u8`, the launcher uses the
report's recommendation. A homography-fit run refuses a selected level unless
the report contains passing evidence for every requested camera. The report
path, SHA-256, recommended gray, saturation threshold, saturation limit, and
contrast limit are carried into the Orange result and each Citrus candidate.

Citrus repeats the photometry measurement on the exact homography source
image, using the same detected-center core/annulus method as the sweep. Source
photometry is an independent acceptance gate alongside orientation, full-fit,
and perimeter-holdout geometry. A clipped or low-contrast source can still
produce diagnostic overlays and a matrix, but its candidate is
`quality_gate_failed` and cannot be promoted.

The first integrated four-camera run using this contract is
`/tmp/orange_gui_arena_centering_20260719T223825Z.json`, with candidate set
`homography_set_homography_fit_2026-07-19T22_39_47Z`. All four exact source
frames passed:

| Camera | Full-fit RMS, canvas px | Perimeter holdout RMS, canvas px | Contrast, Mono8 | Core p99, Mono8 | Saturated core fraction |
| --- | ---: | ---: | ---: | ---: | ---: |
| `2010093` | 0.108 | 0.131 | 64 | 164 | 0 |
| `2010094` | 0.138 | 0.166 | 87 | 210 | 0 |
| `2010095` | 0.102 | 0.142 | 89 | 207 | 0.00000105 |
| `2010096` | 0.105 | 0.167 | 85 | 203 | 0 |

The first attempt exposed a separate orientation-QC weakness on Cam2010093:
one global Otsu threshold confused its spatial illumination gradient with dot
diameter. Orientation evidence schema v2 now measures each dot's area at its
own local background-plus-half-peak threshold. In the accepted rerun, the
enlarged marker area was `2.78-2.81x` the regular-dot median and `2.59-2.66x`
the next-largest dot on all cameras. This preserves a strong unique-marker
gate without requiring spatially uniform brightness.

The run committed verified arena centers but deliberately left homography
acceptance disarmed. Citrus marked the set
`quality_passed_but_accept_homographies_not_armed`; the May active homography
files were not modified.
