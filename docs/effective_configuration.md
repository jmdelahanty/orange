# Effective configuration provenance (sealed at record start)

Date: 2026-10-05. Code: `src/effective_configuration.{h,cpp}`; capture in
`prepare_recording_run_start` (`src/session/recording_session.cpp`) right after
the subject references; test `tools/effective_configuration_tests.cpp`;
validator check `check_effective_configuration` in
`scripts/validate_gui_ptp_recording.py`.

## Why

Orange takes settings from three places: the app configuration file
(`~/orange_data/config/app/default.json`, or `ORANGE_APP_CONFIG_PATH`),
`ORANGE_*` environment variables (which win over the file), and a bridge in
`src/orange.cpp` (`set_gui_env_from_app_config_if_absent`) that exports file
values into the environment when the variable is absent. An audit on
2026-10-05 found about 217 distinct `ORANGE_*` names read by the app, about 45
with an app-config key, and no record in the recording folder of the app config
file (path, hash or contents) or of the environment; where a value came from was
only printed to stdout. The sealed start snapshot therefore now carries the
effective configuration, so a run is reproducible from its folder no matter
which path set a knob.

## What is sealed

`recording_snapshot_start.json` -> `session.effective_configuration`:

| Key | Content |
| --- | --- |
| `schema_id`, `schema_version` | `orange.effective_configuration`, 1 |
| `captured_at_utc` | capture time (record start, before acquisition) |
| `process` | `pid`, `executable` (`/proc/self/exe`), `cwd` |
| `app_config` | `path`, `status` (`loaded`, `not_configured`, `missing`, `unreadable`, `invalid_json`), `sha256` of the file bytes, `size_bytes`, `contents` (the parsed JSON, as loaded), `error` |
| `environment` | every `ORANGE_*` variable present, plus `CUDA_VISIBLE_DEVICES`, `CUDA_DEVICE_ORDER`, `CUDA_MODULE_LOADING`, `CUDA_LAUNCH_BLOCKING`, `CUDA_MPS_*`, `LD_LIBRARY_PATH`, `DISPLAY`, `WAYLAND_DISPLAY`, `XDG_SESSION_TYPE`, `SUDO_USER`, `OMP_NUM_THREADS`: `{value, source}` where `source` is `environment` (set by the operator or launcher) or `app_config` (exported by the bridge) |
| `exported_from_app_config` | the names the bridge exported, sorted |

Values of variables whose names contain `SECRET`, `TOKEN`, `PASSWORD`,
`PASSWD` or `API_KEY` are replaced by `<redacted>` with `redacted: true`.
`app_config.contents` is the operator's rig configuration and is copied whole.

The capture never blocks a record start: a failure is logged and the snapshot
simply lacks the block (the validator warns). The block is part of the sealed
start snapshot, so it is immutable for the life of the recording and travels
with the folder; the transfer snapshot already hashes the sealed snapshot.
Nothing in Orange reads it back.

## Reading it

```python
import json
s = json.load(open("recording_snapshot_start.json"))["session"]["effective_configuration"]
print(s["app_config"]["path"], s["app_config"]["sha256"])
for name, entry in sorted(s["environment"].items()):
    print(f"{entry['source']:12} {name}={entry['value']}")
```

`scripts/validate_gui_ptp_recording.py` prints one line per run:
`effective_configuration sealed: app_config loaded sha256=... env N (n from environment, m bridged from app config)`.

## Not done (second slice, needs a decision per knob)

Knobs that change recorded data or recording shape but have no app-config key
(so they are env-only): owner-push timing (`ORANGE_EXTERNAL_RECORDER_OWNER_PUSH_{MAX_AGE_MS,DEADLINE_MS,TRANSFER_ALLOWANCE_MS,CHUNK_BYTES}`),
recorder deferred-release / ack-timeout, NVENC extra-output-delay and harvest
knobs, `ORANGE_MP4_WRITEBACK_PACE_BYTES`, `ORANGE_PTP_LATCH_AFTER_FANOUT`,
`ORANGE_YOLO_SPATIAL_MASK_MODE`, `ORANGE_YOLO_STREAM_PRIORITY`, YOLO affinity
and RT priority, `ORANGE_CITRUS_OBSERVATION_BINDING_{MODE,SOCKET,TIMEOUT_MS}`,
`ORANGE_GUI_RECORDER_PREPARE_STRICT`, `ORANGE_GUI_OWNER_PUSH_READY_TIMEOUT_S`.
Recording strategy, `fail_on_drop` and NVENC direct input live in the camera
JSON. With the capture above every one of them is now recorded per run, which
is the prerequisite for deciding which deserve a config key.
