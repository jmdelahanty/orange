# GUI Autorun Validation Plan

Status: first internal state-machine slice is implemented. The immediate goal
is live validation on the real display session with four cameras, external
full-frame IPC recording, and external crop recording.

Last updated: 2026-05-28.

## Goal

Provide an opt-in mode that launches the real Orange GUI process and drives the
same open/stream/record/stop/finalize lifecycle without manual clicks.

This is not a headless substitute. The point is to keep the real GLFW, OpenGL,
CUDA interop, display preview, external recorder supervision, and GUI timing
paths active while reducing operator effort and making repeat runs less
variable.

## Non-Goals

- Do not replace headless acquisition benchmarks.
- Do not use Xvfb or software rendering as proof of GUI performance.
- Do not bypass the normal GUI stream/record lifecycle.
- Do not change manual GUI behavior when autorun is disabled.

## Proposed Controls

- `ORANGE_GUI_AUTORUN=1`: enable the state machine.
- `ORANGE_GUI_AUTORUN_RECORD_SECONDS=<N>`: recording duration after warmup.
- `ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS=<N>`: delay between stream start and
  record start.
- `ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=1`: close the GUI after the recording
  session finalizes.
- `ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=1`: disable crop preview display before
  streaming starts while leaving crop recording enabled.
- `ORANGE_GUI_AUTORUN_ENABLE_STREAM=1`: enable streaming for all opened cameras.
- `ORANGE_GUI_AUTORUN_ENABLE_RECORD=1`: enable recording for all opened cameras.
- `ORANGE_GUI_AUTORUN_ENABLE_YOLO=1`: enable YOLO for all opened cameras.
- `ORANGE_GUI_AUTORUN_ENABLE_CROP=1`: enable crop recording for all opened cameras.

When crop autorun is enabled, the GUI forces recording and YOLO on for the same
cameras because crop recording depends on YOLO-selected detections.

The existing launcher should forward these through the privileged launch
boundary.

Current launcher defaults:

- `ORANGE_GUI_AUTORUN=0`
- `ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS=3`
- `ORANGE_GUI_AUTORUN_RECORD_SECONDS=10`
- `ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=0`
- `ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=0`
- `ORANGE_GUI_AUTORUN_ENABLE_STREAM=1`
- `ORANGE_GUI_AUTORUN_ENABLE_RECORD=1`
- `ORANGE_GUI_AUTORUN_ENABLE_YOLO=1`
- `ORANGE_GUI_AUTORUN_ENABLE_CROP=1`
- `ORANGE_GUI_PTP_STACK_MODE=auto` when `ORANGE_GUI_AUTORUN=1` and
  `ORANGE_GUI_EXPECT_SYNC_MODE=ptp_gate`; otherwise `off`
- `ORANGE_CROP_FRAME_POOL_SIZE=2 * ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH`
  when `ORANGE_CROP_RECORDING_SINK_MODE=external_ipc`, crop/YOLO autorun are
  enabled, and the user has not set an explicit crop frame pool size. The value
  is clamped to `[64,512]`; with the default external crop queue depth of `64`,
  the launcher forwards `ORANGE_CROP_FRAME_POOL_SIZE=128`.

The external crop IPC pool default matters because queued crop encode jobs hold
small `256x256` device crop buffers until the external recorder has consumed
them. The old pool default of `32` was enough for in-process crop encoding, but
could drop crop outputs when all four cameras had detections and the external
crop encode queue reached the 40-50 frame range.

## Host PTP Stack Readiness

Manual GUI runs should use the existing `Host PTP Stack` panel before opening
or streaming PTP-gated cameras:

1. Click `Refresh PTP status`.
2. If `ptp4l`, `phc2sys`, or the socket are missing, click
   `Start PTP stack`.
3. Open cameras and start streaming after the status is healthy.

Automated GUI validation cannot rely on that click. The privileged wrapper now
supports `--ptp-stack-mode off|require|auto`, and the validation launcher maps
`ORANGE_GUI_PTP_STACK_MODE` onto that wrapper option. For autorun PTP-gated
validation, the launcher defaults to `auto`, so the wrapper runs
`scripts/ptp_stack.sh status`, starts the host stack if needed, and rechecks
before launching Orange.

Use `ORANGE_GUI_PTP_STACK_MODE=require` to fail fast instead of repairing, or
`off` to skip wrapper-side PTP checks when intentionally testing the GUI panel
itself.

## Display Session Requirements

Live GUI autorun still needs a real desktop display. Xvfb or software rendering
is useful only for smoke checks and is not evidence for GUI performance.

The launcher forwards the display/session variables needed by common Linux
desktop sessions through the `sudo env` boundary:

- `DISPLAY`
- `XAUTHORITY`
- `WAYLAND_DISPLAY`
- `XDG_RUNTIME_DIR`
- `XDG_SESSION_TYPE`

For X11 or XWayland sessions, the root-launched Orange process must also be
allowed to connect to the user's display, for example from the graphical
terminal:

```bash
xhost +SI:localuser:root
```

If a terminal has no `DISPLAY` and no `WAYLAND_DISPLAY`, it is not attached to
the desktop session and cannot launch the live GUI validation directly. The
launcher fails before `sudo` in that state unless
`ORANGE_GUI_ALLOW_NO_DISPLAY=1` is set for a non-performance smoke diagnostic.

For tmux sessions, refresh the display variables from a graphical terminal
before launching Orange from inside tmux:

```bash
tmux set-environment -g DISPLAY "$DISPLAY"
tmux set-environment -g XAUTHORITY "${XAUTHORITY:-$HOME/.Xauthority}"
tmux set-environment -g WAYLAND_DISPLAY "$WAYLAND_DISPLAY"
tmux set-environment -g XDG_RUNTIME_DIR "$XDG_RUNTIME_DIR"
tmux set-environment -g XDG_SESSION_TYPE "$XDG_SESSION_TYPE"
```

On the current local GNOME/X11 setup, the SSH/tmux shell can use the physical
display without forwarding GUI traffic to a laptop by setting:

```bash
export DISPLAY=:1
export XAUTHORITY=/run/user/1000/gdm/Xauthority
export XDG_RUNTIME_DIR=/run/user/1000
export XDG_SESSION_TYPE=x11
```

## Privileged Launch Wrapper

The GUI validation launcher supports a privileged wrapper, matching the
headless `orange-local-benchmark` pattern. The wrapper source lives at
`scripts/orange_gui_validation_wrapper.sh`, and the installer is
`scripts/install_orange_gui_validation_wrapper.sh`.

The intended installed command is:

```text
/usr/local/bin/orange-gui-validation
```

Install it and add the narrow sudoers entry once from an interactive shell:

```bash
cd /home/jeremy/orange-gop-split-a16
sudo scripts/install_orange_gui_validation_wrapper.sh --install-sudoers
```

Then verify the non-interactive path:

```bash
sudo -n /usr/local/bin/orange-gui-validation --help
```

When the wrapper is installed, `scripts/run_gui_aq_off_validation.sh` uses it
automatically (`ORANGE_GUI_USE_PRIVILEGE_WRAPPER=auto`). Set
`ORANGE_GUI_USE_PRIVILEGE_WRAPPER=1` to require it, or `0` to force the older
`sudo env` path.

The launcher also dry-runs the installed wrapper before privileged launch to
confirm it accepts the current validation env contract, including
`ORANGE_GUI_FRAME_MAX_FPS`. If the wrapper is stale, reinstall it:

```bash
cd /home/jeremy/orange-gop-split-a16
sudo scripts/install_orange_gui_validation_wrapper.sh --install-sudoers
```

The current four-camera validation display default is the fast profile:
`ORANGE_GUI_SWAP_INTERVAL=0`, `ORANGE_GUI_FRAME_MAX_FPS=60`, and
`ORANGE_DISPLAY_PREVIEW_MAX_FPS=15`. This removes the vblank wait that made
`render_present_ms` dominate earlier four-camera runs, while still capping the
GUI loop so it does not consume unbounded display-GPU time.

When Citrus stimulus generation is active on the same display GPU, use the
Citrus-safe profile:

```bash
./scripts/run_gui_fourcam_external_ipc_validation.sh --citrus-display-safe
```

That profile defaults Orange to `ORANGE_GUI_SWAP_INTERVAL=1`,
`ORANGE_GUI_FRAME_MAX_FPS=30`, and `ORANGE_DISPLAY_PREVIEW_MAX_FPS=10`.
Environment variables can still override those values for a specific run.

## Automation Research

Dear ImGui applications can be automated through the upstream Dear ImGui Test
Engine (`ocornut/imgui_test_engine`). That project is designed for Dear ImGui
application/game/engine automation, injects inputs through ImGui IO, can run
windowed or headless, and can export screenshots/videos.

For Orange acquisition validation, the first slice should still be internal
autorun rather than ImGui Test Engine integration. The reason is scope: the
critical behavior is camera open, GPUDirect acquisition, CUDA/OpenGL display,
external recorder supervision, crop fanout, and recording finalization. Calling
the same internal lifecycle branches as the manual buttons is less fragile than
label/click automation and keeps the hardware path representative. ImGui Test
Engine remains useful later for UI-specific checks once the runtime lifecycle
is scriptable.

## Implementation Sketch

1. Resolve and select `ORANGE_GUI_CONFIG_DIR` during startup.
2. Reuse the existing local-config load path to select cameras with JSON files.
3. First slice: trigger the existing button bodies via internal request flags:
   - open cameras
   - start streaming
   - start recording
   - stop recording
   - stop streaming and finalize
4. Later cleanup: extract the current button bodies into callable helpers:
   - open selected cameras
   - start streaming
   - start recording
   - stop recording
   - stop streaming and finalize
5. Add a small frame-tick state machine in `src/orange.cpp`.
6. Stop on preflight failure and leave the GUI open unless
   `ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=1` was explicitly set.

First-slice state sequence:

```text
select_config
  -> open_cameras
  -> apply stream/record/YOLO/crop camera selections
  -> start_streaming
  -> stream_warmup
  -> start_recording
  -> recording
  -> stop_recording
  -> stop_streaming
  -> done
```

For external IPC recording, autorun intentionally goes from `stop_recording` to
`stop_streaming`. The stream shutdown path stops the worker graph, closes IPC
clients, stops supervised recorders, and writes the final `recording_session.json`.
Waiting for drain while streaming can hang because the external IPC workers are
still alive by design.

## Validation

- `ORANGE_GUI_VALIDATE_ONLY=1 ./scripts/run_gui_aq_off_validation.sh` still only
  validates config and launcher environment.
- `ORANGE_GUI_PRINT_EXEC_ENV_ONLY=1` prints the autorun env values and
  `ORANGE_GUI_CONFIG_DIR` crossing the `sudo env` boundary. It also prints
  display/session variables that will be forwarded to the root-launched GUI.
- A live autorun recording must produce the same artifacts as a manual GUI run:
  `recording_session.json`, full-frame external IPC outputs, crop outputs when
  enabled, GUI timing telemetry, and validator pass/fail output.

Latest hardware validation, 2026-05-28:

- Command shape:

  ```bash
  DISPLAY=:1 \
    XAUTHORITY=/run/user/1000/gdm/Xauthority \
    XDG_RUNTIME_DIR=/run/user/1000 \
    XDG_SESSION_TYPE=x11 \
    ./scripts/run_gui_fourcam_external_ipc_validation.sh --hidden-crop-preview
  ```
- This profile script expands to the `100_cam4_ptp_fourcam` config, expected
  cameras `2010093,2010094,2010095,2010096`, full-frame external IPC, crop
  external IPC, separate crop recorder GPU placement
  `2010093->4`, `2010094->2`, `2010095->8`, `2010096->6`, PTP register-read
  decimation `100`, `ORANGE_GUI_SWAP_INTERVAL=0`, and
  `ORANGE_GUI_FRAME_MAX_FPS=60`.
- Artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_00_34_27`.
- The launcher auto-forwarded `ORANGE_CROP_FRAME_POOL_SIZE=128`.
- Full validator passed with `0` warnings using the hidden-crop-preview command
  printed by the launcher, with `--min-crop-frame-pool-size 128`.
- All four cameras recorded `1016` full-frame external IPC frames and `1016`
  crop frames/metadata rows; crop perf rows, keyframe sidecars, MP4 frame
  counts, and YOLO rows all matched.
- External crop recorders received/encoded `1016/1016` frames per camera with
  `0` drops. Crop fanout matched detection rows, and
  `producer_crop_frame_pool_misses_total = 0` for every camera.
