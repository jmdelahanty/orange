# Encoding Master Experiment Plan

Date: 2026-06-05
Scope: staged plan for real-fish full-frame recording experiments across NVENC
settings, with objective checks and human visual review.

See also:

- `docs/external_ipc_split_gop_60fps_hq_encoder_experiment_2026_06_05.md`
- `docs/codec_quality_evaluation_protocol.md`
- `docs/pre_encoder_reference_capture_plan.md`
- `docs/headless_codec_quality_test_handoff.md`
- `docs/nvenc_benchmark_runsheet.md`
- `docs/external_split_gop_recorder_design.md`

## Goal

Build a repeatable experiment suite that answers:

- which encoding settings preserve fish-visible detail well enough?
- which settings sustain the required frame rate without drops or queue growth?
- what file size and storage growth do those settings imply?
- which settings remain plausible for many cameras over many days or weeks?

The immediate live condition is one fish under one `4512x4512 @ 60 fps` camera.
The current installed Orange rig has four cameras, so the practical validation
ladder is one camera first and then the maximum successful settings across all
four installed cameras. The long-term colleague target may be eight similar
cameras recording across weeks, so file size is still a primary decision
signal, not a secondary detail, but eight-camera behavior must be projected or
validated on a different rig.

Encoding choices are mainly tradeoffs among visual fidelity,
latency/throughput, and file size/storage cost. For long high-resolution
recordings, file size is often one of the dominant constraints.

## Core Principle

Do not treat "highest preset" as the goal.

The goal is the best acceptable point on the curve:

```text
quality / task utility
  versus
latency / throughput safety
  versus
file size / storage cost
```

Preset, tuning, rate control, bitrate or QP target, GOP length, and shard count
must be tracked separately. A pass/fail throughput result is not enough, and a
pretty clip that cannot be stored for weeks is not a usable setting.

## Real-Fish Capture Setup

Use a controlled live scene before broadening the matrix:

- one camera first, expected first target `2010096`;
- full-frame `4512x4512`;
- `60 fps`;
- external IPC recorder;
- four A16 shard GPUs `5,6,7,8` for high-effort settings;
- fixed exposure, focus, iris, lighting, and dish position;
- one fish visible for representative motion;
- same camera config folder unless the experiment explicitly changes camera
  settings.

The scaling target on this rig is four cameras, not eight. Treat the one-camera
matrix as a way to find plausible high-end settings, then rerun only the
survivors across the installed four-camera set.

Recommended scene notes per run:

- fish present: yes/no;
- motion level: mostly still, slow swim, rapid turns;
- contrast/reflection notes;
- focus/iris/exposure;
- any operator-visible anomaly.

For quality comparisons, short clips are useful. For storage and stability,
longer soaks are required only after a setting survives the short matrix.

## Master Experiment List

This is the proposed first bounded matrix. It is intentionally staged so we do
not run hundreds of settings before knowing which families are plausible.

### Stage 0: Sanity And Reference

Purpose: prove the live condition is stable and create high-fidelity anchors.

Runs:

```text
0A. P1 lossless, GOP 1, 4 shards
0B. P7 lossless, GOP 1, 4 shards
0C. P5 HQ VBR, 150 Mbps, GOP 30, 4 shards
```

Questions:

- does live-fish acquisition stay at `60 fps`?
- does dmon cover all used GPUs?
- how large is lossless with real fish motion?
- does P7 lossless reduce file size relative to P1 lossless?
- does the old P5 HQ baseline still look acceptable with fish present?

### Stage 1: Bitrate-Capped VBR

Purpose: find the best quality under explicit storage budgets.

Runs:

```text
codec:     hevc
preset:    p5, p7
tuning:    hq
rc:        vbr
gop:       30
bitrate:   150, 250, 400, 600 Mbps
shards:    4 GPUs
```

This is `8` runs.

Questions:

- at the same bitrate, does P7 visibly improve fish detail over P5?
- which bitrate is the knee where visual gains flatten?
- do higher bitrates increase write pressure or mux latency?
- does P7 remain throughput-safe at each bitrate?

### Stage 2: Fixed-QP Size Discovery

Purpose: discover how file size changes when quality target is fixed.

Runs:

```text
codec:   hevc
preset:  p1, p5, p7
tuning:  hq
rc:      cqp
qp:      16, 20, 24
gop:     30
shards:  4 GPUs
```

This is `9` runs.

Questions:

- how much larger is QP 16 than QP 20 or 24?
- does P7 reduce bytes per frame at the same QP?
- which QP preserves fish detail without excessive file size?
- does CQP avoid bitrate-control artifacts such as pumping?

### Stage 3: GOP Sweep

Purpose: isolate GOP effects after one or two promising settings are known.

Choose at most two candidate settings from Stages 1 and 2, then sweep:

```text
GOP: 1, 5, 15, 30, 60
```

Hold fixed:

- codec;
- preset;
- tuning;
- rate control;
- bitrate or QP;
- shard count;
- camera scene.

Questions:

- does shorter GOP reduce latency or queue burstiness?
- does longer GOP reduce file size enough to matter?
- does quality degrade at keyframe cadence?
- does split-GOP routing become too bursty at longer GOPs?

Single-encoder discriminator:

Before assuming GOP 1 is cheap because it removes inter-frame dependency, also
test one camera with exactly one external encoder shard. This separates
all-intra encoder capacity from split-GOP multi-shard capacity.

Checked-in manifest:

```text
experiment_specs/encoding_master_singlecam_60fps_gop1_single_encoder_manifest.json
```

Initial result on 2026-06-05 with `2010096`, `60 fps`, `GOP=1`,
`VBR 150 Mbps`, one external shard on GPU `5`:

- `P5 HQ`: acquired `421` frames with `0` camera gaps/errors, but encoded
  `327/421` and dropped `94`; queue high-water `31/32`.
- `P7 HQ`: acquired `421` frames with `0` camera gaps/errors, but encoded
  `141/421` and dropped `280`; queue high-water `31/32`.
- Both partial MP4s passed decoded video sanity, so this was an encoder
  throughput failure, not a camera or content failure.

Interpretation: for this frame size and rate, GOP 1 HQ is not sustainable on a
single external encoder at these settings. The previous one-camera GOP 1
successes used four shard GPUs, so they should be treated as split-GOP
multi-encoder results, not single-encoder capacity results.

### Stage 4: Lossless Diagnostics

Purpose: decide whether lossless is a reference-only tool or a plausible
production mode.

Runs:

```text
P1 lossless, GOP 1
P7 lossless, GOP 1
optional diagnostic: P7 lossless, GOP 30 if the code allows it later
```

Questions:

- bytes per frame and TB/day at real fish motion;
- P1 versus P7 file-size difference;
- encoder utilization and queue pressure;
- decoded equality against the prepared source, once that checker exists.

For now, GOP 1 is the clean reference mode. Lossless GOP 30 would be a separate
inter-frame-lossless experiment with different dependency and shard-balancing
behavior.

### Stage 5: Finalist Soaks

Purpose: expose storage, mux, and queue trends that short clips hide.

Run only the top `2-4` settings:

```text
duration: 10-30 minutes first
then:     longer overnight or day-scale storage test if needed
cameras:  1 camera first, then 4 installed cameras
```

Questions:

- does queue age stay flat over time?
- does MP4 write/mux latency grow?
- does storage throughput stay stable?
- does file growth match projections?
- do visible artifacts appear only after longer motion or lighting changes?

### Stage 6: Four-Camera Maximum Settings

Purpose: find the strongest settings that remain sustainable on the installed
four-camera rig.

Input:

- only settings that survived one-camera short clips and one-camera soaks;
- preferably `2-4` finalists, not the full Stage 1/2 matrix.

Run shape:

```text
cameras:   4 installed cameras
fps:       60 fps if that is the colleague target
duration:  short smoke first, then longer soak for survivors
shards:    explicit per-camera shard placement
```

Questions:

- do all four cameras sustain frame rate with zero drops?
- does per-camera encoder shard placement avoid shared-NVENC saturation?
- does dmon show expected GPU use for every camera's shard set?
- does storage write pressure become the limiting factor before NVENC?
- how far below the one-camera maximum settings do we need to back off?

Do not assume that a one-camera pass across four GPUs extrapolates to four
cameras. Four cameras multiply acquisition, IPC, NVENC, mux, PCIe, and storage
pressure, and the maximum sustainable setting may be lower.

## Required Outputs Per Run

Each run should produce or be summarized with:

- experiment id and run id;
- camera serials and camera config path;
- codec, preset, tuning, rate control, QP or bitrate, GOP, shard GPUs;
- frame count, duration, achieved FPS;
- frames received, ACKed, encoded, skipped, and dropped;
- encode queue high-water and p95 enqueue age;
- `encode_total_p95_ms`, `nvEncLockBitstream_p95_ms`, and MP4 write p95/max;
- per-shard encoded/dropped counts;
- dmon `enc`, `sm`, PCIe RX/TX for all analytics/recorder/shard GPUs;
- merged MP4 path and size;
- bytes per frame;
- achieved Mbps from actual file size and duration;
- projected TB/day for 1 camera and 8 cameras;
- projected TB/day for 4 installed cameras;
- video sanity result;
- human visual review status.

## Metric Suite

The suite should report metrics at three levels: per run, per camera/shard, and
across the whole matrix. The matrix view is what makes parameter tradeoffs
visible instead of leaving results as isolated pass/fail runs.

### Per-Run And Per-Camera Metrics

One row per run/camera should include:

- codec, preset, tuning, rate-control mode, bitrate or QP, GOP, shard count;
- camera serial, source GPU, shard GPU list;
- frames received, ACKed, encoded, skipped, and dropped;
- achieved FPS and duration;
- encode queue high-water;
- enqueue age p50/p95/max;
- encode total p50/p95/max;
- `nvEncLockBitstream` p50/p95/max;
- MP4 write p50/p95/max;
- dmon encoder utilization per GPU;
- merged MP4 size;
- bytes per frame;
- achieved Mbps;
- projected TB/day for 1 camera, 4 installed cameras, and 8 projected cameras.

### Per-Shard Metrics

One row per run/camera/shard should include:

- assigned shard GPU;
- shard id;
- frames encoded and dropped;
- bytes emitted;
- per-shard MP4 byte counters and retained file size when available;
- encode p50/p95/max;
- lock-bitstream p50/p95/max;
- queue or slot wait p50/p95/max;
- dmon encoder utilization for the shard GPU.

This view should answer whether one shard is carrying more work, stalling, or
sharing an encoder resource with another active path.

### GOP-Specific Metrics

For GOP-size comparisons, report:

- GOP size versus encode queue high-water;
- GOP size versus enqueue age p50/p95/max;
- GOP size versus encode p50/p95/max;
- GOP size versus MP4 size and bytes per frame;
- GOP size versus achieved Mbps;
- per-GOP burstiness, if derivable from routing/encode CSVs;
- drops by GOP index;
- shard balance by GOP index;
- GOP boundary quality artifacts from sampled frames or contact sheets.

These views should answer:

- does GOP 1 reduce latency but increase file size?
- does GOP 30 or 60 reduce file size but create burstier queues?
- does a longer GOP cause visible keyframe-recovery artifacts?
- does split-GOP routing remain balanced for the selected GOP length?

### Quality Metrics

Objective quality metrics should be split by reference availability:

- no reference:
  - video sanity;
  - black/flat-frame checks;
  - frame count and duration correctness;
  - sampled decoded-frame luma statistics;
  - contact sheets.
- pre-encoder reference available:
  - PSNR;
  - SSIM;
  - optional VMAF if installed and meaningful;
  - ROI-focused versions of those metrics.
- lossless reference/equality:
  - decoded-frame equality against the prepared source;
  - mismatch count and first mismatch frame if equality fails.

Human visual review remains required for lossy settings, especially for fish
ROI detail and temporal artifacts.

### Matrix Output Files

The first implementation should produce these summary files:

```text
matrix_summary.csv
per_camera_summary.csv
per_shard_summary.csv
gop_latency_summary.csv
storage_projection.csv
quality_summary.json
review_report.md
```

Later, `review_report.html` can replace or supplement the Markdown report once
contact sheets and side-by-side image tables are generated automatically.

### Decision Views

The report should include comparison tables or plots for:

- GOP versus queue depth, latency, and file size;
- bitrate cap versus file size, quality score, and drops;
- QP versus file size, quality score, and drops;
- preset versus encode time and file size;
- P1 lossless versus P7 lossless file size and encoder load;
- one-camera survivor setting versus four-camera sustainability.

The practical decision metric is not one scalar. A candidate wins only if it
has acceptable fish quality, zero drops, stable queues, and acceptable projected
storage.

## Objective Quality Checks

The objective suite should screen candidates before human review. It should not
replace visual review for lossy settings.

### Existing Or Near-Existing Checks

- MP4 present, non-empty, decodable;
- frame count and duration match expectation;
- black-frame and flat-frame sanity;
- external recorder summary present;
- no camera gaps, GetFrame errors, encode failures, or IPC failures;
- no encode drops;
- queue high-water below an agreed threshold;
- dmon covers all used GPUs;
- output size and achieved bitrate are recorded.

### New Checks To Build

1. Decode candidate videos to selected frames.
2. Generate full-frame contact sheets.
3. Generate fish ROI contact sheets.
4. Compute bytes per frame and projected storage cost.
5. Compute temporal stability metrics:
   - frame-to-frame luma delta;
   - suspicious duplicate frames;
   - sudden quality or bitrate jumps near GOP boundaries.
6. Compare decoded frames to a reference when available:
   - PSNR;
   - SSIM;
   - optional VMAF if installed and meaningful for these images;
   - decoded equality for true lossless validation.
7. Compute ROI-focused metrics:
   - whole frame;
   - dish mask if available;
   - fish bounding box or operator-selected ROI.
8. Produce a compact HTML or Markdown report per matrix.

For moving fish, the preferred reference is a short pre-encoder reference
capture from the same source frames. A lossless encoded clip is useful as a
fallback but should not be treated as perfect sensor ground truth.

## Human Visual Review

Human review should be structured so it is not just "looks good" notes.

Review both:

- full-frame playback;
- fish ROI crops;
- still contact sheets at key timepoints;
- short side-by-side clips for finalist comparisons.

Score or annotate:

- fish body edge fidelity;
- fin/tail detail;
- eye/head detail;
- low-contrast feature retention;
- blockiness;
- ringing;
- shimmer/flicker;
- temporal pumping;
- GOP-cadence quality dips;
- background artifacts that could affect tracking or segmentation;
- whether the artifact is acceptable for the actual downstream use.

Keep the reviewer blind to the encoding setting when practical. At minimum,
review the clips before reading file-size and preset labels.

## Storage Projection

Every candidate should include:

```text
bytes_per_second = mp4_size_bytes / encoded_duration_seconds
bytes_per_day_per_camera = bytes_per_second * 86400
bytes_per_week_8_cameras = bytes_per_day_per_camera * 7 * 8
```

This is required because the long-term target may involve eight cameras across
weeks. A setting can be visually excellent and still fail the experiment if its
storage footprint is not operationally plausible.

For this rig, also compute the four-camera installed-rig projection:

```text
bytes_per_week_4_cameras = bytes_per_day_per_camera * 7 * 4
```

Use the four-camera projection for local feasibility, and the eight-camera
projection for the colleague's longer-term planning.

## Implementation Checklist

### Experiment Specs And Runner

- [x] Create a master matrix spec or generator for Stage 0.
      First slice:
      `experiment_specs/encoding_master_singlecam_60fps_stage0_manifest.json`
      plus `scripts/run_encoding_master_experiment.py`.
- [ ] Add matrix support for bitrate caps if not already ergonomic.
- [ ] Add matrix support for GOP sweeps with fixed preset/tuning/RC.
- [x] Ensure external recorder contract paths are uniquely stamped per run.
- [x] Ensure dmon GPU lists include all analytics/recorder/shard GPUs.
      The runner stamps specs and the existing headless dmon collector derives
      GPU coverage from the external recorder contract.
- [ ] Add storage preflight thresholds appropriate for lossless/high-bitrate
      experiments.
- [x] Add an option for shorter smoke duration versus longer soak duration.
      Use `--duration-s` and `--warmup-s` to override manifest defaults.
- [x] Add a summary artifact that groups all runs in the matrix.
- [ ] Add a four-camera finalist spec set that reuses only the one-camera
      survivors.
- [x] Emit first matrix-level summary CSVs:
      - `matrix_summary.csv`;
      - `per_camera_summary.csv`;
      - `per_shard_summary.csv`;
      - `storage_projection.csv`.
      `gop_latency_summary.csv` remains a later GOP-sweep-specific output.

Usage:

```bash
cd /home/jeremy/orange-gop-split-a16

# Plan only; writes stamped resolved specs and summary scaffolding.
scripts/run_encoding_master_experiment.py --dry-run --print-commands

# Execute all Stage 0 one-camera runs.
scripts/run_encoding_master_experiment.py --execute --continue-on-failure

# Execute one run for a short live sanity check.
scripts/run_encoding_master_experiment.py \
  --execute \
  --run-id stage0_p5_hq_vbr150_gop30_4shard \
  --duration-s 6 \
  --warmup-s 1
```

The runner writes a stamped matrix folder under `/tmp/orange_encoding_master`
by default. Each selected run gets:

- a resolved spec under `resolved_specs/`;
- command logs under `run_logs/`;
- external recorder artifacts under `recorder_artifacts/<run_id>/`;
- aggregate `matrix_summary.csv`, `per_camera_summary.csv`,
  `per_shard_summary.csv`, `storage_projection.csv`, `run_results.jsonl`, and
  `review_report.md`.

### Reference Capture And Alignment

- [ ] Enable bounded `pre_encoder_reference_capture` in quality runs.
- [ ] Confirm the reference captures the prepared encoder input, not a different
      representation.
- [ ] Add tooling to read `Cam<serial>_preenc_ref.bin` and index CSV.
- [ ] Decode candidate MP4s frame-by-frame.
- [ ] Align decoded frames to reference frames by recording frame id or frame
      index.
- [ ] Record alignment failures explicitly.

### Objective Quality Analyzer

- [ ] Add a script such as `scripts/analyze_encoding_quality.py`.
- [ ] Parse recording/session metadata and external recorder summaries.
- [ ] Compute file size, bytes/frame, achieved Mbps, TB/day, and TB/week.
- [ ] Run ffprobe/ffmpeg decode checks.
- [ ] Compute black/flat-frame sanity on sampled decoded frames.
- [ ] Compute PSNR/SSIM against pre-encoder references when available.
- [ ] Add optional VMAF only if the dependency is installed and useful.
- [ ] Add ROI metric support:
      - full frame;
      - dish mask;
      - fish bbox/operator ROI.
- [ ] Emit machine-readable JSON and human-readable Markdown/HTML.
- [ ] Emit `quality_summary.json` with objective checks, quality metrics, and
      reference-alignment status.

### Visual Review Artifacts

- [ ] Generate full-frame contact sheets.
- [ ] Generate fish ROI contact sheets.
- [ ] Generate side-by-side crops for selected candidate pairs.
- [ ] Include GOP boundary markers in contact sheets when relevant.
- [ ] Include file-size and performance metrics separately from blind visual
      review labels.
- [ ] Generate `review_report.md` with links to videos, contact sheets, and
      matrix comparison tables.

### Decision Gates

- [ ] Reject any run with frame drops, encode drops, camera gaps, or failed video
      sanity before visual quality ranking.
- [ ] Reject any run with sustained queue growth or queue high-water near the
      configured limit unless it is explicitly marked diagnostic-only.
- [ ] Reject any run whose projected storage cost is outside the operational
      budget, even if quality is excellent.
- [ ] Promote only a small number of survivors to long soaks.
- [ ] Promote only long-soak survivors to four-camera tests on this rig.
- [ ] Treat eight-camera outcomes as storage/performance projections unless an
      eight-camera rig is available.

## Best Next Implementation Target

Build the matrix-summary and quality-report scaffold before adding more codec
knobs. The most useful first slice is:

1. run the Stage 0 live-fish matrix;
2. collect external recorder summaries, dmon, MP4 sizes, and video sanity;
3. generate one Markdown report with storage projections and contact-sheet
   paths;
4. add PSNR/SSIM later once pre-encoder reference alignment is wired.

That gives immediate decision support without blocking on the full reference
comparison suite.
