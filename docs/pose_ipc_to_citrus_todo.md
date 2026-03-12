# Pose IPC to Citrus TODO

Date: 2026-02-25
Scope: deliver pose outputs from `orange-jeremy` to `citrus` over SHM IPC with clear contracts and without regressing existing frame/YOLO IPC consumers.

## Short Answer

Primary plan: extend the existing IPC update payload (same queue, same update type),
not a separate third message type.

That means pose is attached to update payload objects (for example keypoints in
`Object.kps/num_kps`) and/or additional optional update fields, keyed by the same
`frame_id`.

## Current IPC Baseline

- Producer emits two event types into one SHM queue today:
  - `sendFrame(...)` base frame event,
  - `updateFrameWithDetections(...)` YOLO update event.
- Refs:
  - `src/frame_ipc_manager.h:43`
  - `src/frame_ipc_manager.h:66`
  - `src/frame_ipc_manager.h:217`
  - `src/frame_ipc_manager.h:233`
- Current payload has no event-type discriminator:
  - `src/shaman.h:45`
- Contract currently documents only base + YOLO update semantics:
  - `../agent_contracts/orange_jeremy_ipc_contract.md`

## Recommendation

Use single-queue update extension first:

- Keep base frame event as-is.
- Keep update event type as-is.
- Extend update semantics so pose can be included in the same per-frame update
  payload (or a second "upsert" update for the same frame id).

Why:
- minimal change to current producer/consumer control flow,
- avoids introducing a new transport channel immediately,
- aligns with existing object payload (`kps`, `num_kps`) already present.

Fallback if this proves too coupled:

- separate pose queue per camera (new channel),
- or explicit `payload_kind` in slot struct (breaking schema).

## TODO Plan

## Phase 0: Operator Opt-In and Worker Lifecycle

- [ ] Add explicit per-camera pose enable control (UI toggle and/or config field).
- [ ] Default pose to OFF unless explicitly enabled by operator/session config.
- [ ] Gate pose worker creation on opt-in:
  - only construct/start pose worker for cameras with pose enabled,
  - do not spawn idle pose workers when pose is disabled.
- [ ] Define runtime toggle behavior while streaming:
  - enable pose => spawn/start worker and begin best-effort pose updates,
  - disable pose => drain/finalize/tear down worker safely and stop new pose updates.
- [ ] Surface pose enabled/active state in runtime status/telemetry.

## Phase 1: Contract Update (Single-Queue Extension)

- [ ] Update contract to define pose as an extension of update payload semantics.
- [ ] Define whether multiple update emissions for same `frame_id` are allowed:
  - recommended: "upsert allowed; latest update for `(camera_id, frame_id)` is authoritative".
- [ ] Record this in `agent_contracts/orange_jeremy_ipc_contract.md` as additive version bump.

## Phase 2: Pose Payload Schema

- [ ] Define pose payload fields per frame:
  - `frame_id`, `camera_id`,
  - optional `recording_frame_id` mirror semantics,
  - pose list: bbox/ref + keypoints + per-point/instance confidence.
- [ ] Define exact encoding in current `shaman::Object`:
  - how pose keypoints map into `kps[]`,
  - how many floats per keypoint,
  - how `num_kps` is interpreted.
- [ ] Confirm keypoint capacity for target model(s) vs `MAX_KEYPOINTS` limits.
  - refs: `src/shaman.h:30`, `src/common.hpp:121`
- [ ] Decide coordinate convention (crop-local vs full-frame) and document.

## Phase 3: Producer Wiring

- [ ] Emit pose IPC when `send_frame_ipc` and `pose` are enabled (not gated by recording state).
- [ ] Use same frame identity policy as existing IPC (`ipc_frame_id` / recording-aware behavior).
  - refs: `src/acquire_frames.cpp:430`, `src/acquire_frames.cpp:441`
- [ ] Add pose update method (or extend existing update path) in `FrameIPCManager`:
  - merge YOLO + pose for same frame when possible,
  - or emit second update for same frame id with enriched payload.
- [ ] Keep pose IPC best-effort/non-blocking:
  - bounded queues,
  - drop oldest on overflow,
  - explicit drop counters.

## Phase 4: Citrus Consumer

- [ ] Extend Citrus update decoder to parse pose fields from update payload.
- [ ] Join pose to frame timeline by `(camera_id, frame_id)` with upsert semantics.
- [ ] Handle missing/late pose updates gracefully (no hard dependency on every frame).

## Phase 5: Observability and Validation

- [ ] Add producer counters:
  - pose events sent,
  - queue drops,
  - stale drops,
  - push failures.
- [ ] Add integration test:
  - stream with frame IPC + YOLO + pose enabled,
  - verify Citrus receives pose and aligns to the intended frame IDs.
- [ ] Add soak test under load with backpressure; verify YOLO/acquisition are not stalled by pose IPC.

## Phase 6: Model and Skeleton Runtime Metadata

- [ ] Define pose config block in per-camera config JSON (operator intent/defaults):
  - engine path/id, skeleton id/path, enable flag.
- [ ] Persist resolved runtime model/skeleton info into recording snapshot under
  `models[<camera_key>].pose`.
- [ ] Ensure Citrus reads model/skeleton interpretation from snapshot metadata
  for each recording run, not from static config files.
- [ ] Include immutable identifiers for reproducibility:
  - `engine_sha256`, `skeleton_sha256`, version/id fields.
- [ ] If model/skeleton changes during a run, emit effective frame boundary info
  so Citrus can interpret downstream data correctly.

## Explicit Gating Rules (target state)

- Base frame IPC: sent when `send_frame_ipc` is enabled.
- YOLO IPC update: sent when YOLO result is available.
- Pose worker: runs only when pose is explicitly enabled for that camera.
- Pose IPC data: carried by update payload when pose is enabled and pose result is available.
- Recording state should not be required for pose IPC emission.
