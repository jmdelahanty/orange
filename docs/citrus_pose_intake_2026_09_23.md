# Pose intake for Citrus over Shaman v2 (handoff, 2026-09-23)

This is what a Citrus-side agent needs to read pose keypoints from Orange
today, what Orange guarantees, and what is still convention.

## ABI revision (updated 2026-09-23, later)

Orange now publishes Shaman v2 ABI revision 4 (integration commit 54721ba,
cherry-picked from the spatial-roi branch): `kSchemaVersion = 4`, 35368-byte
slots, grouped-live observation provenance, producer restart identity,
detection reasons and counts, and payload validation. The struct layouts
are byte-identical to Citrus's `src/ipc/shaman/shaman_v2.h`. Revision 3
queues from older Orange builds fail closed in a revision-4 reader, which
is the intended behaviour.

## Reader start-up rule (2026-09-23, ABI v4)

The v4 queue keeps an index lock file under `/tmp` (a sticky directory). This
host runs `fs.protected_regular=2`, which forbids even root an `O_CREAT`
open of a regular file another user created there. A reader that starts
before Orange and creates the lock as its own user therefore blocks the root
producer (`shaman v2 index lock open failed ... Permission denied`). Orange's
`shaman_v2.h` now opens the lock without `O_CREAT` on the reader side and
retries until the producer has created it; the producer unlinks a foreign
stale lock once. Citrus's copy of `shaman_v2.h` still creates the lock on
the reader side and should take the same change; until then, start Citrus
after Orange has opened its queues (the normal workflow) and never leave a
Citrus-owned lock behind.

## Transport

- One POSIX shared-memory ring per camera: `/shm_cam_<serial>_v2`
  (`shaman_v2::queue_name_for_camera_serial`). Layout and types are in
  `src/shaman_v2.h` (`SharedLiveStateQueue`, `Slot`, `Object`, `Keypoint`).
- Single producer (Orange), single consumer, destructive pop, 64 slots.
  When nobody drains it the ring fills after 64 slots and every later push
  is rejected (`push_failures` in Orange's `[FRAME_IPC_V2]` exit line). A
  reader that attaches late first drains stale slots. Run exactly one
  reader per camera.
- Orange enables v2 only with `ORANGE_SHAMAN_V2_LIVE_STATE=1` (the Citrus
  orchestrator sets it via `--shaman-v2-authoritative`; the four-camera GUI
  launcher does not). The legacy `/shm_cam_<serial>` v1 queue never carries
  pose.
- Reference reader: `targets/release/shaman_v2_reader_probe --serials
  2010093,2010094 --seconds 30` prints a per-camera summary and one sample
  pose slot as JSON (`tools/shaman_v2_reader_probe.cpp`).

## What a reader sees per frame

Each merge step publishes its own slot with the same `state_frame_id` and
an increasing `sequence_id`: base (detection pending, pose disabled), then
base + YOLO, then base + YOLO + pose. Every slot carries the full state so
far. Take the newest `sequence_id` per `state_frame_id`.

- `state_frame_id` is the absolute per-camera frame id (fixed 2026-09-23:
  pose updates used the recording id before and were dropped as stale
  during recordings). `recording_frame_id` is a mirror (0 when not
  recording); `camera_frame_id` and `camera_timestamp_ns` come from the
  camera.
- Pose is present iff `pose_status == PoseStatus::kPoses (3)` AND
  `objects[i].flags & kObjectHasPose` AND `objects[i].keypoint_count > 0`.
  `kNoResult (4)` means the pose stage ran and found nothing; `kDisabled (0)`
  means no pose stage or no result for this frame yet.
- Under revision 4 a pose result is merged INTO the detection object with
  the same bbox (exact match); it no longer replaces the object list. A pose
  that arrives before its frame's authoritative YOLO update is held as
  pending evidence and merged when the YOLO update lands, so the other
  detections stay in the slot and a later YOLO update cannot strip the
  keypoints. Apply the reader rule per object. `keypoint_count` is 3 with
  the current models, `track_id = -1`.
- Keypoints are in SOURCE (full-frame, 4512x4512) pixels. `Keypoint.flags &
  kKeypointVisible` marks confidence >= 0.25. `label_id` is the keypoint
  index in the model's training order.

## Measured on the ABI v4 build (2026-09-23 04:33, four cameras, 60 s recording, reader draining)

Artifact `2026_09_23_04_33_47`, strict validator PASS, `ORANGE_SHAMAN_V2_LIVE_STATE=1`:
per camera 7172-7173 base frames published, ~7088 YOLO and ~7087 pose updates
published, 81-87 stale-suppressed (all in the first seconds of streaming),
`push_failures = 0`. The reader attached to all four queues and saw 7173
distinct state ids, 0 sequence gaps, pose objects on 7088 of 7090 pose slots
(2 slots per camera carried `pose_status = poses` with no object holding
keypoints yet: the pending-evidence window between a pose result and its
frame's YOLO merge; apply the reader rule per object). Sample slot:
`pose_model_id_hash 17167804398972226780`, `pose_skeleton_id_hash
13995128734323424025`, three keypoints in full-frame pixels, label ids 0-2.

## Measured on the revision-3 build (2026-09-23 00:17, historical)

Artifact `2026_09_23_00_17_04`, strict validator PASS, `ORANGE_SHAMAN_V2_LIVE_STATE=1`,
`targets/release/shaman_v2_reader_probe` attached to all four queues:

| Per camera | value |
| --- | --- |
| frames published (base slots) | 7151-7152 |
| YOLO updates published / stale-suppressed | ~7090 / 59-63 |
| pose updates published / stale-suppressed | ~7090 / 60-65 |
| recording frames in the window (max recording_frame_id) | 6005 |
| reader: distinct state_frame_ids / sequence gaps | 7152 / 0 |
| reader: slots with pose_status poses = objects carrying pose | 7028-7091 (inconsistent 0) |
| reader: pose no_result | 0-64 |

Before the frame-id fix every pose update inside the 6005 recording frames
was stale-suppressed. The remaining ~60 suppressions per camera per kind
are all a startup effect: a full trace (run 2026_09_23_00_54_13) puts
every one of them in the first 3.3 s of streaming (local frames 209-546,
before the recording started at local frame ~644), in bursts where the
YOLO and pose threads lag the acquisition by 1-10 frames while the engines
and graphs warm up. Inside the recording window the stale count is zero.
A reader should expect no pose or YOLO merges for the first few seconds
after a stream starts and treat `pose_status = kDisabled` there as
"not yet", not as an error.

`[FRAME_IPC_V2] ... push_failures` and `queue_drops` are read from the
shared-memory segment header and therefore accumulate across runs (the
segment persists in /dev/shm); compare deltas, or unlink the queues before
a run. `sequence_gaps = 0` on the reader side is the delivery check.

## Skeleton identity (sidecar adopted 2026-09-23)

Orange now loads Palette's sidecar (`palette.pose_model_skeleton` v1) from
app config `models.pose_skeleton_path`, currently the byte-identical copy at
`.../pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915/engines/pose_model_skeleton.json`
(SHA-256 `c238af6f3b35bb19c1b9dfeb5b1d5d9ffe2086f5f1fd1c4ca73e2766f51c3f11`,
source `palette_deployment_20260923_v2/`, read `README.acquisition.md`
there first). At start each pose worker checks the sidecar's K against the
engine output, adopts the exact ordered labels `0:swim_bladder 1:eye_left
2:eye_right` and edges `[0,1] [0,2] [1,2]`, and logs
`skeleton sidecar adopted: ...`. Unsupported schema versions, node/edge
inconsistencies or a K mismatch refuse to start.

Identity in the slots (content hashes, first 8 bytes of the file SHA-256s):

| field | value with the current model | derived from |
| --- | --- | --- |
| `pose_skeleton_id_hash` | 13995128734323424025 | sidecar file SHA-256 `c238af6f...` |
| `pose_model_id_hash` | 17167804398972226780 | engine file SHA-256 `ee404b245620ccdcc4297a65513e4bf64e87d288af1a6fa9ca479a8a8ad0e944` |

The recording snapshot republishes the resolved identity under
`models[<camera>].pose`: `skeleton_id`, `skeleton.labels`, `skeleton.edges`,
`skeleton.kpt_shape`, `skeleton.sidecar_path`, `skeleton.sidecar_sha256`,
`skeleton.ipc_skeleton_id_hash`, `engine_sha256`, `ipc_model_id_hash`. A
reader should join a slot to the sidecar by the 64-bit hash, then verify and
retain the exact sidecar bytes (full SHA-256) rather than treating the
64-bit value as the identity.

Heading: the sidecar's `model_schema_binding.pose_schema.metadata.heading_computation`
(version 1) defines it: direction from `swim_bladder` toward the midpoint of
`eye_left`/`eye_right`, origin at that midpoint. Citrus should implement
heading from that policy keyed on the skeleton identity, by label name, with
confidence and missing-point checks; Orange does not compute heading.

## Skeleton identity before the sidecar (historical)

The model payload does not name its skeleton. The ONNX metadata carries
only `kpt_shape=[3,3]` and `names={0:'fish'}`; the engine manifest's
`keypoints` field is `null`; Orange hard-codes the labels
`{bladder, eye_left, eye_right}` for K=3 (`src/pose_worker.cpp:154-165`;
Palette calls the first one `swim_bladder`). `pose_skeleton_id_hash` is
FNV-1a-64 of the config string (`fish_v1` in the app config) and
`pose_model_id_hash` is FNV-1a-64 of the engine filename stem, so neither
identifies content. Nothing publishes edges. Until 2026-09-23 the pose
worker's FNV offset basis was missing a digit (1469598103934665603), so
slots carried non-standard hashes (fish_v1 -> 9831287610537993535). It now
uses the standard basis 14695981039346656037: `fish_v1 ->
7092773256536156437`, and the current A16 engine stem
`pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915_a16_gpu5_trt100_fp16_bo5_avg32
-> 10804422749911617557`. A reader can recompute both with standard FNV-1a
over the UTF-8 bytes.

For the current K=3 models the training order and edges come from Palette's
`configs/fisheye/pose_schemas/traditional_v1.json`:

| label_id | Palette node | Orange label |
| --- | --- | --- |
| 0 | swim_bladder | bladder |
| 1 | eye_left | eye_left |
| 2 | eye_right | eye_right |

Edges: `[0,1], [0,2], [1,2]`. A Citrus reader should hard-code exactly this
for `pose_skeleton_id_hash == fnv1a64("fish_v1") == 7092773256536156437` and refuse other hashes
until the self-describing path below exists.

## Making skeleton identity self-describing (planned)

1. Palette writes `<stem>.pose_skeleton.json` (skeleton_id, nodes, edges,
   kpt_shape) next to the ONNX/engine and lists its sha256 in the canonical
   manifest, or fills the engine manifest's `keypoints` field.
2. Orange reads it through `ORANGE_POSE_SKELETON_PATH` /
   `models.pose_skeleton_path`, fails at init if the node count differs
   from the engine output, uses its names as labels, and derives
   `pose_skeleton_id_hash` from the file's sha256 and `pose_model_id_hash`
   from the engine sha256.
3. Orange republishes the resolved labels/edges/hashes in
   `recording_snapshot.json` under `models[<cam>].pose` and optionally in
   `/dev/shm/orange_pose_skeletons/<hash>.json`, so a reader can resolve a
   slot hash without static config. No `shaman_v2.h` ABI change.

## Known gaps a reader should tolerate

- Fused/device path ordering: pose can publish before YOLO's CPU update;
  the later YOLO merge then replaces the objects with boxes without
  keypoints while `pose_status` stays `kPoses`. Apply the reader rule per
  object, not per slot.
- Pose replaces the whole YOLO object list with the single ROI.
- During the first ~3 s of streaming, YOLO and pose updates lag the base
  frames by up to 10 frames and are stale-suppressed (`yolo_stale_suppressed`,
  `pose_stale_suppressed`); those frames stay at detection pending. In
  steady state and inside recordings the stale count is zero (the IPC writer
  drains base, YOLO and pose events in arrival order since 27c83c7).
