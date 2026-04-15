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

Prefer a structured experiment spec, not ad hoc env overrides.

Why:

- the sudo wrapper already forwards `--experiment-spec` cleanly
- the same JSON can be checked in, reviewed, and rerun later
- the split-GOP recording block now matches the per-camera `recording` schema
- `run_config.json` preserves the requested split-GOP settings per run

Use the experiment spec to define the split-GOP policy and helper pair. Use the
camera config folder or `selection.gpu_ids` only for the source GPU assignment.

### Recommended Spec Shape

Use the same experiment-spec structure as other headless runs, with one added
`fixed.recording` block:

```json
{
  "experiment_id": "2010096_split_gop_smoke_a16_pair_1_2",
  "notes": "First one-camera split-GOP smoke test on an A16 PIX pair.",
  "selection": {
    "camera_serials": ["2010096"],
    "gpu_ids": [1]
  },
  "fixed": {
    "output_root": "/home/jeremy/orange_data/exp/unsorted",
    "config_folder": "/home/jeremy/orange_data/config/local/jeremy",
    "duration_s": 12,
    "warmup_s": 2,
    "stream_start_delay_s": 0,
    "nvenc_direct_input": false,
    "recording": {
      "mode": "split_gop",
      "split_gop": {
        "placement": "multi_gpu",
        "encoder_gpu_ids": [1, 2],
        "source_encoder_policy": "hybrid_split",
        "transfer_mode": "raw",
        "max_inflight_gops": 2,
        "max_buffered_bytes": 134217728,
        "strict": true,
        "writer_queue": {
          "max_packets": 4096,
          "max_bytes": 134217728,
          "fail_on_overflow": true
        }
      }
    }
  },
  "matrix": {
    "codec": ["h264"],
    "preset": ["p1"],
    "tuning": ["ll"],
    "rate_control_mode": ["vbr"],
    "quality_value": [20],
    "gop_length": [60]
  },
  "policy": {
    "target_fps_tolerance_pct": 1.0,
    "require_zero_acq_starve": true,
    "require_zero_pre_drops": true,
    "require_zero_enc_fail": true
  }
}
```

If a later multi-camera experiment needs different helper GPU sets per camera,
layer `fixed.recording_by_camera` on top of the default:

```json
"fixed": {
  "recording": { "...": "default for all selected cameras" },
  "recording_by_camera": {
    "2010096": {
      "mode": "split_gop",
      "split_gop": {
        "placement": "multi_gpu",
        "encoder_gpu_ids": [1, 2],
        "source_encoder_policy": "hybrid_split",
        "transfer_mode": "raw"
      }
    }
  }
}
```

The intended precedence is:

- per-camera JSON config provides the baseline
- `fixed.recording` overrides that baseline for all selected cameras
- `fixed.recording_by_camera.<serial>` overrides the shared default for one camera

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

Write the experiment spec to `/tmp/2010096_split_gop_smoke_a16_pair_1_2.json`.

Once the sudo wrapper is installed from the experiment branch, or after the
split-GOP experiment-spec support is merged into the main headless binary, run
one short recording through the wrapper:

```bash
sudo /usr/local/bin/orange-local-benchmark \
  --orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  /tmp/2010096_split_gop_smoke_a16_pair_1_2.json
```

Equivalent direct invocation, if root is not required on the target host:

```bash
/home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  --mode local \
  --experiment-spec /tmp/2010096_split_gop_smoke_a16_pair_1_2.json
```

Notes:

- `fixed.warmup_s = 2` gives a short stream warmup before the measured window
- `fixed.duration_s = 12` is long enough to cross multiple GOP boundaries
  without turning the first run into a long soak test
- if lower latency is more important than matching production GOP, use
  `matrix.gop_length = [30]` for the first run
- for camera `2010096` at full `4512x4512` resolution, prefer `hevc` instead
  of `h264` for the first real recording run

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
  - camera runtime recording strategy after any experiment-spec override
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

## Workflow Note

The structured experiment-spec path is the preferred workflow for this bring-up.
Keep env overrides as a temporary developer escape hatch only.

## 2026-04-15 Bring-Up Results

Two runs were especially informative:

- `single_session + h264 + 4512x4512`
  - failed during `nvEncInitializeEncoder(...)`
  - this happened even without split-GOP enabled
- `split_gop + multi_gpu + hybrid_split + raw + hevc`
  - both encoder workers started
  - helper GPU `2` came up
  - acquisition held at about `60 fps`
  - each encoder worker ran at about `30 fps`
  - camera dropped frames stayed at `0`
  - the run then aborted on:
    - `split_gop pending GOP count exceeded configured limit`

What this means:

- the spec-based wrapper workflow is working
- the multi-GPU helper path is working at startup / dispatch level
- `h264` is not the right first codec for this full-resolution camera
- the current blocker is the pending-GOP coordinator limit / release behavior

Recommended next code step:

- instrument and fix pending-GOP accounting / flush progression
- do not widen to more cameras or more GPUs until that is fixed

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
