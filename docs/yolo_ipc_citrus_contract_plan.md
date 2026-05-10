# YOLO IPC and Citrus Contract Plan

Date: 2026-04-21
Scope: clarify how Orange should publish frame and YOLO detection state over
Shaman shared memory when Citrus is the live-control consumer, and separate that
from future complete Orange recording/audit logs.

## Decision

Split the design into two contracts:

1. Citrus live-control contract: latest usable state.
2. Orange recording/audit contract: complete semantic event history.

The current Citrus consumer is a latest-state consumer. Orange must not depend
on the existing Citrus queue to preserve every base-frame, detection-update, or
zero-detection event. It is unsafe for Orange to publish delayed older-frame
YOLO updates into the current Citrus live queue because Citrus can treat that
older update as current state for stimulus execution.

## Citrus Findings

Citrus-side audit summary reported on 2026-04-21:

- `ShamanIPCReaderModule` has its own reader thread and pops one SHM slot per
  loop until the queue is empty.
- `Arena::ProcessTrackingData()` drains all pending reader-thread packets during
  Citrus update and intentionally keeps only the latest effective tracking
  state for stimulus use.
- Citrus does not distinguish Orange base-frame messages from YOLO detection
  update messages. Every popped slot is converted into one
  `ExternalTrackingData` payload with `frame_id` and boxes.
- Citrus currently calls a Shaman `pop` overload that does not read the slot
  `yolo_enabled` field.
- A delayed non-empty detection update for an older `frame_id` can replace
  `m_latest_tracking_data` and regress Citrus's current camera frame id.
- Citrus H5 logging is mixed:
  - non-empty bbox payload rows are logged by the IPC reader thread,
  - empty payload rows are skipped by bounding-box logging,
  - per-stimulus-frame metadata logs only the latest IPC frame id Citrus had at
    output time.
- Citrus cannot distinguish "base frame only", "YOLO has not completed yet",
  "YOLO completed with zero detections", and "YOLO update dropped".

Interpretation: Citrus H5 currently means "what Citrus popped and/or used", not
"the complete semantic event stream Orange produced".

## Current Orange Behavior

Orange currently writes one queue per camera:

- `/shm_cam_<camera_serial>`
- writer: `FrameIPCManager`
- slot layout: `src/shaman.h`

Relevant producer paths:

- `src/acquire_frames.cpp`: sends a base frame IPC slot immediately after frame
  receive and stores `ipc_frame_id` on the work entry.
- `src/yolo_worker.cpp`: sends a detection update only when YOLO produces a
  non-empty detection vector.
- `src/frame_ipc_manager.h`: drains base events and update events through a
  writer thread.

Important current safety behavior:

- Base frame slots are emitted with a monotonic `frame_id` when acquisition is
  healthy.
- Detection updates are only emitted when their `frame_id` matches the latest
  emitted base frame, or when a future pending update later reaches its base
  frame.
- Detection updates older than the latest emitted base frame are counted as
  `update_stale_drops` and are not emitted.

That stale-update behavior is correct for the current Citrus live-control
contract because it prevents old YOLO results from regressing Citrus state.

## Citrus Live-Control Contract

The existing `/shm_cam_<serial>` queue should be treated as a latest-state
stream for Citrus.

Producer rules:

- Each message is a complete tracking state Citrus may use immediately.
- `frame_id` must be monotonic from Citrus's perspective within a run.
- `camera_id` must be stable for the queue.
- Object vectors represent the complete current detection state for that
  message.
- Delayed YOLO results for an older frame must not be emitted with that older
  `frame_id` on this queue.
- If a delayed result cannot be represented as a monotonic complete-state
  message, Orange should suppress it from this queue and record/drop-count it.

Consumer assumptions preserved:

- Citrus may drain many messages and keep only the latest effective state.
- Citrus may use any non-empty popped payload for immediate stimulus logic.
- Citrus does not need to perform frame-id keyed merges for current behavior.
- Citrus H5 remains a consumer-side record of what Citrus saw/used, not a full
  Orange semantic event log.

## Orange Recording/Audit Contract

Orange should become the source of truth for full camera and YOLO semantic
history.

Target audit events:

- base frame received
- YOLO scheduled or skipped
- YOLO completed with detections
- YOLO completed with zero detections
- YOLO failed/timed out
- YOLO update suppressed from Citrus live IPC because it was stale
- frame ids and timestamps needed to join those events to video metadata

This contract should be on disk in Orange recording artifacts first, not
retrofit into the current Citrus live queue. The selected planned v1 artifact is
per-camera JSONL:

```text
<recording_folder>/Cam<serial>_yolo_events.jsonl
```

See [yolo_event_log_jsonl_contract.md](./yolo_event_log_jsonl_contract.md) for
the concrete row schema. The important point is that Orange owns the complete
event history and Citrus owns live stimulus state.

## Future Versioned IPC Contract

If Citrus must eventually receive or log every semantic Orange event, Orange and
Citrus need an explicit versioned IPC model. That can be a live-state queue, a
separate event-log queue, or both.

Updated 2026-05-10: the first v2 queue should be a versioned live-state queue,
not a complete event-log queue. See
[shaman_v2_live_state_contract.md](./shaman_v2_live_state_contract.md). It adds
explicit schema/status/source-frame fields while preserving the stale-update
rule: delayed older-frame YOLO or pose updates are suppressed from Citrus live
state and remain available through Orange audit artifacts.

Candidate additive fields for a versioned live-state queue:

- `schema_version`
- `sequence_id`
- `payload_kind`
- `producer_timestamp_us_epoch`
- `producer_timestamp_us_monotonic`
- `state_frame_id`
- `source_frame_id`
- `detection_status`
- `pose_status`

Candidate `payload_kind` values for the first live-state v2 queue:

- `latest_tracking_state`
- `stream_status`

Historical event-style payloads such as `yolo_detections`,
`yolo_zero_detections`, `yolo_failed`, or `pose_update` should belong to a
separate event-log IPC contract if Citrus later needs every semantic event.
They should not be mixed into the first v2 live-state queue unless they still
obey the complete-state and stale-update rules.

This is a breaking or coordinated schema change. Citrus would need to:

- reject or warn on backward `sequence_id`,
- distinguish complete latest state from any future keyed historical events,
- avoid keyed historical merge behavior for the first v2 live-state queue,
- define stale-update policy explicitly,
- log zero-detection completions intentionally,
- keep live-control state monotonic even when old event-log updates arrive.

## Immediate Orange Plan

1. Document current Citrus contract and the two-contract split.
2. Preserve the current stale-update suppression in `FrameIPCManager` for the
   Citrus live queue.
3. Rename or clarify telemetry so `update_stale_drops` is understood as
   "detection update suppressed from Citrus live IPC", not necessarily a lost
   detection from Orange's internal point of view.
4. Add tests around the live queue rule:
   - base frames `1..N` are emitted,
   - a delayed update for frame `K < N` is suppressed,
   - `update_stale_drops` increments,
   - no non-monotonic payload is visible to a reader.
5. Add or plan an Orange recording-side YOLO event artifact so suppressed live
   IPC updates are still auditable.
6. Before adding pose to this same queue, decide whether pose is live latest
   state, audit event history, or both.

## Implementation Checklist

- [ ] Add a small unit or synthetic integration test for stale YOLO update
      suppression in `FrameIPCManager`.
- [ ] Make headless IPC verifier able to exercise non-empty detection update
      behavior without requiring real YOLO, or add a lower-level SHM test.
- [ ] Add documentation to the GUI/IPC status for `update_stale_drops`.
- [x] Decide the first Orange-owned YOLO event artifact format
      (`jsonl` recommended for long runs).
- [x] Implement `Cam<serial>_yolo_events.jsonl` writer.
- [x] Emit explicit YOLO zero-detection completions into the Orange audit
      artifact.
- [x] Record Citrus live-IPC request status in `yolo_result` rows.
- [ ] Record final Citrus live-IPC publish/suppress decisions in the audit
      artifact.
- [ ] Defer Shaman slot schema changes until Citrus is ready to coordinate a
      versioned `payload_kind` / `sequence_id` contract.

## Non-Goals For The Next Patch

- Do not make Citrus merge async updates by frame id from Orange.
- Do not publish older-frame detection updates into `/shm_cam_<serial>`.
- Do not make Citrus H5 the authoritative complete Orange event log.
- Do not add pose into the current queue as delayed keyed updates without a
  separate contract pass.
