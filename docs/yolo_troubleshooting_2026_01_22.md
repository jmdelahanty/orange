# YOLO Pipeline Troubleshooting Notes (2026-01-22)

This document summarizes the YOLO pipeline jitter/queue investigation, what we measured, the root causes we found, and why the fixes worked.

## Problem Summary

- Multi-camera YOLO (20MP, 60 FPS input) showed intermittent enqueue/infer spikes and queue depth growth.
- The queue was stable at decimate=2, but at decimate=1 or during recording, CPU-side latency balloons appeared.
- GPU utilization (SM/shared memory) looked low, so the observed latency did not align with GPU saturation.

## System Context

- Multi-camera system on A16 (multi-die GPUs; separate CUDA devices on the same card).
- Each camera assigned a distinct CUDA device (no shared device IDs).
- YOLO runs in per-camera worker threads, each with its own TensorRT context and CUDA stream.
- Recording pipeline uses NVENC with a GPU preprocess worker (NPP calls).

## Instrumentation Added

### YOLO per-frame timing
Added per-frame timing in `src/yolo_worker.cpp`:

- GPU event timing: `wait`, `pre`, `gap`, `enqueue`, `infer`, `sync`, `queue`, `post`, `track`, `ipc`, `enet`, `total`.
- CPU sub-timers (key for root cause):
  - `cpu_wait_event_ms`
  - `cpu_npp_set_stream_ms`
  - `cpu_preprocess_ms`
  - `cpu_dump_ms`
  - `cpu_infer_call_ms`
  - `cpu_event_record_ms`
  - `cpu_pre_sync_ms`
  - `cpu_pre_sync_other_ms`
  - `cpu_post_sync_ms`

### YOLO FPS and CSV logging
- Added per-camera YOLO FPS logging.
- Added async CSV logger (`yolo_perf` namespace) writing `Cam<serial>_yolo_perf.csv` into the recording folder.
- Logging controls:
  - `ORANGE_YOLO_PERF_LOG=0` disables logging
  - `ORANGE_YOLO_PERF_SAMPLE=N` logs every Nth frame

### Plot/Stats Script
`scripts/plot_yolo_perf.py` generates plots and summary stats (p50/p90/p95/p99/mean/min/max/stdev).

### Scheduling / CPU contention checks
`scripts/measure_schedstat.py` samples `/proc/<pid>/task/<tid>/schedstat` to estimate off-CPU time per YOLO worker.

## Key Findings

### 1) TensorRT enqueue contention (host-side mutexes)

Symptoms:
- `enqueue` spikes under load (multi-threaded inference).
- Nsight showed enqueue ~40ms under full pipeline load.

Resolution:
- Use CUDA graph capture by default (no enqueue at runtime).
- In `src/yolov8_det.cpp`, CUDA graph is now default unless `ORANGE_YOLO_CUDA_GRAPH=0`.
- `bind_tensors()` is called once; `setTensorAddress()` no longer runs every frame.

Why it worked:
- CUDA Graph captures the entire inference sequence and avoids TensorRT’s internal global locks during runtime.
- Binding tensor addresses once removes repeated host-side setup costs.

### 2) NPP stream switching stall (root cause for 12–40ms CPU delays)

Symptoms:
- `cpu_pre_sync_ms` ~12–30ms even when GPU event timings were tiny.
- `cpu_npp_set_stream_ms` spikes in the same range.
- Queue depth increases only during recording.

Root cause:
- NPP uses a global stream state. Per-frame `nppSetStream()` was called in multiple threads (YOLO, encoder preprocess, crop/encode, display), forcing internal synchronization (`cudaStreamSynchronize` on the previously set stream).

Fix:
- Cache NPP stream per thread and only call `nppSetStream()` when the stream changes.
  - Added `src/npp_utils.h` with `EnsureNppStream()`.
  - Replaced per-frame `nppSetStream()` in:
    - `src/yolo_worker.cpp`
    - `src/encoder_preprocess_worker.cpp`
    - `src/crop_and_encode_worker.cpp`
    - `src/opengldisplay.cpp`
    - `src/gpu_video_encoder.cpp`

Result:
- `cpu_npp_set_stream_ms` dropped to ~0.03ms median (p95 ~0.036ms).
- `cpu_pre_sync_ms` dropped to sub-ms median and low single‑ms p95.
- End-to-end `total_ms` stabilized around ~3.4ms mean (p95 < 4ms in latest run).

### 3) Host-side CPU contention with external renderer

Symptoms:
- Queue depth grew only when running the rendering engine that reads YOLO shared memory.
- Queue was stable when running YOLO alone.

Cause:
- Core affinity overlap with renderer threads (pinned cores).

Fix:
- Run Orange on a core mask that excludes renderer-pinned cores.
  - Example: `taskset -c 3,4,5,7-15 ./orange`
  - Or set `ORANGE_YOLO_AFFINITY="7-15"` for YOLO threads.

Result:
- Queue depth remained at 0; FPS stable ~60.

## Experimental Results (Post-Fix)

From `/home/jeremy/orange_data/exp/unsorted/2026_01_22_22_20_32`:

- `queue_depth`: 0 across all cameras.
- FPS: ~60.0 ± 0.01.
- `total_ms`: mean ~3.43–3.45ms, p95 ~3.86–3.98ms, p99 ~4.2–4.4ms.
- `enqueue_ms`: median ~0.05ms.
- `infer_ms`: median ~2.78ms.
- `sync_ms`: median ~2.63ms.

## Config/Flags Added or Relevant

- `ORANGE_YOLO_CUDA_GRAPH=0` disables graph capture (default is enabled).
- `ORANGE_YOLO_PERF_LOG=0` disables CSV logging.
- `ORANGE_YOLO_PERF_SAMPLE=N` logging sample rate.
- `ORANGE_YOLO_SKIP_CPU_RESULTS=1` bypasses postprocess/tracking/IPC/ENet.
- `ORANGE_YOLO_SYNC_EVENT=1` use `cudaEventSynchronize` instead of polling.
- `ORANGE_DISPLAY_SKIP_YOLO_WAIT=1` skip busy-wait for display overlay.
- `ORANGE_YOLO_AFFINITY="X-Y"` pin YOLO worker threads.
- `ORANGE_YOLO_ENQUEUE_MUTEX=1` enables global enqueue mutex (fallback option).
- `ORANGE_YOLO_STREAM_PRIORITY`, `ORANGE_YOLO_STREAM_NONBLOCKING` were tested; no strong improvements beyond graph capture.

## IPC Behavior Changes

- The “YOLO IPC” checkbox was removed (it was unused).
- Frame IPC is now enabled by default (`send_frame_ipc = true`).
- Frame IPC creates a per-camera shared memory queue `/shm_cam_<serial>`.

## Why the Fixes Worked

- **CUDA Graphs** remove TensorRT enqueue contention entirely.
- **NPP stream caching** removes a per-frame `cudaStreamSynchronize` hidden inside `nppSetStream()`.
- **CPU affinity isolation** prevents queue buildup caused by CPU starvation from another process.

## Future Options (If Needed)

1) **Use NPP `_Ctx` APIs**  
   Avoid global NPP stream state completely (stronger fix if needed).

2) **Separate processes per GPU die**  
   If host-side contention persists due to shared libraries, isolate by process.

3) **IPC sampling knob**  
   If the external consumer is still a bottleneck, add `ORANGE_FRAME_IPC_SAMPLE=N`.

4) **Encoder queue decoupling**  
   If recording impacts YOLO again, consider deeper encoder queues or separate stream priority.

---

### Files Modified (Core Fixes)

- `src/yolov8_det.cpp`: default CUDA graph capture, single `bind_tensors()`, optional enqueue mutex.
- `src/yolo_worker.cpp`: CPU timing, NPP caching, perf logging.
- `src/encoder_preprocess_worker.cpp`: NPP caching.
- `src/crop_and_encode_worker.cpp`: NPP caching.
- `src/opengldisplay.cpp`: NPP caching.
- `src/gpu_video_encoder.cpp`: NPP caching.
- `src/npp_utils.h`: `EnsureNppStream()` helper.
- `scripts/plot_yolo_perf.py`: stats output.
- `scripts/measure_schedstat.py`: off-CPU sampling helper.
- `src/video_capture.h` / `src/orange.cpp`: remove YOLO IPC checkbox, default Frame IPC on.

## Repro/Verification Checklist

1) Build with profiling:
   - `./build.sh --yolo-profile --nvtx`
2) Run:
   - `ORANGE_YOLO_CUDA_GRAPH=1 ORANGE_YOLO_PERF_SAMPLE=5 ./targets/release_nvtx/orange`
3) Check stats:
   - `python scripts/plot_yolo_perf.py <run_dir> --stats`
4) Confirm:
   - `cpu_npp_set_stream_ms` ~0
   - `queue_depth` stays 0
   - `total_ms` stable ~3–4ms
