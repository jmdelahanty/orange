# Display Pipeline Notes (Current Behavior)

This document summarizes the current display path and where stalls can occur,
based on code inspection.

## UI/render loop

- Main UI loop is in `src/orange.cpp`:
  - `while (!glfwWindowShouldClose(...)) { create_new_frame(); ... render_a_frame(); }`
  - Per‑camera display runs under `if (camera_control->subscribe)`:
    - Uploads PBO → texture with `upload_texture_from_pbo(...)` (see `src/gui.h`).
    - Draws via `ImPlot::PlotImage(...)` in `src/orange.cpp`.

## Display worker thread

- Per‑camera display worker: `COpenGLDisplay::WorkerFunction` in `src/opengldisplay.cpp`.
- Worker consumes frames from a queue, keeps only the latest frame, and drops older ones.

## GPU/CPU data flow

### Normal (same GPU)
- Acquisition writes frames to GPU memory.
- Display worker copies `latest_frame->d_image` → `frame_original_gpu_.d_orig` (device→device).
- Debayer/duplicate on GPU into RGBA.
- Optional GPU overlay draw (`gpu_draw_box` in `src/kernel.cu`).
- Optional GPU downsample via NPP.
- Final GPU→GPU copy into mapped PBO (`display_buffer_pbo_cuda_ptr_`).
- UI thread uploads PBO to texture (OpenGL `glTexSubImage2D`).

### Cross‑GPU (acquire GPU != display GPU)
- Display worker uses host‑pinned staging:
  - `cudaMemcpyDeviceToHost` (acquire GPU → CPU pinned)
  - `cudaMemcpyHostToDevice` (CPU pinned → display GPU)
- This round‑trip is a likely stutter source at high resolution / high FPS.

## Potential stalls / sync points

- `cudaStreamWaitEvent` on acquisition completion in display worker.
- Optional wait for YOLO completion event (`yolo_completion_event`).
- Busy‑wait on CPU for `latest_frame->detections_ready` when YOLO overlays are enabled.
- `cudaStreamSynchronize(m_stream)` in display worker after the PBO write.
- Cross‑GPU CPU round‑trip when `camera_params->gpu_id != display_gpu_id`.

## CPU display path

- No `glReadPixels` or explicit CPU readback for display.
- OpenGL upload uses PBO and `glTexSubImage2D` (see `src/gx_helper.h`).

## Where to look in code

- UI loop: `src/orange.cpp`
- Display worker: `src/opengldisplay.cpp`
- PBO/texture helpers: `src/gui.h`, `src/gx_helper.h`
- GPU overlay kernels: `src/kernel.cu`

## Optimization ideas (incremental)

- Cap UI draw rate (e.g., render at 30 Hz) and always display the latest frame.
- Skip YOLO overlay wait when display latency matters:
  - Use `ORANGE_DISPLAY_SKIP_YOLO_WAIT=1` to avoid CPU busy‑wait.
- Avoid cross‑GPU display if possible:
  - Prefer running the display on the same GPU as acquisition (or consolidate cameras by GPU).
- Replace CPU busy‑wait with an event/condition variable to avoid burning a core.
- Reduce per‑frame work:
  - Lower display resolution (downsample).
  - Disable overlays for non‑focused cameras.
- Drop frames aggressively:
  - Keep only the newest frame in the display queue (already done), and cap queue size.
- Consider PBO upload throttling:
  - Only call `upload_texture_from_pbo` when a new display frame is available.
- Avoid full‑frame `cudaStreamSynchronize` in the display worker:
  - Use async GL sync or a fence and only sync when needed for the next frame.

## Profiling checklist

- Measure UI FPS (`ImGui::GetIO().Framerate`) and streaming FPS simultaneously.
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

## TODO: Recording toggle behavior

- Recording restart within a live stream can yield an output file with no visible frames in some players.
  This does not happen if the stream is fully stopped and restarted.
- Suspected cause: the encoder/file pipeline does not force an IDR/keyframe on the first frame of a new recording,
  so the resulting file lacks an initial decodeable frame.
- Follow‑ups to investigate:
  - Force IDR on recording start (per stream).
  - Consider reinitializing the encoder on each record start.
