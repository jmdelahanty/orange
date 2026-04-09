# NVENC Direct Input V1 TODO

Date: 2026-04-08
Scope: implementation checklist for the first direct registered NV12 input
slice in the modern recording path.

See also:

- `docs/nvenc_direct_input_v1_plan.md`
- `docs/nvenc_direct_input_plan.md`

## Rules For This Slice

- [ ] Support only the modern full-frame recording path.
- [ ] Support only `NV12` direct input.
- [ ] Do not support mid-recording encoder reconfiguration.
- [ ] Keep `WORKER_ENTRY::ref_count` unchanged for raw-frame lifetime.
- [ ] Keep the current copy path available as fallback during bring-up.

## Phase 1. Wrapper

- [ ] Add external-input mode to the local NVENC wrapper.
- [ ] Skip internal input-surface allocation when external-input mode is active.
- [ ] Keep normal bitstream output buffer allocation intact.
- [ ] Add an application-facing way to register externally allocated CUDA input
  surfaces.
- [ ] Expose the resolved encoder buffer count cleanly to the application.
- [ ] Expose or derive a clean slot-retire point after mapped-resource unmap /
  output progress.

## Phase 2. Ring Setup

- [ ] Remove dependence on the arbitrary preprocess prepared-frame pool size for
  direct-input mode.
- [ ] Create the direct-input ring using exactly `GetEncoderBufferCount()`
  slots.
- [ ] Allocate each direct-input slot as a pitched CUDA `NV12` surface.
- [ ] Register every slot with NVENC once at recording start.
- [ ] Initialize a free-slot queue containing all slot ids.

## Phase 3. Data Model

- [ ] Extend `ENCODER_WORKER_ENTRY` to carry `slot_id`.
- [ ] Extend `ENCODER_WORKER_ENTRY` to carry the selected surface pointer.
- [ ] Extend `ENCODER_WORKER_ENTRY` to carry surface pitch.
- [ ] Keep timestamps and preprocess completion event in the handoff struct.
- [ ] Add explicit app-level slot state or equivalent bookkeeping:
  `free`, `preprocessing`, `submitted`, `retired`.

## Phase 4. Preprocess Worker

- [ ] Pop a free slot id before starting preprocess work in direct-input mode.
- [ ] Write preprocess output directly into that slot's registered surface.
- [ ] Record preprocess completion on the existing handoff event.
- [ ] Release the upstream `WORKER_ENTRY` exactly as today after preprocess
  submission.
- [ ] Push slot metadata to `EncoderHwWorker`.

## Phase 5. Hardware Worker

- [ ] Wait on the preprocess completion event before encode submission.
- [ ] Submit the direct-input slot to NVENC without `CopyToDeviceFrame(...)`.
- [ ] Keep the existing copy path in a separate fallback branch.
- [ ] Retire direct-input slots only after the wrapper retire point.
- [ ] Return retired slots to the free-slot queue.
- [ ] Preserve stop / drain / flush correctness.

## Phase 6. Control Plane

- [ ] Treat encode settings as immutable for one recording session.
- [ ] Apply changed encode settings only on the next recording start.
- [ ] Reject or ignore any attempt to rebuild the direct-input ring while
  recording is active.
- [ ] Rebuild the ring only between recording sessions.

## Phase 7. Telemetry

- [ ] Log ring size used for direct-input mode.
- [ ] Log free-slot depth trend.
- [ ] Log slot-starvation events.
- [ ] Log encode-retire progress clearly enough to debug stuck slots.
- [ ] Distinguish copy path vs direct-input path in benchmark output.

## Validation

- [ ] Short-run sanity test: recording starts, encodes, and stops cleanly.
- [ ] Long-run stability test: no slot corruption or early reuse.
- [ ] Downsample mode test.
- [ ] Mono-camera test if routed through the same direct-input branch.
- [ ] Drain / stop behavior test.
- [ ] Compare throughput of:
  - current copy path
  - direct registered input path

## Definition Of Done

- [ ] Direct-input mode works in the modern path.
- [ ] No extra `CopyToDeviceFrame(...)` is used in the direct-input branch.
- [ ] Raw-frame refcounting behavior is unchanged.
- [ ] Ring size is driven by `GetEncoderBufferCount()`.
- [ ] Mid-recording reconfigure is not supported.
- [ ] Copy-path fallback remains available.
- [ ] We have enough measurements to decide whether direct input should be kept
  and broadened.
