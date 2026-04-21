# GPUDirect Buffer Lifetime Implementation Checklist

Date: 2026-04-21
Branch: `exp/gop-split-a16`

Related notes:

- `docs/gpudirect_buffer_lifetime_review.md`
- `docs/multi_camera_failure_modes.md`
- `docs/headless_experiment_backend.md`

## Goal

Reduce residual SDK receive-buffer pressure in dual-camera `100 fps` split-GOP
recording by making GPUDirect receive-buffer ownership explicit and observable.

The first implementation target is correctness: avoid deferred requeue through
the reusable `ecam->frame_recv` scratch object. Performance tuning should come
after we can prove buffer leases are being returned through stable frame
descriptors.

## Current Working Hypothesis

The latest split receive-error telemetry shows:

- true camera frame-ID gaps can be `0`
- `EVT_CameraGetFrame` can still return error `12` (`EVT_ERROR_NOMEM`)
- this means SDK receive-buffer pressure can occur without confirmed image loss

The strongest code-level suspect is deferred requeue of SDK buffers through a
mutable scratch frame:

- acquisition receives into `ecam->frame_recv`
- direct pass-through stores `&ecam->frame_recv` for downstream requeue
- ring-copy pending requeues also store `&ecam->frame_recv`
- the next `EVT_CameraGetFrame()` can overwrite that object before deferred
  requeue happens

## Implementation Order

### 1. Baseline Before Code Changes

- [x] Record current branch and commit hash.
- [x] Keep the latest known reference artifact handy:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_real_receive_split_20260421_004637`
- [x] Record current reference metrics:
  `2010095 get_frame_errors_final = 126`, all code `12`
- [x] Record current reference metrics:
  both cameras `camera_frame_id_gaps = 0`
- [x] Confirm current binary is the A16 worktree binary:
  `/home/jeremy/orange-gop-split-a16/targets/release/orange_client`

### 2. Replace Scratch Receive Descriptor With Stable Receive/Requeue Handles

Preferred first implementation:

- [x] Add an `Emergent::CEmergentFrame` receive descriptor to `WORKER_ENTRY`.
- [x] In `acquire_frames()`, call `EVT_CameraGetFrame()` into the current
      `WORKER_ENTRY` receive descriptor rather than `ecam->frame_recv`.
- [x] Use a local pointer such as `received_frame` for all metadata reads in
      that acquisition iteration.
- [x] Replace direct uses of `ecam->frame_recv.imagePtr`, `frame_id`,
      `timestamp`, `bufferSize`, `size_x`, `size_y`, and `pixel_type` in the
      main acquisition loop with the stable per-entry descriptor.
- [x] Resolve the received `imagePtr` back to the stable `ecam->evt_frame[]`
      descriptor that owns that SDK receive buffer.
- [x] For direct GPUDirect pass-through, set:
  `current_entry->camera_frame_struct = frame_to_requeue`.
- [x] For ring-copy pending requeues, store:
  `PendingRequeue::frame = frame_to_requeue`.
- [x] For immediate requeue error paths after a successful get-frame, call
      `EVT_CameraQueueFrame()` with `frame_to_requeue`, not
      `&ecam->frame_recv`.
- [x] Keep `ecam->frame_recv` only for legacy paths that are not part of the
      main queued-worker recording path.

Why this is preferred:

- SDK examples allocate stable receive-buffer pools but call
  `EVT_CameraGetFrame()` into a separate receive descriptor, then queue that
  received descriptor.
- A per-entry descriptor remains alive until the `WORKER_ENTRY` is recycled.
- Using the stable `evt_frame[]` descriptor as the requeue handle avoids a
  ring-copy lifetime hazard where a `WORKER_ENTRY` can be recycled before the
  acquisition-thread pending requeue drains.

Fallback behavior if the `imagePtr -> evt_frame[]` lookup fails:

- [ ] If lookup fails, copy into Orange-owned memory and requeue immediately
      using the scratch descriptor. The current patch falls back to the receive
      descriptor; a stricter copy-and-requeue fallback is still future hardening.

### 3. Prevent Double Requeue And Make Errors Visible

- [ ] Add a small helper for SDK frame return, for example
      `queue_camera_frame_if_present(...)`.
- [ ] The helper should call `EVT_CameraQueueFrame()` only when both camera and
      frame pointers are present.
- [ ] The helper should clear `camera_instance`, `camera_frame_struct`, and
      `camera_buffer_ptr` after successful or failed requeue.
- [ ] The helper should count/log `EVT_CameraQueueFrame()` failures instead of
      ignoring the return value.
- [ ] Use the helper in preprocess release paths first.
- [ ] Audit other direct requeue paths after the first patch:
  `recording_ingress.cpp`, `opengldisplay.cpp`, `gpu_video_encoder.cpp`,
  `yolo_worker.cpp`, and `crop_and_encode_worker.cpp`.

### 4. Add Minimal Lease Telemetry

Add telemetry only after the stable-descriptor patch compiles, so the first diff
stays reviewable.

- [ ] Count GPUDirect leases acquired.
- [ ] Count GPUDirect leases returned.
- [ ] Track current outstanding GPUDirect leases.
- [ ] Track max outstanding GPUDirect leases.
- [ ] Track stable receive-descriptor misses if any fallback path exists.
- [ ] Track deferred requeue errors by SDK error code.
- [ ] Record receive-to-source-safe latency where the source-release event is
      recorded.
- [ ] Record source-safe-to-requeue latency where the frame is actually queued
      back to the SDK.
- [ ] Surface the counters in `recording_snapshot.json`.
- [ ] Surface final counters in headless `summary.json` / CSV rows if the run is
      an experiment spec.

### 5. Build And Non-Hardware Checks

- [x] Build `orange_client`:
  `cmake --build /home/jeremy/orange-gop-split-a16/targets/release --target orange_client`
- [ ] Run existing lightweight unit tests if available and relevant.
- [x] Search for remaining deferred uses of `&ecam->frame_recv`.
- [ ] Search for unchecked `EVT_CameraQueueFrame()` calls in the modified path.
- [x] Confirm unrelated dirty files are not included in the diff.

### 6. Hardware Validation Sequence

Run short tests first. Only move to longer recording once the short run has
healthy frame IDs and no obvious requeue errors.

- [x] Single-camera `100 fps` real recording on `2010095`.
- [x] Single-camera `100 fps` real recording on `2010096`.
- [ ] Dual-camera `100 fps` stream-only run, if we want a source-only check.
- [x] Dual-camera `100 fps` split-GOP real recording with the recabled topology.
- [x] Four-camera `100 fps` stream-only and real recording smoke runs.
- [x] Four-camera `100 fps` `ptp_gate` stream-only and real recording smoke
      runs.
- [ ] Optional dual-camera `100 fps` `force_ring_copy` real recording to confirm
      the ring-copy pending requeue path is also safe.

Primary pass criteria:

- [x] `camera_frame_id_gaps = 0` on both cameras.
- [ ] `EVT_CameraQueueFrame()` deferred requeue errors are `0`.
- [x] No new preprocess drops or encode failures.
- [x] Videos are present and near target duration.

Comparison metrics:

- [x] `get_frame_errors_final`
- [ ] `get_frame_errors_by_code`
- [ ] max outstanding GPUDirect leases
- [ ] source-safe-to-requeue max latency
- [x] acquisition FPS mean and p95
- [x] helper dispatched frames and primary/helper balance

Completed validation artifacts:

- Checked-in recabled config:
  `config/validated_split_gop_hevc_100fps_gop25_recabled_a16/`
- Checked-in four-camera config:
  `config/validated_split_gop_hevc_100fps_gop25_fourcam_a16/`
- Checked-in validation specs:
  `experiment_specs/2010095_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch.json`,
  `experiment_specs/2010096_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch.json`,
  and
  `experiment_specs/2010095_2010096_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch.json`
- Checked-in PTP validation specs:
  `experiment_specs/2010095_2010096_split_gop_hevc_100fps_ptp_stream_only_recabled_stable_frame_patch.json`
  and
  `experiment_specs/2010095_2010096_split_gop_hevc_100fps_ptp_real_recabled_stable_frame_patch_12s.json`
- Checked-in four-camera validation specs:
  `experiment_specs/2010093_2010094_2010095_2010096_split_gop_hevc_100fps_stream_only_sys_pair.json`,
  `experiment_specs/2010093_2010094_2010095_2010096_split_gop_hevc_100fps_real_sys_pair.json`,
  `experiment_specs/2010093_2010094_2010095_2010096_split_gop_hevc_100fps_ptp_stream_only_sys_pair.json`,
  and
  `experiment_specs/2010093_2010094_2010095_2010096_split_gop_hevc_100fps_ptp_real_sys_pair.json`
- Single `2010095`:
  `/home/jeremy/orange_data/exp/unsorted/2010095_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch`
- Single `2010096`:
  `/home/jeremy/orange_data/exp/unsorted/2010096_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch`
- Dual `2010095 + 2010096`:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch`
- Dual `2010095 + 2010096`, `ptp_gate`, stream-only:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_ptp_stream_only_recabled_stable_frame_patch`
- Dual `2010095 + 2010096`, `ptp_gate`, real recording:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_ptp_real_recabled_stable_frame_patch_12s`
- Four `2010093 + 2010094 + 2010095 + 2010096`, stream-only:
  `/home/jeremy/orange_data/exp/unsorted/2010093_2010094_2010095_2010096_split_gop_hevc_100fps_stream_only_sys_pair`
- Four `2010093 + 2010094 + 2010095 + 2010096`, real recording:
  `/home/jeremy/orange_data/exp/unsorted/2010093_2010094_2010095_2010096_split_gop_hevc_100fps_real_sys_pair`
- Four `2010093 + 2010094 + 2010095 + 2010096`, `ptp_gate`, stream-only:
  `/home/jeremy/orange_data/exp/unsorted/2010093_2010094_2010095_2010096_split_gop_hevc_100fps_ptp_stream_only_sys_pair`
- Four `2010093 + 2010094 + 2010095 + 2010096`, `ptp_gate`, real recording:
  `/home/jeremy/orange_data/exp/unsorted/2010093_2010094_2010095_2010096_split_gop_hevc_100fps_ptp_real_sys_pair`

Dual-camera result:

- `2010095`: `1001` frames, `0` frame-ID gaps, `0` GetFrame errors, `0`
  preprocess drops, `0` encode failures.
- `2010096`: `1001` frames, `0` frame-ID gaps, `0` GetFrame errors, `0`
  preprocess drops, `0` encode failures.
- PTP stream-only: both cameras `701` frames, `0` frame-ID gaps,
  `0` GetFrame errors.
- PTP real recording `12 s`: `2010095` `1001` submitted frames and `2010096`
  `1000` submitted frames, both with `0` frame-ID gaps, `0` GetFrame errors,
  `0` preprocess drops, and `0` encode failures.
- Four-camera `free_run` stream-only: all cameras `1001` frames with `0`
  frame-ID gaps and `0` GetFrame errors.
- Four-camera `free_run` real recording: all cameras passed with `799-800`
  submitted post-warmup frames, `0` frame-ID gaps, `0` GetFrame errors, `0`
  preprocess drops, and `0` encode failures.
- Four-camera `ptp_gate` stream-only: all cameras `701` frames with `0`
  frame-ID gaps and `0` GetFrame errors.
- Four-camera `ptp_gate` real recording: all cameras passed with `600`
  submitted post-warmup frames, `0` frame-ID gaps, `0` GetFrame errors, `0`
  preprocess drops, `0` encode failures, `overflow_events = 0`, and
  `peak_backlog_gops = 2`.

### 7. Decision Points After Validation

If `EVT_ERROR_NOMEM` drops substantially:

- [x] Treat the scratch receive descriptor as a confirmed contributor for the
      recabled headless free-run validation path.
- [x] Keep the stable descriptor patch.
- [ ] Expand telemetry only as needed for long-run confidence.
- [ ] Rerun a longer dual-camera `100 fps` recording.

If `EVT_ERROR_NOMEM` remains but frame IDs stay clean:

- [ ] Keep the stable descriptor patch as correctness hardening.
- [ ] Move same-GPU source release earlier where safe.
- [ ] Add configurable SDK receive buffer count for headless and GUI.
- [ ] Test receive buffer pool sizes such as `100`, `150`, and `200`, while
      tracking GPU memory cost.

If true frame-ID gaps appear:

- [ ] Stop treating the issue as only SDK receive-buffer pressure.
- [ ] Compare camera timestamp deltas and host receive deltas around the first
      gap.
- [ ] Re-run with helper source-read no-op to confirm whether helper copy
      pressure is still the trigger.

If helper copy pressure remains the limiting factor:

- [ ] Revisit helper copy scheduling.
- [ ] Consider GOP-level helper copy pacing rather than per-frame sleeps.
- [ ] Consider smaller GOP length to reduce burst size.
- [ ] Consider keeping helper GPUs local to the same PIX island and avoiding SYS
      or cross-host-bridge routes.

## Out Of Scope For The First Patch

- Increasing `evt_buffer_size`.
- Changing camera cable topology.
- Changing PTP gate behavior.
- Reworking split-GOP routing policy.
- Bulk staging a GOP of source frames before helper copy.
- Refactoring all consumers into a new ownership abstraction.

Those may still be useful, but they should come after stable SDK frame
descriptor ownership is correct and measurable.
