# GUI Display And Recording Buffer Ownership Plan

Date: 2026-04-21
Branch: `exp/gop-split-a16`

Related notes:

- `docs/gui_recording_status.md`
- `docs/session_orchestration_architecture.md`
- `docs/recording_panel_modularization_plan.md`
- `docs/gpudirect_buffer_lifetime_review.md`
- `docs/multi_camera_failure_modes.md`

## Purpose

This note defines a refactor target for the GUI recording path after the
four-camera headless `20 MP` `100 fps` split-GOP validation.

The immediate issue is that GUI display fanout currently changes acquisition
buffer ownership. Headless record-only can pass an SDK GPUDirect receive buffer
directly to recording, but GUI stream-plus-record usually forces acquisition to
copy every frame into an Orange-owned buffer before display and recording share
it.

That coupling is too strong. Display should not silently change recording's
lowest-overhead safe buffer path.

## Current Behavior

The relevant current code paths are:

- [src/acquire_frames.cpp](/home/jeremy/orange-gop-split-a16/src/acquire_frames.cpp)
- [src/opengldisplay.cpp](/home/jeremy/orange-gop-split-a16/src/opengldisplay.cpp)
- [src/threadworker.h](/home/jeremy/orange-gop-split-a16/src/threadworker.h)
- [src/recording_ingress.cpp](/home/jeremy/orange-gop-split-a16/src/recording_ingress.cpp)
- [src/encoder_preprocess_worker.cpp](/home/jeremy/orange-gop-split-a16/src/encoder_preprocess_worker.cpp)

In `acquire_frames(...)`, each received SDK frame is classified by consumers:

- display if `camera_select->stream_on && openGLDisplay`
- recording if `camera_control->record_video && recording_ingress`
- YOLO if `camera_select->yolo && yolo_worker`

Those booleans are collapsed into `dispatch_count`.

Then acquisition chooses the buffer ownership path:

- `dispatch_count == 1` with a device pointer on the acquisition GPU can use
  direct GPUDirect pass-through.
- `dispatch_count > 1` forces `use_ring_copy = true`.

That means GUI stream-plus-record changes source ownership even when recording
itself could have safely consumed the SDK GPUDirect buffer directly.

## Why This Matters

For a `4512 x 4512 Mono8` camera, one frame is about `20 MiB`.

At `100 fps`, forcing a full-frame acquisition-side copy adds roughly `2 GiB/s`
per camera before the recording pipeline sees the frame. With four cameras,
that is a large extra load in the most timing-sensitive part of the pipeline.

Display also has its own cost:

- `COpenGLDisplay` currently uses display GPU `0`.
- When acquisition is on an A16 GPU, display copies from device to pinned host
  and then from pinned host to display GPU.
- The display worker keeps only the latest queued frame, but acquisition still
  offers every frame to display before the display worker drops old frames.
- `PutObjectToQueueIn(...)` blocks when the queue is full, so slow display can
  backpressure acquisition.

The important conclusion is not just "display does extra work." The stronger
problem is:

- display changes recording buffer semantics,
- display can add acquisition-side full-frame copies,
- display can become an acquisition backpressure source,
- GUI validation can diverge from headless validation even when the recording
  workers are shared.

## GPUDirect Receive Versus NVENC Input

The goal is not to send camera frames directly into NVENC as-is.

There are two separate "direct" concepts:

- GPUDirect camera receive,
- NVENC input surfaces.

With GPUDirect camera receive, the Emergent SDK receives camera image data
directly into CUDA device memory on the acquisition GPU. That buffer is still
SDK/camera-owned. Orange can borrow the pointer, but it must return/requeue the
SDK frame once all source reads are safe.

NVENC input is different. NVENC expects an encoder-compatible input surface,
such as NV12 or another supported layout. The current camera frame is full-frame
`Mono8`, so Orange still has to run recording preprocess work before encode.
That preprocess step reads the camera image and prepares the encoder input
surface.

The desired low-copy recording path is:

```text
camera
  -> SDK GPUDirect CUDA buffer
  -> recording preprocess
  -> NVENC input surface
  -> encode
```

The current GUI stream-plus-record path can become:

```text
camera
  -> SDK GPUDirect CUDA buffer
  -> full-rate Orange-owned ring/staging copy
  -> recording preprocess
  -> NVENC input surface
  -> encode
```

The extra ring/staging copy is what this plan tries to avoid for frames where
recording is the only full-rate every-frame consumer.

The desired GUI preview shape is closer to:

```text
camera
  -> SDK GPUDirect CUDA buffer
  -> recording preprocess
  -> NVENC input surface
  -> encode

same source frame, selected by preview policy only when needed
  -> display preview copy
  -> OpenGL display
```

Longer-term `nvenc_direct_input` work is related but separate. That would target
the preprocess-to-NVENC handoff. The buffer-ownership issue in this document is
about avoiding an unnecessary acquisition-side full-frame copy before recording
preprocess.

## Desired Semantics

Recording should get the lowest-overhead safe path available for the selected
recording mode.

Display should be a latest-only or rate-limited observer of acquisition. It
should not require every recording frame unless explicitly configured.

YOLO and IPC may have different requirements and should declare them
separately. They should not be implicitly bundled into the same display
semantics.

The long-term model should separate:

- SDK receive-buffer lease ownership,
- Orange-owned image payload ownership,
- per-consumer frame subscription policy,
- source-safe CUDA events,
- final frame recycling.

## Target Ownership Model

### Camera Frame Lease

A `CameraFrameLease` represents the SDK receive buffer and the exact stable SDK
frame descriptor needed to requeue it.

Responsibilities:

- hold `Emergent::CEmergentCamera*`
- hold the stable `Emergent::CEmergentFrame*` requeue handle
- hold the source image pointer
- requeue exactly once, after all required source reads are source-safe
- expose telemetry for lease lifetime and requeue errors

This is the explicit version of the stable receive/requeue descriptor fix.

### Frame Payload

A `FramePayload` describes the image that consumers read.

Possible backing modes:

- borrowed SDK GPUDirect pointer,
- Orange-owned acquisition staging buffer,
- derived display-sized buffer,
- consumer-owned copy.

The payload should know:

- source GPU,
- pointer,
- dimensions and pixel format,
- CUDA event indicating source readiness,
- whether reading this payload extends the camera lease.

### Consumer Subscription

Each consumer should declare what it needs:

- `every_frame`: recording and some IPC modes
- `latest_only`: GUI display
- `decimated`: GUI preview, YOLO, or future lightweight monitoring
- `on_demand`: snapshot/manual save

Each consumer should also declare whether it can accept:

- borrowed camera buffer,
- shared Orange-owned staging buffer,
- private copy,
- stale/drop-oldest behavior.

## Lease Policy Mental Model

The lease/subscription policy should be configured before a stream or recording
session starts, then applied for each acquired frame.

At setup time, Orange already knows which consumers are active:

- recording,
- GUI display,
- YOLO,
- IPC,
- snapshot/manual frame save.

Each active consumer should describe what it needs. The acquisition loop should
then decide, frame by frame, which consumers actually receive the frame and what
buffer ownership strategy is safe.

This means a refcount-like `dispatch_count` is still useful, but only as a
low-level lifetime detail after the subscription policy has selected consumers.
It should not be the high-level ownership policy by itself.

Current overloaded model:

```cpp
dispatch_count = display + recording + yolo;
if (dispatch_count > 1) {
    use_ring_copy = true;
}
```

Better model:

```cpp
selected_consumers = apply_subscription_policy(frame);
lease_plan = choose_buffer_strategy(selected_consumers);
ref_count = selected_consumers.size();
dispatch_to(selected_consumers, lease_plan);
```

The key difference is that "display exists" does not automatically mean display
receives every frame or changes recording's source-buffer mode.

## Who Gets A Ring Copy

It is more accurate to say "acquisition creates an Orange-owned payload" than
"a consumer gets a ring copy."

The SDK camera buffer is camera-owned. A ring/staging copy is the handoff point
where the frame becomes Orange-owned. Once Orange owns the copied payload, the
SDK receive buffer can be requeued independently of slower downstream
consumers.

Ring/staging copies are useful when:

- multiple full-frame consumers need the same frame and at least one may outlive
  the safe camera-buffer lease window,
- a consumer is droppable or slow and should not hold an SDK receive buffer,
- a consumer needs a different GPU or format,
- Orange wants to requeue the SDK buffer quickly to protect acquisition
  stability,
- the source pointer is not a usable GPUDirect device pointer,
- a diagnostic/config mode explicitly forces ring-copy.

Ring/staging copies are not ideal when:

- recording is the only full-frame every-frame consumer,
- display is only a preview and can skip most frames,
- YOLO is off,
- IPC is metadata-only,
- a borrowed SDK buffer only needs to be held until a short CUDA source-safe
  event.

Practical V1 policy:

- recording-only: use borrowed SDK GPUDirect pointer when available and requeue
  after recording's source-safe event
- recording plus display preview: recording keeps the borrowed SDK pointer;
  display receives only selected preview frames and should skip/drop when busy
- recording plus YOLO every frame: use ring/staging or a proper multi-source-safe
  lease plan, because both consumers may need full-frame image reads
- display-only: use latest-only Orange-owned preview/staging payloads, because
  GUI rendering should not hold SDK camera buffers
- multiple non-droppable full-frame consumers: use ring/staging copy unless a
  more explicit multi-lease source-safe plan has been implemented

The ring/staging buffer should live at the ownership boundary between the
camera-owned SDK receive buffer and Orange-owned frame payloads. That boundary
belongs in acquisition or an acquisition-adjacent fanout layer, not hidden
inside the display or recording workers.

## Proposed V1 Refactor

Do not attempt the full ownership abstraction in one patch. The first useful
slice is to make display latest-only before it influences acquisition ownership.

V1 behavior:

- recording remains the primary owner of the full-rate frame path when
  recording is active,
- display is offered frames only at a configured preview cadence,
- display enqueue should be non-blocking or drop-oldest/drop-new when saturated,
- display should no longer force ring-copy for every recording frame,
- telemetry should show how many frames were offered to display, accepted,
  dropped, and rendered.

For GUI recording, a reasonable default is:

- recording: every frame,
- display preview: at most `30` or `60 fps`,
- YOLO: explicit existing enable/decimation behavior,
- IPC: unchanged until its consumer requirements are clarified.

## Open Design Questions

1. Should display preview rate be fixed at `60 fps`, configurable in app config,
   or per-camera?
2. Should display drop newest when busy, or drop oldest and keep latest?
3. Should display consume borrowed SDK buffers at all, or only Orange-owned
   preview copies?
4. Should recording always win over display when there is contention?
5. Should GUI expose a "preview FPS" control, or keep it as an advanced/app
   config field?
6. Should display preview be disabled automatically for high-rate four-camera
   validation runs?

The current recommendation is:

- make preview FPS configurable but default it to `60`,
- make display latest-only,
- never let display block acquisition,
- never let display force full-rate recording frames into ring-copy by default.

## Implementation Checklist

### 1. Add Display Subscription Policy

- [ ] Add a small display subscription policy struct.
- [ ] Fields should include `enabled`, `mode`, and `max_fps`.
- [ ] Initial modes should be `every_frame` and `latest_only`.
- [ ] Default GUI mode should be `latest_only`.
- [ ] Keep headless behavior unchanged.

### 2. Gate Display Before Dispatch Count

- [ ] In `acquire_frames(...)`, compute display eligibility before
      `dispatch_count`.
- [ ] Track last display-offered timestamp per camera.
- [ ] Set `will_display = false` when preview cadence says to skip.
- [ ] Ensure skipped display frames do not increment `dispatch_count`.
- [ ] Confirm record-only and GUI record-plus-preview can still use direct
      GPUDirect when no other full-rate consumer needs the frame.

### 3. Make Display Enqueue Non-Blocking

- [ ] Add a non-blocking queue insert path to `CThreadWorker` or a display-only
      wrapper.
- [ ] Prefer latest-only semantics for display: keep newest, release older
      display references promptly.
- [ ] Do not let display queue saturation block acquisition.
- [ ] Track display enqueue drops separately from camera frame drops.

### 4. Preserve Correct Lifetime

- [ ] If display receives a borrowed SDK buffer, ensure its source read records a
      source-safe event before requeue.
- [ ] If that is too invasive for V1, display should receive only Orange-owned
      preview copies when it is active.
- [ ] Ensure recording source release still controls SDK lease lifetime when
      recording is the only every-frame consumer.
- [ ] Ensure `ref_count` cannot recycle the `WORKER_ENTRY` before all selected
      consumers are done.

### 5. Add Telemetry

- [ ] Add per-camera display counters to pipeline metrics or a display sidecar:
      offered, accepted, skipped by cadence, dropped by queue, rendered.
- [ ] Include display queue high-water mark.
- [ ] Include whether a frame used `direct`, `ring_copy`, or another copy mode.
- [ ] Make GUI validation artifacts easy to compare against headless artifacts.

### 6. Validate With Headless Controls

- [ ] Confirm existing headless specs still pass.
- [ ] Confirm headless record-only still reports direct GPUDirect frames.
- [ ] Add or run a headless-style control with synthetic display subscription if
      practical.
- [ ] Confirm ring-copy counts do not rise unless another full-rate consumer is
      enabled.

### 7. Validate With GUI

- [ ] Single-camera GUI stream-only at `100 fps`.
- [ ] Single-camera GUI stream-plus-record at `100 fps`, YOLO off.
- [ ] Dual-camera GUI stream-plus-record at `100 fps`, YOLO off.
- [ ] Four-camera GUI stream-plus-record at `100 fps`, YOLO off.
- [ ] Four-camera GUI no-stagger `ptp_gate` stream-plus-record at `100 fps`,
      YOLO off.

GUI pass criteria:

- `camera_frame_id_gaps = 0`,
- `get_frame_errors = 0`,
- `preprocess_frames_dropped = 0`,
- `encode_failures = 0`,
- `overflow_events = 0`,
- display drops/skips are allowed only as preview behavior,
- recording output duration and frame counts match policy.

## Risks

- A display latest-only queue still has to release skipped frame references
  correctly.
- If display consumes borrowed SDK buffers, source-safe release becomes more
  complex.
- If display uses Orange-owned preview copies, preview copy cost remains but is
  rate-limited.
- Refactoring `WORKER_ENTRY` lifetime too broadly could destabilize the recently
  validated recording path.

## Recommended Slice Order

1. Add display cadence gating before `dispatch_count`.
2. Add display counters.
3. Make display enqueue non-blocking/latest-only.
4. Run GUI single-camera and dual-camera smoke.
5. Run four-camera GUI smoke.
6. Only then consider a deeper `CameraFrameLease` / `FramePayload` abstraction.

This order gives us a practical safety improvement without immediately
rewriting the full acquisition fanout model.
