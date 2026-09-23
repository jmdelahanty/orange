# Shaman V2 Live-State Queue Contract

Date: 2026-08-30
Status: Orange producer and Citrus opt-in authoritative consumer implemented;
grouped-live observation metadata is ABI revision 4;
production default remains v1 pending a four-camera live validation. Orange
creates the v2 queue behind `ORANGE_SHAMAN_V2_LIVE_STATE=1`. Citrus can select
it per arena or, for an autorun validation without editing the canonical
canvas, through `CITRUS_GUI_AUTORUN_SHAMAN_V2_AUTHORITATIVE=1`.

## V1 versus v2

| Property | v1 `/shm_cam_<serial>` | v2 `/shm_cam_<serial>_v2` |
| --- | --- | --- |
| Meaning | One object-vector update | Complete latest tracking-state snapshot |
| ABI | Implicit C++ layout | Magic, schema version, queue/slot byte sizes |
| Empty result | Ambiguous empty vector | Explicit pending, zero, failed, disabled, or not-scheduled status |
| Frame identity | One legacy frame ID | Separate state, source, camera, and recording frame IDs |
| Recording membership | Implicit | Per-slot canonical recording token |
| Time | Orange publish timestamps | Camera timestamp, host timestamp, and Orange publish timestamps |
| Camera | Numeric runtime ID only | Runtime ID, stable serial, and native source extent |
| Payload | Bounding boxes | Bounding boxes, track/flags, pose state, and bounded keypoints |
| Ordering | Consumer interprets vector updates | Explicit `unordered_payload_local` objects with producer stale-update suppression |
| Queue | 8 slots | 64 slots |

V2 remains a best-effort control stream, not a complete scientific event log.
Orange's persisted YOLO and pose records remain the audit authority.

## Decision

Add an opt-in versioned Shaman queue for Citrus live control:

```text
/shm_cam_<camera_serial>_v2
```

Keep the current queue unchanged:

```text
/shm_cam_<camera_serial>
```

The v2 queue is still a Citrus live-control stream, not Orange's complete
semantic event history. Orange recording artifacts remain the source of truth
for complete YOLO and pose history:

- `Cam<serial>_yolo_events.jsonl`
- `Cam<serial>_pose_events.jsonl`

This is not too early to design because the v2 queue can be added beside the
current ABI. It is too early to replace the current queue until Citrus has a
matching opt-in reader.

## Goals

- Preserve monotonic live-state semantics for Citrus stimulus execution.
- Add explicit schema/version fields so Orange and Citrus can negotiate a
  coordinated ABI instead of changing the current slot layout in place.
- Carry camera timestamps, status fields, detections, and pose keypoints in a
  single complete latest-state payload.
- Suppress stale delayed semantic updates before they reach Citrus.
- Keep producer paths best-effort and non-blocking.

## Non-Goals

- Do not guarantee Citrus receives every YOLO or pose result.
- Do not turn Citrus H5 into Orange's complete semantic event log.
- Do not publish delayed older-frame pose or YOLO updates as live state.
- Do not support multiple active readers on the same queue in v1.

## Queue Model

- POSIX shared memory name: `/shm_cam_<camera_serial>_v2`.
- Permissions: same as current Shaman, group `ipc`, mode `0660`.
- Topology: one Orange writer, one Citrus reader.
- Ring behavior: bounded, non-blocking, latest-state oriented.
- Compatibility: current `/shm_cam_<camera_serial>` remains available until all
  consumers explicitly move to v2.
- Writer ownership: exactly one Orange writer holds an advisory SHM file lock;
  a second simultaneous writer is rejected.
- Restart behavior: a new writer clears predecessor slots and advances
  `producer_generation`; every slot carries the generation and a
  `producer_instance_id`.

Restart reset and reader access are synchronized by the shared
`shaman_v2::index_lock_path(queue_name)` sidecar helper. Orange and Citrus must
use that helper byte-for-byte; its safe queue-name prefix is disambiguated by
the standard FNV-1a hash suffix. Readers must treat a changed
`producer_generation` as a new stream and reset sequence/frame gap baselines
before accepting the next slot.

The v2 queue should be unlinked/recreated when the ABI changes. The shared
memory header should carry enough fixed metadata for a reader to reject an
incompatible queue before consuming slots.

If a writer is interrupted after marking an otherwise valid ABI4 queue
`initialized=false`, the next writer waits briefly for an active reset and then
self-heals the queue under the writer/index locks. A queue whose first-time
construction was interrupted before a valid header was installed, or whose
header is from a stale/incompatible ABI, is rejected fail-closed and requires
manual `shm_unlink` before recreation; the writer never guesses how to repair
such a header.

## Live-State Identity

Every visible v2 slot must include:

- `schema_version`: v2 ABI version.
- `slot_bytes`: expected `sizeof(ShamanV2Slot)`.
- `sequence_id`: strictly increasing per queue.
- `state_frame_id`: the live frame id Citrus should treat as current.
- `source_frame_id`: the frame id that produced the semantic content.
- `camera_frame_id`: camera SDK/acquisition frame id when available.
- `recording_frame_id`: recording-local frame id, or `0` when not recording.
- `recording_identity_token`: exact `sha256:<64 lowercase hex>` binding to
  `recording_session.json.session_id`, or empty when not recording.
- `camera_timestamp_ns`: original camera/acquisition timestamp domain.
- `timestamp_sys_ns`: original host `CLOCK_REALTIME` timestamp supplied by
  acquisition.
- `orange_publish_timestamp_us_epoch`: Orange wall-clock publish time.
- `orange_publish_timestamp_us_monotonic`: Orange steady-clock publish time.
- `producer_generation`: monotonically advanced generation for this camera
  queue, incremented whenever the authoritative writer starts.
- `producer_instance_id`: nonzero identity for the particular writer run.

Rules:

- `sequence_id` must strictly increase.
- `state_frame_id` must never move backward.
- `recording_frame_id == 0` requires an empty token; a positive recording
  frame ID requires the token derived from the closed
  `orange.shaman_v2.recording_identity` v1 subject.
- Multiple slots may use the same `state_frame_id` when Orange publishes a base
  state and then a same-frame detection or pose enrichment.
- A semantic update is live-publishable only when its `source_frame_id` matches
  the current `state_frame_id`.
- A semantic update with `source_frame_id < current_state_frame_id` is stale and
  must be suppressed from v2.
- A semantic update with `source_frame_id > current_state_frame_id` may be held
  briefly as pending until the base frame arrives, subject to a bounded pending
  queue.

`recording_frame_id` is never the live-state join key. Pose publication uses
the crop's stream-local `local_frame_id` for both `state_frame_id` and
`source_frame_id`; recording-local numbering remains metadata only.

## Payload Model

The first v2 payload kind should be:

```text
latest_tracking_state
```

Every `latest_tracking_state` slot is a complete replacement state for Citrus.
Citrus does not need to merge old keyed events to understand live state. If it
drains several slots during one update, it can keep the highest valid
`sequence_id` / latest `state_frame_id`.

Recommended status enums:

```text
detection_status:
  disabled
  not_scheduled
  pending
  detections
  zero_detections
  failed

pose_status:
  disabled
  not_requested
  pending
  poses
  no_result
  failed
```

Stale updates should not be represented as published statuses in v2 live slots.
They belong in producer counters and Orange JSONL audit rows.

### Grouped-live detection semantics (revision 4)

The object vector is an unordered set. `object_order` is always
`unordered_payload_local`; payload index is provenance within one observation,
not a track, region, or fish identity. `track_id = -1` means untracked and
must not be interpreted as a stable identity.

Each detection terminal slot carries these counts:

- `source_detection_count`: detector/post-NMS count before the outer spatial
  mask;
- `retained_detection_count`: count after the mask (the post-mask vector);
- `transmitted_object_count`: number copied into `objects[]`;
- `object_count`: v3 compatibility alias for `transmitted_object_count`; and
- `objects_truncated`: number of retained objects omitted by the fixed
  `kMaxObjects` capacity.

If `objects_truncated > 0`, the producer sends the bounded prefix for
diagnostics but changes `detection_status` to `failed` and sets the stable
`detection_reason = objects_truncated`. Consumers must fail closed and must
not treat that prefix as a complete observation.

`detection_reason` values are stable ABI numbers:

| Value | Meaning |
| ---: | --- |
| 0 | none (successful nonzero result) |
| 1 | no source detections |
| 2 | all source detections rejected by the outer mask |
| 3 | objects truncated by SHAMAN capacity |
| 4 | scheduled YOLO worker enqueue rejected |
| 5 | inference timeout |
| 6 | CPU results unavailable/skipped |
| 7 | other processing failure |

Thus `zero_detections` distinguishes an empty detector result from an
all-outside mask result without relying on object count alone. A scheduled
YOLO frame whose worker enqueue is rejected publishes a terminal `failed` slot
with reason 4; this v2-only terminal publication does not change v1 behavior.

`detection_model_id_hash` is the stable 64-bit FNV-1a hash of the detector
model ID used in Orange's event record (the model filename stem); when the
model is unavailable, the literal ID `unknown` is hashed instead. The value is
copied into base, terminal, and later pose-enrichment states.

The FNV-1a offset basis is the standard
`14695981039346656037ULL`. This is part of ABI revision 4: Orange and Citrus
must use the same basis and byte order when comparing model hashes. Pose
publication rejects results with more than 64 objects or more than 32
keypoints per object; it never publishes a bounded prefix as a complete pose
observation.

## Coordinates

All live v2 geometry should be in source-frame camera pixels unless a field
explicitly says otherwise.

- Detection boxes: source-frame pixels.
- Pose keypoints: source-frame pixels.
- Origin: top-left of the camera frame.
- Units: pixels.

The pose worker may decode keypoints in crop-local pixels internally, but the
v2 IPC producer should convert them before publishing:

```text
source_x_px = crop_x_px + crop_local_x_px
source_y_px = crop_y_px + crop_local_y_px
```

Citrus should continue to own homography/application-space transforms.

## Fixed ABI Shape (revision 4)

The implementation uses this fixed C++ ABI shape. `slot_bytes` and
`queue_bytes` are checked exactly, so revision 3 queues fail closed and must be
recreated. On the supported x86_64 toolchain, `sizeof(Object) == 548`,
`sizeof(Slot) == 35368`, and `sizeof(SharedQueue) == 2263640`; both Orange and
Citrus assert these sizes. Numeric enum values below are stable.

```cpp
constexpr uint64_t SHAMAN_V2_MAGIC = 0x4f524e4753484d32ULL; // ORNGSHM2
// Shaman-v2 protocol, ABI revision 4.
constexpr uint32_t SHAMAN_V2_SCHEMA_VERSION = 4;
constexpr uint32_t SHAMAN_V2_QUEUE_SIZE = 64;
constexpr uint32_t SHAMAN_V2_MAX_OBJECTS = 64;
constexpr uint32_t SHAMAN_V2_MAX_KEYPOINTS_PER_OBJECT = 32;

enum class ShamanV2PayloadKind : uint32_t {
    kLatestTrackingState = 1,
    kStreamStatus = 2,
};

enum class ShamanV2DetectionStatus : uint32_t {
    kDisabled = 0,
    kNotScheduled = 1,
    kPending = 2,
    kDetections = 3,
    kZeroDetections = 4,
    kFailed = 5,
};

enum class ShamanV2PoseStatus : uint32_t {
    kDisabled = 0,
    kNotRequested = 1,
    kPending = 2,
    kPoses = 3,
    kNoResult = 4,
    kFailed = 5,
};

struct ShamanV2Keypoint {
    float x_px;
    float y_px;
    float confidence;
    uint16_t label_id;
    uint16_t flags; // visible, interpolated, reserved
};

struct ShamanV2Object {
    float x_px;
    float y_px;
    float width_px;
    float height_px;
    float confidence;
    int32_t label_id;
    int32_t track_id;
    uint32_t flags; // has_bbox, has_pose, synthetic, reserved
    uint32_t keypoint_count;
    ShamanV2Keypoint keypoints[SHAMAN_V2_MAX_KEYPOINTS_PER_OBJECT];
};

struct ShamanV2Slot {
    uint64_t magic;
    uint32_t schema_version;
    uint32_t slot_bytes;

    uint64_t sequence_id;
    uint32_t payload_kind;
    uint32_t flags;

    uint64_t state_frame_id;
    uint64_t source_frame_id;
    uint64_t camera_frame_id;
    uint64_t recording_frame_id;
    uint64_t camera_timestamp_ns;
    uint64_t timestamp_sys_ns;
    uint64_t orange_publish_timestamp_us_epoch;
    uint64_t orange_publish_timestamp_us_monotonic;

    char recording_identity_token[72];

    uint32_t camera_id;
    char camera_serial[32];
    uint32_t source_width_px;
    uint32_t source_height_px;

    uint32_t detection_status;
    uint32_t pose_status;
    uint64_t detection_model_id_hash;
    uint64_t pose_model_id_hash;
    uint64_t pose_skeleton_id_hash;

    uint32_t object_count;
    ShamanV2Object objects[SHAMAN_V2_MAX_OBJECTS];

    // Revision-4 append-only grouped-live metadata.
    uint32_t source_detection_count;
    uint32_t retained_detection_count;
    uint32_t transmitted_object_count;
    uint32_t objects_truncated;
    uint32_t object_order; // 1 = unordered_payload_local
    uint32_t detection_reason;
    uint64_t producer_generation;
    uint64_t producer_instance_id;
};

struct ShamanV2SharedQueue {
    uint64_t magic;
    uint32_t schema_version;
    uint32_t queue_bytes;
    uint32_t slot_bytes;
    uint32_t queue_size;
    std::atomic<bool> initialized;
    std::atomic<uint64_t> writer_sequence;
    std::atomic<size_t> head;
    std::atomic<size_t> tail;
    std::atomic<uint64_t> push_failures;
    std::atomic<uint64_t> stale_suppressed;
    ShamanV2Slot queue[SHAMAN_V2_QUEUE_SIZE];
    std::atomic<uint64_t> producer_generation;
    std::atomic<uint64_t> producer_instance_id;
};
```

String metadata such as model paths, skeleton names, and calibration identifiers
should live in Orange recording/session manifests and Citrus runtime config.
The live queue should carry fixed-size ids/hashes plus enough status to make
stimulus decisions.

If C++ atomics remain in the shared-memory ABI, Orange and Citrus must compile
against the same `shaman_v2.h` definition. A later pure-C ABI is fine, but the
first implementation should prioritize one shared header over duplicating
struct definitions.

## Producer Stale-Update Rule

Orange v2 writer state should track at least:

- `latest_base_state_frame_id`
- `last_published_sequence_id`
- current complete state for `latest_base_state_frame_id`
- bounded pending semantic updates keyed by frame id
- counters for stale suppression and queue/push failures

The shared-memory writer also owns an advisory single-writer lock. On startup
it marks the queue uninitialized, clears all ring slots, advances
`producer_generation`, assigns a new `producer_instance_id`, resets sequence
and queue counters, and then marks the queue initialized. Readers must reset
sequence expectations and any held state when generation changes.

Base frame `N`:

- Publish a `latest_tracking_state` slot for `N`.
- Mark YOLO/pose as `pending`, `not_scheduled`, or `disabled` according to the
  runtime config for that frame.
- Drop any pending semantic updates with source frame `< N`.
- Apply and publish any pending semantic updates with source frame `N`.

YOLO result for frame `K`:

- If `K < latest_base_state_frame_id`, suppress it from v2 and increment
  `yolo_stale_suppressed`.
- If `K == latest_base_state_frame_id`, publish a complete latest-state slot for
  `K` with `detection_status = detections`, `zero_detections`, or `failed`.
- If `K > latest_base_state_frame_id`, hold it in a bounded pending map.
- Carry the raw/source and retained/post-mask counts. Set
  `transmitted_object_count = min(retained_detection_count, kMaxObjects)`.
- If retained count exceeds capacity, publish `failed` with reason
  `objects_truncated`; a consumer must not use the bounded prefix as complete.

Pose result for frame `K`:

- If `K < latest_base_state_frame_id`, suppress it from v2 and increment
  `pose_stale_suppressed`.
- If `K == latest_base_state_frame_id`, publish a complete latest-state slot for
  `K` with `pose_status = poses`, `no_result`, or `failed`.
- If `K > latest_base_state_frame_id`, hold it in a bounded pending map.
- Use the crop's stream-local `local_frame_id` as `K`, even when
  `recording_frame_id` differs.

Pose enrichment must preserve the complete authoritative YOLO object vector.
When a pose object's bounding box matches exactly one current detection, its
keypoints are merged into that object; ambiguous or unmatched boxes do not
replace or shrink the detection vector. If pose arrives before YOLO, its
bounded evidence is held privately: interim slots retain the base
`pending`/`not_scheduled` detection status and zero detection counts/objects.
The evidence is reconciled only after the same-frame YOLO terminal arrives,
which establishes the authoritative detection vector.

The published pose state for frame `K` should include the detection box and
source-frame keypoints for frame `K` when available. It should not publish a
pose from frame `K` as state for frame `K + 1`.

## Backpressure Rule

The v2 queue should never block camera acquisition, YOLO, crop production, or
pose inference.

Because the queue is latest-state oriented, old backlog has little value. The
Orange-side manager should coalesce pending work to the newest complete state it
can safely publish. If the SHM ring is full, Orange should increment
`v2_push_failures` or `v2_queue_drops`, skip that live publish attempt, and let
the next state supersede it. Complete history remains in Orange JSONL artifacts.

An overwrite-oldest SHM ring is acceptable only if the implementation is
explicitly written and tested for one writer plus one reader. The first
implementation can stay simpler and use push failure counters.

## Consumer Rule

Citrus v2 reader should:

- reject incompatible `magic`, `schema_version`, or `slot_bytes`;
- reject or warn on non-increasing `sequence_id`;
- reject or warn on backward `state_frame_id`;
- replace latest state when `state_frame_id` increases;
- replace same-frame state when `state_frame_id` is equal and `sequence_id` is
  newer;
- use explicit detection/pose statuses instead of inferring state from object
  count alone;
- require `object_order = unordered_payload_local` and treat `track_id = -1`
  as untracked;
- require `objects_truncated == 0` before treating a detections slot as a
  complete observation; failed/truncated slots are diagnostic only;
- reset sequence/state expectations and held targets on producer-generation
  change;
- apply homography/transforms from source-frame pixels inside Citrus.

If Citrus drains multiple slots in one update, keeping the newest valid slot is
safe because each slot is complete latest state.

## Observability

Orange should expose per-camera v2 counters:

- `v2_frames_published`
- `v2_yolo_updates_published`
- `v2_pose_updates_published`
- `v2_yolo_stale_suppressed`
- `v2_pose_stale_suppressed`
- `v2_pending_drops`
- `v2_queue_drops`
- `v2_push_failures`

Orange JSONL artifacts should remain complete even when v2 suppresses stale
live updates. A stale v2 suppression is not a lost Orange result; it is a
deliberate live-control safety decision.

Headless `verify_drain_v2` writes these counters into
`frame_ipc_summary.json` beside the existing v1 IPC counters. In that mode,
`ipc_push_failures` means active v2 queue push failures; transitional v1 queue
failures are reported separately as `v1_ipc_push_failures`.

First headless v2 verifier smoke:

- Artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_frame_ipc_verify_v2_stream_only_a16_gpu5_rerun/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25/frame_ipc_summary.json`
- Camera `2010096` published and drained `501` v2 latest-state slots.
- `reader_frame_id_gaps = 0`, `reader_sequence_id_gaps = 0`,
  `v2_ipc_push_failures = 0`.
- `v1_ipc_push_failures` was nonzero because this verifier drains only the v2
  queue; that is reported separately and does not fail v2 validation.

First headless pose-to-v2 verifier smoke:

- Spec:
  `experiment_specs/2010096_headless_real_yolo_pose_noop_synthetic_center_box_v2_ipc_a16_gpu5.json`
- Artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_pose_noop_synthetic_center_box_v2_ipc_rerun2/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25__aq_off__tempaq_off__lookahead_off/frame_ipc_summary.json`
- Camera `2010096` published `410` base v2 states and `282` same-frame pose
  update states; the verifier drained `692` total v2 slots.
- `require_v2_pose_results = true`, `reader_v2_pose_result_messages = 282`,
  `reader_v2_detection_result_messages = 282`, `reader_frame_id_gaps = 0`,
  `reader_sequence_id_gaps = 0`, and `v2_ipc_push_failures = 0`.
- `v2_pose_stale_suppressed = 128`, which means delayed older-frame pose
  results were preserved in `Cam2010096_pose_events.jsonl` but intentionally
  not published as live Citrus state.
- This smoke uses synthetic center-box runtime detections and validates
  pose-to-v2 IPC plumbing only. It is not production YOLO detection, ROI
  selection, or real detection-to-pose validation.

## Implementation Slices

1. [x] Add `src/shaman_v2.h` with fixed ABI structs, constants, and a minimal
   single-writer/single-reader ring wrapper.
2. [x] Add low-level unit tests for ABI size/version checks and ring push/pop.
3. [x] Add an Orange-side `LiveStatePublisher` that enforces monotonic
   same-frame publish, bounded pending updates, and stale YOLO/pose suppression.
4. [x] Add Orange runtime v2 queue creation behind an opt-in switch:
   `ORANGE_SHAMAN_V2_LIVE_STATE=1`.
5. [x] Wire base-frame and YOLO-detection v2 latest-state publishing through
   `FrameIPCManager` while leaving the current queue unchanged.
6. [x] Add runtime stale-suppression tests around `FrameIPCManager` or the
   headless verifier boundary.
7. [x] Add a headless `verify_drain_v2` mode and runtime integration summary.
8. [x] Add pose v2 latest-state publishing from `PoseWorker` results with
   source-frame keypoint conversion.
9. [x] Add Citrus opt-in `ShamanV2IPCReaderModule`; when selected for a SHAMAN
   arena it replaces v1 as the sole authoritative Arena tracking producer.
10. [x] Carry independent stream-state, camera, recording, camera-clock, and
    host-clock identities from Orange acquisition into each base state.
11. [x] Publish explicit terminal YOLO status for detections, zero detections,
    and failures rather than leaving a base state indefinitely pending.
12. [x] Advance the ABI to revision 4 with grouped source/retained/transmitted
    counts, explicit unordered ordering, stable detection reasons, and
    producer restart identity.
13. [x] Publish terminal v2 failure when scheduled YOLO worker enqueue is
    rejected, while preserving v1 behavior.
14. [x] Bind pose state/source identity to stream-local `local_frame_id`, with
    a regression test where it differs from `recording_frame_id`.
15. [ ] Run the controlled four-camera Orange/Citrus live validation, inspect
    queue/identity/status counters and Chaser behavior, then decide whether to
    change the canonical Shadow default.

The orchestrator exposes the paired switch without altering the saved canvas:

```bash
scripts/run_orange_citrus_fourcam_orchestrator.sh \
  --shaman-v2-authoritative
```

Run it first without `--execute` to inspect the plan. For a live validation,
add `--execute` and the normal protocol/recording options. Do not combine this
with Orange's `verify_drain_v2` mode: the queue has one consumer, which must be
Citrus during the integration run.
