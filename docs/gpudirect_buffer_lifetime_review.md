# GPUDirect Buffer Lifetime Review

Date: 2026-04-21
Branch: `exp/gop-split-a16`

Related checklist:

- `docs/gpudirect_buffer_lifetime_implementation_checklist.md`

## Purpose

This note records the current understanding of how Orange holds Emergent SDK
GPUDirect buffers during recording, where those buffers may be held longer than
necessary, and what should be changed before further receive-buffer-pressure
tuning.

The immediate motivation is the recabled dual-camera `100 fps` run where true
camera frame-ID gaps were `0`, but one camera still reported
`EVT_CameraGetFrame` error `12` (`EVT_ERROR_NOMEM`). That points at SDK receive
buffer pressure rather than confirmed image loss.

## Current Buffer Model

At stream setup, Orange allocates a fixed pool of SDK receive frames:

- GUI and headless currently use `evt_buffer_size = 100`.
- Each pool entry is an `Emergent::CEmergentFrame`.
- `allocate_frame_buffer()` initializes each frame and queues it with
  `EVT_CameraQueueFrame()`.
- With GPUDirect enabled, those SDK frames are zero-copy receive buffers whose
  `imagePtr` is CUDA device memory on the configured acquisition GPU.

During acquisition, Orange calls:

- `EVT_CameraGetFrame(&ecam->camera, &ecam->frame_recv, 1000)`

`ecam->frame_recv` is a single reusable scratch `CEmergentFrame`, not one of the
stable entries in `ecam->evt_frame[]`.

When recording is the only consumer and the received frame pointer is already a
CUDA device pointer on `camera_params->gpu_id`, Orange currently uses direct
pass-through:

- `WORKER_ENTRY::d_image` points directly at `ecam->frame_recv.imagePtr`.
- `WORKER_ENTRY::gpu_direct_mode = true`.
- `WORKER_ENTRY::owns_memory = false`.
- The SDK receive buffer is not requeued immediately.
- Downstream workers eventually call `EVT_CameraQueueFrame()` once CUDA work no
  longer needs the source image.

When there are multiple consumers, or when `fixed.acquisition_buffer_mode` is
`force_ring_copy`, Orange copies into an Orange-owned acquisition buffer and
requeues the SDK source after a CUDA event proves the copy is complete.

## Key Finding: Stable SDK Frame Handle Risk

The most important review finding is that direct pass-through stores a pointer
to the reusable scratch frame:

- `WORKER_ENTRY::camera_frame_struct = &ecam->frame_recv`

The ring-copy pending requeue path similarly stores:

- `PendingRequeue::frame = &ecam->frame_recv`

That is risky because acquisition continues calling `EVT_CameraGetFrame()` into
the same `ecam->frame_recv` object. If a previous GPUDirect receive buffer is
still leased downstream, the scratch `CEmergentFrame` can be overwritten before
the deferred `EVT_CameraQueueFrame()` occurs.

The device pointer itself remains a valid source pointer while the SDK buffer is
not requeued, but the frame object used to return that buffer to the SDK may no
longer describe the same buffer. In the worst case, Orange can requeue the wrong
SDK frame, double-requeue a newer one, or fail to return the older one promptly.

This is a better first suspect for residual `EVT_ERROR_NOMEM` pressure than
simply increasing the SDK buffer pool.

## Correction: Idle GOP Drain Is Not The Main Suspect

An earlier hypothesis was that helper preprocess workers might only drain
completed source-release events when they receive the next helper GOP, holding
SDK buffers for roughly a full GOP interval.

Inspection corrected that hypothesis:

- `CThreadWorker::ThreadRunning()` calls `WorkerFunction(nullptr)` when the
  input queue is empty.
- `EncoderPreprocessWorker::WorkerFunction()` drains
  `pending_source_releases_` before checking for `nullptr`.
- The default worker interval is short, so completed source-release events are
  still polled while the worker is idle.

That means the larger risk is not GOP-idle drain cadence. The larger risk is
whether the delayed requeue points at a stable SDK frame handle.

## Current Good Behavior

The current source-read safety model is still directionally correct:

- Helper cross-GPU routing records a source-release CUDA event immediately after
  the peer copy from source GPU to helper GPU is queued.
- Primary same-GPU routing records source release after preprocess work that may
  read the source image.
- Deferred release waits for the CUDA event before requeueing the SDK source.
- `force_ring_copy` can move source ownership into Orange-owned buffers, which
  is useful as a diagnostic knob.

The issue is not "we requeue before CUDA is done." The issue is "the object we
later pass to `EVT_CameraQueueFrame()` may not be the stable SDK frame object
associated with the borrowed buffer."

## Efficiency And Correctness Opportunities

1. Store stable SDK frame leases.

   Prefer receiving directly into a per-`WORKER_ENTRY` frame descriptor, then
   requeue that same descriptor after downstream CUDA work is source-safe. If
   that proves incompatible with the SDK, fall back to matching
   `ecam->frame_recv.imagePtr` back to the corresponding `ecam->evt_frame[]`
   entry and requeueing that stable frame instead of `&ecam->frame_recv`.

2. Add explicit GPUDirect lease telemetry.

   Track:

   - outstanding SDK GPUDirect leases
   - max outstanding leases
   - receive-to-source-safe latency
   - source-safe-to-requeue latency
   - `EVT_CameraQueueFrame()` errors from deferred requeue paths

   This should make `EVT_ERROR_NOMEM` explainable as a buffer-retirement delay
   instead of a vague receive-side symptom.

3. Move same-GPU source release earlier where safe.

   For mono non-resize recording, the source image is needed for the Y-plane
   copy. The UV fill reads only `d_uv_default_plane_`, not the camera source.
   Recording the source-release event immediately after the source-dependent
   copy could shorten same-GPU GPUDirect leases.

4. Keep larger SDK receive pools as a fallback, not the first fix.

   A full `4512 x 4512` `Mono8` frame is about `20 MiB`. A `100`-frame SDK
   GPUDirect pool is already roughly `2 GiB` per camera. Raising this can mask
   pressure, but it is expensive and does not fix incorrect or delayed buffer
   return.

## Implemented Patch And Validation

Implemented in commit `951f910` (`fix gpudirect receive buffer requeue`).
Acquisition now receives into a per-`WORKER_ENTRY`
`Emergent::CEmergentFrame` descriptor for current-frame metadata, then resolve
that frame's `imagePtr` back to the stable `ecam->evt_frame[]` descriptor used
as the actual SDK requeue handle.

That split matters because direct pass-through keeps the `WORKER_ENTRY` alive
until source release, but ring-copy can recycle the entry before the
acquisition-thread pending requeue drains.

Validation used the A16 worktree binary:

- `/home/jeremy/orange-gop-split-a16/targets/release/orange_client`

Checked-in validation config and specs:

- `config/validated_split_gop_hevc_100fps_gop25_recabled_a16/`
- `experiment_specs/2010095_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch.json`
- `experiment_specs/2010096_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch.json`
- `experiment_specs/2010095_2010096_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch.json`

The clean dual-camera validation artifact is:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch`

The pre-fix comparison artifact was:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_real_receive_split_20260421_004637`

Post-fix validation summary:

| Run | Result |
| --- | --- |
| Single `2010095`, `100 fps`, real split-GOP GPUDirect | pass, `801` frames, `0` frame-ID gaps, `0` GetFrame errors |
| Single `2010096`, `100 fps`, real split-GOP GPUDirect | pass, `801` frames, `0` frame-ID gaps, `0` GetFrame errors |
| Dual `2010095 + 2010096`, `100 fps`, real split-GOP GPUDirect | pass, both cameras `1001` frames, `0` frame-ID gaps, `0` GetFrame errors, `0` preprocess drops, `0` encode failures |

Current conclusion:

- The residual `EVT_ERROR_NOMEM` receive-buffer pressure seen in the pre-fix
  recabled run was addressed by stable receive/requeue descriptor handling for
  this headless free-run path.
- Dual-camera `20 MP` `100 fps` split-GOP HEVC recording is now validated for
  the recabled A16 topology used by this run.
- This does not yet validate GUI recording, PTP-gated recording, longer-duration
  soak runs, or more than two cameras.
