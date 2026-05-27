# GUI Autorun Validation Plan

Status: deferred. The current priority is crop-frame fanout and display timing
diagnostics. GUI autorun is useful validation infrastructure, but it is not
expected to explain the current four-camera GUI FPS collapse by itself.

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

## Implementation Sketch

1. Resolve and select `ORANGE_GUI_CONFIG_DIR` during startup.
2. Reuse the existing local-config load path to select cameras with JSON files.
3. Extract the current button bodies into callable helpers:
   - open selected cameras
   - start streaming
   - start recording
   - stop recording
   - stop streaming and finalize
4. Add a small frame-tick state machine in `src/orange.cpp`.
5. Stop on preflight failure and leave the GUI open unless
   `ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE=1` was explicitly set.

## Validation

- `ORANGE_GUI_VALIDATE_ONLY=1 ./scripts/run_gui_aq_off_validation.sh` still only
  validates config and launcher environment.
- `ORANGE_GUI_PRINT_EXEC_ENV_ONLY=1` prints the autorun env values when set.
- A live autorun recording must produce the same artifacts as a manual GUI run:
  `recording_session.json`, full-frame external IPC outputs, crop outputs when
  enabled, GUI timing telemetry, and validator pass/fail output.

## Deferred Reason

The latest live runs were slow even with crop preview hidden and YOLO speed
graphs disabled. That makes crop-frame fanout and GUI timing analysis higher
yield than automation of the manual clicks. Autorun should come back after the
recording/display pipeline shape is stable enough that repeatability, not
architecture, is the main bottleneck.
