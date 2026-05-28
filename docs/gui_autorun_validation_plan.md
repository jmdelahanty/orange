# GUI Autorun Validation Plan

Status: first internal state-machine slice in progress. The immediate goal is
to let an agent launch the real GUI, run the same open/stream/record/finalize
lifecycle, and validate the artifact without manual clicks.

Last updated: 2026-05-27.

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
  `ORANGE_GUI_CONFIG_DIR` crossing the `sudo env` boundary.
- A live autorun recording must produce the same artifacts as a manual GUI run:
  `recording_session.json`, full-frame external IPC outputs, crop outputs when
  enabled, GUI timing telemetry, and validator pass/fail output.
