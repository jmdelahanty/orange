# One configuration surface for the validated production shape

Date: 2026-09-21. Status: proposal, pending Jeremy's decisions (section 5).
Source investigations: journal 2026-09-21 18:40 (fused-graph wiring) and the
component inventory of the same evening (this file's tables condense it).

## 1. What the headless gate enables that the GUI does not

The 30-minute four-camera fish gate (`experiment_specs/..._od8ff_s16_endurance30.json`)
runs, per camera: INT8 min-max detect engine; device ROI + device crop + fused
per-slot graph (preprocess, detect, ROI, crop, pose, pool copy); real TensorRT
pose (192 px head, `fish_v1`, 8 device slots with per-slot graphs, 3 prewarm
iterations); YOLO warmup of 3 iterations (which is where the fused graphs are
captured); external full-frame split-GOP recorder with native NV12 input on the
landing die, owner push with per-card serialization and the deadline gate, 16
staging slots, `extra_output_delay 8` on full-frame shards only; external crop
recorder with GOP-parity interleave on the paired die; PTP gate with register
reads decimated 1/100; paced page-cache writeback on the host.

The GUI today gets the recorder shape and the engine from the app config, but
its analytics path is the pre-fused one. Root cause (verified): the pose
worker's device stage is enabled only by `PoseWorker::EnableDeviceStage`, whose
single caller is the headless client (`src/orange_headless_client.cpp:4928`);
the GUI also never calls `YoloWorker::SetPoseWorker` nor `YoloWorker::Warmup`.
The fix is one ordered sequence at `src/gui/camera_startup_controller.cpp:874`
(background construction thread, before any worker thread starts):

1. `device_crop_requested = ORANGE_ANALYTICS_DEVICE_CROP`; refuse when
   `ORANGE_ANALYTICS_DEVICE_ROI` is off or the camera is colour.
2. `pose_crop_px` = `ORANGE_POSE_CROP_SIZE_PX` (must equal the pose engine
   input) else the video crop size.
3. `fanout = !pose->EnableDeviceStage(pose_crop_px, &err)` (log and keep the
   fan-out path on failure; never throw).
4. `crop_producer->SetPoseWorker(pose, fanout)`.
5. `if (pose->device_stage_enabled()) yolo->SetPoseWorker(pose)`.
6. `if (pose->device_stage_enabled()) yolo->Warmup(prewarm_iterations)` in a
   try/catch; `prewarm_iterations` from config (3 in the gate).

Fan-out and the YOLO attach must derive from the same `EnableDeviceStage`
result, otherwise pose receives only flush ticks (what happened on 2026-09-21
18:07). Verification: `[PoseWorker] Device crop path ... enabled ... slots=8
graphs=8/8`, `[YOLO] fused frame graphs captured ... slots=8`, `[YOLO] Warmed
... iterations=3` before acquisition; at shutdown `fused frame summary ready=1
frames=N`; `device_crop = 5` in `Cam*_yolo_perf.csv`.

## 2. Inventory: where each component is configured today

Legend: spec = headless experiment spec key; env = what the runtime reads; app
= `~/orange_data/config/app/default.json`; cam = per-camera JSON under
`~/orange_data/config/local/<rig>/`.

| Component | spec | env read by runtime | app / cam today | gate value |
|---|---|---|---|---|
| detect engine | `yolo_worker.engine_path` | `ORANGE_DEFAULT_DETECT_ENGINE` (GUI) | app `models.default_detect_engine` | INT8 min-max |
| YOLO prewarm (fused capture) | `yolo_worker.prewarm_iterations` | none, direct `Warmup()` call | none; GUI never calls | 3 |
| YOLO decimate | `yolo_worker.decimate` | `ORANGE_YOLO_DECIMATE` | none | 1 |
| YOLO affinity / RT priority | none | `ORANGE_YOLO_AFFINITY_CAM_<serial>`, `_RT_PRIORITY_CAM_<serial>` | none (launcher env) | 6/8/10/12 |
| sync event, GPU timing, detach input, ready-event fastpath | spec / defaults | `ORANGE_YOLO_SYNC_EVENT`, `_GPU_TIMING`, `_DETACH_INPUT`, `_READY_EVENT_FASTPATH` | none | all on (defaults) |
| early owned frame | `analytics_early_owned_frame` | `ORANGE_ANALYTICS_EARLY_OWNED_FRAME` | none (default on) | on |
| device ROI / device crop / fused frame / copy after pose | `analytics_*` | `ORANGE_ANALYTICS_*` | app `analytics.*` (parsed, exported; not in live file) | on |
| copy stream | `analytics_copy_stream` | `ORANGE_ANALYTICS_COPY_STREAM` | none | off |
| pool NV12 layout | derived from registered source | `ORANGE_POOL_NV12_LAYOUT` | none | on |
| pose engine / mode / skeleton / crop size | `pose_worker.*` | `ORANGE_POSE_ENGINE_PATH`, `_MODE`, `_SKELETON_ID`, `_CROP_SIZE_PX` | app `models.pose_*` (parsed, exported) | 192 head, real, fish_v1, 192 |
| pose device stage | implied by device crop | none: `EnableDeviceStage` call | none; GUI never calls | on |
| pose prewarm, device graph, device slots, queue depth | `pose_worker.*` / defaults | `ORANGE_POSE_PREWARM_ITERATIONS`, `_DEVICE_GRAPH`, `_DEVICE_SLOTS`; queue direct | none (GUI hardcodes queue 32) | 3, on, 8, 32 |
| crop size | `crop_recording.crop_size_px` | none | cam `crop_pipeline.crop_size_px` | 384 |
| crop sink mode / queue depth / frame pool | `crop_recording.*` | `ORANGE_CROP_RECORDING_SINK_MODE`, `_EXTERNAL_ENCODE_QUEUE_DEPTH`, `ORANGE_CROP_FRAME_POOL_SIZE` | app `recording.crop.*` | external_ipc, 128, 32 |
| crop recorder GPU per camera | `crop_recording.recorder_gpus` | `ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_<serial>` | app `recording.crop.external_ipc.recorder_gpu_ids_by_serial` | 4/2/8/6 |
| crop GOP-parity interleave | `crop_recording.interleave` | `ORANGE_CROP_EXTERNAL_INTERLEAVE` | none (GUI default off) | on |
| full-frame sink mode | `recording_sink_mode` | struct | app `recording.sink_mode` | external_ipc |
| recorder tool (CUDA 13 native) | contract `recorder_tool_path` | `ORANGE_EXTERNAL_RECORDER_FULL_FRAME_TOOL` | app `recording.external_ipc.recorder_tool_path` | native binary |
| native local input / kernel PTX | `external_recorder_native_*` | `_FULL_FRAME_NATIVE_LOCAL_INPUT`, `_FULL_FRAME_NATIVE_KERNEL_PTX` (recorder binary reads the un-prefixed names) | app `recording.external_ipc.*` | on, copy mode |
| owner push, slots | `external_recorder_owner_push[_slots]` | `ORANGE_EXTERNAL_RECORDER_OWNER_PUSH[_SLOTS]` | app `recording.external_ipc.*` | on, 16 |
| owner push chunk / deadline / allowance / max age | chunk only | `_OWNER_PUSH_CHUNK_BYTES`, `_DEADLINE_MS`, `_TRANSFER_ALLOWANCE_MS`, `_MAX_AGE_MS` | none | 64 MiB, 10, 3.6, 8 |
| full-frame extra output delay | `external_recorder_full_frame_extra_output_delay` | `_FULL_FRAME_EXTRA_OUTPUT_DELAY` | app `recording.external_ipc.*` | 8 |
| registered source, detect priority, split submit, direct input, deferred caps | `external_recorder_*` | `ORANGE_EXTERNAL_RECORDER_*` | none | on, on, off, off, defaults |
| per-stream encode params + shard GPUs | contract streams | none | app `recording.external_recorder_contract` (opaque blob) | hevc p1 ll gop 25, 150 Mbps, shards [3,4] [1,2] [7,8] [5,6] |
| MP4 writeback pacing (in-process sink) | none | `ORANGE_MP4_WRITEBACK_PACE_BYTES` | none | 32 MiB default |
| sync mode, PTP | `sync_mode` | none | cam `sync_mode`, `ptp.*` | ptp_gate |
| PTP register read decimate | `ptp_register_read_decimate` | `ORANGE_PTP_REGISTER_READ_DECIMATE` | app `recording.ptp_register_read_decimate` | 100 |
| PTP latch after fanout | `ptp_latch_after_fanout` | `ORANGE_PTP_LATCH_AFTER_FANOUT` | none (default on) | on |
| ring release log, cadence probe all, force direct read | `acq_*` | `ORANGE_ACQ_*` | none | diagnostics |
| acquisition buffer mode | `acquisition_buffer_mode` | struct | not parsed from cam cfg | auto |
| record_for_seconds, clip_seconds | `recording_control.*` | `ORANGE_GUI_RECORD_FOR_SECONDS`, `_CLIP_SECONDS` | app `recording.recording_control.*` | per run |
| host writeback sysctl, isolcpus, PTP stack | none | none | `config/sysctl/90-orange-writeback.conf`, kernel cmdline, `scripts/ptp_stack.sh` | installed |

Traps recorded by the inventory: `set_gui_env_from_app_config_if_absent`
ignores empty strings and never overwrites, so `false` must be exported as the
literal `"0"`; the un-prefixed `ORANGE_EXTERNAL_RECORDER_NATIVE_LOCAL_INPUT` and
`ORANGE_EXTERNAL_RECORDER_EXTRA_OUTPUT_DELAY` reach every supervised child and
break the crop recorders; `ORANGE_POSE_CROP_SIZE_PX` is cached in a function
static (`src/yolo_worker.cpp:1260`), so it is rig-level unless that static is
removed; `ORANGE_CROP_FRAME_POOL_SIZE` has two config owners today.

## 3. Proposed layout

Precedence everywhere: environment (validation one-offs) > camera config (for
per-camera keys) > app config (per rig) > code default. The env tier is gated
by app config `precedence.allow_env_overrides` (false on the production rig;
the validation wrapper sets it), restricted to names that map to a config key
through the same export table, printed as one block at startup
(`analytics.yolo.prewarm_iterations = 5 (env ORANGE_YOLO_PREWARM_ITERATIONS,
overrides app 3)`), and recorded per run under `config_resolution` in
`recording_snapshot.json`. A value seen at the env tier in more than one
snapshot gets a config key. Decisions D1-D3 agreed 2026-09-21 (single rig, so
no legacy fallbacks: the app-config serial map and the launcher's per-camera
env go away in slice 3). The GUI exports the
resolved values as the env the workers read, in `src/orange.cpp` in the block
that already does this (after `load_app_storage_config`, before any worker or
the autorun config resolve).

### Per rig, `~/orange_data/config/app/default.json`

```jsonc
"models": {
  "default_detect_engine": ".../..._int8mm_bo5_avg32.engine",
  "pose_engine": ".../pose_head_192_..._fp16_bo5_avg32.engine",
  "pose_mode": "real", "pose_skeleton_id": "fish_v1", "pose_crop_size_px": 192
},
"analytics": {
  "device_roi": true, "device_crop": true, "fused_frame": false,  // GUI: fused on loses card-A frames (2026-09-21 bisect)
  "copy_after_pose": true, "early_owned_frame": true, "copy_stream": false,
  "yolo": { "prewarm_iterations": 3, "decimate": 1,
            "sync_event": true, "gpu_timing": true,
            "detach_input": true, "ready_event_fastpath": true },
  "pose": { "device_stage": true, "prewarm_iterations": 3,
            "device_graph": true, "device_slots": 8, "queue_depth": 32 }
},
"recording": {
  "sink_mode": "external_ipc",
  "external_ipc": {
    "recorder_tool_path": "/opt/orange/bin/external_recorder_ipc_probe_native",
    "native_local_input": true, "native_kernel_ptx": "",
    "owner_push": true, "owner_push_slots": 16, "owner_push_chunk_bytes": 67108864,
    "owner_push_deadline_ms": 10.0, "owner_push_transfer_allowance_ms": 3.6,
    "owner_push_max_age_ms": 8.0,
    "full_frame_extra_output_delay": 8,
    "registered_source": true, "detect_priority": true,
    "split_submit": false, "direct_input": false,
    "encode": { "codec": "hevc", "preset": "p1", "tuning": "ll", "gop": 25,
                "bitrate_bps": 150000000, "fps": 100, "queue_depth": 32,
                "prewarm_slots": 4 }
  },
  "crop": { "sink_mode": "external_ipc", "frame_pool_size": 32,
            "external_ipc": { "encode_queue_depth": 128, "interleave": true } },
  "ptp_register_read_decimate": 100,
  "acquisition": { "ptp_latch_after_fanout": true, "ring_release_log": false,
                   "cadence_probe_all": false }
},
"host": { "writeback_sysctl": "config/sysctl/90-orange-writeback.conf",
          "expect_kernel_cmdline": ["isolcpus", "nohz_full", "tsc=reliable", "iommu=pt"] }
```

### Per camera, `~/orange_data/config/local/<rig>/<serial>.json` (schema 4 -> 5)

```jsonc
"crop_pipeline": { "crop_size_px": 384, "preview_max_fps": 10 },   // exists
"sync_mode": "ptp_gate", "ptp": { "enabled": true, "mode": "TwoStep" },  // exists
"source_gpu_id": 5,                                                 // exists
"analytics": { "yolo_affinity_cpus": "12", "yolo_rt_priority": 0 }, // new
"acquisition": { "buffer_mode": "auto" },                            // new
"recording": {
  "shard_gpu_ids": [5, 6],          // new: full-frame split-GOP shards (landing die first)
  "crop_recorder_gpu_id": 6         // new: crop recorder die (today app-config map by serial)
}
```

The GUI materializes the full-frame external recorder contract from these
(analytics GPU = `source_gpu_id`, shards = `recording.shard_gpu_ids`, encode
params from `recording.external_ipc.encode`), the way it already materializes
the crop contract (`src/session/recording_session.cpp:751`), instead of
carrying an opaque hand-written contract blob in the app config. The headless
client keeps its spec keys; a spec that omits a key falls back to the same
camera/app config resolution, so both paths read one truth.

### Per run (spec or session UI)

Duration, warmup, record_for_seconds, clip_seconds, output roots, session id,
matrix sweeps, diagnostic toggles, and any deliberate one-off override.

## 4. Implementation slices (each buildable and testable alone)

1. GUI fused-path wiring (section 1) + `analytics.yolo.prewarm_iterations`,
   `analytics.pose.*` keys, exports for `ORANGE_POSE_PREWARM_ITERATIONS`,
   `ORANGE_ANALYTICS_EARLY_OWNED_FRAME` (as "0"/"1"), `ORANGE_POSE_DEVICE_GRAPH`,
   `ORANGE_POSE_DEVICE_SLOTS`; pose queue depth from config. Tests:
   `tools/app_storage_config_tests.cpp` (no `analytics` coverage today),
   `tools/gui_autorun_tests.cpp`. Gate: 60 s GUI run shows `device_crop = 5`,
   pose events present, then the drop-attribution run.
2. Remaining rig-level keys: owner-push timing, registered source, detect
   priority, split submit, crop interleave, acquisition diagnostics, MP4
   pacing; all exported with explicit "0"/"1".
3. Camera-config schema 5: `analytics.yolo_affinity_cpus`, `acquisition.buffer_mode`,
   `recording.shard_gpu_ids`, `recording.crop_recorder_gpu_id`; parser in
   `src/camera_config_schema.h` / `src/project.cpp`; tests in
   `tools/camera_config_validation_tests.cpp` and
   `tools/external_crop_recorder_config_tests.cpp`; the GUI launcher stops
   exporting per-camera affinity and crop GPU env.
4. Contract materialization from camera config for the full-frame recorder
   (largest slice); `recording.external_recorder_contract` becomes an optional
   override; `tools/recording_session_phased_start_tests.cpp` and
   `tools/external_recorder_supervisor_tests.cpp` extended.
5. Headless client reads the same camera/app config keys as fallbacks so the
   experiment specs can shrink to per-run keys.

## 5. Decisions needed

- D1: per-camera keys in the camera config as proposed (affinity, buffer mode,
  shard GPUs, crop recorder GPU), or keep them all in the app config keyed by
  serial? Proposal: camera config, they are properties of the die/NIC/core the
  camera is bound to.
- D2: materialize the full-frame recorder contract from config (slice 4) or keep
  the opaque contract blob in the app config for now? Proposal: materialize,
  but as the last slice.
- D3: precedence env > camera > app > default, with env reserved for validation
  one-offs? Proposal: yes, and the GUI logs every value it resolved and where it
  came from, in the recording snapshot.
- D4: order of work: slice 1 first (it unblocks the GUI gate), then 2 and 3 in
  parallel, then 4 and 5.
