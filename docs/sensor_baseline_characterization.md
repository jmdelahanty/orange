# Production-Baseline Sensor Characterization

Status: first bounded implementation; no camera-setting sweep or promotion

This workflow establishes a reproducible dark and one-level uniform-field
baseline at Orange's current production camera operating point:

- Cam2010093 through Cam2010096;
- `4512x4512` Mono8;
- `100 fps` with PTP gating;
- `50 us` exposure;
- digital `Gain = 256` (`1x`);
- `PGAGain = 0` and `Offset = 0`;
- AutoGain, LUT, and DualADC disabled; and
- ADC `Bit8` with output `PixelFormat = Mono8`.

It does not estimate conversion gain, electrons/DN, full well, PRNU, or a
rig-wide physical noise floor. It does not measure fish/background contrast.
It never changes or promotes a production camera setting.

## Why This Reuses The Pre-Encoder Reference Tap

The bounded pre-encoder writer preserves individual, uncompressed NV12 frames
before NVENC. For the production monochrome path, active sensor samples are
copied into the NV12 Y plane and UV is neutral `128`. The analyzer uses only
the active `width` bytes of each Y row and independently requires every UV byte
to remain `128`.

This avoids another full-frame camera hot-path copy. It also carries camera
frame IDs, embedded PTP timestamps, exact output geometry, and the recording
snapshot containing the new post-configuration sensor-pipeline readback.

The brief in-process recording is only a capture vehicle. It is not a codec
quality or throughput benchmark and does not replace the normal external
split-GOP production architecture.

## Bounded Capture

Each condition records 24 unaveraged frames per camera. The current split-GOP
profile has a 25-frame GOP, so this remains one consecutive 100 Hz segment on
the primary path instead of crossing a routing boundary.

At `4512x4512`, one NV12 frame is approximately 30.5 MB. The two-condition,
four-camera session therefore preserves approximately 5.9 GB of raw reference
data, plus short diagnostic videos, indexes, metadata, and reports.

The runner uses the checked-in
`config/validated_split_gop_hevc_100fps_gop25_fourcam_a16` folder and records
each config checksum when the session begins. It refuses the second phase if a
config changed in between.

## Physical Checkpoints

Stop Orange and Citrus before either phase. The runner also refuses to proceed
if it finds either program still active.

### Dark

1. Leave each camera, lens, normal experimental filter, focus, and mount in
   place.
2. Place an opaque cap over the front of every lens without disturbing the
   camera or lens.
3. Remove fish and prevent projector or ambient-light leaks where practical.
4. Confirm the production optical path and opaque caps explicitly.

Create the session and capture dark evidence:

```bash
cd /home/jeremy/orange-gop-split-a16
python3 scripts/run_sensor_baseline_characterization.py \
  --phase dark \
  --execute \
  --confirm-production-optical-path \
  --confirm-lens-capped \
  --condition-notes "All four lens fronts opaque-capped; no projector output"
```

The command prints the new session directory. Keep that exact path for the
flat phase.

### Uniform field

1. Remove the caps without moving a camera, lens, or filter.
2. Present a stationary matte/diffuse uniform target in every camera view.
3. Record the target identity and effective reference plane. Do not call a
   projection surface a fish plane unless it actually occupies that plane.
4. Use the normal production NIR illumination/trigger condition.
5. Keep fish and other moving objects out of view.

Capture the flat condition into the same session:

```bash
python3 scripts/run_sensor_baseline_characterization.py \
  --session-dir /home/jeremy/orange_data/calibrations/rig_characterization/sensorbaseline_<stamp> \
  --phase flat \
  --execute \
  --confirm-production-optical-path \
  --confirm-uniform-stationary-field \
  --confirm-production-illumination \
  --flat-target-id "<target identity>" \
  --flat-reference-plane "<physical plane>" \
  --condition-notes "<fixture and illumination notes>"
```

Once both phases exist, analysis runs automatically in the `juicebox` Conda
environment. It can also be invoked later:

```bash
python3 scripts/run_sensor_baseline_characterization.py \
  --analyze-only /path/to/sensorbaseline_<stamp>
```

Run either phase without `--execute` to print and validate the planned capture
without opening a camera.

## Fail-Closed Validation

For each phase and camera, analysis requires:

- exact frame count and contiguous raw byte offsets;
- full-resolution, unresized, monochrome NV12 metadata;
- post-configuration/pre-stream feature readbacks matching production:
  Width, Height, FrameRate, Exposure, Gain, AutoGain, PGAGain, Offset,
  LUTEnable, ADC, DualADC, and PixelFormat;
- strictly increasing, consecutive approximately 10 ms camera timestamps;
- neutral UV bytes (`min = max = 128`); and
- cross-camera nearest-frame PTP span no greater than 100,000 ns.

Failure invalidates the measurement. The runner does not overwrite a captured
or failed phase; start a new session instead, preserving the failed evidence.

## Products

The session contains:

- `run_manifest.json`: session contract, physical confirmations, config
  checksums, commands, and phase lifecycle;
- `specs/`: the exact headless experiment spec for each phase;
- `logs/`: Orange stdout and stderr for each phase;
- `captures/<phase>/.../sensor_reference/`: immutable per-camera NV12 dumps,
  frame indexes, and metadata;
- each phase's `recording_snapshot.json`, including applied sensor-pipeline
  state;
- `analysis/analysis_<stamp>/report.json` and `report.md`;
- per-camera `metrics.json` and 8x8 `tile_metrics.csv`; and
- block-16 mean-luma and temporal-noise PGM previews.

Every source dump, index, metadata file, and recording snapshot is SHA-256
identified in the analysis report.

## Reported Diagnostics

The first report includes:

- frame mean and frame-mean stability;
- sampled luma percentiles;
- fractions at `0`, `<=5`, `>=250`, and `255` DN;
- temporal sigma from adjacent-frame differences divided by `sqrt(2)`;
- descriptive 8x8 field nonuniformity;
- dark-to-flat mean signal; and
- per-phase cross-camera timestamp span.

Flat-field tile variation is intentionally labeled *observed field
nonuniformity*. At one illumination level it combines illumination geometry,
optics, sensor response, and fixed-pattern effects. It is not PRNU. Likewise,
flat-minus-dark divided by temporal sigma is only a uniform-field signal
diagnostic; it is not fish CNR or conversion gain.

## Next Experimental Slices

Only after this baseline is reviewed should separate, opt-in workflows add:

1. a controlled multi-level flat-field/photon-transfer series;
2. `ADC Bit10 + Mono10Packed` characterization;
3. PGAGain trials if the baseline indicates a read/quantization-noise limit;
4. DualADC at a compatible lower frame rate if dynamic range is the actual
   problem; and
5. a distinct optical-contrast comparison using fish masks, tail keypoints,
   reflections, and behavioral outcomes.

None of those future products should automatically rewrite the production
camera profile.
