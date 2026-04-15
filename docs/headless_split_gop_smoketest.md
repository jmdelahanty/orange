# Headless Split-GOP Smoke Test

Date: 2026-04-14
Scope: define the first headless bring-up test for the `exp/gop-split-a16`
experiment branch, including setup, commands, pass / fail criteria, and the
telemetry that is and is not available today.

## Goal

Prove that one camera can record successfully through the new split-GOP
multi-GPU path in headless mode, using one A16 `PIX` pair, without involving
display, YOLO, crop, or pose.

This is a correctness and bring-up test, not a throughput bakeoff.

## Non-Goals

- multi-camera scheduling policy
- direct-input mode
- `pure_offload`
- more than one helper GPU
- final throughput comparison against all other recording modes

## Branch And Binary Under Test

- experiment branch: `exp/gop-split-a16`
- binary:
  - `/home/jeremy/orange-gop-split-a16/targets/release/orange_client`

## Recommended First Hardware Topology

- one camera only
- source GPU: one A16 GPU in a favorable `PIX` group
- helper GPU: one peer-access-capable A16 partner in the same `PIX` group

Recommended initial pair on `pancake0`:

- `GPU1 + GPU2`

Avoid for the first run:

- any `GPU0 + A16` pairing
- any cross-group A16 pairing such as `GPU1..GPU4 <-> GPU5..GPU8`

## Recording Strategy Under Test

Use:

- `mode = split_gop`
- `placement = multi_gpu`
- `encoder_gpu_ids = [1, 2]`
- `source_encoder_policy = hybrid_split`
- `transfer_mode = raw`
- `strict = true`

Keep the camera source GPU fixed on `GPU1`.

Rationale:

- `hybrid_split` is the most plausible first policy because it uses the source
  GPU encoder and one helper encoder without forcing every frame off-device.
- `raw` is the best first transfer mode for a full-resolution or lightly
  downsampled mono / Bayer recording because it avoids moving inflated `NV12`.
- `strict = true` makes helper-routing mistakes fail loudly instead of silently
  falling back to local-only behavior.

## Why Headless First

The local headless client already uses the same `ModernRecordingPipeline` as
the GUI recording path, but disables display / YOLO / crop in the test flow.

That makes headless mode the cleanest first proving ground for:

- GOP routing
- helper preprocess / helper encode dispatch
- shared MP4 output
- ordered GOP release
- drain / close behavior

## Preconditions

- the experiment branch is built successfully
- cameras enumerate with the headless client
- the selected source / helper A16 pair has peer access
- no `--nvenc-direct-input` flag is used
- only one camera is selected for the first run

Recommended quick preflight:

```bash
/home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  --mode local \
  --list-cameras
```

Optional source / helper topology re-check:

```bash
nvidia-smi topo -m
/tmp/check_cuda_peer_access
```

## Configuration Method For The First Smoke Test

For the first smoke test, prefer env overrides for the split-GOP knobs.

Why:

- the headless CLI does not yet expose a dedicated split-GOP flag surface
- env overrides avoid mutating production camera JSON during the first bring-up
- the experiment branch already resolves recording strategy from env overrides

Use `--gpu-id 1` only to force the selected camera's source GPU at runtime.
Use env overrides to define the helper pair and split-GOP policy.

## Test Sequence

## Step 1: Camera Enumeration

Confirm the target serial exists:

```bash
/home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  --mode local \
  --list-cameras
```

Record:

- selected camera serial
- chosen source GPU
- chosen helper GPU

## Step 2: Stream-Only Sanity Check

Run a short stream-only check first:

```bash
/home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  --mode local \
  --camera <serial> \
  --gpu-id 1 \
  --stream-only \
  --duration 5
```

Expected result:

- clean startup
- clean shutdown
- no recording artifacts created

## Step 3: Split-GOP Recording Smoke Test

Run one short recording:

```bash
ORANGE_RECORDING_MODE=split_gop \
ORANGE_SPLIT_GOP_PLACEMENT=multi_gpu \
ORANGE_SPLIT_GOP_ENCODER_GPU_IDS=1,2 \
ORANGE_SPLIT_GOP_SOURCE_ENCODER_POLICY=hybrid_split \
ORANGE_SPLIT_GOP_TRANSFER_MODE=raw \
ORANGE_SPLIT_GOP_MAX_INFLIGHT_GOPS=2 \
ORANGE_SPLIT_GOP_MAX_BUFFERED_BYTES=134217728 \
ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_PACKETS=4096 \
ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_BYTES=134217728 \
ORANGE_SPLIT_GOP_FAIL_ON_WRITER_OVERFLOW=1 \
ORANGE_SPLIT_GOP_STRICT=1 \
/home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  --mode local \
  --camera <serial> \
  --gpu-id 1 \
  --record-folder <record-folder> \
  --codec h264 \
  --preset p1 \
  --tuning ll \
  --rate-control vbr \
  --quality 20 \
  --gop 60 \
  --record-delay 2 \
  --duration 12
```

Notes:

- `--record-delay 2` gives a short stream warmup before recording arms
- `--duration 12` is long enough to cross multiple GOP boundaries without
  turning the first run into a long soak test
- if lower latency is more important than matching production GOP, use
  `--gop 30` for the first run

## Required Artifacts

The recording folder should contain, at minimum:

- `Cam<serial>.mp4`
- `Cam<serial>_meta.csv`
- `Cam<serial>_keyframe.csv`
- `recording_snapshot.json`
- `nvidia_smi_dmon.csv`
- `nvidia_smi_dmon.stderr.log`

The pipeline snapshot should also point to the rolling pipeline perf artifact
path for the camera.

## Telemetry Available Today

Available today, without additional code changes:

- `recording_snapshot.json`
  - encoder configuration
  - resolved recording strategy
  - source GPU id
  - primary encode GPU id
  - shared pending-GOP buffer stats
  - shared writer-queue stats
  - pre-encoder reference capture summary
- `pipeline_metrics.<serial>`
  - acquisition / preprocess / encode FPS aggregates
  - preprocess / encode queue depths
  - resource low-watermarks
  - aggregate failure / drop counters
  - path to the pipeline perf CSV artifact
- `Cam<serial>_meta.csv`
  - per-frame metadata continuity
- `Cam<serial>_keyframe.csv`
  - keyframe / IDR sidecar output
- `Cam<serial>.mp4`
  - final muxed video artifact
- `gpu_monitoring.nvidia_smi_dmon`
  - GPU monitoring command, artifact path, and monitor status

Most useful snapshot fields for this smoke test:

- `encoders.<serial>.recording_strategy.split_gop.pending_gop_buffer`
- `encoders.<serial>.recording_strategy.split_gop.writer_queue`
- `encoders.<serial>.recording_strategy.split_gop.encoder_gpu_ids`
- `encoders.<serial>.recording_strategy.split_gop.source_encoder_policy`
- `encoders.<serial>.recording_strategy.split_gop.transfer_mode`
- `encoders.<serial>.source_gpu_id`
- `encoders.<serial>.encode_gpu_id`

## Telemetry Gaps Today

Not available yet in persisted artifacts:

- helper-routing counters from `RecordingIngress`
  - `submitted_frames`
  - `helper_requested_frames`
  - `helper_dispatched_frames`
  - `helper_fallback_frames`
  - `last_target_gpu_id`
- explicit helper-GPU transfer timing
- explicit helper-GPU transfer byte counts
- aggregated helper + primary queue / FPS reporting in the pipeline snapshot
- a persisted per-helper encoder snapshot

Practical consequence:

- the current artifacts can prove that split-GOP mode was configured and that
  the shared pending-GOP / writer machinery stayed healthy
- but they do not yet prove, by themselves, how many frames or GOPs actually
  routed through the helper path

For the first smoke test, this means success evidence is partly artifact-based
and partly log / behavior-based.

## Pass Criteria

Hard pass requires all of the following:

- the process exits cleanly
- the recording completes without drain timeout
- `Cam<serial>.mp4` exists and is non-empty
- `recording_snapshot.json` exists and is readable
- the encoder snapshot resolves to:
  - `mode = split_gop`
  - `placement = multi_gpu`
  - `encoder_gpu_ids = [1, 2]`
  - `source_encoder_policy = hybrid_split`
  - `transfer_mode = raw`
- `pending_gop_buffer.overflow_detected = false`
- `writer_queue.overflow_detected = false`
- metadata CSV row count is plausible for the recording duration
- the MP4 decodes and has expected duration / order at a quick inspection level

Soft pass for the first run:

- source / helper dispatch is not yet persisted, but no strict-mode helper
  routing error occurs
- no obvious artifact corruption appears

## Fail Conditions

Immediate fail:

- helper-routing exception in strict mode
- writer queue overflow
- pending-GOP buffer overflow
- drain timeout
- missing MP4 or unreadable `recording_snapshot.json`
- obviously corrupt output artifact

Follow-up required even if the run "works":

- snapshot shows split-GOP mode, but there is no additional evidence yet that
  helper dispatch actually occurred
- repeated short runs are needed to validate shutdown ordering

## Manual Review Checklist

After the run, inspect:

1. `recording_snapshot.json`
   - confirm resolved split-GOP config
   - confirm no pending-GOP or writer-queue overflow
2. `Cam<serial>.mp4`
   - confirm file exists
   - confirm rough expected duration
   - confirm no immediately obvious decode / ordering issue
3. `Cam<serial>_meta.csv`
   - confirm frame ids are monotonic
   - confirm row count is plausible
4. `Cam<serial>_keyframe.csv`
   - confirm keyframe output exists
5. `nvidia_smi_dmon.csv`
   - confirm the source and helper GPUs were active during the run

## Recommended Next Steps After First Pass

If the smoke test passes:

- run the same scenario again with `single_session` as the baseline
- compare against `split_gop + multi_gpu + hybrid_split + raw`
- add helper-routing counters to `recording_snapshot.json`
- then add a second comparison using `transfer_mode = prepared_nv12`

If the smoke test fails:

- keep the test single-camera and headless
- do not add more GPUs or more cameras yet
- fix artifact correctness or shutdown correctness before measuring throughput
