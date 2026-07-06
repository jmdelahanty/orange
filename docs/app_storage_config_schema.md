# App Config Schema

This document proposes an app-level schema for Orange storage defaults,
model defaults, and latest-recording pointer behavior.

This is intentionally separate from the camera config schema:

- camera config answers:
  - how a specific camera should run
  - where its source GPU lives
  - how its recording path should behave
- app config answers:
  - where recordings should go by default
  - where latest-recording pointers should be written
  - which optional runtime model assets should be preselected

This schema is meant for process/session defaults, not camera acquisition
configuration. The one intentional serial-keyed exception is host-local crop
external recorder GPU placement, because that is a workstation topology
default rather than a camera sensor setting.

## Why This Exists

Today the default GUI recording root is effectively derived from the current
user and hardcoded into the Orange data tree:

- [orange.cpp](/home/jeremy/orange-gop-split-a16/src/orange.cpp:2312)
- [orange.cpp](/home/jeremy/orange-gop-split-a16/src/orange.cpp:2314)

That gives us a default base folder like:

- `~/orange_data/exp/unsorted`

At the same time, latest-recording metadata is written to:

- `<base_folder>/.orange/latest_recording.json`
- `/run/orange/latest_recording.json`

See:

- [recording_metadata.md](/home/jeremy/orange-gop-split-a16/docs/recording_metadata.md:14)
- [recording_pointer_compatibility_plan.md](/home/jeremy/orange-gop-split-a16/docs/recording_pointer_compatibility_plan.md:1)

We want a first-class config for the default recording root, optional model
defaults, and a cleaner model for how the latest-recording pointers relate to
the recording root.

## Schema Identity

- `schema_id = "orange.app.config"`
- `schema_version = 1`

## Proposed File Location

Preferred host-local file:

- `~/orange_data/config/app/default.json`

Concrete example on this machine:

- `/home/jeremy/orange_data/config/app/default.json`

Repo-tracked example:

- [config/app/default.example.json](/home/jeremy/orange-gop-split-a16/config/app/default.example.json)
- [config/system/orange-tmpfiles.conf.example](/home/jeremy/orange-gop-split-a16/config/system/orange-tmpfiles.conf.example)

Why this location:

- it matches the existing Orange data tree under `~/orange_data`
- it lives beside the existing `config/local` and `config/network` folders
- it is host/user-local runtime configuration, not a repo-tracked camera artifact

Missing-file behavior should remain non-fatal:

- if the file is absent, Orange should continue using built-in defaults

## Proposed Shape

```json
{
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "models": {
    "default_detect_engine": ""
  },
  "recording": {
    "sink_mode": "",
    "recording_control": {
      "record_for_seconds": 0,
      "clip_seconds": 0
    },
    "crop": {
      "sink_mode": "in_process",
      "frame_pool_size": null,
      "external_ipc": {
        "encode_queue_depth": 64,
        "recorder_gpu_id": null,
        "recorder_gpu_ids_by_serial": {}
      }
    },
    "ptp_register_read_decimate": 1,
    "external_recorder_contract_path": ""
  },
  "gui": {
    "stream": {
      "downsample": 4
    },
    "display": {
      "profile": "default",
      "display_preview_max_fps": null,
      "swap_interval": null,
      "frame_max_fps": null
    },
    "telemetry": {
      "show_speed_graphs": false
    },
    "local_control": {
      "recording_start_enabled": false,
      "recording_stop_enabled": false,
      "citrus_completion_stop_enabled": true,
      "exit_after_finalize": false,
      "drain_timeout_seconds": 60
    }
  },
  "storage": {
    "default_recording_root": "/home/jeremy/orange_data/exp/unsorted",
    "latest_recording": {
      "write_local_pointer": true,
      "canonical_pointer_root": "/home/jeremy/orange_data/.orange",
      "write_run_pointer": true,
      "run_pointer_path": "/run/orange/latest_recording.json"
    }
  }
}
```

## Field Semantics

### `models.default_detect_engine`

Type:

- string

Meaning:

- optional TensorRT detect engine path to preselect in the GUI

Recommended default:

- empty string

Orange should not silently fall back to a bundled or historical YOLO engine. If
this field is empty, the GUI starts with no selected detect model. Enabling
YOLO then requires the user to select an `.engine` file manually or configure a
valid host-local path here.

Example:

- `/home/jeremy/orange_data/detect/my_detector.engine`

### `gui.display`

Type:

- object

Meaning:

- optional GUI display pacing defaults for direct Orange launches

Fields:

- `profile`: one of `default`, `fast`, or `citrus_safe`
- `display_preview_max_fps`: integer in `[0,10000]` or `null`
- `swap_interval`: integer in `[0,4]` or `null`
- `frame_max_fps`: integer in `[0,1000]` or `null`

Profile defaults:

- `default`: leaves Orange's built-in direct-launch defaults in effect
- `fast`: `display_preview_max_fps = 15`, `swap_interval = 0`,
  `frame_max_fps = 60`
- `citrus_safe`: `display_preview_max_fps = 10`, `swap_interval = 1`,
  `frame_max_fps = 30`

Explicit integer fields override the selected profile. Runtime environment and
validation launcher values still take precedence over app config. For the
current Orange/Citrus co-run workstation default:

```bash
scripts/update_app_config_display_profile.py \
  --profile citrus_safe \
  --stream-downsample 4 \
  --hide-speed-graphs \
  --manual-citrus-completion-control \
  --crop-recording-sink-mode external_ipc \
  --crop-external-encode-queue-depth 128 \
  --crop-frame-pool-size 256 \
  --crop-external-recorder-gpu 2010093=4 \
  --crop-external-recorder-gpu 2010094=2 \
  --crop-external-recorder-gpu 2010095=8 \
  --crop-external-recorder-gpu 2010096=6
```

### `gui.stream`

Type:

- object

Meaning:

- optional GUI display-stream defaults for direct Orange launches

Fields:

- `downsample`: one of `1`, `2`, `4`, `8`, or `16`

Recommended default:

- `4`

This controls only the GUI preview stream size. It does not change acquisition,
YOLO input, full-frame recording, crop recording, or crop/pose ROI generation.

Environment precedence:

- `ORANGE_GUI_STREAM_DOWNSAMPLE` wins when set.
- `ORANGE_DISPLAY_DOWNSAMPLE` is accepted as the legacy alias.

### `gui.telemetry`

Type:

- object

Meaning:

- optional GUI telemetry-rendering defaults

Fields:

- `show_speed_graphs`: boolean

Recommended default:

- `false`

The live YOLO speed graphs are useful for operator diagnostics but can add GUI
rendering work during four-camera validation. Keep them disabled for
performance runs unless the graphs are the thing being inspected.

Environment precedence:

- `ORANGE_GUI_SHOW_SPEED_GRAPHS` wins when set.

### `gui.local_control`

Type:

- object

Meaning:

- optional defaults for the Orange GUI local-control socket lifecycle commands

Fields:

- `recording_start_enabled`: boolean
- `recording_stop_enabled`: boolean
- `citrus_completion_stop_enabled`: boolean
- `exit_after_finalize`: boolean
- `drain_timeout_seconds`: integer in `[0,86400]` or `null`

Recommended configured-workstation default:

```json
{
  "recording_start_enabled": false,
  "recording_stop_enabled": false,
  "citrus_completion_stop_enabled": true,
  "exit_after_finalize": false,
  "drain_timeout_seconds": 60
}
```

That leaves the operator in control of camera streaming and recording start,
while allowing Citrus `citrus_completion` requests to schedule the same
recording stop path as the GUI stop button. The GUI remains open after
finalization unless `exit_after_finalize` is explicitly enabled. If a machine
has no app config at all, Orange's built-in fallback remains more conservative:
all local-control recording lifecycle commands are disabled.

For manual GUI sessions that should not accept Citrus completion-stop, set:

```json
{
  "citrus_completion_stop_enabled": false
}
```

Use `scripts/update_app_config_display_profile.py --manual-citrus-completion-control`
to apply the full manual-session profile without preserving stale orchestrator
start/stop settings.

Environment precedence:

- `ORANGE_GUI_*` local-control env vars take precedence over their generic
  `ORANGE_*` counterparts when both are set.
- `ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START` and
  `ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_START` override
  `recording_start_enabled`; use `1`/`true` to enable and `0`/`false` to
  disable.
- `ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP`,
  `ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_STOP`, and
  `gui.local_control.recording_stop_enabled` control the generic
  `stop_recording` command. Env values override app config; enabled generic
  stop also allows `citrus_completion`.
- `ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP`,
  `ORANGE_LOCAL_CONTROL_ENABLE_CITRUS_STOP`, and
  `gui.local_control.citrus_completion_stop_enabled` control the Citrus-only
  `citrus_completion` stop path. Env values override app config. Use this for
  normal manual GUI sessions where Citrus should end the current recording but
  generic socket stop should remain disabled.
- `ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE` and
  `ORANGE_LOCAL_CONTROL_EXIT_AFTER_FINALIZE` override
  `exit_after_finalize`.
- `ORANGE_GUI_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS` and
  `ORANGE_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS` override
  `drain_timeout_seconds`.

### `storage.default_recording_root`

Type:

- string

Meaning:

- the default base folder for recordings when the caller does not explicitly
  provide a recording folder

Examples:

- GUI default output root
- headless local mode when no `--record-folder` is provided and recording is
  enabled
- future automation defaults

This should generally point at a persistent user-data tree such as:

- `~/orange_data/exp/unsorted`

It is a base folder, not a single run folder.

### `recording.sink_mode`

Type:

- string

Current supported values:

- empty string or omitted
- `real`
- `preprocess_only`
- `immediate_recycle`
- `threaded_handoff_only`
- `external_ipc`

Meaning:

- app-level default for the GUI recording sink used when recording pipelines are
  created

Recommended default:

- empty string or omitted

With no selected camera preference, empty or omitted resolves to `real`. Leaving
the field empty is preferred for general app configs because camera profiles can
then opt into `external_ipc` when their throughput profile needs it.

`ORANGE_GUI_RECORDING_SINK_MODE` still overrides this field for diagnostics.
If this field is omitted from the app config, selected camera configs may set
`recording.preferred_sink_mode` to choose the GUI session sink. Any selected
recording camera that prefers `external_ipc` makes the session use
`external_ipc`. An explicit app-level `recording.sink_mode` wins over camera
preferences, so keep this field omitted when you want camera profiles to drive
the default.

`external_ipc` is the first GUI/session path for process-isolated recording.
On record start, Orange materializes the external recorder contract, starts the
supervised diagnostic recorder processes, and routes full-frame descriptors
through the existing IPC handoff path. On drain/finalize, the GUI stops the
supervised recorders and writes a shared `recording_session.json` with
`producer = "orange_gui_external_ipc"` and
`recording_backend.mode = "external_ipc"`.

This path passed the first two-camera GUI hardware validation on 2026-05-21 at:

```text
/home/jeremy/orange_data/exp/unsorted/2026_05_21_12_39_24
```

The run used `ORANGE_GUI_RECORDING_SINK_MODE=external_ipc`,
`ORANGE_PTP_REGISTER_READ_DECIMATE=100`, and `100_cam4_ptp`. Both cameras
recorded `1645` submitted/ACKed/encoded frames with no external IPC failures,
ACK timeouts, frame gaps, GetFrame errors, or encode failures. The standard GUI
validator now follows `recording_session.json` external video paths and passes
for this layout. Remaining production-hardening work is GUI-visible recorder
health/failure reporting and GUI PTP-stack preflight/repair.

### `recording.incremental_clip_shadow`

Type:

- boolean (optional, default `false`)

Behavior:

- When `true`, sets `ORANGE_GUI_INCREMENTAL_CLIP_SHADOW=1` at startup unless the
  environment variable is already set (the environment wins when set).
- Enables the shadow-mode incremental clip finalizer for external-IPC rolling
  recordings: completed clips are split into `*.shadow.csv` files during the
  recording, a `recording_clip_index.shadow.jsonl` partial index is kept in the
  recording folder, and finalize logs a
  `shadow cross-check: N/M clip CSVs identical, ...` comparison against the
  authoritative split. Observation only: shadow output never replaces the
  authoritative artifacts and mismatches never fail a recording. The terminal
  clip always reports `missing` in the cross-check (it completes during recorder
  shutdown, after the shadow worker has stopped).

### `recording.recording_control`

Type:

- object

Fields:

- `record_for_seconds`: integer, minimum `0`
- `clip_seconds`: integer, minimum `0`

Meaning:

- app-level GUI recording-control intent for timed and rolling full-frame
  recording sessions

Recommended default:

```json
{
  "record_for_seconds": 0,
  "clip_seconds": 0
}
```

`clip_seconds = 0` keeps the GUI compatibility single-clip layout.
`clip_seconds > 0` requests full-frame rolling clips and requires
`record_for_seconds > 0`. In the GUI external IPC path, Orange applies this
control when materializing the supervised full-frame external recorder contract,
and finalization mirrors recorder `rolling_output.clips[]` into
`recording_session.json`, `recording_clip_index.json/csv`, and
`recording_snapshot.json` pointers.

The GUI Recording panel exposes in-memory controls for these same values when
the effective full-frame sink mode is `external_ipc`. The controls are locked
while streaming is active and do not rewrite the JSON file.

Environment precedence:

- `ORANGE_GUI_RECORD_FOR_SECONDS` overrides
  `recording.recording_control.record_for_seconds`.
- `ORANGE_GUI_CLIP_SECONDS` overrides
  `recording.recording_control.clip_seconds`.
- If `ORANGE_GUI_CLIP_SECONDS > 0` is used for an autorun and
  `ORANGE_GUI_RECORD_FOR_SECONDS` is unset, Orange uses
  `ORANGE_GUI_AUTORUN_RECORD_SECONDS` as `record_for_seconds`.

GUI external crop IPC follows this same rolling control when the effective
full-frame sink mode is `external_ipc`. With `clip_seconds > 0`, crop outputs
are finalized as rolling sidecars using the external recorder writer-rotation
path. The crop encoder still uses GOP size `1`, but Orange asks the crop
recorder to coalesce a short terminal tail using the full-frame GOP window so
crop sidecar clips align with the parent full-frame clip manifest. With
`clip_seconds = 0`, crop outputs remain single-clip sidecars. Environment
variables still take precedence over the JSON file and in-memory GUI controls.

### `recording.crop`

Type:

- object

Meaning:

- app-level defaults for GUI crop recording worker behavior

Fields:

- `sink_mode`: one of `in_process`, `inprocess`, `real`, or `external_ipc`
- `frame_pool_size`: integer in `[1,512]` or `null`
- `external_ipc.encode_queue_depth`: integer in `[1,4096]`
- `external_ipc.recorder_gpu_id`: integer in `[0,255]` or `null`
- `external_ipc.recorder_gpu_ids_by_serial`: object mapping camera serial
  strings to integer GPU ids in `[0,255]`

Recommended default:

```json
{
  "sink_mode": "in_process",
  "frame_pool_size": null,
  "external_ipc": {
    "encode_queue_depth": 64,
    "recorder_gpu_id": null,
    "recorder_gpu_ids_by_serial": {}
  }
}
```

`sink_mode = "real"` and `sink_mode = "inprocess"` are accepted as aliases for
`in_process`, matching the full-frame sink-mode naming and common command-line
shorthand. `external_ipc` routes crop frames through the supervised external
crop recorder path when crop recording is enabled.

`frame_pool_size = null` keeps Orange's built-in crop producer default. For the
current four-camera Orange/Citrus co-run, the launcher still supplies
`ORANGE_CROP_FRAME_POOL_SIZE=256`, derived from the larger crop external queue.
If a workstation should use that profile for ordinary direct launches, set
`recording.crop.frame_pool_size = 256` explicitly in the app config.

`recorder_gpu_id` is a global fallback for crop external recorder placement.
`recorder_gpu_ids_by_serial` is the preferred host-local four-camera form
because it records the actual A16 topology used by the rig. If neither field is
set, crop external recorders fall back to the camera analytics/source GPU unless
the validation launcher supplies env overrides.

Environment precedence:

- `ORANGE_CROP_RECORDING_SINK_MODE` overrides `recording.crop.sink_mode`.
- `ORANGE_CROP_FRAME_POOL_SIZE` overrides `recording.crop.frame_pool_size`.
- `ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH` overrides
  `recording.crop.external_ipc.encode_queue_depth`.
- `ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID` overrides
  `recording.crop.external_ipc.recorder_gpu_id`.
- `ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_<serial>` overrides the matching
  `recording.crop.external_ipc.recorder_gpu_ids_by_serial` entry.

### `recording.ptp_register_read_decimate`

Type:

- integer, minimum `1`

Meaning:

- GUI default for `ORANGE_PTP_REGISTER_READ_DECIMATE` when the environment
  variable is not already set

Recommended local PTP validation value:

- `100`

The value controls how often the GUI acquisition hot path polls the camera
`GevTimestampValue*` registers for diagnostics. `1` keeps the old per-frame
polling behavior. Larger values keep PTP gate synchronization and embedded
frame timestamps, but move most register-read diagnostics out of the per-frame
hot path.

Environment precedence:

- `ORANGE_PTP_REGISTER_READ_DECIMATE` wins when it is already set.
- Otherwise the GUI applies `recording.ptp_register_read_decimate` before
  starting acquisition threads.

### `recording.external_recorder_contract_path`

Type:

- string

Meaning:

- optional path to a JSON object matching
  `orange.external_recorder.contract`

Recommended default:

- empty string

If omitted, the GUI synthesizes an intended contract from the open recording
cameras, camera GPU ids, split-GOP shard ids, and the current recording folder.
The config may also provide an inline object as
`recording.external_recorder_contract`. String fields in the contract may use
`{recording_folder}` and `{recording_id}` placeholders.

### `storage.latest_recording.write_local_pointer`

Type:

- boolean

Meaning:

- whether Orange writes:
  - `<actual_base_folder>/.orange/latest_recording.json`

Recommended default:

- `true`

This should remain the required local pointer because it is colocated with the
actual output tree.

### `storage.latest_recording.canonical_pointer_root`

Type:

- string

Meaning:

- a stable user-level location where Orange also writes:
  - `<canonical_pointer_root>/latest_recording.json`

Recommended default:

- `~/orange_data/.orange`

This is the proposed durable user-data pointer root.

Unlike the local pointer, this path should remain stable even if a single run
overrides the recording folder somewhere else.

### `storage.latest_recording.write_run_pointer`

Type:

- boolean

Meaning:

- whether Orange attempts to write the compatibility pointer used by Citrus and
  other system-level consumers

Recommended default:

- `true`

This should remain enabled initially for compatibility.

### `storage.latest_recording.run_pointer_path`

Type:

- string

Meaning:

- the absolute compatibility pointer path

Recommended default:

- `/run/orange/latest_recording.json`

This is a full file path, not just a directory.

Operational recommendation:

- treat `/run/orange/latest_recording.json` as the canonical live
  producer/consumer rendezvous path
- provision its parent directory with `tmpfiles.d` rather than relying on
  Orange to create it opportunistically during a recording run

Recommended sample rule:

- [config/system/orange-tmpfiles.conf.example](/home/jeremy/orange-gop-split-a16/config/system/orange-tmpfiles.conf.example)

## Precedence Rules

The effective recording base folder should resolve in this order:

1. explicit per-run recording folder
   - GUI-selected folder
   - headless `--record-folder`
   - experiment-run resolved output folder
2. `storage.default_recording_root`
3. legacy fallback:
   - `~/orange_data/exp/unsorted`

Current implementation status:

- `models.default_detect_engine` is used to preselect the GUI detect engine
- `ORANGE_DEFAULT_DETECT_ENGINE` can override that preselection for one runtime
  process; this is useful for validation before changing persistent app config
- if no detect engine is configured or selected, starting a YOLO-enabled stream
  is blocked by GUI preflight
- `storage.default_recording_root` is used for the GUI default recording root
- headless CLI and experiment-spec flows still choose their recording folders
  explicitly
- `recording.sink_mode`, `recording.recording_control.*`, and
  `recording.ptp_register_read_decimate` are used by the GUI unless overridden
  by environment or launcher values; if `recording.sink_mode` is omitted, the
  GUI can use selected camera `recording.preferred_sink_mode` values
- `recording.crop.sink_mode`, `recording.crop.frame_pool_size`, and
  `recording.crop.external_ipc.*` are applied to the GUI by setting the
  existing worker/session environment controls when those env vars are not
  already present
- `gui.stream.downsample` and `gui.telemetry.show_speed_graphs` are used by the
  GUI unless overridden by environment or launcher values
- `storage.latest_recording.*` now controls the local, canonical, and `/run`
  pointer writes emitted by the recording snapshot path
- `gui.display` controls direct GUI display pacing defaults, and
  `scripts/update_app_config_display_profile.py` can update those fields,
  `gui.local_control`, and crop recording defaults
  without hand-editing JSON

The pointer outputs should then use the resolved base folder plus the configured
metadata roots.

## How `latest_recording.json` Should Behave

Assume:

- `storage.default_recording_root = /home/jeremy/orange_data/exp/unsorted`
- `storage.latest_recording.canonical_pointer_root = /home/jeremy/orange_data/.orange`
- `storage.latest_recording.run_pointer_path = /run/orange/latest_recording.json`

And assume Orange resolves:

- `actual_base_folder = /home/jeremy/orange_data/exp/unsorted`
- `recording_folder = /home/jeremy/orange_data/exp/unsorted/2026_04_15_22_19_15`

Then Orange should write:

1. local pointer:
   - `/home/jeremy/orange_data/exp/unsorted/.orange/latest_recording.json`
2. canonical user-data pointer:
   - `/home/jeremy/orange_data/.orange/latest_recording.json`
3. compatibility run pointer:
   - `/run/orange/latest_recording.json`

All of those pointers should contain the same payload:

```json
{
  "recording_id": "2026_04_15_22_19_15",
  "timestamp_utc": "2026-04-16T02:19:15Z",
  "recording_folder": "/home/jeremy/orange_data/exp/unsorted/2026_04_15_22_19_15",
  "snapshot_path": "/home/jeremy/orange_data/exp/unsorted/2026_04_15_22_19_15/recording_snapshot.json"
}
```

### Important detail

The canonical pointer should point at the actual recording folder used for the
run, even if that run did not use the configured default root.

Example:

- configured default root:
  - `/home/jeremy/orange_data/exp/unsorted`
- actual one-off run folder override:
  - `/mnt/fast/orange/exp/unsorted`

Then the canonical pointer under:

- `/home/jeremy/orange_data/.orange/latest_recording.json`

should still point at:

- `/mnt/fast/orange/exp/unsorted/<recording_id>`

That is what makes it a stable lookup location rather than just a mirror of the
default root.

## Runtime Flag Disposition

The four-camera GUI/Orange-Citrus validation profile still uses many
environment variables because the launcher is a convenient integration harness.
Those variables should not all become durable application settings.

### Already App Config

These should be set in `~/orange_data/config/app/default.json` when they are
intended as workstation defaults:

- `recording.sink_mode`
  - environment override: `ORANGE_GUI_RECORDING_SINK_MODE`
  - built-in default should remain `real`
  - production-like four-camera GUI validation should use `external_ipc`
- `recording.recording_control.record_for_seconds`
  - environment override: `ORANGE_GUI_RECORD_FOR_SECONDS`
  - built-in default should remain `0`
- `recording.recording_control.clip_seconds`
  - environment override: `ORANGE_GUI_CLIP_SECONDS`
  - built-in default should remain `0`; set a positive value only for
    timed/rolling profiles
- `recording.crop.sink_mode`
  - environment override: `ORANGE_CROP_RECORDING_SINK_MODE`
  - built-in default should remain `in_process`
  - production-like four-camera GUI validation should use `external_ipc`
- `recording.crop.external_ipc.encode_queue_depth`
  - environment override: `ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH`
  - built-in/default example value should remain `64`
  - the current four-camera Orange/Citrus profile uses `128`
- `recording.crop.external_ipc.recorder_gpu_id`
  - environment override: `ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID`
  - keep `null` unless a rig wants one fallback crop recorder GPU for every
    camera
- `recording.crop.external_ipc.recorder_gpu_ids_by_serial`
  - environment override: `ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_<serial>`
  - the current local four-camera Orange/Citrus profile uses
    `2010093=4`, `2010094=2`, `2010095=8`, and `2010096=6`
- `recording.crop.frame_pool_size`
  - environment override: `ORANGE_CROP_FRAME_POOL_SIZE`
  - `null` keeps the built-in crop producer default
  - the current four-camera Orange/Citrus profile uses `256`
- `recording.ptp_register_read_decimate`
  - environment override: `ORANGE_PTP_REGISTER_READ_DECIMATE`
  - built-in default should remain `1` for diagnostic compatibility
  - the local PTP validation profile should set this to `100`
- `gui.display.profile`
  - launcher overrides: `ORANGE_DISPLAY_PREVIEW_MAX_FPS`,
    `ORANGE_GUI_SWAP_INTERVAL`, and `ORANGE_GUI_FRAME_MAX_FPS`
  - use `citrus_safe` on a workstation that shares the display GPU with Citrus
  - use `fast` for Orange-only GUI validation when display-GPU contention is
    not a concern
- `gui.stream.downsample`
  - environment override: `ORANGE_GUI_STREAM_DOWNSAMPLE`
  - built-in default should remain `4`
  - the current four-camera validation profile uses `4`
- `gui.telemetry.show_speed_graphs`
  - environment override: `ORANGE_GUI_SHOW_SPEED_GRAPHS`
  - built-in default should remain `false`
  - enable only for operator diagnostics that need live per-camera graphs
- `models.default_detect_engine`
  - environment override: `ORANGE_DEFAULT_DETECT_ENGINE`
  - once model quality is accepted, the A16 high-effort detect engine should
    move here instead of living only in launcher env

### Core Performance Defaults

These started as environment-gated experiments, then became the healthy
external-IPC validation profile. After the strict four-camera Orange/Citrus
rolling run at
`/home/jeremy/orange_data/exp/unsorted/2026_05_28_21_55_25`, they are core
defaults with env opt-outs for diagnostics and A/B comparisons:

- `ORANGE_ANALYTICS_EARLY_OWNED_FRAME` defaults to enabled
  - keeps analytics input lifetime management off the fragile old ring path
- `ORANGE_YOLO_DETACH_INPUT` defaults to enabled
  - decouples YOLO input ownership from source-frame reuse
- `ORANGE_YOLO_READY_EVENT_FASTPATH` defaults to enabled
  - keeps the common ready-event path cheap in the validated profile
- `ORANGE_CROP_STAGE_SOURCE` defaults to enabled
  - keeps crop source staging aligned with the validated external crop path
- `ORANGE_CROP_COPY_TIMING` defaults to disabled
  - keeps optional CUDA timing events off the crop hot path unless a diagnostic
    run explicitly needs GPU copy timings

Do not turn these into app-config fields just to preserve the old env spelling.
They are internal runtime-path toggles, not operator preferences. Use
`ORANGE_<FLAG>=0` to recover the old behavior for targeted comparisons.

### Keep As Launch Or Diagnostic Flags

These should stay outside durable app config unless a separate operator product
surface is designed for them:

- GUI automation and validation control:
  - `ORANGE_GUI_AUTORUN*`
  - `ORANGE_GUI_VALIDATE_ONLY`
  - `ORANGE_GUI_PRINT_EXEC_ENV_ONLY`
  - `ORANGE_GUI_ALLOW_NO_DISPLAY`
- local-control mutating gates:
  - `ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START`
  - `ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP`
  - `ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP`
  - the read-only local-control socket can stay default-on, but mutating
    commands should remain explicit opt-in for orchestrator/profile runs
- run-specific camera/config selection:
  - `ORANGE_GUI_CONFIG_DIR`
  - `ORANGE_GUI_EXPECT_CAMERAS`
- validation thresholds and exceptions:
  - `ORANGE_CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER`
  - `ORANGE_CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS`
  - `ORANGE_GUI_MAX_YOLO_*`
  - `ORANGE_GUI_ALLOW_MAIN_VIDEO_CONTENT_FAILURE_CAMERAS`
  - source-version and dirty-worktree expectation flags
- diagnostics:
  - `ORANGE_YOLO_PERF_LOG`
  - `ORANGE_YOLO_PERF_SAMPLE`
  - `ORANGE_CROP_COPY_TIMING=1`
- scheduling experiments:
  - `ORANGE_YOLO_AFFINITY`
  - `ORANGE_YOLO_AFFINITY_CAM_<serial>`
  - `ORANGE_YOLO_RT_*`

The current good four-camera CPU affinity is `2010093->6`, `2010094->8`,
`2010095->10`, and `2010096->12`. That should become a rig-local performance
profile only after the CPU-isolation and Citrus CPU budget remain stable; it
should not become a global app default.

## What This Schema Does Not Cover

This schema should not contain:

- per-camera recording strategy
- source GPU placement
- full-frame split-GOP shard ids
- codec/preset/GOP defaults for a specific camera
- Citrus session output policy

Those belong elsewhere:

- per-camera config schema
- experiment specs
- Citrus contracts

## Relationship to Citrus

This schema does not remove `/run/orange/latest_recording.json`.

It only makes the Orange-side storage model cleaner by adding:

- a configurable default recording root
- a configurable canonical user-data pointer root

During migration, Citrus can continue preferring:

- `/run/orange/latest_recording.json`

while Orange also writes:

- `~/orange_data/.orange/latest_recording.json`

See:

- [recording_pointer_compatibility_plan.md](/home/jeremy/orange-gop-split-a16/docs/recording_pointer_compatibility_plan.md)

## Recommended Implementation Order

1. Add app-level config parsing for this schema.
2. First minimal runtime slice:
   - create/read `~/orange_data/config/app/default.json`
   - use `storage.default_recording_root` where the GUI currently falls back to
     `~/orange_data/exp/unsorted`
   - leave headless CLI/experiment paths explicit for now
3. Keep the existing local pointer.
4. Add canonical user-data pointer writing.
5. Keep `/run/orange/latest_recording.json` as compatibility output.
6. Update docs/contracts after runtime support lands.

## What I Think

This should be an app-level schema, not a camera schema.

The most important design property is:

- one stable configured default for where recordings go
- one stable configured user-data pointer location
- continued `/run/orange` compatibility while Citrus still expects it

That gives Orange a cleaner ownership model without immediately breaking the
existing Citrus discovery flow.
