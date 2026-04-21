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

- [ ] Record current branch and commit hash.
- [ ] Keep the latest known reference artifact handy:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_real_receive_split_20260421_004637`
- [ ] Record current reference metrics:
  `2010095 get_frame_errors_final = 126`, all code `12`
- [ ] Record current reference metrics:
  both cameras `camera_frame_id_gaps = 0`
- [ ] Confirm current binary is the A16 worktree binary:
  `/home/jeremy/orange-gop-split-a16/targets/release/orange_client`

### 2. Replace Scratch Receive Descriptor With Stable Per-Entry Descriptor

Preferred first implementation:

- [ ] Add an `Emergent::CEmergentFrame` receive descriptor to `WORKER_ENTRY`.
- [ ] In `acquire_frames()`, call `EVT_CameraGetFrame()` into the current
      `WORKER_ENTRY` receive descriptor rather than `ecam->frame_recv`.
- [ ] Use a local pointer such as `received_frame` for all metadata reads in
      that acquisition iteration.
- [ ] Replace direct uses of `ecam->frame_recv.imagePtr`, `frame_id`,
      `timestamp`, `bufferSize`, `size_x`, `size_y`, and `pixel_type` in the
      main acquisition loop with the stable per-entry descriptor.
- [ ] For direct GPUDirect pass-through, set:
  `current_entry->camera_frame_struct = received_frame`.
- [ ] For ring-copy pending requeues, store:
  `PendingRequeue::frame = received_frame`.
- [ ] For immediate requeue error paths after a successful get-frame, call
      `EVT_CameraQueueFrame()` with `received_frame`, not `&ecam->frame_recv`.
- [ ] Keep `ecam->frame_recv` only for legacy paths that are not part of the
      main queued-worker recording path.

Why this is preferred:

- SDK examples allocate stable receive-buffer pools but call
  `EVT_CameraGetFrame()` into a separate receive descriptor, then queue that
  received descriptor.
- A per-entry descriptor remains alive until the `WORKER_ENTRY` is recycled.
- It avoids relying on pointer matching from `imagePtr` back to `evt_frame[]`.

Fallback implementation if the per-entry descriptor proves incompatible:

- [ ] Build an `imagePtr -> Emergent::CEmergentFrame*` lookup over
      `ecam->evt_frame[]`.
- [ ] Store the stable `evt_frame[]` pointer on `WORKER_ENTRY`.
- [ ] Requeue the stable `evt_frame[]` pointer instead of `&ecam->frame_recv`.
- [ ] If lookup fails, copy into Orange-owned memory and requeue immediately
      using the scratch descriptor.

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

- [ ] Build `orange_client`:
  `cmake --build /home/jeremy/orange-gop-split-a16/targets/release --target orange_client`
- [ ] Run existing lightweight unit tests if available and relevant.
- [ ] Search for remaining deferred uses of `&ecam->frame_recv`.
- [ ] Search for unchecked `EVT_CameraQueueFrame()` calls in the modified path.
- [ ] Confirm unrelated dirty files are not included in the diff.

### 6. Hardware Validation Sequence

Run short tests first. Only move to longer recording once the short run has
healthy frame IDs and no obvious requeue errors.

- [ ] Single-camera `100 fps` real recording on `2010095`.
- [ ] Single-camera `100 fps` real recording on `2010096`.
- [ ] Dual-camera `100 fps` stream-only run, if we want a source-only check.
- [ ] Dual-camera `100 fps` split-GOP real recording with the recabled topology.
- [ ] Optional dual-camera `100 fps` `force_ring_copy` real recording to confirm
      the ring-copy pending requeue path is also safe.

Primary pass criteria:

- [ ] `camera_frame_id_gaps = 0` on both cameras.
- [ ] `EVT_CameraQueueFrame()` deferred requeue errors are `0`.
- [ ] No new preprocess drops or encode failures.
- [ ] Videos are present and near target duration.

Comparison metrics:

- [ ] `get_frame_errors_final`
- [ ] `get_frame_errors_by_code`
- [ ] max outstanding GPUDirect leases
- [ ] source-safe-to-requeue max latency
- [ ] acquisition FPS mean and p95
- [ ] helper dispatched frames and primary/helper balance

### 7. Decision Points After Validation

If `EVT_ERROR_NOMEM` drops substantially:

- [ ] Treat the scratch receive descriptor as a confirmed contributor.
- [ ] Keep the stable descriptor patch.
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
