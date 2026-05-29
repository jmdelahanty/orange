# Display Pipeline Notes

Last updated: 2026-05-28.

This document summarizes the current GUI display path and where stalls can
still occur, based on code inspection and four-camera GUI validation runs.
After the 2026-05-27 downsample/preview work and external crop-recorder slice,
crop artifacts were healthy with crop preview hidden, but the GUI still ran
near `25 fps` while recording four cameras. The 2026-05-28 GUI timing pass
found two dominant UI-side causes:

- the advanced split-GOP validation panel was running expensive GPU-topology
  checks every frame; it now runs those checks only when the tree is expanded
- `swap_interval=1` left `render_present_ms` dominated by vblank/compositor
  waits; the validation launcher now uses `ORANGE_GUI_SWAP_INTERVAL=0` with
  `ORANGE_GUI_FRAME_MAX_FPS=60` so no-vsync validation is explicitly capped

## UI/render loop

- Main UI loop is in `src/orange.cpp`:
  - `while (!glfwWindowShouldClose(...)) { create_new_frame(); ... render_a_frame(); }`
  - Per‑camera display runs under `if (camera_control->subscribe)`:
    - Uploads PBO to texture with `upload_texture_from_pbo(...)` only when the
      display worker publishes a new preview serial.
    - Draws main camera textures with `ImGui::Image(...)`.
    - Draws crop preview textures with `ImGui::Image(...)`.

The main display no longer uses `ImPlot::PlotImage`. The display is an image
preview, not a plot, so avoiding ImPlot removes unnecessary plot setup and
interaction work.

## Display Resolution And Cadence

- GUI stream downsample defaults to `4`. Persistent direct-launch defaults can
  be set with `gui.stream.downsample` in app config.
- `ORANGE_GUI_STREAM_DOWNSAMPLE=<1|2|4|8|16>` overrides the app-config/default
  value for the GUI display preview. `ORANGE_DISPLAY_DOWNSAMPLE` is accepted as
  a legacy alias.
- The `downsample streaming` combo controls display preview size only. It does
  not change acquisition, YOLO input, full-frame recording, or crop recording.
- The downsample value is fixed while streaming because GL textures and display
  worker buffers are allocated when streaming starts.
- `CameraEachSelect.display_preview_max_fps` still defaults to `60`, but the
  four-camera validation launcher sets `ORANGE_DISPLAY_PREVIEW_MAX_FPS=15` by
  default after the `30 fps` preview cap still left the GUI near `25 fps`.
  `ORANGE_DISPLAY_MAX_FPS` remains a legacy alias.
- Direct Orange launches default to `ORANGE_GUI_SWAP_INTERVAL=1` and
  `ORANGE_GUI_FRAME_MAX_FPS=0` unless overridden. The validation launcher
  defaults to the fast profile: `ORANGE_GUI_SWAP_INTERVAL=0`,
  `ORANGE_GUI_FRAME_MAX_FPS=60`, and `ORANGE_DISPLAY_PREVIEW_MAX_FPS=15`; this
  avoids vblank stalls without spinning the GUI loop at hundreds of FPS. If
  Citrus is using the same display GPU for `120 Hz` stimulus generation, use
  `scripts/run_gui_fourcam_external_ipc_validation.sh --citrus-display-safe`.
  That profile defaults Orange to `ORANGE_GUI_SWAP_INTERVAL=1`,
  `ORANGE_GUI_FRAME_MAX_FPS=30`, and `ORANGE_DISPLAY_PREVIEW_MAX_FPS=10`.
- Persistent workstation defaults can live in
  `~/orange_data/config/app/default.json` under `gui.display`, `gui.stream`,
  and `gui.telemetry`. Supported display profiles are `default`, `fast`, and
  `citrus_safe`; explicit `display_preview_max_fps`, `swap_interval`, and
  `frame_max_fps` values override the profile. Runtime env/launcher values
  still take precedence over app config for one-off validation runs. Use this
  to apply the current Orange/Citrus co-run default:

```bash
scripts/update_app_config_display_profile.py \
  --profile citrus_safe \
  --stream-downsample 4 \
  --hide-speed-graphs \
  --crop-recording-sink-mode external_ipc \
  --crop-external-encode-queue-depth 128 \
  --crop-frame-pool-size 256 \
  --crop-external-recorder-gpu 2010093=4 \
  --crop-external-recorder-gpu 2010094=2 \
  --crop-external-recorder-gpu 2010095=8 \
  --crop-external-recorder-gpu 2010096=6
```

- `gui.telemetry.show_speed_graphs=false` is the recommended performance
  default. `ORANGE_GUI_SHOW_SPEED_GRAPHS` overrides it for a single run. Set it
  to `1` only when live per-camera ImPlot speed graphs are needed during
  recording.
- Orange's own render helper caches the main window framebuffer size after
  initialization and updates it through the GLFW framebuffer-size callback, so
  `render_a_frame(...)` no longer calls `glfwGetFramebufferSize(...)` every
  frame. The Dear ImGui GLFW backend is compiled with Orange's size-cache shim,
  so its main-window `ImGui_ImplGlfw_NewFrame()` display-size path reads cached
  window/framebuffer dimensions instead of polling GLFW every frame.
- Display preview cadence is enforced before a frame is offered to the display
  worker. Skipped display frames are preview skips, not acquisition or
  recording drops.

## Display worker thread

- Per‑camera display worker: `COpenGLDisplay::WorkerFunction` in `src/opengldisplay.cpp`.
- Worker consumes frames from a queue, keeps only the latest frame, and drops older ones.
- Worker increments `PreviewSerial()` only after the CUDA work that updates the
  PBO has completed. The GUI uses that serial to avoid redundant PBO uploads.

## GPU/CPU data flow

### Fast mono/no-overlay path

Most current production cameras are Mono8. When the camera is mono and there is
no detection overlay to draw:

- Acquisition writes frames to GPU memory.
- Display worker copies the source mono frame to the display GPU staging frame.
- If display downsample is greater than `1`, NPP resizes the mono frame first.
- A small CUDA kernel converts the downsampled mono frame directly to RGBA in
  the mapped OpenGL PBO.
- The worker synchronizes the display stream, then publishes a new preview
  serial.

This avoids expanding the full `4512x4512` frame to full-resolution RGBA when
the GUI only needs a downsampled preview.

### Overlay/color fallback path

When the source is color, or YOLO detection overlays are enabled and available:

- Acquisition writes frames to GPU memory.
- Display worker copies `latest_frame->d_image` → `frame_original_gpu_.d_orig` (device→device).
- Debayer/duplicate on GPU into RGBA.
- Optional GPU overlay draw (`gpu_draw_box` in `src/kernel.cu`).
- Optional GPU downsample via NPP.
- Final GPU→GPU copy into mapped PBO (`display_buffer_pbo_cuda_ptr_`).
- Worker synchronizes the display stream, then publishes a new preview serial.
- UI thread uploads PBO to texture only when that serial changes.

### Cross‑GPU (acquire GPU != display GPU)
- Display worker uses host‑pinned staging:
  - `cudaMemcpyDeviceToHost` (acquire GPU → CPU pinned)
  - `cudaMemcpyHostToDevice` (CPU pinned → display GPU)
- This round‑trip is still a likely stutter source at high resolution / high
  FPS. The mono fast path reduces RGBA expansion and texture upload size, but
  it does not yet eliminate the full mono source transfer to the display GPU.

## Potential stalls / sync points

- `cudaStreamWaitEvent` on acquisition completion in display worker.
- Optional wait for YOLO completion event (`yolo_completion_event`).
- Busy‑wait on CPU for `latest_frame->detections_ready` when YOLO overlays are enabled.
- `cudaStreamSynchronize(m_stream)` in display worker after the PBO write.
- Cross‑GPU CPU round‑trip when `camera_params->gpu_id != display_gpu_id`.
- OpenGL presentation/compositor pressure, especially when the desktop output
  resolution is high or the GUI is waiting on vsync.
- Per-camera ImPlot speed graphs in the recording branch. These are now opt-in
  because they only appear once recording starts and can make the measured GUI
  frame rate look like a crop/recording problem even when artifacts are clean.
- Advanced split-GOP validation in the recording panel. This now runs only
  when the operator expands `Advanced Recording Validation`; keep it collapsed
  during performance validation.

## CPU display path

- No `glReadPixels` or explicit CPU readback for display.
- OpenGL upload uses PBO and `glTexSubImage2D` (see `src/gx_helper.h`).

## Where to look in code

- UI loop: `src/orange.cpp`
- Display worker: `src/opengldisplay.cpp`
- PBO/texture helpers: `src/gui.h`, `src/gx_helper.h`
- GPU overlay kernels: `src/kernel.cu`
- Display frame-rate telemetry: `src/gui_display_frame_rate.h`
- Acquisition-side display cadence: `src/acquire_frames.cpp`
- GUI phase timing telemetry is written under
  `recording_snapshot.json session.gui_display_frame_rate.timings` and includes
  frame total, main/crop texture upload, camera/crop window draw, speed graph
  draw, and render/present buckets.
- The ImGui GLFW size-cache shim writes recording-scoped counters under
  `recording_snapshot.json session.gui_display_frame_rate.imgui_glfw_size_cache`.
  Validation can require cache hits with no fallback size polling via
  `scripts/validate_gui_ptp_recording.py --require-imgui-glfw-size-cache`.

## Optimization ideas (incremental)

- Keep `ORANGE_GUI_SHOW_SPEED_GRAPHS=0` for performance validation. Re-enable
  with `ORANGE_GUI_SHOW_SPEED_GRAPHS=1` only for operator diagnostics that need
  the graphs.
- Keep validation runs explicitly paced with `ORANGE_GUI_FRAME_MAX_FPS=60`.
  `ORANGE_GUI_SWAP_INTERVAL=0` without a frame cap can consume unnecessary
  display-GPU time and should not be used while Citrus stimulus generation is
  timing-critical on the same GPU.
- Raise `ORANGE_DISPLAY_PREVIEW_MAX_FPS` only when a smoother live preview is
  more important than GUI control responsiveness.
- Skip YOLO overlay wait when display latency matters:
  - Use `ORANGE_DISPLAY_SKIP_YOLO_WAIT=1` to avoid CPU busy‑wait.
- Avoid cross‑GPU display if possible:
  - Prefer running the display on the same GPU as acquisition (or consolidate cameras by GPU).
- Replace CPU busy‑wait with an event/condition variable to avoid burning a core.
- Reduce per‑frame work:
  - Raise GUI display downsample from `4` to `8`.
  - Disable overlays for non‑focused cameras.
- Drop frames aggressively:
  - Keep only the newest frame in the display queue (already done), and cap queue size.
- Move display preprocessing to the acquisition GPU and transfer only the
  downsampled preview to display GPU. This is the next likely high-yield slice
  if the current cross-GPU transfer remains expensive.
- Avoid full‑frame `cudaStreamSynchronize` in the display worker:
  - Use async GL sync or a fence and only sync when needed for the next frame.

## Profiling checklist

- Measure UI FPS (`ImGui::GetIO().Framerate`) and streaming FPS simultaneously.
- Confirm `recording_snapshot.json` `session.gui_display_frame_rate` reports
  the intended `stream_downsample`, `display_preview_max_fps`,
  `swap_interval`, and `frame_max_fps`.
- Confirm `session.gui_display_frame_rate.imgui_glfw_size_cache` reports
  window/framebuffer cache hits and zero fallback calls when validating the
  ImGui backend size-cache optimization.
- Inspect `session.gui_display_frame_rate.timings` first:
  - high `main_texture_upload_ms` points at PBO upload/texture transfer,
  - high `camera_window_draw_ms` points at ImGui image/window drawing,
  - high `render_present_ms` points at OpenGL render, swap, vsync, or compositor
    pressure.
- Inspect cross‑GPU transfers:
  - Log `display_same_gpu_frames_` vs `display_cross_gpu_frames_` in `COpenGLDisplay`.
- Check for display thread stalls:
  - Time spent in `cudaStreamSynchronize(m_stream)` and YOLO busy‑wait.
- Verify if YOLO overlays are gating display:
  - Toggle `cameras_select[i].yolo` and compare stutter.
- Watch CPU utilization:
  - Busy‑wait can peg a core; confirm with `top` or similar.
- GPU profiling:
  - Use Nsight Systems to identify sync points and device↔host transfers.

## Recording toggle behavior

- Recording restart within a live stream previously risked an output file whose
  first decodable picture depended on frames from the prior recording.
- The current encoder path forces IDR/SPS/PPS on recording start and on each
  rolling clip boundary, so each new MP4 should start with keyframe frame `0`
  without requiring the stream to stop.
- GUI/external rolling validation now checks each full-frame and crop clip
  keyframe sidecar for `keyframe_frames[0] == 0`, in addition to frame-count
  and `recording_frame_id` continuity checks.
