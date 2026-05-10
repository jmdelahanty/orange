# Pose IPC to Citrus TODO

Date: 2026-02-25
Scope: deliver pose outputs from `orange-jeremy` to `citrus` over SHM IPC with clear contracts and without regressing existing frame/YOLO IPC consumers.

## Short Answer

Updated 2026-04-21: do not extend the existing Citrus live-control queue with
delayed keyed pose or YOLO updates until Citrus has an explicit versioned event
contract.

Updated 2026-05-10: design the versioned path as a separate
`/shm_cam_<serial>_v2` live-state queue, not as an in-place mutation of the
current queue. The v2 queue should still enforce stale-update suppression:
pose results for older frames are recorded in Orange JSONL but not published to
Citrus as current live state.

The prior primary plan was to extend the existing IPC update payload in the same
queue. That is only safe if the payload represents complete latest live state
with monotonic ids. It is not safe for delayed older-frame semantic updates
because Citrus currently drains the queue and keeps latest state, without keyed
merge semantics.

See [yolo_ipc_citrus_contract_plan.md](./yolo_ipc_citrus_contract_plan.md).
See also [shaman_v2_live_state_contract.md](./shaman_v2_live_state_contract.md).

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

## Audit Update (2026-03-16)

- The shared-memory object shape can already carry pose-like data (`Object.kps[]` and `num_kps`), so the storage substrate is available.
- Current producer behavior is still detection-only:
  - `FrameIPCManager` models base frame + detection update flows only,
  - no pose-specific update helper or merge policy exists,
  - current YOLO result production in the repo does not populate keypoints into IPC payloads.
- No pose worker lifecycle, per-camera pose enable control, or Citrus-side pose consumer landed yet.
- The target rule "recording state should not be required for pose IPC emission" is still future-state only.

## Recommendation

Use the existing queue only for live latest-state data:

- Keep Citrus-facing messages monotonic.
- Treat each message as a complete current state Citrus may use now.
- Suppress or separately log delayed older-frame pose/YOLO results instead of
  emitting them into `/shm_cam_<serial>`.

Why:
- this matches current Citrus behavior,
- it avoids regressing live stimulus state with old updates,
- it keeps Citrus H5 as a consumer-side record of what Citrus saw/used.

Fallback if this proves too coupled:

- Orange-owned recording/audit artifact for delayed semantic history,
- separate v2 live-state queue per camera with a new consumer contract,
- explicit `schema_version`, `payload_kind`, `sequence_id`, `state_frame_id`,
  `source_frame_id`, and status fields in the Shaman v2 slot struct
  coordinated with Citrus.

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

## Phase 1: Contract Update

- [x] Update the contract to distinguish Citrus live latest-state IPC from
      Orange recording/audit event history.
- [ ] For the current `/shm_cam_<serial>` queue, require monotonic live-control
      state. Do not allow delayed older-frame pose updates.
- [x] For delayed pose history, define Orange-owned audit artifacts as the
      source of truth. The v2 Shaman queue remains live-state only.
- [x] Draft the separate Shaman v2 live-state contract:
      [shaman_v2_live_state_contract.md](./shaman_v2_live_state_contract.md).
- [ ] Record this in `agent_contracts/orange_jeremy_ipc_contract.md` only after
      Citrus has agreed to the consumer behavior.

## Phase 2: Pose Payload Schema

- [ ] Define pose payload fields per frame:
  - `frame_id`, `camera_id`,
  - optional `recording_frame_id` mirror semantics,
  - pose list: bbox/ref + keypoints + per-point/instance confidence.
- [ ] For the current queue, avoid adding delayed pose updates. If pose is ever
      packed into current `shaman::Object`, it must obey the same latest-state
      stale-update rule.
- [x] For v2, define fixed keypoint structs instead of packing triples into the
      current `kps[]` float array.
- [x] Decide coordinate convention for v2: source-frame camera pixels for boxes
      and keypoints; Citrus owns homography/application-space transforms.
- [x] Add the first Orange-side v2 queue/publisher implementation in
      `src/shaman_v2.h` and `src/shaman_v2_live_state.h`, with unit coverage in
      `tools/shaman_v2_tests.cpp`.
- [x] Add opt-in Orange runtime v2 queue creation and base/YOLO live-state
      publishing through `FrameIPCManager` behind
      `ORANGE_SHAMAN_V2_LIVE_STATE=1`.

## Phase 3: Producer Wiring

- [ ] Emit Citrus live pose IPC only when the payload is complete current state
      and preserves monotonic `frame_id` behavior.
- [ ] Use the same live-state frame identity policy as existing IPC
      (`ipc_frame_id` / recording-aware behavior) only for Citrus-facing state.
- [ ] Do not emit a late pose result for an older `frame_id` into
      `/shm_cam_<serial>`.
- [ ] Add an Orange audit path for late pose results if pose history matters.
- [ ] Keep pose IPC best-effort/non-blocking:
  - bounded queues,
  - drop oldest on overflow,
  - explicit drop counters.

## Phase 4: Citrus Consumer

- [ ] If pose remains live latest-state only, extend Citrus to parse pose fields
      without changing its latest-state execution model.
- [ ] If Citrus must receive delayed pose events, add a separate versioned event
      ingestion path with `payload_kind`, `sequence_id`, and keyed merge policy.
- [ ] Handle missing/late pose updates gracefully without regressing live state.

## Phase 5: Observability and Validation

- [ ] Add producer counters:
  - pose events sent,
  - queue drops,
  - stale drops,
  - push failures.
- [ ] Add integration test:
  - stream with frame IPC + YOLO + pose enabled,
  - verify Citrus receives monotonic live pose state,
  - verify delayed older-frame pose events are not published to the live queue.
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
- YOLO live IPC update: sent only when it is safe for the Citrus latest-state
  queue.
- Pose worker: runs only when pose is explicitly enabled for that camera.
- Pose live IPC data: carried by live latest-state payload only when monotonic
  and safe for Citrus.
- Recording state should not be required for pose computation, but full delayed
  pose history belongs in Orange recording/audit artifacts unless Citrus gains a
  versioned event-log consumer.
