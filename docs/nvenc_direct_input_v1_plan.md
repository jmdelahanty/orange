# NVENC Direct Input V1 Plan

Date: 2026-04-08
Scope: concrete implementation plan for the first direct registered NV12 input
path in the modern recording pipeline.

See also:

- `docs/nvenc_direct_input_plan.md`
- `docs/nvenc_throughput_todo.md`
- `docs/color_recording_pipeline.md`

## Goal

Implement a bounded first version of direct registered NV12 input for the
modern recording path so preprocess can write directly into NVENC-registered
input surfaces and the hardware worker can encode without the extra
`CopyToDeviceFrame(...)` step.

This v1 is intentionally narrow. The goal is to remove one avoidable
device-to-device copy while keeping ownership and rollback behavior easy to
reason about.

## In-Scope

- modern full-frame recording path:
  - `EncoderPreprocessWorker -> EncoderHwWorker -> FFmpegWriter`
- `NV12` input only
- external CUDA input surfaces registered with the local NVENC wrapper
- explicit encoder-slot lifecycle separate from raw-frame refcounting
- copy-path fallback during bring-up
- minimal telemetry for slot reuse and starvation

## Out Of Scope

- legacy `GPUVideoEncoder` path
- direct RGB input to NVENC
- motion-estimation-only mode
- dynamic encoder reconfiguration during an active recording session
- broad recording architecture cleanup outside the direct-input slice

## Core Decisions

### 1. No Mid-Recording Reconfigure Support

Encode configuration should be treated as immutable for the lifetime of one
recording session.

Practical rule:

- codec, preset, tuning, rate control, GOP, and output size are fixed when the
  recording session starts,
- changes made in the UI or control plane take effect on the next recording
  start, not mid-stream,
- if the resolved encoder buffer count would change, the direct-input ring is
  rebuilt only between recording sessions.

Why:

- direct-input ring size depends on NVENC's resolved buffer count,
- changing that while slots may still be in flight creates avoidable lifecycle
  complexity,
- the user experience for mid-recording reconfigure is not worth the added
  implementation risk in this slice.

### 2. Ring Size Comes From NVENC

The direct-input surface ring should be sized exactly from
`GetEncoderBufferCount()`.

Practical rule:

- do not size the direct-input ring from the current preprocess pool constant,
- do not add speculative extra margin in v1,
- keep upstream raw-frame queue depth separate from encoder-slot count.

Why:

- over-allocating direct-input slots beyond NVENC's actual ring depth does not
  buy throughput in the current wrapper model,
- under-allocating below that depth can create avoidable stalls,
- exact sizing keeps lifecycle reasoning simple for the first implementation.

### 3. Keep Raw-Frame Ownership Separate

`WORKER_ENTRY::ref_count` should remain responsible only for source-frame
ownership across acquisition, preview, YOLO, recording, and any other upstream
consumers.

Practical rule:

- do not extend raw-frame refcounting to cover encode completion,
- model encoder input surfaces as their own slot lifecycle:
  `free -> preprocessing -> submitted -> retired -> free`.

Why:

- raw-frame fan-out and encoder-slot reuse are different problems,
- mixing them would make correctness harder to reason about,
- the current raw-frame refcount is already doing the right job.

### 4. Keep Rollback Cheap

The copy path should stay available behind a simple switch while direct input is
being validated.

Practical rule:

- direct-input mode should be opt-in at first,
- the existing `CopyToDeviceFrame(...)` path remains the fallback until the new
  path passes long-run validation.

## Target Shape

The desired runtime shape for v1 is:

1. Start recording.
2. Create and initialize the encoder.
3. Read `GetEncoderBufferCount()` from the resolved encoder.
4. Allocate exactly that many pitched `NV12` CUDA surfaces.
5. Register those surfaces with NVENC in external-input mode.
6. Push all slot ids into a free-slot queue.
7. Preprocess pops a free slot, writes directly into that surface, and passes
   slot metadata to the hardware worker.
8. Hardware worker submits the corresponding registered surface to NVENC
   without `CopyToDeviceFrame(...)`.
9. The wrapper retires slots only after mapped-resource unmap / output progress.
10. Retired slots return to the free-slot queue.
11. Stop recording, flush, retire all in-flight slots, tear down the ring.

## API And Data Model Changes

### Wrapper

The local NVENC wrapper should gain an external-input mode that:

- skips internal input-surface allocation,
- still allocates normal bitstream output buffers,
- exposes the resolved encoder buffer count,
- accepts externally allocated CUDA surfaces for registration,
- preserves the existing map / encode / unmap behavior.

The wrapper should also expose enough information for the application to know
when a submitted slot is safe to recycle.

### Handoff Struct

`ENCODER_WORKER_ENTRY` should stop being just a generic prepared-frame pointer.

For v1 it should carry:

- `slot_id`
- `surface_ptr`
- `pitch`
- `recording_frame_id`
- `timestamp`
- `timestamp_sys`
- `preprocess_complete_event`

### Slot State

The direct-input ring should use explicit slot identity and slot-state
transitions rather than refcounting.

Minimum useful slot state:

- free
- being filled by preprocess
- submitted to NVENC
- retired and ready to return

## Implementation Sequence

### Phase 1. Wrapper Support

- add external-input mode to the local NVENC wrapper
- add registration entry points for externally allocated CUDA `NV12` surfaces
- expose `GetEncoderBufferCount()` as the authoritative ring-size input for the
  app layer
- expose or derive a clean retire point for submitted slots

Deliverable:

- application code can create an encoder that uses external input surfaces
  without allocating its own internal input pool

### Phase 2. Ring Ownership

- replace the arbitrary preprocess prepared-frame pool with a registered
  surface-ring owner
- allocate the ring with real pitched CUDA surfaces
- register exactly one ring sized from `GetEncoderBufferCount()`
- initialize free-slot bookkeeping

Deliverable:

- preprocess can request a free registered surface by slot id

### Phase 3. Preprocess Write Path

- change `EncoderPreprocessWorker` so it writes into the selected registered
  surface directly
- carry slot id, pointer, pitch, timestamps, and preprocess event to the
  hardware worker
- keep raw-frame release behavior unchanged

Deliverable:

- preprocess output lands directly in an NVENC-registered surface

### Phase 4. Hardware Submit Path

- remove `CopyToDeviceFrame(...)` from the direct-input branch
- submit the slot's registered surface to NVENC
- keep copy-path fallback available
- recycle slots only after the wrapper retire point, not immediately after
  `EncodeFrame()`

Deliverable:

- direct-input mode performs encode submission without an extra staging copy

### Phase 5. Validation

- confirm no slot reuse before retire
- confirm stop / drain / flush behavior
- confirm mono path still works if routed through the same direct-input path
- compare current copy path vs direct-input path on representative cases

Deliverable:

- enough evidence to decide whether direct input should become the default

## Files Likely To Change

- `src/NvEncoder/NvEncoder.h`
- `src/NvEncoder/NvEncoder.cpp`
- `src/NvEncoder/NvEncoderCuda.h`
- `src/NvEncoder/NvEncoderCuda.cpp`
- `src/encoder_pipeline.h`
- `src/encoder_preprocess_worker.h`
- `src/encoder_preprocess_worker.cpp`
- `src/encoder_hw_worker.h`
- `src/encoder_hw_worker.cpp`
- `src/modern_recording_pipeline.cpp`

Expected non-changes for v1:

- `FFmpegWriter`
- legacy `GPUVideoEncoder`
- encode quality policy and metadata format

## Risks

### Early Slot Reuse

If a slot returns to preprocess before NVENC has actually retired it, the next
frame can overwrite encoder input that is still in use.

### Wrapper Lifecycle Drift

If the app-level slot bookkeeping does not match the wrapper's actual
map/unmap progress, the direct-input path will be brittle even if it appears to
work in short runs.

### Pitch Or Geometry Mismatch

If preprocess, registration, and submission disagree about surface pitch or
shape, output can be subtly corrupted.

## Success Criteria

V1 is successful when all of the following are true:

- direct-input mode works for the modern recording path without corruption
- copy-path fallback remains available
- no active-recording reconfigure support is needed
- raw-frame refcounting remains unchanged
- the direct-input ring is sized from `GetEncoderBufferCount()`
- we can measure whether removing `CopyToDeviceFrame(...)` buys meaningful
  throughput headroom

