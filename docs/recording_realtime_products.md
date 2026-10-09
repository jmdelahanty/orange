# Realtime products: detections, pose and the model identity chain

Status 2026-10-09. The realtime-products slice declares, per camera, what
the live detector and pose network produced during a recording and exactly
which models produced it, so Palette can ingest the JSONL logs beside the
media without guessing.

## Item 1: `realtime_products` in `recording_session.json` (de773b1)

`build_realtime_products_json` (`src/session/realtime_products.cpp`) is
called by both session manifest builders. Per camera serial:

- `detections` and `pose`: `status` present/absent, `files`
  (role, path, size, sha256 of the JSONL/CSV logs), `line_schema`,
  `frame_identity_key`, row counts by kind and status, first/last
  recording frame id, `model_ref` (`models_key`, enabled, model_id,
  engine_path, engine_sha256, weights_sha256) and `pose_crop_size_px`.
- `crop_files` and `acquisition_files` for the per-camera sidecars.

Schema: `docs/schemas/orange_recording_realtime_products_v1.schema.json`
(unchanged by item 2; `model_ref` reads the digests the snapshot now
carries).

## Item 2: model identity in the snapshot (this change)

`models.<serial>.detect.runtime` and `models.<serial>.pose.runtime` in
`recording_snapshot_start.json` / `recording_snapshot.json` now carry the
full chain weights -> ONNX -> engine:

| Key | Source |
| --- | --- |
| `engine_sha256` | SHA-256 Orange computes over the engine bytes that ran (record-start path, cached per path) |
| `engine_bytes` | size of that file |
| `weights_sha256`, `onnx_sha256` | `source.weights.sha256` / `source.onnx.sha256` from the engine's Palette build manifest |
| `engine_manifest` | the manifest `<stem>.manifest.json` beside the engine (`foo.engine` -> `foo.manifest.json`) (schema_id `orange.tensorrt_engine_manifest`): path, sha256, schema, status, task, `run_id` (Palette training run), `set_id`, `build_id`, precision, hardware class, `engine_sha256_declared` and `engine_sha256_matches` (declared == computed), plus the onnx / weights / canonical-manifest / onnx-manifest digest refs; `present=false` when missing, `error` when unparsable |
| pose only: `onnx_sha256_matches_skeleton_sidecar` | whether the skeleton sidecar's `source_onnx_sha256` names the same ONNX export (null when either side is absent) |

Implementation: `src/model_identity.{h,cpp}` (`resolve_model_identity`,
`engine_manifest_json`), used by `build_gui_detect_model_snapshot` and
`build_gui_pose_model_snapshot` in `src/gui/recording_snapshots.cpp`, so the
GUI and the headless runner write the same block. Unit tests:
`model_identity_tests`, `recording_snapshots_tests`.

Schemas: `docs/schemas/orange_recording_detect_model_v1.schema.json` (new;
the detect block had no schema before) and
`docs/schemas/orange_recording_pose_model_v2.schema.json` (v1 was closed on
`runtime`, so the new keys needed a version; v1 stays for recordings before
2026-10-09).

What Palette gets: a recording names the engine that ran by content, the
ONNX export Palette registered and the training weights, and says whether
the engine on disk still matched its manifest at record start. Palette can
join `engine_manifest.run_id` / `onnx_sha256` to its registry instead of
parsing engine file names.

Production engines at the time of writing (both manifests present,
`engine_sha256_matches` true):

- detect `..._int8mm_bo5_avg32.engine`: engine
  `479e82d2bbd127703437765b36ebcf33d6095c03224531453334128539f5d007`,
  onnx `d9bbeebd09ef7a30bc15759b0a149e0de51508735b5c32d9abc247514d950536`,
  weights `74458dc7a91b93d70edd7974617b8ad920d30ee78084a56161bcad57dd62ac58`,
  run `detect_all_available_detect_training_v004_yolo11n_trt_20260520`.
- pose `pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915_a16_gpu5_trt100_fp16_bo5_avg32.engine`:
  engine `ee404b245620ccdcc4297a65513e4bf64e87d288af1a6fa9ca479a8a8ad0e944`,
  onnx `db5c8c3305b71c52f147152507dd8cd890d42396162adf74b1b8afbafe84f12f`,
  weights `2edf67ad1fd97b5ac3835c2423a8d54775402c7d97a17c2a99a45e9e11d7b191`,
  run `pose_head_192_recovered_reviewed_v001_yolo11n_100e_20260915`.

## Item 3: v2 line formats (2026-10-09)

The detector and pose JSONL logs now lead with a `session_header` line and
carry slim, rounded frame lines; `realtime_products.<serial>.<product>.line_schema`
says version 2 and `header_rows` counts the header (and any
`spatial_mask_policy`) lines. The full description is in
`docs/yolo_event_log_jsonl_contract.md` ("Version 2"); schemas
`docs/schemas/orange_yolo_event_v2.schema.json` and
`docs/schemas/orange_pose_event_v2.schema.json`. The header's model block
carries the same digests as the snapshot (item 2), so a log file is
bindable to its model without the snapshot.

## Next items

7-8. A rig session with Citrus (Shadow run, unified H5, pose on, one fish
   per dish).
