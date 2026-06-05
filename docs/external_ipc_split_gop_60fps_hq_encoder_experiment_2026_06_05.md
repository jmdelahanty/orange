# External IPC Split-GOP 60 FPS HQ Encoder Experiment

Date: 2026-06-05

## Goal

Check whether one full-frame `4512x4512` camera stream at `60 fps` can be
encoded through external IPC split-GOP HEVC with GOP `30` using:

- `P5` + `HQ`
- `P7` + `HQ`

The initial runs use camera `2010096`, analytics/source GPU `5`, and external
recorder GOP shards on A16 GPUs `5,6`. Follow-up P7 capacity runs add shards
`7` and `8`, which are also `PIX`-connected to source GPU `5` on `pancake0`.

## Specs

Camera config clone:

```text
config/validated_split_gop_hevc_60fps_gop30_recabled_a16
```

Experiment specs:

```text
experiment_specs/2010096_external_ipc_split_gop_60fps_gop30_p5_hq_a16_gpu5_6.json
experiment_specs/2010096_external_ipc_split_gop_60fps_gop30_p7_hq_a16_gpu5_6.json
experiment_specs/2010096_external_ipc_split_gop_60fps_gop30_p7_hq_q64_a16_gpu5_6.json
experiment_specs/2010096_external_ipc_split_gop_60fps_gop30_p7_hq_a16_gpu5_6_7.json
experiment_specs/2010096_external_ipc_split_gop_60fps_gop30_p7_hq_a16_gpu5_6_7_8.json
```

The base two-shard specs use:

- `recording_sink_mode = external_ipc`
- `routing_policy = gop_modulo`
- `expected_shard_gpu_ids = [5, 6]`
- `encode_fps = 60`
- `encode_max_fps = 0`
- `encode_queue_depth = 32`
- `bitrate_bps = 150000000`
- `max_bitrate_bps = 150000000`
- `vbv_buffer_size = 150000000`

The external recorder probe applies the NVENC preset and tuning GUIDs, then
explicitly disables AQ, temporal AQ, and lookahead for non-lossless modes. These
results therefore measure `P5/P7` with `HQ` tuning and low-extra-buffering
controls, not a lookahead/AQ-heavy HQ configuration.

## Commands

```bash
cd /home/jeremy/orange-gop-split-a16

sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  experiment_specs/2010096_external_ipc_split_gop_60fps_gop30_p5_hq_a16_gpu5_6.json

sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  experiment_specs/2010096_external_ipc_split_gop_60fps_gop30_p7_hq_a16_gpu5_6.json

sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  experiment_specs/2010096_external_ipc_split_gop_60fps_gop30_p7_hq_a16_gpu5_6_7.json

sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  experiment_specs/2010096_external_ipc_split_gop_60fps_gop30_p7_hq_a16_gpu5_6_7_8.json
```

## Results

### P5 HQ

Analytics artifact:

```text
/home/jeremy/orange_data/exp/unsorted/2010096_external_ipc_split_gop_60fps_gop30_p5_hq_a16_gpu5_6
```

Recorder artifact:

```text
/tmp/orange_external_recorder_2010096_60fps_gop30_p5_hq
```

Summary:

```text
frames_received:          841
acks_sent:                841
frames_encoded:           841
encode_dropped:           0
encode_queue_high_water:  11 / 32
video_sanity:             pass
camera_frame_id_gaps:     0
GetFrame errors:          0
acquisition FPS:          60.017
```

External encode timing:

```text
enqueue_age_p95_ms:       191.622
encode_total_p95_ms:      24.743
nvEncLockBitstream p95:   24.657
```

`nvidia-smi dmon` summary:

```text
GPU 5: enc_mean=69.4%, enc_p95=100%, sm_mean=14.7%, rxpci_mean=1578 MB/s
GPU 6: enc_mean=76.9%, enc_p95=100%, sm_mean=21.4%, rxpci_mean=798 MB/s
```

Interpretation: P5 HQ kept up for this 12 s smoke. It is still close enough to
NVENC saturation that a longer soak is warranted before treating it as a safe
production setting.

### P7 HQ

Analytics artifact:

```text
/home/jeremy/orange_data/exp/unsorted/2010096_external_ipc_split_gop_60fps_gop30_p7_hq_a16_gpu5_6
```

Recorder artifact:

```text
/tmp/orange_external_recorder_2010096_60fps_gop30_p7_hq
```

Summary:

```text
frames_received:          840
acks_sent:                840
frames_encoded:           454
encode_dropped:           386
encode_queue_high_water:  31 / 32
video_sanity:             pass for partial encoded video
camera_frame_id_gaps:     0
GetFrame errors:          0
acquisition FPS:          60.015
```

External encode timing:

```text
enqueue_age_p95_ms:       2153.417
encode_total_p95_ms:      68.632
nvEncLockBitstream p95:   68.548
```

`nvidia-smi dmon` summary:

```text
GPU 5: enc_mean=92.9%, enc_p95=100%, sm_mean=7.4%, rxpci_mean=1383 MB/s
GPU 6: enc_mean=92.9%, enc_p95=100%, sm_mean=11.9%, rxpci_mean=603 MB/s
```

Interpretation: P7 HQ did not keep up at `4512x4512 @ 60 fps` with two A16 GOP
shards. The recorder saturated, the queue reached `31/32`, and 386 frames were
dropped.

### P7 HQ With 64-Deep Encode Queue

Analytics artifact:

```text
/home/jeremy/orange_data/exp/unsorted/2010096_external_ipc_split_gop_60fps_gop30_p7_hq_q64_a16_gpu5_6
```

Recorder artifact:

```text
/tmp/orange_external_recorder_2010096_60fps_gop30_p7_hq_q64
```

Summary:

```text
frames_received:          840
acks_sent:                840
frames_encoded:           518
encode_dropped:           322
encode_queue_high_water:  63 / 64
video_sanity:             pass for partial encoded video
camera_frame_id_gaps:     0
GetFrame errors:          0
acquisition FPS:          60.015
```

External encode timing:

```text
enqueue_age_p95_ms:       4381.713
encode_total_p95_ms:      68.673
nvEncLockBitstream p95:   68.603
```

`nvidia-smi dmon` summary:

```text
GPU 5: enc_mean=92.9%, enc_p95=100%, sm_mean=8.0%,  rxpci_mean=1550 MB/s
GPU 6: enc_mean=91.5%, enc_p95=100%, sm_mean=12.0%, rxpci_mean=582 MB/s
```

Interpretation: increasing queue depth from `32` to `64` reduced drops
(`386 -> 322`) and increased encoded frames (`454 -> 518`), but did not solve
the capacity problem. The queue still filled (`63/64`), p95 queue age roughly
doubled (`2.15 s -> 4.38 s`), and encode p95 stayed about `68.6 ms`. This is
consistent with sustained P7 HQ encoder under-capacity rather than a transient
buffer-depth issue.

### P7 HQ With Three GOP Shards

Analytics artifact:

```text
/home/jeremy/orange_data/exp/unsorted/2010096_external_ipc_split_gop_60fps_gop30_p7_hq_a16_gpu5_6_7
```

Recorder artifact:

```text
/tmp/orange_external_recorder_2010096_60fps_gop30_p7_hq_gpu5_6_7
```

Summary:

```text
frames_received:          840
acks_sent:                840
frames_encoded:           660
encode_dropped:           180
encode_queue_high_water:  31 / 32
video_sanity:             pass for partial encoded video
camera_frame_id_gaps:     0
GetFrame errors:          0
acquisition FPS:          60.000
```

Per-shard encoded/dropped:

```text
GPU 5 shard 0: 234 encoded, 66 dropped
GPU 6 shard 1: 213 encoded, 57 dropped
GPU 7 shard 2: 213 encoded, 57 dropped
```

External encode timing:

```text
enqueue_age_p95_ms:       2148.639
encode_total_p95_ms:      68.676
nvEncLockBitstream p95:   68.628
```

Interpretation: three shards improved P7 HQ throughput substantially relative
to two shards (`454 -> 660` encoded frames), but still did not sustain
`4512x4512 @ 60 fps`. The queue again reached `31/32`, and the verifier failed
because `encode_dropped` was nonzero.

### P7 HQ With Four GOP Shards

Analytics artifact:

```text
/home/jeremy/orange_data/exp/unsorted/2010096_external_ipc_split_gop_60fps_gop30_p7_hq_a16_gpu5_6_7_8
```

Recorder artifact:

```text
/tmp/orange_external_recorder_2010096_60fps_gop30_p7_hq_gpu5_6_7_8
```

Summary:

```text
frames_received:          841
acks_sent:                841
frames_encoded:           841
encode_dropped:           0
encode_queue_high_water:  28 / 32
video_sanity:             pass
camera_frame_id_gaps:     0
GetFrame errors:          0
acquisition FPS:          60.000
```

Per-shard encoded/dropped:

```text
GPU 5 shard 0: 211 encoded, 0 dropped
GPU 6 shard 1: 210 encoded, 0 dropped
GPU 7 shard 2: 210 encoded, 0 dropped
GPU 8 shard 3: 210 encoded, 0 dropped
```

External encode timing:

```text
enqueue_age_p95_ms:       1675.236
encode_total_p95_ms:      68.761
nvEncLockBitstream p95:   68.680
```

Interpretation: four shards sustained this short P7 HQ smoke with AQ,
temporal AQ, and lookahead disabled. It is still close to the edge:
`encode_queue_high_water` reached `28/32`, and p95 queue age was still about
`1.68 s`. Treat this as a successful feasibility result for one camera on an
entire four-GPU A16 island, not as a comfortable production setting.

## Dmon Capture

Headless runs already capture best-effort `nvidia-smi dmon` output when
recording artifacts are enabled:

```text
<run_folder>/nvidia_smi_dmon.csv
<run_folder>/nvidia_smi_dmon.stderr.log
```

The current command is equivalent to:

```text
nvidia-smi dmon -i <gpu_ids> -s putcm -d 1 -o DT
```

It includes `enc` utilization, SM/memory utilization, PCIe RX/TX throughput,
power, clocks, and memory. Sampling is one second, so it is useful for coarse
capacity evidence rather than per-frame timing.

The original three- and four-shard P7 follow-up artifacts only captured GPUs
`5,6` because the benchmark's GPU list came from the selected camera/config
path rather than the external recorder shard list. The external recorder
summaries and logs still report per-shard encode/drop counts for GPUs `7` and
`8`.

After the headless dmon collector was updated on 2026-06-05, future external
IPC runs merge the selected camera GPU list with each selected camera's
external-recorder stream contract:

- `analytics_gpu_id`
- `recorder_gpu_id`
- all `expected_shard_gpu_ids`

Use rerun artifacts, not the first three/four-shard artifacts above, when using
the dmon CSV as full N-shard utilization evidence.

Validation rerun:

```text
analytics artifact:
/home/jeremy/orange_data/exp/unsorted/2010096_external_ipc_split_gop_60fps_gop30_p7_hq_a16_gpu5_6_7_8_dmon_check

recorder artifact:
/tmp/orange_external_recorder_2010096_60fps_gop30_p7_hq_gpu5_6_7_8_dmon_check
```

The short four-shard rerun encoded `300/300` frames with `0` drops and video
sanity passed. Its `recording_snapshot.json` captured:

```text
nvidia-smi dmon -i 5,6,7,8 -s putcm -d 1 -o DT
gpu_ids: [5, 6, 7, 8]
```

The resulting `nvidia_smi_dmon.csv` contains rows for GPUs `5`, `6`, `7`, and
`8`.

## Lossless Follow-Up

Lossless in the current NVENC profile path is not GOP-30. It forces
`CONSTQP 0` and all-intra/GOP `1`, so the first feasibility probe used a
GOP-1 stream routed across all four shards:

```text
experiment_specs/2010096_external_ipc_lossless_60fps_gop1_p1_a16_gpu5_6_7_8.json
```

Command:

```bash
sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  experiment_specs/2010096_external_ipc_lossless_60fps_gop1_p1_a16_gpu5_6_7_8.json
```

Analytics artifact:

```text
/home/jeremy/orange_data/exp/unsorted/2010096_external_ipc_lossless_60fps_gop1_p1_a16_gpu5_6_7_8
```

Recorder artifact:

```text
/tmp/orange_external_recorder_2010096_lossless_60fps_gop1_p1_gpu5_6_7_8
```

Summary:

```text
frames_received:          300
acks_sent:                300
frames_encoded:           300
encode_dropped:           0
encode_queue_high_water:  3 / 32
video_sanity:             pass
camera_frame_id_gaps:     0
GetFrame errors:          0
acquisition FPS:          60.046
merged MP4 size:          476,879,492 bytes
```

External encode timing:

```text
enqueue_age_p95_ms:       0.427
encode_total_p95_ms:      2.242
nvEncLockBitstream p95:   0.003
bitstream_fetch_p95_ms:   2.194
mp4_write_mean_ms:        4.139
mp4_write_max_ms:         5.420
```

Per-shard encoded/dropped:

```text
GPU 5 shard 0: 75 encoded, 0 dropped, encode_total_p95_ms=1.470
GPU 6 shard 1: 75 encoded, 0 dropped, encode_total_p95_ms=2.242
GPU 7 shard 2: 75 encoded, 0 dropped, encode_total_p95_ms=1.531
GPU 8 shard 3: 75 encoded, 0 dropped, encode_total_p95_ms=1.502
```

`nvidia-smi dmon` captured all four shard GPUs:

```text
nvidia-smi dmon -i 5,6,7,8 -s putcm -d 1 -o DT
GPU 5: enc_mean=24.2%, enc_max=31%
GPU 6: enc_mean=24.0%, enc_max=30%
GPU 7: enc_mean=24.0%, enc_max=30%
GPU 8: enc_mean=24.0%, enc_max=30%
```

Interpretation: this short one-camera HEVC lossless probe survived cleanly
across four A16 shard GPUs. The result is promising, but it is only a short
`300`-frame feasibility test; the next useful check is a longer soak to expose
storage pressure, mux/write jitter, and sustained artifact size.

## Next Stage: Quality, Latency, And File Size

The next encoding experiments should treat file size as a first-class outcome,
not just a side effect. The real design space is a tradeoff between:

- visual fidelity or exact reconstruction;
- encoder latency and queue buildup;
- GPU/NVENC utilization;
- PCIe and storage pressure;
- artifact size and downstream transfer/storage cost.

Encoding choices are mainly tradeoffs among visual fidelity,
latency/throughput, and file size/storage cost. For long high-resolution
recordings, file size is often one of the dominant constraints.

The key correction is that preset, tuning, rate control, and GOP length are
different axes:

```text
preset:
  search/effort/compression-efficiency setting, usually P1 fastest and P7
  highest effort

tuning:
  objective bias such as low-latency, high-quality, or lossless

rate control:
  how size/quality is targeted: bitrate cap, fixed QP, or lossless

GOP length:
  frame dependency structure and random-access interval
```

For lossy bitrate-capped encoding, `P7` usually means better visual quality at
roughly the same file size. For fixed-QP encoding, `P7` may produce better
compression efficiency, so file size can change. For lossless encoding, visual
quality should already be exact; the interesting questions become throughput
and bytes per frame.

GOP length can also change encode speed, latency, and file size. The current
short results show a large performance gap between `P7 HQ VBR GOP 30` and
`P1 lossless GOP 1`, but that comparison changes preset, tuning, rate control,
and GOP length at the same time. Treat it as evidence that GOP structure
matters, not as proof that GOP 1 is always faster in isolation.

### Required Metrics

Every future matrix run should report:

- frames received, ACKed, encoded, skipped, and dropped;
- encode queue high-water and p95 enqueue age;
- `encode_total_p95_ms`, `nvEncLockBitstream_p95_ms`, and MP4 write p95/max;
- per-shard encoded/dropped counts;
- dmon `enc` utilization for every analytics/recorder/shard GPU;
- merged MP4 size;
- bytes per frame;
- achieved Mbps from actual file size and encoded duration;
- video sanity result;
- storage free-space warning if available.

Objective checks are useful for screening candidates before human review:

- bitrate and file size;
- dropped or skipped frames;
- black-frame and flat-frame sanity;
- frame count and duration correctness;
- encode latency, queue high-water, and queue-age trends;
- PSNR, SSIM, or VMAF when comparing against a reference;
- decoded-frame equality for true lossless validation.

For lossy recordings, visual review remains the authority. Metrics can narrow
the candidate set, but they do not answer whether a clip is scientifically or
operationally good enough.

For longer soaks, also report:

- output directory growth rate;
- sustained write throughput;
- whether MP4 write/mux latency grows over time;
- queue age trend, not just final pass/fail.

### Storage Scale

The colleague's target use case is not just a short clip: it may be eight
full-frame cameras recording across weeks. That makes storage scale a primary
engineering constraint.

Use this estimate for every candidate:

```text
bytes_per_day =
  bytes_per_second_per_camera * 86400 * camera_count
```

For the short `P1 lossless GOP 1` result above, the merged MP4 was about
`476.9 MB` for `300` frames at `60 fps`, or roughly `95 MB/s` per camera. At
that observed rate:

```text
1 camera:  about 8.2 TB/day
8 cameras: about 66 TB/day
8 cameras: about 460 TB/week
```

For comparison, a hard `150 Mbps` bitrate budget is about `18.75 MB/s` per
camera before container overhead:

```text
1 camera:  about 1.6 TB/day
8 cameras: about 13 TB/day
8 cameras: about 91 TB/week
```

These are rough decimal estimates, but they show why file size must be tracked
beside quality and latency. A setting that is technically encodable can still
be operationally unusable if the storage and transfer budget cannot support it.

### GOP Length

GOP length should be a first-class experiment variable because it affects more
than random access:

- GOP 1/all-intra gives independent frames, simple finalization, and even
  per-frame shard balancing, but usually larger files.
- GOP 15/30 can improve compression and file size by using inter-frame
  prediction, but may increase encoder work, buffering, and dependency
  handling.
- Longer GOPs can reduce bitrate further for some content, but increase
  dependency length, random-access cost, and recovery/finalization risk.

For external IPC split-GOP routing, GOP length also controls how work is
distributed:

- GOP 1 distributes every frame across shards with `gop_index % shard_count`.
- GOP 30 distributes 30-frame bursts across shards, so per-shard work arrives in
  larger chunks.
- Larger GOPs can create burstier queue behavior even when the average frame
  rate is unchanged.

Future matrices should isolate GOP effects by holding preset, tuning, rate
control, bitrate/QP, and shard count constant while sweeping only GOP length.

Suggested GOP sweeps:

```text
latency/random-access sweep:
  GOP 1, 5, 15, 30

compression sweep:
  GOP 15, 30, 60
```

For lossless, the current production-safe reference profile keeps GOP 1. A
lossless GOP 30 diagnostic may be worth testing later, but it should be treated
as a different mode because it introduces inter-frame dependencies and less even
shard balancing.

### Experiment Families

Use three separate families instead of one mixed matrix.

#### A. Bitrate-Capped Throughput

Purpose: find the best quality we can get under a storage/network budget.

This family answers: at a fixed file-size budget, which preset/tuning survives
and looks best?

Suggested first matrix:

```text
codec:     hevc
preset:    p5, p7
tuning:    hq
rc:        vbr
gop:       30
bitrates:  150 Mbps, 250 Mbps, 400 Mbps, 600 Mbps
shards:    4 GPUs
duration:  short smoke first, then longer soak for survivors
```

Interpretation rules:

- At the same bitrate, file sizes should be similar.
- P7 should be judged mainly by quality and whether it keeps up.
- Queue high-water near the limit means the setting is not production-safe even
  if a short run has zero drops.

#### B. Fixed-QP / Size Discovery

Purpose: find the file size required for a chosen quality level.

This family answers: if we ask the encoder for a fixed quantization level, how
large are the files and which preset is more efficient?

Suggested first matrix:

```text
codec:   hevc
preset:  p1, p5, p7
tuning:  hq or ll, depending on latency target
rc:      cqp
qp:      16, 20, 24
gop:     30
shards:  4 GPUs
```

Interpretation rules:

- Lower QP means higher quality and larger files.
- P7 may reduce bytes per frame relative to P1 at the same QP, but this must
  be measured.
- This is the best family for discovering the quality/file-size curve.

#### C. Lossless Reference

Purpose: create exact or near-reference clips and test whether this is
operationally feasible.

This family answers: can we sustain true lossless, and what is the storage
cost?

Suggested first matrix:

```text
codec:   hevc
preset:  p1, p7
tuning:  lossless
rc:      constqp QP 0 via lossless mode
gop:     1 initially
shards:  4 GPUs
```

Interpretation rules:

- P7 lossless should not improve pixel fidelity over P1 lossless.
- P7 lossless may improve compression ratio or may only add work; measure file
  size and encoder load.
- GOP 1 is the clean reference mode because every frame is independently
  decodable and shard balancing is per-frame.
- A later diagnostic can try lossless GOP 30, but that becomes an inter-frame
  lossless mode with different shard-balancing and dependency behavior.

### Recommended Order

1. Run `P7 lossless GOP 1` across four shards and compare it directly to the
   `P1 lossless GOP 1` result above.
2. Run `P7 HQ VBR` at higher bitrate caps: `250`, `400`, then `600 Mbps` if
   storage can tolerate the output.
3. Run `P1/P5/P7 CQP` at QP `16/20/24` to map quality versus bytes per frame.
4. Promote only the surviving settings to longer soaks.

The decision criteria should not be "highest preset wins." The decision should
be the best acceptable point on the quality, latency, and file-size curve.
