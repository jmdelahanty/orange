# External Recorder Contract

## Conceptual Model

The external recorder splits recording across two local Linux processes:

```text
orange_client analytics process -> external_recorder_ipc_probe recorder process
```

The analytics process owns camera acquisition, YOLO, crop/pose decisions, and
the first GPU buffer that contains each camera frame. The recorder process owns
an external recorder stream's encode path and MP4 output, whether that stream is
full-frame video or crop video.

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
stream_id = 2010096
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

Each supervised recorder stream also has stable identity fields in the contract:

- `camera_serial`: the real source camera serial, such as `2010096`.
- `stream_id`: the logical recorder stream id, such as `2010096` for full-frame
  video or `2010096_crop` for crop video.
- `stream_kind`: the source stream type, currently `full_frame` or `crop`.
- `output_kind`: the recording output kind, currently `full` or `crop`.
- `env_key`: the environment/socket/log namespace key. Crop streams use a
  crop-suffixed key such as `2010096_crop` so their supervised recorder
  environment variables cannot collide with the full-frame recorder for the same
  camera.

Do not encode logical crop identity by changing `camera_serial` to
`2010096_crop`; consumers should use `stream_id`, `stream_kind`, `output_kind`,
and `env_key` for that distinction.

When the Unix socket connects, the recorder now sends a versioned protocol
hello before accepting frames:

```text
RECORDER_HELLO protocol=orange.external_recorder.ipc version=1 frame_rate=100 resolved_gop_length=25 recording_config_fingerprint_scope=frame_rate_and_gop_v1 recording_config_fingerprint=fnv1a64:... ...
CLIENT_HELLO protocol=orange.external_recorder.ipc version=1 frame_rate=100 resolved_gop_length=25 recording_config_fingerprint_scope=frame_rate_and_gop_v1 recording_config_fingerprint=fnv1a64:... ...
RECORDER_STATUS protocol=orange.external_recorder.ipc version=1 ...
CLIENT_CONTROL protocol=orange.external_recorder.ipc version=1 command=drain ...
CLIENT_CONTROL protocol=orange.external_recorder.ipc version=1 command=finalize ...
```

This is intentionally separate from per-frame ACK/RELEASE. It proves the peer
is the expected recorder protocol and that both processes resolved the same
frame rate and GOP before Orange starts handing out CUDA IPC frame descriptors.
The fingerprint scope is deliberately explicit: `frame_rate_and_gop_v1` binds
the two values that define frame grouping; it is not yet a fingerprint of every
NVENC option. Both peers recompute it independently and reject missing or
mismatched values. The recorder also checks every descriptor against the
declared GOP, so a 25/30 disagreement is rejected at frame 26 even if a future
handshake regression allowed it through.

For merged split-GOP output, the descriptor's
`{recording_frame_id, gop_index, frame_index_within_gop}` is registered at
encoder submission. NVENC returns `recording_frame_id` in its output timestamp;
the merger uses it only to retrieve and consume that exact submitted identity.
It does not calculate a second GOP from the timestamp. A missing, duplicate, or
unknown output identity fails the recording, as does finalization with a
submitted frame that never produced a packet. This preserves one grouping
authority even when two encoder shards complete out of order.

The recorder writes the observed handshake into
`ipc_protocol` fields in both `status_json` and summary JSON. After the hello,
the diagnostic recorder can also send low-rate in-band `RECORDER_STATUS`
messages with the same heartbeat sequence and frame counters that are written
to the status sidecar. Full-frame and crop handoff clients tolerate these
messages while waiting for `ACK` / `RELEASE`, so status telemetry no longer
depends only on the JSON sidecar path. The sidecar remains the durable
operator-facing health artifact. On orderly recording-session shutdown, Orange
sends `CLIENT_CONTROL command=drain` when it has stopped accepting new work for
that session, then sends `CLIENT_CONTROL command=finalize` before closing the
socket. These controls give the recorder explicit session boundaries instead of
relying only on EOF. They do not mean the camera stream has stopped: the GUI can
continue streaming and previewing while the recorded video, metadata, bounding
box rows, clip index, and manifests are drained and finalized. Rolling clip
boundaries are internal recorder writer rotations and do not require socket or
camera stream teardown. `drain` is advisory and can precede descriptors that
were already accepted into Orange's local recording queues; `finalize` is the
point where descriptor intake should end. The recorder records the observed
client control state in `ipc_protocol`, including the final
`client_control_state`, drain/finalize message counts, the frame counts at
which drain/finalize were observed, and the descriptor-intake end reason. A
production recording session is clean only when descriptor intake ends via
`client_finalize`; EOF, malformed descriptors, ACK/RELEASE write failures, or
signal stop before finalize are recorded as unclean intake termination.

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

Each shard writes diagnostic CSV/log sidecars. In continuous multi-shard mode,
a coordinator writes the single GOP-ordered MP4 for the camera. Shard MP4
writers are not opened unless the contract explicitly sets
`preserve_shard_mp4s = true` for diagnostics.

In rolling mode, the same coordinator orders GOPs but opens only the active
clip writer. It does not write a monolithic session MP4. The finalized clip
list in `recording_session.json` and `recording_clip_index.json` is the logical
recording.

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
- For continuous multi-shard output, did the merged MP4 exist?
- For rolling output, was the merged MP4 absent and were the clips declared
  authoritative?
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
    "require_storage_preflight": true,
    "require_protocol_hello": true,
    "preserve_shard_mp4s": false,
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
        "stream_kind": "full_frame",
        "output_kind": "full",
        "camera_serial": "2010095",
        "env_key": "2010095",
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
        "recording_config_source": "resolved_recording_config",
        "recording_config_fingerprint_scope": "frame_rate_and_gop_v1",
        "recording_config_fingerprint": "fnv1a64:...",
        "max_pending_gops": 8,
        "max_pending_bytes": 268435456,
        "bitrate_bps": 150000000,
        "max_bitrate_bps": 150000000,
        "vbv_buffer_size": 150000000,
        "min_free_bytes": 0,
        "low_space_warning_bytes": 0
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
- `require_storage_preflight = true` requires `storage_preflight` payloads in
  recorder summary/status JSON and, when runtime status is required, parsed
  runtime storage fields. Current generated full-frame and crop external IPC
  contracts set it by default.
- `require_protocol_hello = true` requires `ipc_protocol` payloads showing
  `recorder_hello_sent = true` and `client_hello_received = true` in recorder
  summary/status JSON and, when runtime status is required, parsed runtime
  protocol fields. When present, `recorder_status_send_failures` must be zero.
  New recorder summaries/status sidecars also report
  `client_control_messages_received`, `client_drain_messages_received`,
  `client_finalize_messages_received`, `client_drain_received`,
  `client_finalize_received`, `client_control_state`,
  `descriptor_intake_end_reason`, and
  `descriptor_intake_completed_cleanly`; when those fields are present, strict
  validators require at least one control message, a received drain command, a
  received finalize command, a final
  `client_control_state = "finalize_requested"`,
  `descriptor_intake_end_reason = "client_finalize"`, and clean descriptor
  intake completion.
  Current generated full-frame and crop external IPC contracts set it by
  default.
- Shell-launched diagnostic runs validate external recorder files after Orange
  exits through `scripts/verify_external_recorder_session.py`.
- Supervised headless and GUI/session runs finalize the recorder lifecycle from
  Orange, then use recorder summaries to write shared recording-session
  metadata. Supervised headless also records verifier/finalization status in
  the external-recorder lifecycle artifacts.
- `require_merged_mp4` applies to continuous multi-shard runs. Rolling
  contracts must set it to `false`; the supervisor rejects the contradictory
  `clip_seconds > 0` plus `require_merged_mp4 = true` shape.
- `preserve_shard_mp4s` defaults to `false`. With that production default,
  shard MP4 writers are never opened when an ordered coordinator owns the
  output. Setting it to `true` explicitly creates and retains diagnostic shard
  MP4s in addition to the authoritative output.
- An explicitly requested raw bitstream path is also diagnostic. Generated
  production supervisor commands do not request one.
- `expected_shard_gpu_ids` is ordered by shard id. For GOP modulo routing,
  shard id is `gop_index % shard_count`.
- `encode_queue_depth`, `prewarm_*`, codec, GOP, and bitrate fields are
  optional launch-plan fields. If a stream omits them, the dry-run supervisor
  plan tool fills in production-like defaults.
- `max_pending_gops`, `max_pending_bytes`, and
  `max_pending_frontier_age_ms` are hard merger reorder limits, not desired
  queue depths. The defaults are 8 GOPs, 256 MiB, and 2000 ms. A healthy
  two-shard stream normally needs only one or two pending GOPs while a later
  shard waits for the earlier release frontier. At GOP 25 and 100 fps, eight
  GOPs represent up to 200 unresolved frames (about two seconds), so reaching
  the cap is evidence of a missing/stalled frontier rather than normal
  buffering. The ninth pending GOP, first byte above 256 MiB, or a pending
  release frontier older than two seconds fails the recorder instead of
  allowing memory to grow with recording duration.
- `max_writer_queue_packets` and `max_writer_queue_bytes` bound each external
  recorder `FFmpegWriter` independently. The defaults are 512 encoded packets
  and 128 MiB for each shard, merged-session, and rolling-clip writer. These
  are safety ceilings, not buffering targets. A push that would cross either
  limit latches overflow and immediately fails the recorder; it is not deferred
  until finalization.
- Live status JSON exposes current and peak merger bytes/GOPs, release-frontier
  age, merged/rolling writer queue occupancy and peaks, plus process `VmRSS` and
  `VmHWM`. Final summary JSON preserves the peaks and process memory snapshot.
- Recorder latency percentiles use a deterministic run-wide reservoir capped at
  65,536 samples per metric. Summary JSON records the observed, retained, and
  capacity counts. This keeps long-run timing telemetry bounded while retaining
  an approximate whole-run p95 rather than silently switching to a recent-only
  window.
- `min_free_bytes` is an optional hard recorder preflight threshold. When it is
  greater than zero, `external_recorder_ipc_probe` checks available bytes on
  the output directories before listening and exits failed if the minimum is
  not met.
- `low_space_warning_bytes` is an optional runtime/status warning threshold.
  When it is greater than zero, recorder status/summary JSON marks
  `storage_preflight.low_space = true` if available bytes fall below it; current
  validators treat reported low-space as a failed validation gate.
- `terminal_tail_coalesce_frames` is optional. If omitted, the recorder uses
  one configured GOP as the terminal-tail coalesce threshold. GUI external crop
  rolling sets this explicitly to the parent full-frame GOP while keeping crop
  GOP size `1`, so crop sidecar clips align with full-frame parent clips.
- `recording_control` is copied from the Orange session/spec intent so external
  consumers can distinguish continuous, timed, and rolling sessions. If a
  headless spec places the control at `fixed.recording_control`, the supervisor
  and verifier treat it as the contract-level default unless a stream overrides
  it.
- `rollover.requested = true` is supported for the supervised diagnostic IPC
  recorder when `clip_seconds > 0`. The recorder owns GOP-boundary writer
  rotation and reports
  `rollover.implementation = "external_recorder_gop_boundary_writer_rotation"`.
  It writes only per-clip media under `clips/clip_%06d/`; the ordered
  coordinator does not also write a merged session MP4.
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
normally named `Cam<serial>_external_status.json`. For rolling runs, that
heartbeat includes a `rolling` object with the requested control values,
GOP-aligned `clip_span_frames`, estimated `current_clip_index`,
`next_rollover_at_recording_frame_id`, `frames_until_next_rollover`, completed
clip count, and last completed rollover outcome. This is live progress
telemetry; the final `rolling_output.clips[]` list in
`external_recorder_summary.json` remains the authoritative clip artifact list.

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
- A finite `record_for_seconds` also materializes
  `duration_safety_limit` in the supervisor plan and recorder artifacts. The
  recorder accepts at most `record_for_seconds * fps + max(2 * fps, gop)`
  descriptors. This is a frame-count backstop for a missing GUI stop, not the
  primary duration clock; the GUI owns the monotonic session deadline and sends
  the normal `DRAIN`/`FINALIZE` controls.
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
- Finalization requires clean descriptor intake, a non-exceeded duration
  backstop for finite runs, and a final recorder storage preflight that still
  satisfies `min_free_bytes`. A meaningful session overrun (more than two
  seconds at the monotonic stop-request boundary) is persisted as incomplete.
- For non-rolling streams, the materialized contract carries `metadata_csv`
  (default `Cam<serial>_external_meta.csv`). The recorder writes one row only
  after a frame is accepted by NVENC, preserving the descriptor's camera
  `timestamp` and host `timestamp_sys`. Summary JSON reports the path, row
  count, first/last recording frame ID, gaps, and zero-timestamp counts. GUI
  and headless finalization fail closed unless the CSV exists, contains both
  clock columns, and matches `frames_encoded` exactly.
  The CSV begins with the legacy-compatible
  `frame_id,timestamp,timestamp_sys` columns and also carries
  `recording_frame_id` plus GOP/shard routing fields; `frame_id` and
  `recording_frame_id` are identical for this full-frame output.
- Recorder child-process, socket state, parsed `status_json` heartbeat state,
  and storage preflight state are visible in the GUI. The status line shows
  heartbeat coverage plus recorder-side received/encoded frame totals when
  sidecars are present. For rolling full-frame runs it also shows the recorder's
  current clip index and next GOP-aligned rollover boundary from the live status
  sidecar, plus the last completed clip when a rollover has already occurred.
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
  "stream_kind": "full_frame",
  "output_kind": "full",
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

`scripts/verify_external_recorder_session.py` accepts either an explicit
external recorder artifact root or, for GUI/headless runs that wrote the
contract/plan metadata, just `--analytics-root <recording_folder-or-run-root>`.
When `artifact_root` is omitted, the verifier derives it from
`external_recorder_contract.json`, `external_recorder_supervisor_plan.json`,
`recording_session.json`, or the experiment spec.

- analytics `runs.json` rows pass and report `recording_sink_mode =
  "external_ipc"`
- external IPC failures and ACK timeouts are zero
- ACKed frames are at least submitted frames
- recorder summary schema, `stream_id`, and declared stream/output kind match
  the contract
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
