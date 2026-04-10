# NVENC Direct Input V1 TODO

Date: 2026-04-08
Scope: implementation checklist for the first direct registered NV12 input
slice in the modern recording path.

See also:

- `docs/nvenc_direct_input_v1_plan.md`
- `docs/nvenc_direct_input_plan.md`

Status note:

- Checked items below reflect implementation landed in code and a successful
  build.
- Runtime confidence still depends on the validation section at the end of this
  file.

## Rules For This Slice

- [x] Support only the modern full-frame recording path.
- [x] Support only `NV12` direct input.
- [x] Do not support mid-recording encoder reconfiguration.
- [x] Keep `WORKER_ENTRY::ref_count` unchanged for raw-frame lifetime.
- [x] Keep the current copy path available as fallback during bring-up.

## Support Policy

- [x] Keep the copy path during direct-input validation as:
  - a known-good fallback,
  - a debugging baseline,
  - and a benchmark reference path.
- [ ] Do not treat copy path and direct-input path as equal long-term peer
  architectures once direct-input is validated.
- [ ] Promote direct-input to the preferred path for validated operating points.
- [ ] Keep copy path afterward as:
  - explicit fallback,
  - explicit debug mode,
  - and benchmark comparison mode.

## Phase 1. Wrapper

- [x] Add external-input mode to the local NVENC wrapper.
- [x] Skip internal input-surface allocation when external-input mode is active.
- [x] Keep normal bitstream output buffer allocation intact.
- [x] Add an application-facing way to register externally allocated CUDA input
  surfaces.
- [x] Expose the resolved encoder buffer count cleanly to the application.
- [x] Expose or derive a clean slot-retire point after mapped-resource unmap /
  output progress.

## Phase 2. Ring Setup

- [x] Remove dependence on the arbitrary preprocess prepared-frame pool size for
  direct-input mode.
- [x] Create the direct-input ring using exactly `GetEncoderBufferCount()`
  slots.
- [x] Allocate each direct-input slot as a pitched CUDA `NV12` surface.
- [x] Register every slot with NVENC once before direct-input encode
  submission.
- [x] Initialize a free-slot queue containing all slot ids.

## Phase 3. Data Model

- [x] Extend `ENCODER_WORKER_ENTRY` to carry `slot_id`.
- [x] Extend `ENCODER_WORKER_ENTRY` to carry the selected surface pointer.
- [x] Extend `ENCODER_WORKER_ENTRY` to carry surface pitch.
- [x] Keep timestamps and preprocess completion event in the handoff struct.
- [x] Add explicit app-level slot state or equivalent bookkeeping:
  `free`, `preprocessing`, `submitted`, `retired`.

## Phase 4. Preprocess Worker

- [x] Pop a free slot id before starting preprocess work in direct-input mode.
- [x] Write preprocess output directly into that slot's registered surface.
- [x] Record preprocess completion on the existing handoff event.
- [x] Release the upstream `WORKER_ENTRY` exactly as today after preprocess
  submission.
- [x] Push slot metadata to `EncoderHwWorker`.

## Phase 5. Hardware Worker

- [x] Wait on the preprocess completion event before encode submission.
- [x] Submit the direct-input slot to NVENC without `CopyToDeviceFrame(...)`.
- [x] Keep the existing copy path in a separate fallback branch.
- [x] Retire direct-input slots only after the wrapper retire point.
- [x] Return retired slots to the free-slot queue.
- [x] Preserve stop / drain / flush correctness.

## Phase 6. Control Plane

- [x] Treat encode settings as immutable for one recording session.
- [x] Apply changed encode settings only on the next recording start.
- [x] Reject or ignore any attempt to rebuild the direct-input ring while
  recording is active.
- [x] Rebuild the ring only between recording sessions.

## Phase 7. Telemetry

- [x] Log ring size used for direct-input mode.
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
- [ ] Run the same `orange_client --mode local --experiment-spec ...` matrix
  twice:
  - once with default copy path,
  - once with `ORANGE_NVENC_DIRECT_INPUT=1`.
- [ ] Keep pre-encoder reference capture out of the first direct-input
  throughput comparison until direct-input capture parity is implemented.
- [ ] Record and compare:
  - `enc_fps_mean`
  - `enc_fps_p95`
  - `pre_waits_final`
  - `pre_drops_final`
  - `enc_fail_final`
  - `enc_slow_final`
  - `pre_buffers_min`
  - `pre_events_min`
  - `nvenc_direct_input`

## Definition Of Done

- [ ] Direct-input mode works in the modern path.
- [x] No extra `CopyToDeviceFrame(...)` is used in the direct-input branch.
- [x] Raw-frame refcounting behavior is unchanged.
- [x] Ring size is driven by `GetEncoderBufferCount()`.
- [x] Mid-recording reconfigure is not supported.
- [x] Copy-path fallback remains available.
- [ ] Direct-input is the preferred path for validated runs.
- [ ] Copy path remains available only as fallback / debug / benchmark mode.
- [ ] We have enough measurements to decide whether direct input should be kept
  and broadened.
