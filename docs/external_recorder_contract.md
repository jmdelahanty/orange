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
recorder import the source GPU allocation and copy the frame into
recorder-owned GPU memory.

The per-frame lifecycle is:

1. Orange acquires a frame into GPU memory.
2. Orange sends a descriptor over the Unix domain socket.
3. The recorder imports the CUDA IPC handle.
4. The recorder copies the pixels into recorder-owned GPU memory.
5. The recorder sends an ACK back over the socket.
6. Orange can safely recycle the source frame buffer.
7. The recorder later encodes the detached copy.

The ACK matters because Orange must not reuse a source frame buffer before the
recorder has made its own copy.

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
    "streams": {
      "2010095": {
        "stream_id": "2010095",
        "camera_serial": "2010095",
        "analytics_gpu_id": 5,
        "recorder_gpu_id": 5,
        "expected_shard_gpu_ids": [5, 6],
        "routing_policy": "gop_modulo",
        "summary_json": "/tmp/.../Cam2010095_external_summary.json",
        "video_sanity_json": "/tmp/.../Cam2010095_external_video_sanity.json",
        "mp4": "/tmp/.../Cam2010095_external.mp4",
        "gop_routing_csv": "/tmp/.../Cam2010095_external_gop_routing.csv",
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
- `supervise_processes = false` preserves the current smoke-runner behavior
  where a shell script launches the recorder. Set it to `true` only for specs
  where `orange_client` should own recorder startup, socket readiness, and
  shutdown.
- Orange parses this block, fails fast on malformed specs, preserves it in
  `experiment_spec.json`, and exposes per-camera expected artifact paths in
  `runs.json` / `runs.csv`.
- The analytics process does not validate external recorder files directly.
  Those files are finalized after Orange exits, so validation is performed by
  `scripts/verify_external_recorder_session.py`.
- `require_merged_mp4` only applies to multi-shard runs. Single-shard runs use
  the shard output as the final MP4.
- `expected_shard_gpu_ids` is ordered by shard id. For GOP modulo routing,
  shard id is `gop_index % shard_count`.
- `encode_queue_depth`, `prewarm_*`, codec, GOP, and bitrate fields are
  optional launch-plan fields. If a stream omits them, the dry-run supervisor
  plan tool fills in production-like defaults.

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
- On record start, the GUI materializes the same contract shape into the
  proposed recording folder as `external_recorder_contract.json` and also writes
  `external_recorder_supervisor_plan.json`.
- The GUI then fails fast before setting `record_video = true` with:
  `external recorder GUI supervision is not implemented yet; use headless
  supervised spec or in-process recording`.
- This is intentionally metadata-only until the GUI/session layer owns recorder
  process startup, heartbeat, drain, finalization, and user-visible failure
  reporting.

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
  "frames_received": 100,
  "acks_sent": 100,
  "encode_enqueued": 100,
  "encode_skipped": 0,
  "encode_dropped": 0,
  "frames_encoded": 100,
  "worker_failed": false,
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
- `detach_copied == encode_enqueued`
- no encode drops and no worker failures
- expected shard GPU ids and routing policy match
- merged output is finalized for multi-shard runs
- MP4 exists, has a valid video stream, and passes video sanity when required
- GOP routing CSV has one data row per received frame

Smoke runners:

- `scripts/run_external_recorder_smoke.sh`
- `scripts/run_external_recorder_two_camera_ptp_smoke.sh`

Both runners now generate `external_recorder_session.json`, inject the same
contract into the temporary experiment spec, run video sanity, and then run
`scripts/verify_external_recorder_session.py`.
