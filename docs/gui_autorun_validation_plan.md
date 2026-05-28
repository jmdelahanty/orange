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

The existing launcher should forward these through the `sudo env` boundary.

Current launcher defaults:

- `ORANGE_GUI_AUTORUN=0`
- `ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS=3`
- `ORANGE_GUI_AUTORUN_RECORD_SECONDS=10`
- `ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=0`
- `ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW=0`

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
  -> start_streaming
  -> stream_warmup
  -> start_recording
  -> recording
  -> stop_recording
  -> wait_finalize
  -> stop_streaming
  -> done
```

## Validation

- `ORANGE_GUI_VALIDATE_ONLY=1 ./scripts/run_gui_aq_off_validation.sh` still only
  validates config and launcher environment.
- `ORANGE_GUI_PRINT_EXEC_ENV_ONLY=1` prints the autorun env values and
  `ORANGE_GUI_CONFIG_DIR` crossing the `sudo env` boundary. It also prints
  display/session variables that will be forwarded to the root-launched GUI.
- A live autorun recording must produce the same artifacts as a manual GUI run:
  `recording_session.json`, full-frame external IPC outputs, crop outputs when
  enabled, GUI timing telemetry, and validator pass/fail output.
