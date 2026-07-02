# Encoder Performance Summary For 4512x4512 60 FPS Recording

Date: 2026-06-10

This is a colleague-facing summary of the Orange external-IPC split-GOP encoder
experiments run on 2026-06-05. The detailed lab notes are in:

- `docs/encoding_master_measured_summary_2026_06_05.md`
- `docs/external_ipc_split_gop_60fps_hq_encoder_experiment_2026_06_05.md`
- `docs/encoding_master_experiment_plan.md`

## Short Answer

For one `4512x4512 @ 60 fps` camera using HEVC through the current external
IPC split-GOP path:

- `P5 HQ` sustained 60 FPS on two encoder shards at both `150 Mbps` and
  `250 Mbps`.
- `P7 HQ` did not sustain 60 FPS on two encoder shards. It filled the encoder
  queue and dropped frames.
- `P7 HQ` can pass a short one-camera run with four encoder shards, but it is
  queue-stressed and should not be treated as a scalable four-camera setting.
- HEVC lossless sustained 60 FPS for one camera on two encoder shards, but the
  storage rate is about `8.1 TB/day/camera`.

The current best practical two-shard candidate is:

```text
HEVC P5 HQ, external IPC split-GOP, GOP 5, VBR 150-250 Mbps
```

Use `250 Mbps` if visual quality is the priority. Use `150 Mbps` if storage is
the priority and visual review says it is good enough.

## Test Context

Measured setup:

- Camera: `2010096`
- Resolution: `4512x4512`
- Frame rate: `60 fps`
- Codec: HEVC
- Recorder path: external IPC split-GOP
- Primary two-shard GPUs: A16 GPUs `5,6`
- AQ, temporal AQ, and lookahead: disabled for these HQ tests
- Runs were short feasibility tests, not final week-scale soaks

These results answer throughput and storage feasibility. They do not replace
human visual review of fish detail.

## Preset / Tuning Interpretation

NVENC preset and tuning are different controls:

- Preset `P1` through `P7` controls encoder effort. `P1` is the fastest end;
  `P7` is the highest-effort, slowest end.
- Tuning controls the objective, such as low latency, high quality, or
  lossless.
- Rate control controls how size/quality is targeted, such as VBR bitrate cap,
  CQP, or lossless.
- GOP length controls frame dependency and random-access structure.

For lossy bitrate-capped encoding, a higher-effort preset can improve quality
at a similar file size, but only if the encoder can keep up. In these tests,
`P7 HQ` was too expensive for two shards at this resolution and frame rate.

For lossless encoding, visual quality should already be exact for the prepared
input if the path is truly lossless. Preset differences are then mainly about
throughput, encoder behavior, and compressed size.

## Two-Shard HQ Results

All rows below used one camera, HEVC HQ tuning, external IPC split-GOP, two
shards on GPUs `5,6`, and video sanity checks.

| Setting | Result | Encoded | Drops | Queue high-water | Encode p95 | Achieved Mbps | TB/day/cam |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| P5 HQ VBR150 GOP5 | Pass | 420 | 0 | 4/32 | 0.136 ms | 151.5 | 1.64 |
| P5 HQ VBR250 GOP5 | Pass | 421 | 0 | 4/32 | 0.124 ms | 255.2 | 2.76 |
| P7 HQ VBR150 GOP5 | Fail | 269 | 152 | 31/32 | 68.942 ms | 155.8 | 1.68 |
| P7 HQ VBR250 GOP5 | Fail | 268 | 152 | 31/32 | 68.612 ms | 258.7 | 2.79 |
| P5 HQ VBR150 GOP30 | Pass | 421 | 0 | 11/32 | 24.869 ms | 152.8 | 1.65 |
| P5 HQ VBR250 GOP30 | Pass | 421 | 0 | 11/32 | 24.340 ms | 251.1 | 2.71 |
| P7 HQ VBR150 GOP30 | Fail | 253 | 167 | 31/32 | 68.699 ms | 154.6 | 1.67 |
| P7 HQ VBR250 GOP30 | Fail | 254 | 166 | 31/32 | 67.961 ms | 250.7 | 2.71 |

Interpretation:

- `P5 HQ` is the highest measured sustainable HQ preset on two shards.
- `P7 HQ` is not sustainable on two shards for this frame size and rate.
- `GOP 5` had much lower queue pressure than `GOP 30` for `P5 HQ` in these
  short runs.

## Lossless Results

All rows below used one camera, HEVC lossless tuning, GOP `1`, two shards on
GPUs `5,6`, CQP quality value `0`, and video sanity checks.

| Setting | Result | Encoded | Drops | Queue high-water | Encode p95 | Achieved Mbps | TB/day/cam |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| P1 lossless GOP1 | Pass | 420 | 0 | 4/32 | 1.545 ms | 752.1 | 8.12 |
| P5 lossless GOP1 | Pass | 421 | 0 | 4/32 | 1.526 ms | 750.6 | 8.11 |
| P7 lossless GOP1 | Pass | 421 | 0 | 3/32 | 2.107 ms | 753.6 | 8.14 |

Interpretation:

- Lossless can keep up for one `60 fps` camera on two shards.
- File size is the limiting issue: about `750 Mbps`, or `8.1 TB/day/camera`.
- For week-scale multi-camera recording, lossless is probably a reference or
  diagnostic mode, not the default production mode, unless storage is very
  large.

## Storage Scale

Approximate projections from the measured one-camera rows:

| Setting | TB/day/cam | TB/day/4 cams | TB/day/8 cams | TB/week/8 cams |
| --- | ---: | ---: | ---: | ---: |
| P5 HQ VBR150 GOP5 | 1.64 | 6.54 | 13.09 | 91.6 |
| P5 HQ VBR250 GOP5 | 2.76 | 11.02 | 22.05 | 154.3 |
| P5 HQ VBR150 GOP30 | 1.65 | 6.60 | 13.20 | 92.4 |
| P5 HQ VBR250 GOP30 | 2.71 | 10.85 | 21.69 | 151.8 |
| Lossless GOP1 | 8.1 | 32.4 | 64.9 | 454.6 |

For an eight-camera, week-scale experiment, storage is a first-order design
constraint. Even the more storage-friendly measured P5 setting is roughly
`92 TB/week` for eight cameras.

## What We Can Say Now

- Two-shard `P5 HQ` is feasible for one camera at 60 FPS.
- Two-shard `P7 HQ` is not feasible for one camera at 60 FPS.
- Four-shard `P7 HQ` can pass a short one-camera feasibility run, but it uses
  an entire four-GPU A16 island for one camera and still has high queue
  pressure.
- Lossless is feasible for one camera but storage-prohibitive for long,
  many-camera recordings.

## What We Still Need To Test

Before recommending a production profile, the next useful tests are:

1. Visual review of real-fish clips for `P5 HQ VBR150 GOP5` vs
   `P5 HQ VBR250 GOP5`.
2. Four-camera 60 FPS runs using the surviving P5 HQ settings.
3. Longer soaks to check write/mux stability, queue age trends, and disk growth.
4. Optional fixed-QP sweeps, such as QP 16/20/24, to find better
   quality-per-byte points.
5. Lossless decoded-equality checks against pre-encoder reference frames if
   lossless is used as a scientific reference.

## Recommendation

For the next practical experiment, test:

```text
P5 HQ VBR250 GOP5, two shards per camera
P5 HQ VBR150 GOP5, two shards per camera
```

Use the `250 Mbps` row as the higher-quality candidate and the `150 Mbps` row
as the storage-conscious candidate. Do not promote `P7 HQ` to multi-camera
testing without changing the resource allocation or lowering the workload,
because it already fails the one-camera two-shard case.
