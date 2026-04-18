# PTP Recording Sink Experiment Plan

Date: 2026-04-17
Branch: `exp/gop-split-a16`

Related notes:

- `docs/multi_camera_failure_modes.md`
- `docs/ptp_sync_hardening_todo.md`

## Purpose

This note captures the next diagnostic step for the unstable:

- dual-camera `100 fps`
- `ptp_gate`
- recording-enabled
- nonzero-stagger

failure mode.

The immediate goal is to separate:

- "recording enabled changes buffer ownership / frame requeue timing"

from:

- "real preprocess / encode / output work is required to trigger the failure"

## Current Read

The current evidence is already fairly specific:

- dual-camera `100 fps` `ptp_gate` `stream_only` is healthy
- dual-camera `100 fps` `ptp_gate` recording is not healthy
- stale-onset receive logging shows the bad camera already receiving stale
  frames before visible app-side starvation
- stale-onset recording-submit logging shows the bad camera is still:
  - primary-only
  - shallow at the handoff
  - not yet showing preprocess waits/drops

That means:

- recording must be enabled to trigger the bad interaction
- but the first visible onset is still upstream of the normal recording hot
  spots

One plausible mechanism is direct-pointer buffer lifetime:

- without recording, acquisition can often recycle/requeue camera buffers
  quickly
- with recording enabled, the `WORKER_ENTRY` lifetime extends downstream
- in direct-pointer mode, camera requeue is deferred until the last consumer
  releases the entry
- that can perturb camera / SDK / transport buffering without requiring
  obvious queue growth first

This is still a hypothesis. The sink experiments are meant to test it.

## Proposed Experiment

Add a headless-only experimental recording sink mode that replaces the real
recording path with simpler staged sinks.

This should be controlled by experiment spec and local headless CLI only.

Suggested controls:

- `fixed.recording_sink_mode = real|preprocess_only|immediate_recycle|threaded_handoff_only`
- `--recording-sink-mode real|preprocess_only|immediate_recycle|threaded_handoff_only`

Default remains:

- `real`

## Stage 1: Immediate Recycle Sink

Behavior:

- recording is still logically enabled
- acquisition still assigns `recording_frame_id`
- acquisition still takes the "recording active" branch
- but instead of calling the real recording pipeline, the entry is released
  and recycled immediately

Important intent:

- preserve "recording on" bookkeeping
- avoid preprocess / encode / output work
- avoid a cross-thread handoff to the recording pipeline

What this tests:

- whether bookkeeping alone is enough to trigger the failure

Expected interpretation:

- if this is healthy:
  - bookkeeping alone is not the trigger
- if this is unhealthy:
  - the trigger is very early and may be tied to frame-id mode,
    reference-counting, or direct-pointer ownership transitions

## Stage 2: Threaded Handoff-Only Sink

Behavior:

- recording is still logically enabled
- acquisition still hands the frame to a dedicated recording-side worker or
  queue
- that worker immediately releases / recycles the entry
- no preprocess, encode, or output work is performed

Important intent:

- preserve the cross-thread handoff and delayed release semantics
- still avoid real recording work

What this tests:

- whether ownership transfer and delayed camera-buffer requeue timing are
  sufficient to trigger the failure

Expected interpretation:

- if `immediate_recycle` passes but `threaded_handoff_only` fails:
  - the trigger is likely handoff / ownership / requeue timing
- if both pass:
  - the trigger is further downstream in real recording work
- if both fail:
  - the trigger is earlier than expected and not dependent on real recording
    work

## Recommended Initial Matrix

Start with the known-problem case:

- dual-camera `100 fps`
- `ptp_gate`
- `2 ms` stagger
- validated PIX pairs

Run:

1. `real`
2. `immediate_recycle`
3. `threaded_handoff_only`

Track:

- `acq_fps_mean`
- `dropped_frames_camera`
- `[PTP_STALE_DUMP]` presence
- `latch_minus_frame_ns` behavior
- whether the bad behavior still follows the offset camera

## Why This Is Better Than More Handoff Logging Right Now

The current handoff logs already show:

- the bad camera is still primary-only
- helper dispatch has barely started
- preprocess queues are shallow
- preprocess pools are near full

So more counters at the same boundary are unlikely to discriminate the root
cause as effectively as replacing the downstream behavior with a simpler sink.

## Non-Goals

This experiment is not meant to:

- validate a permanent production mode
- replace the real recording pipeline
- explain the entire `2 x 100 fps free_run` backlog-overflow mode

It is specifically aimed at:

- the `100 fps` `ptp_gate` recording-enabled stale-frame onset problem

## Exit Criteria

This experiment is successful if it tells us which of these is true:

1. recording bookkeeping alone is enough
2. cross-thread handoff / delayed release is enough
3. real recording work is required

Once one of those is established, the next instrumentation step can be much
more targeted.

## First Results (2026-04-17)

The first two sink probes were run against the known-bad case:

- dual-camera `100 fps`
- `ptp_gate`
- `2 ms` stagger
- validated PIX pairs

Artifacts:

- `immediate_recycle`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_immediaterecycle_rerun2`
- `threaded_handoff_only`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_threadedhandoff_rerun2`

Results:

- both sink modes sustained about `100 fps` on both cameras
- both sink modes had:
  - `0` camera drops
  - `0` acquisition starvation
  - no stale-frame onset

Current interpretation:

- bookkeeping alone is not enough to trigger the failure
- a simple cross-thread handoff / delayed release is also not enough

## Follow-Up: Acquisition Buffer Ownership Probe (2026-04-18)

Because the sink modes passed cleanly, the next probe forced acquisition to
copy GPUDirect camera buffers into Orange-owned ring-buffer memory before
recording work:

- control:
  - `fixed.acquisition_buffer_mode = force_ring_copy`
- artifact:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_forceringcopy_rerun1`

Result:

- `2010095` improved substantially relative to the failing direct-pointer run
  but still did not sustain the target:
  - `enc_fps_mean = 80.2815`
- `2010096` still collapsed badly:
  - `enc_fps_mean = 5.35862`
- stale-frame onset still occurred
- the run used `direct=0 ring=1` at acquisition, so the direct camera-buffer
  pass-through path was genuinely disabled

Interpretation:

- direct camera-buffer lifetime / requeue behavior is not the whole problem
- forcing ring-copy changes the shape of the failure, but does not eliminate
  the offset-camera collapse
- the remaining trigger still requires real downstream recording work

## Follow-Up: Preprocess-Only Probe (2026-04-18)

The next discriminator ran the same known-bad case with:

- `fixed.recording_sink_mode = preprocess_only`

Artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_preprocessonly_rerun1`

Result:

- the run still failed
- `2010095` degraded to `acq_fps_mean = 90.6936`
- `2010096` degraded to `acq_fps_mean = 82.2947`
- stale-frame onset still occurred on the offset camera
- there was still no encoder/output work

Most important clue:

- the stale-onset dump occurred just after helper routing began on the bad
  camera
- the bad camera was still primary-only through recording frame `100`
- helper routing started at recording frame `101`
- the stale threshold fired at local frame `106`

Interpretation:

- real preprocess work is enough to trigger the bad interaction
- encode/shared output are not required
- the strongest remaining suspect is now cross-GPU helper preprocess under
  PTP-gated stagger, not the later encode/output path
- the pathological stale-frame onset requires real downstream recording work,
  not just "recording enabled" state or a lightweight recording handoff

That does not yet prove whether the trigger lives in:

- preprocess
- helper routing / split-GOP topology
- encoder-side work
- shared output / ordered GOP release

But it rules out the earlier "ownership timing alone" hypothesis as the primary
cause.
