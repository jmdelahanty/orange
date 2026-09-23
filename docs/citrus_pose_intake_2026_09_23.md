# Pose intake for Citrus over Shaman v2 (handoff, 2026-09-23)

This is what a Citrus-side agent needs to read pose keypoints from Orange
today, what Orange guarantees, and what is still convention.

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
- When pose is present the object list is the single selected detection
  (bbox in source pixels), `confidence` is the pose confidence,
  `track_id = -1`, `keypoint_count` is 3 with the current models.
- Keypoints are in SOURCE (full-frame, 4512x4512) pixels. `Keypoint.flags &
  kKeypointVisible` marks confidence >= 0.25. `label_id` is the keypoint
  index in the model's training order.

## Measured on 2026-09-23 (four cameras, 60 s recording, reader draining)

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
was stale-suppressed; now about 0.9 % are, the same rate as YOLO updates
that arrive after the next base frame.

`[FRAME_IPC_V2] ... push_failures` and `queue_drops` are read from the
shared-memory segment header and therefore accumulate across runs (the
segment persists in /dev/shm); compare deltas, or unlink the queues before
a run. `sequence_gaps = 0` on the reader side is the delivery check.

## Skeleton identity (convention today)

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
- About 1 % of YOLO updates arrive after the next base and are
  stale-suppressed (`yolo_stale_suppressed`), leaving those frames at
  detection pending.
