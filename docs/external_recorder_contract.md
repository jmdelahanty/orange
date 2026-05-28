# External Recorder Contract

## Conceptual Model

The external recorder splits recording across two local Linux processes:

```text
orange_client analytics process -> external_recorder_ipc_probe recorder process
```

The analytics process owns camera acquisition, YOLO, crop/pose decisions, and
the first GPU buffer that contains each camera frame. The recorder process owns
the full-frame encode path and MP4 output.

### Socket

The socket path looks like a file:

```text
/tmp/orange_external_recorder_2010096.sock
```

It is not a normal file, and Orange does not write one frame file to disk for
every camera frame. The `.sock` path is a local address that lets the two
processes find each other. Linux keeps the live socket state and message buffers
in kernel-managed memory.

A useful mental model is:

```text
.sock path on filesystem = address/name
kernel socket buffers = live local messages in system RAM
GPU memory = actual frame pixels
```

For each frame, Orange sends a small descriptor through the socket. The
descriptor says things like:

```text
camera_serial = 2010096
recording_frame_id = 123
source_gpu_id = 5
width = 4512
height = 4512
timestamp = ...
cuda_ipc_handle = ...
gop_index = 4
assigned_shard_id = 0
assigned_gpu_id = 5
```

The actual image pixels are not sent through the socket. They remain in GPU
memory. The `cuda_ipc_handle` is a temporary cross-process handle that lets the
recorder import the source GPU allocation.

There are two source-lifetime modes:

1. Detached-copy mode:
   - Orange acquires a frame into GPU memory.
   - Orange sends a descriptor over the Unix domain socket.
   - The recorder imports the CUDA IPC handle.
   - The recorder copies the pixels into recorder-owned GPU memory.
   - The recorder sends an ACK back over the socket.
   - Orange can safely recycle the source frame buffer.
   - The recorder later encodes the detached copy.
2. Direct-input deferred-release mode:
   - Orange acquires a frame into GPU memory.
   - Orange sends a descriptor over the Unix domain socket.
   - The recorder imports the CUDA IPC handle and accepts the frame.
   - The recorder sends `ACK ... deferred_release`.
   - Orange keeps the source frame buffer leased.
   - The recorder sends `RELEASE` after the source frame has been consumed by
     the recorder-side encode path.
   - Orange can safely recycle the source frame buffer only after `RELEASE`.

The ACK matters because Orange must know whether the recorder accepted the
frame. In detached-copy mode, ACK is also the source-safe boundary. In
deferred-release mode, ACK is not source-safe; `RELEASE` is the source-safe
boundary. Orange must not reuse a source frame buffer before the relevant
source-safe boundary for that mode.

This socket is different from a WebSocket:

| Type | Used For | Address Looks Like |
|---|---|---|
| TCP socket | Network communication | `192.168.1.10:8080` |
| WebSocket | Browser/server messages over HTTP(S) | `ws://host/path` |
| Unix domain socket | Local process-to-process communication | `/tmp/orange_external_recorder_2010096.sock` |

Orange's external recorder uses a Unix domain socket. It is local to one
machine, does not use HTTP, does not use a network port, and is not something a
browser connects to.

### Shard GPUs

A shard is one external recorder encode lane, usually bound to one GPU/NVENC
device. Shards exist because one A16 encoder cannot reliably carry a full
`4512x4512 @ 100 fps` camera stream alone.

Example two-camera topology:

```text
Camera 2010095 -> recorder shards on GPUs 5,6
Camera 2010096 -> recorder shards on GPUs 7,8
```

With `routing_policy = "gop_modulo"`, whole GOPs are assigned by:

```text
assigned_shard_id = gop_index % shard_count
```

For two shards:

```text
GOP 0 -> shard 0 -> GPU 5
GOP 1 -> shard 1 -> GPU 6
GOP 2 -> shard 0 -> GPU 5
GOP 3 -> shard 1 -> GPU 6
```

Each shard can write diagnostic per-shard MP4/CSV outputs. In multi-shard mode,
a coordinator also writes the merged GOP-ordered MP4 for the camera.

### Contract Stream

A contract stream is the per-camera declaration of what the external recorder is
expected to do and produce. It is not the live socket traffic. It is durable
metadata used by the experiment and verifier.

For camera `2010095`, the stream says:

- which camera/stream is being recorded
- which analytics GPU owns the source frame path
- which recorder GPU or shard GPUs should encode
- which routing policy should be used
- where the recorder summary should be written
- where the recorder live status/heartbeat sidecar should be written
- where the final MP4 should be written
- where video sanity and GOP routing sidecars should be written

The verifier uses the stream contract to ask concrete questions:

- Did the recorder summary exist?
- Did it belong to the expected camera/stream?
- Did it use the expected shard GPUs?
- Did it route by the expected policy?
- Did it ACK the frames Orange submitted?
- Did it encode without drops or worker failures?
- Did the merged MP4 exist when multi-shard output was expected?
- Did the MP4 pass video sanity?
- Did the GOP routing CSV have one row per received frame?
- For rolling sessions, did the recorder write multiple GOP-boundary clip MP4s
  with continuous `recording_frame_id` coverage?

This contract covers the current diagnostic external recorder path:

```json
"fixed": {
  "recording_sink_mode": "external_ipc",
  "external_recorder_contract": {
    "schema_id": "orange.external_recorder.contract",
    "schema_version": 1,
    "mode": "diagnostic_ipc_v1",
    "supervise_processes": false,
    "artifact_root": "/tmp/orange_external_recorder_ptp_...",
    "session_id": "experiment_id",
    "require_summary": true,
    "require_video_sanity": true,
    "require_merged_mp4": true,
    "require_gop_routing": true,
    "require_status": true,
    "require_status_runtime": false,
    "recording_control": {
      "record_for_seconds": 0,
      "clip_seconds": 0
    },
    "rollover": {
      "requested": false,
      "status": "not_requested",
      "implementation": "none",
      "seamless_writer_switch": false
    },
    "streams": {
      "2010095": {
        "stream_id": "2010095",
        "camera_serial": "2010095",
        "analytics_gpu_id": 5,
        "recorder_gpu_id": 5,
        "expected_shard_gpu_ids": [5, 6],
        "routing_policy": "gop_modulo",
        "summary_json": "/tmp/.../Cam2010095_external_summary.json",
        "status_json": "/tmp/.../Cam2010095_external_status.json",
        "video_sanity_json": "/tmp/.../Cam2010095_external_video_sanity.json",
        "mp4": "/tmp/.../Cam2010095_external.mp4",
        "gop_routing_csv": "/tmp/.../Cam2010095_external_gop_routing.csv",
        "recording_control": {
          "record_for_seconds": 0,
          "clip_seconds": 0
        },
        "encode_fps": 100,
        "encode_max_fps": 0,
        "encode_queue_depth": 32,
        "prewarm_slots": 4,
        "prewarm_bytes": 20358144,
        "prewarm_peer_copy": true,
        "codec": "hevc",
        "preset": "p1",
        "tuning": "ll",
        "gop": 25,
        "bitrate_bps": 150000000,
        "max_bitrate_bps": 150000000,
        "vbv_buffer_size": 150000000
      }
    }
  }
}
```

Current semantics:

- `mode = "diagnostic_ipc_v1"` means `external_recorder_ipc_probe`, not the
  future production recorder supervisor.
- `supervise_processes = false` preserves the smoke-runner behavior where a
  shell script launches the recorder. Set it to `true` for specs where
  `orange_client` should own recorder startup, socket readiness, shutdown, and
  finalization checks.
- Orange parses this block, fails fast on malformed specs, preserves it in
  `experiment_spec.json`, and exposes per-camera expected artifact paths in
  `runs.json` / `runs.csv`.
- `status_json` is a recorder-owned live status sidecar with
  `schema_id = "orange.external_recorder.status"`. The diagnostic recorder
  rewrites it while listening, after connection, during recording, during
  finalization, and at completion/failure. It is process/sync telemetry and not
  part of the frame ACK protocol. Supervised Orange runs parse this sidecar
  into `external_recorder_supervisor_runtime.json` under
  `processes[].recorder_status`.
- `require_status = true` asks
  `scripts/verify_external_recorder_session.py` to fail if the status sidecar
  is missing, unfinished, unhealthy, or inconsistent with the recorder summary.
  `require_status_runtime = true` additionally requires supervised runtime
  parsing in `external_recorder_supervisor_runtime.json`; use it only for
  Orange-supervised runs, not shell-launched recorder processes.
- Shell-launched diagnostic runs validate external recorder files after Orange
  exits through `scripts/verify_external_recorder_session.py`.
- Supervised headless and GUI/session runs finalize the recorder lifecycle from
  Orange, then use recorder summaries to write shared recording-session
  metadata. Supervised headless also records verifier/finalization status in
  the external-recorder lifecycle artifacts.
- `require_merged_mp4` only applies to multi-shard runs. Single-shard runs use
  the shard output as the final MP4.
- `expected_shard_gpu_ids` is ordered by shard id. For GOP modulo routing,
  shard id is `gop_index % shard_count`.
- `encode_queue_depth`, `prewarm_*`, codec, GOP, and bitrate fields are
  optional launch-plan fields. If a stream omits them, the dry-run supervisor
  plan tool fills in production-like defaults.
- `recording_control` is copied from the Orange session/spec intent so external
  consumers can distinguish continuous, timed, and rolling sessions. If a
  headless spec places the control at `fixed.recording_control`, the supervisor
  and verifier treat it as the contract-level default unless a stream overrides
  it.
- `rollover.requested = true` is supported for the supervised diagnostic IPC
  recorder when `clip_seconds > 0`. The recorder owns GOP-boundary writer
  rotation and reports
  `rollover.implementation = "external_recorder_gop_boundary_writer_rotation"`.
  It writes the merged session MP4 plus per-clip outputs under
  `clips/clip_%06d/`.
- After supervised external recorder finalization, Orange mirrors the external
  clip list into the analytics `recording_session.json` using the shared
  `orange.recording_session` contract. The manifest records
  `recording_backend.mode = "external_ipc"` and per-camera clip video,
  metadata, keyframe paths, frame ranges, and packet counts under
  `clips[].camera_artifacts`.
- The analytics folder also gets `recording_clip_index.json` and
  `recording_clip_index.csv` with one row per external `(clip, camera)` range,
  plus `recording_snapshot.json` pointers to the shared manifest and index
  files.
- External IPC packet counts are sourced from per-clip
  `packets_written` in the recorder summaries and are exposed as
  `packet_count_source = "external_recorder_summary.packets_written"`.
- Rolling summaries also report `target_frame_count`,
  `terminal_tail_coalesce_frames`, and `terminal_tail_coalesced_frames`.
  When timed stop lands just after a nominal clip boundary, the recorder
  coalesces a tiny terminal tail into the previous final clip instead of
  creating a standalone 1-frame clip. The current threshold is one GOP; larger
  overruns still create additional clips and should be treated as diagnostic
  evidence that stop/drain timing missed the intended boundary.
- Rolling clip boundaries are aligned upward to whole GOPs. For example,
  `clip_seconds = 2`, `encode_fps = 100`, and `gop = 25` produce 200-frame
  clip spans.
- Full-rate `4512x4512 @ 100 fps` rolling external IPC should use split-GOP
  shard routing. A single same-GPU external encoder lane can drop frames at
  this rate; the validated smoke uses `expected_shard_gpu_ids = [5, 6]` and
  `routing_policy = "gop_modulo"`.
- GUI/session external IPC has a first supervised lifecycle slice: record start
  launches recorder processes, finalization drains handoff queues, stops the
  recorders, reads summaries, and writes an `orange_gui_external_ipc`
  single-clip `recording_session.json`.

## Supervisor Plan Dry Run

`external_recorder_supervisor_plan` converts a contract into the exact recorder
argv arrays that a future supervisor should launch. It does not start sockets,
camera acquisition, TensorRT, or NVENC. It is a contract/CLI check for the
control-plane shape.

Single-camera plan check:

```bash
./targets/release/external_recorder_supervisor_plan \
  --spec experiment_specs/2010096_external_recorder_supervisor_plan_smoke.json \
  --check
```

Two-camera PTP multi-shard plan check:

```bash
./targets/release/external_recorder_supervisor_plan \
  --spec experiment_specs/2010095_2010096_external_recorder_supervisor_plan_ptp.json \
  --check
```

Use `--print-json` to inspect the generated
`orange.external_recorder.supervisor_plan` artifact. Each stream includes:

- `socket_path`
- expected shard GPU ids and routing policy
- recorder artifact paths
- recorder launch parameters
- `command.argv` for `external_recorder_ipc_probe`

The default supervisor-plan CLI remains metadata only. The process-launch and
shutdown/drain path is available as an opt-in headless slice through
`supervise_processes = true`. When enabled, `orange_client` writes:

- `external_recorder_session.json`
- `external_recorder_supervisor_plan.json`
- `external_recorder_supervisor_runtime.json`
- `external_recorder_verifier_handoff.json`
- `external_recorder_finalization.json`

Each recorder process can also write its stream-level live status sidecar,
normally named `Cam<serial>_external_status.json`.

The provisional `external_recorder_session.json`,
`external_recorder_supervisor_plan.json`,
`external_recorder_supervisor_runtime.json`,
`external_recorder_verifier_handoff.json`, and
`external_recorder_finalization.json` artifact shapes are written or built
through `src/external_recorder_contract_utils.*`, the same helper used by the
GUI external IPC path. The headless parser also uses the shared
contract extraction rule, so a wrapper object with `external_recorder_contract`
resolves the same way in both paths.

Headless supervised process startup/shutdown now goes through
`src/external_recorder_lifecycle.*`. That helper builds the supervisor plan,
writes the initial session/plan artifacts, starts recorder processes, exports
the per-camera socket/session environment variables, stops the recorder
processes, and publishes runtime plus verifier-handoff artifacts. GUI/session
external IPC also uses this shared lifecycle entry point for its first
supervised recorder slice.

Latest validated supervised headless contract:

- One-camera smoke:
  `/tmp/orange_external_recorder_supervised_2010096_20260507_215347`.
- Two-camera PTP/split-GOP smoke:
  `/tmp/orange_external_recorder_supervised_ptp_20260507_222657`.
- The two-camera run wrote all lifecycle artifacts, launched one recorder
  process per stream, used `gop_modulo` routing across GPUs `5,6` and `7,8`,
  encoded/ACKed `400/400` frames per camera, and passed finalization plus MP4
  video sanity for both streams.

The runtime summary records PIDs, socket readiness, log paths, exit status, and
whether termination was requested.
The verifier handoff records the artifact root, the expected analytics root,
and the `verify_external_recorder_session.py` command to run after
`runs.json` and any required video sanity file are present.
The finalization artifact records the post-run checks that `orange_client`
actually ran. When `require_video_sanity = true`, `orange_client` first runs
`scripts/external_video_sanity.py` for every stream MP4, then runs
`scripts/verify_external_recorder_session.py`. Any failure marks the headless
run failed with the verifier or decode-sanity reason in `runs.json`.

GUI/session status:

- The GUI can recognize `recording.sink_mode = "external_ipc"` from app config
  or `ORANGE_GUI_RECORDING_SINK_MODE=external_ipc`.
- GUI app config `recording.recording_control` and the
  `ORANGE_GUI_RECORD_FOR_SECONDS` / `ORANGE_GUI_CLIP_SECONDS` env overrides are
  applied to the materialized full-frame contract. `clip_seconds > 0` requests
  rolling external recorder clips and requires `record_for_seconds > 0`.
- On record start, the GUI uses `src/external_recorder_contract_utils.*` to
  materialize the same contract shape into the proposed recording folder as
  `external_recorder_contract.json`, starts supervised diagnostic recorder
  processes, and writes `external_recorder_supervisor_plan.json`.
- That shared helper expands `{recording_folder}` / `{recording_id}` path
  templates, fills selected-camera stream defaults from camera config and
  split-GOP shard assignments.
- On finalization, the GUI closes IPC connections, stops the supervised
  recorder lifecycle, reads external recorder summary JSON for each selected
  camera, and writes a shared `recording_session.json` with
  `producer = "orange_gui_external_ipc"` and
  `recording_backend.mode = "external_ipc"`. For rolling contracts, the GUI
  mirrors recorder `rolling_output.clips[]` into a `rolling_clips` manifest
  and session clip indexes.
- Recorder child-process, socket state, and parsed `status_json` heartbeat
  state are visible in the GUI. The status line shows heartbeat coverage plus
  recorder-side received/encoded frame totals when sidecars are present.
- First GUI/session hardware validation passed on 2026-05-21:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_21_12_39_24`. Both cameras
  recorded `1645` submitted/ACKed/encoded frames with no external IPC
  failures, no ACK timeouts, no frame gaps/GetFrame errors, and valid decoded
  external MP4s. `scripts/validate_gui_ptp_recording.py --latest-complete`
  now follows `recording_session.json` external video paths for this layout.
- Richer protocol-level health messages remain follow-up work. GUI-side PTP
  stack preflight exists in the validation launcher/wrapper path.

CLI lifecycle smoke without cameras:

```bash
./targets/release/external_recorder_supervisor_plan \
  --spec experiment_specs/2010096_external_recorder_supervisor_plan_smoke.json \
  --start-and-stop
```

In normal hardware use, `--start-and-stop` should target
`external_recorder_ipc_probe`. Unit tests use a socket stub helper so this
lifecycle path can be tested without CUDA/NVENC.

Recorder summary schema:

```json
{
  "schema_id": "orange.external_recorder.summary",
  "schema_version": 1,
  "tool": "external_recorder_ipc_probe",
  "session_id": "experiment_id",
  "stream_id": "2010095",
  "routing_policy": "gop_modulo",
  "shard_count": 2,
  "direct_input_source": false,
  "deferred_source_release": false,
  "frames_received": 100,
  "acks_sent": 100,
  "encode_enqueued": 100,
  "encode_skipped": 0,
  "encode_dropped": 0,
  "encode_queue_depth": 32,
  "encode_queue_high_water": 4,
  "frames_encoded": 100,
  "worker_failed": false,
  "external_encode": {
    "frames_dropped": 0,
    "enqueue_age_p95_ms": 0.25,
    "source_releases_sent": 0,
    "source_release_failures": 0
  },
  "external_encode_shards": [],
  "merged_output": {},
  "outputs": {}
}
```

Verifier checks:

- analytics `runs.json` rows pass and report `recording_sink_mode =
  "external_ipc"`
- external IPC failures and ACK timeouts are zero
- ACKed frames are at least submitted frames
- recorder summary schema and stream identity match the contract
- `acks_sent == frames_received`
- `encode_enqueued + encode_skipped + encode_dropped == frames_received`
- `detach_copied == encode_enqueued` for the current verifier contract; use
  `direct_input_source` and `deferred_source_release` to interpret whether that
  count represents a detached recorder-owned copy or direct-input acceptance
- optional queue-pressure gates can assert `encode_queue_depth`, maximum
  `encode_queue_high_water`, and maximum `external_encode.enqueue_age_p95_ms`
  via `--expect-encode-queue-depth`,
  `--max-encode-queue-high-water`, and `--max-enqueue-age-p95-ms`
- no encode drops and no worker failures
- expected shard GPU ids and routing policy match
- merged output is finalized for multi-shard runs
- MP4 exists, has a valid video stream, and passes video sanity when required
- GOP routing CSV has one data row per received frame
- for rolling runs, analytics `recording_session.json` is `mode =
  "rolling_clips"`, uses `producer = "orange_headless_external_ipc"` or
  `producer = "orange_gui_external_ipc"`, records `recording_backend.mode =
  "external_ipc"`, and its per-camera clip artifacts match the verified
  external summaries
- for rolling runs, analytics `recording_clip_index.json`,
  `recording_clip_index.csv`, and `recording_snapshot.json` index pointers
  match the verified external summaries
- for deferred-release diagnostics, `source_release_failures` should be `0`
  and `source_releases_sent` should match the accepted frame count

Smoke runners:

- `scripts/run_external_recorder_smoke.sh`
- `scripts/run_external_recorder_two_camera_ptp_smoke.sh`

Both runners now generate `external_recorder_session.json`, inject the same
contract into the temporary experiment spec, run video sanity, and then run
`scripts/verify_external_recorder_session.py`.
