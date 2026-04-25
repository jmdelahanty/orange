# NVENC Async And Pipelined Encoding On Linux

Date: 2026-04-24

Scope: determine what NVIDIA NVENC asynchronous encoding means on Linux for the
real-time multi-camera acquisition pipeline, and recommend the practical
threading model for camera capture, GPU preprocess, TensorRT inference, NVENC
encoding, and disk writing.

## Bottom Line

Linux does not support NVENC `enableEncodeAsync` / event-driven asynchronous
completion mode.

Linux can still implement pipelined NVENC encoding with synchronous API calls,
worker threads, bounded queues, and multiple in-flight input/output buffers.
That is not NVIDIA's Windows event-driven async mode. It is synchronous NVENC
used in a producer/consumer pipeline.

Short answers:

- Does Linux support NVENC `enableEncodeAsync` / event-driven async mode? No.
- Can Linux still implement pipelined non-blocking-ish NVENC encoding with
  worker threads and in-flight frames? Yes.

## Runtime Evidence From This Repo

Latest relevant run:

- `/home/jeremy/orange_data/exp/unsorted/2026_04_24_19_34_59`
- `/tmp/orange_yolo_detach_nsys_20260424_193425.sqlite`

Run conditions:

- real full-frame recording sink,
- full-frame `Cam<serial>.mp4` artifacts present,
- crop MP4 artifacts present,
- about `100 fps` on both cameras,
- no camera drops,
- no preprocess drops,
- split-GOP `hybrid_split` active.

The runtime behavior matches the Linux synchronous/pipelined model:

- full-frame `nvEncEncodePicture` is usually fast,
- full-frame `nvEncLockBitstream` is the blocking output completion boundary,
- bitstream host copy and FFmpeg mux/write are small by comparison,
- YOLO CUDA launch delay correlates with libcuda runtime lock waits, not the
  YOLO preprocess GPU kernel itself.

Measured highlights:

- full-frame `NVENC lock bitstream p95 = 11.755 ms`, max `13.115 ms`
- full-frame `NVENC bitstream host copy p95 = 0.021 ms`
- `FFmpeg av_interleaved_write_frame p95 = 0.334 ms`
- `SharedRecordingOutput submit frame p95 = 0.373 ms`
- YOLO `cudaLaunchKernel_v7000 p95 = 8.256 ms`
- YOLO `pthread_rwlock_rdlock p95 = 8.145 ms`

Route split:

- source route full-frame encode/output p95 was about `3.3-3.6 ms`,
- helper route full-frame encode/output p95 was about `12.6-12.8 ms`.

Current conclusion:

```text
The dominant full-frame recording stall is helper-route nvEncLockBitstream.
The current EncodeFrame() submit+harvest coupling puts that Linux synchronous
completion boundary directly in the hardware encoder worker.
```

The light Nsight run was enough to capture useful OSRT callchains:

- YOLO: `pthread_rwlock_rdlock -> libcuda -> libcudart ->
  cudaLaunchKernel -> launch_optimized_yolo_preprocess -> YoloWorker`
- encoder: `poll -> libnvcuvid/libnvidia-encode ->
  nvEncLockBitstream -> NvEncoder::GetEncodedPacket`

Heavy Nsight mode is therefore not the next default step. Use it only if a
future trace no longer explains the blocking path.

## API-Level Finding

NVIDIA exposes `NV_ENC_INITIALIZE_PARAMS::enableEncodeAsync`, completion events,
and `NV_ENC_EVENT_PARAMS` in the API headers, but the Linux support boundary is
explicit.

Local header evidence:

- `nvenc_api/include/nvEncodeAPI.h:2695` says:
  "Asynchronous mode is not supported on Linux."
- `third_party/NvEncoder/include/nvEncodeAPI.h:2695` contains the same note.
- `third_party/NvEncoder/include/nvEncodeAPI.h:1678` documents that
  `enableEncodeAsync` enables asynchronous mode and expects events.
- `third_party/NvEncoder/include/nvEncodeAPI.h:1927` documents the output
  buffer `completionEvent` field for asynchronous mode.
- `third_party/NvEncoder/include/nvEncodeAPI.h:2229` defines
  `NV_ENC_EVENT_PARAMS`.
- `third_party/NvEncoder/include/nvEncodeAPI.h:2000` documents blocking
  `nvEncLockBitstream` behavior: "the call will block until operation
  completes".
- `third_party/NvEncoder/include/nvEncodeAPI.h:546` documents
  `NV_ENC_ERR_LOCK_BUSY` for `doNotWait` polling.
- `third_party/NvEncoder/include/nvEncodeAPI.h:571` documents
  `NV_ENC_ERR_NEED_MORE_INPUT`, which can occur when output is delayed by
  reorder buffering.

Installed/local SDK evidence:

- `/opt/nvidia/Video_Codec_SDK` was not present on this machine.
- `/usr/local/cuda/include` exists but does not include `nvEncodeAPI.h`.
- `/usr/local/include/ffnvcodec/nvEncodeAPI.h` contains the same Linux async
  exclusion.
- `/usr/local/include/nvEncodeAPI.h` exists but is zero bytes.
- A local SDK copy exists at
  `/home/jeremy/Downloads/Video_Codec_SDK_12.1.14`.
- That SDK's `Interface/nvEncodeAPI.h` and sample `NvEncoder.cpp` match the
  local wrapper behavior.

NVIDIA documentation evidence:

- The NVIDIA Video Codec SDK 13.0 NVENC Programming Guide documents
  synchronous encode mode for Windows, Linux, and Jetson Linux, while async
  mode is Windows 10+ only.
- The same guide says Linux must select synchronous mode by setting
  `NV_ENC_INITIALIZE_PARAMS::enableEncodeAsync` to `0`.
- The Linux synchronous path uses `NvEncLockBitstream` as the completion point.

Official references:

- NVIDIA Video Codec SDK 13.0 NVENC Programming Guide:
  <https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html>
- NVIDIA Video Codec SDK overview:
  <https://developer.nvidia.com/video-codec-sdk>

## Windows Event Async Versus Linux Pipelining

Windows event-driven async mode:

- Initialize with `enableEncodeAsync = 1`.
- Allocate/register completion events for output buffers.
- Submit frames with `nvEncEncodePicture`.
- Wait for per-output-buffer completion events from another thread.
- Lock/copy completed bitstreams after the event is signaled.

Linux synchronous/pipelined mode:

- Initialize with `enableEncodeAsync = 0`.
- Submit frames with `nvEncEncodePicture`.
- Preserve output order.
- Retrieve bitstreams with `nvEncLockBitstream`.
- Let a dedicated retrieval thread block in `nvEncLockBitstream`, or poll with
  `NV_ENC_LOCK_BITSTREAM::doNotWait = 1` and retry after
  `NV_ENC_ERR_LOCK_BUSY`.

The polling form is not equivalent to Windows async completion events. It is a
CPU scheduling choice for when and where the blocking/retry work happens.

## Practical Linux Behavior

`nvEncEncodePicture` can be used as the submit side of a pipeline. NVIDIA's
threading guidance treats the encode submit call as work that should be issued
by one thread while a different thread handles output locks.

`nvEncLockBitstream` is the real output completion boundary on Linux. With
`doNotWait = false`, it blocks until the encoded bitstream is available. With
`doNotWait = true`, it can return `NV_ENC_ERR_LOCK_BUSY`; the application must
retry later.

Multiple frames can be in flight if the application allocates enough registered
input surfaces, output buffers, and queue slots. Output must still be harvested
in submission order unless the implementation has a carefully verified reason
to do otherwise.

NVENC hardware can overlap with CUDA and TensorRT work because the encoder is a
dedicated hardware block. That does not eliminate all contention. Some NVENC
features use CUDA internally, including RGB-to-YUV conversion, lookahead,
adaptive quantization, weighted prediction, temporal filtering, and multi-pass
encoding. Host-side CUDA/NVENC driver calls can also contend even when GPU work
uses separate CUDA streams.

## FFmpeg Evidence

FFmpeg is useful operational evidence but not the authority on NVIDIA API
support.

Current FFmpeg NVENC source initializes `enableEncodeAsync = 0`, exposes queue
depth through an `async_depth` style setting, and locks output bitstreams with
blocking `nvEncLockBitstream` behavior. This matches the Linux synchronous
pipeline model, but the NVIDIA documentation and headers are the authoritative
source for the support boundary.

Reference:

- FFmpeg NVENC source:
  <https://ffmpeg.org/doxygen/trunk/nvenc_8c_source.html>

No local FFmpeg `libavcodec/nvenc*` source tree was found during the search.
Installed FFmpeg binaries/docs exist under `/opt/orange/lib/ffmpeg-nvidia`.

## Relevant Local Code Paths

Build and API wiring:

- `CMakeLists.txt` includes and links the local NVENC wrapper/API.
- `nvenc_api/include/nvEncodeAPI.h` is the local NVENC API header.
- `third_party/NvEncoder/include/nvEncodeAPI.h` is the SDK-style wrapper header.

NVENC wrapper:

- `src/NvEncoder/NvEncoder.cpp:127` sets `enableEncodeAsync` only under
  `_WIN32`.
- `src/NvEncoder/NvEncoder.cpp:279` creates/registers async events only under
  `_WIN32`.
- `src/NvEncoder/NvEncoder.cpp:425` calls `DoEncode()` and then
  `GetEncodedPacket(...)` in the same `EncodeFrame()` path.
- `src/NvEncoder/NvEncoder.cpp:530` calls `nvEncEncodePicture`.
- `src/NvEncoder/NvEncoder.cpp:600` configures
  `NV_ENC_LOCK_BITSTREAM::doNotWait = false`.
- `src/NvEncoder/NvEncoder.cpp:625` calls `nvEncLockBitstream`.
- `src/NvEncoder/NvEncoder.cpp:846` has the completion-event wait hook, but it
  is effective only for Windows async mode.
- `src/NvEncoder/NvEncoderCuda.cpp:112` calls `nvEncSetIOCudaStreams`.

Current pipeline:

- `src/modern_recording_pipeline.cpp` wires the modern recording path.
- `src/recording_ingress.cpp` receives/acquires frames into the recording path.
- `src/encoder_preprocess_worker.cpp` handles GPU preprocess and records CUDA
  completion events.
- `src/encoder_hw_worker.cpp` waits on preprocess completion and calls the
  NVENC wrapper.
- `src/shared_recording_output.cpp` and `src/FFmpegWriter.cpp` handle output
  packet writing.
- `src/threadworker.h` provides the queue/worker infrastructure.

Important current behavior:

- The repo already has a staged recording pipeline with queues, preprocess
  workers, hardware encoder workers, and writer threads.
- The current `NvEncoder::EncodeFrame()` path still couples encode submit and
  bitstream retrieval. On Linux, that means the encoder worker can block inside
  `nvEncLockBitstream`.
- `FFmpegWriter` is separated after encoded packets exist, but it does not
  remove the blocking NVENC bitstream lock from the encoder worker.

## Recommended Architecture

Use Linux synchronous NVENC as a pipelined subsystem:

```text
camera capture thread
  -> GPU upload/preprocess
  -> TensorRT inference
  -> encode-submit queue
  -> NVENC submit thread
  -> bitstream retrieval thread
  -> disk writer
```

Recommended details:

1. Camera/acquisition threads should not call NVENC output retrieval APIs.
2. GPU preprocess should write into a bounded ring of NVENC-registered CUDA or
   NV12 surfaces.
3. Each preprocessed surface should carry a CUDA completion event or equivalent
   readiness signal.
4. The NVENC submit thread should wait on the relevant readiness signal, call
   `nvEncEncodePicture`, and enqueue an output token.
5. The submit thread should not call `nvEncLockBitstream`.
6. A dedicated bitstream retrieval thread should consume output tokens in
   encode submission order and call `nvEncLockBitstream`.
7. The retrieval thread should copy/packetize the bitstream, unlock the
   bitstream, unmap or retire the input surface, and return the slot to the
   free pool.
8. The disk writer should remain a separate bounded queue/thread after encoded
   packets are materialized.

For low-latency no-B-frame/no-lookahead recording, start with 4 to 8 in-flight
registered input/output slots per camera and tune from measurements. Increase
depth only when measurements show NVENC stalls or disk jitter require it,
because every extra queued frame can add latency.

If B-frames, lookahead, or multi-pass modes are enabled, increase the queue
depth to cover reorder/output delay and measure the effect on TensorRT/CUDA
submission latency. Avoid CUDA-heavy NVENC features on the inference GPU unless
quality measurements justify the contention.

## Design Implications For This Repo

The most useful implementation experiment is not trying to enable
`enableEncodeAsync` on Linux. That path is unsupported.

The immediate next experiment should be a cheaper output-depth diagnostic
before implementing the full submit/harvest split.

Current full-frame encoder buffer count is `4`, derived from:

```text
frameIntervalP + lookaheadDepth + nExtraOutputDelay = 1 + 0 + 3
```

Implemented environment-controlled full-frame NVENC extra-output-delay setting:

```text
ORANGE_NVENC_EXTRA_OUTPUT_DELAY=<3|7|11|15>
```

Optional per-camera overrides:

```text
ORANGE_NVENC_EXTRA_OUTPUT_DELAY_CAM_2010095=<3|7|11|15>
ORANGE_NVENC_EXTRA_OUTPUT_DELAY_CAM_2010096=<3|7|11|15>
```

The setting applies only to the full-frame `EncoderHwWorker`; crop encoders keep
their default depth so the diagnostic isolates full-frame recording.

This should produce effective encoder buffer counts of about:

```text
4, 8, 12, 16
```

Test depth first because it answers a concrete question:

```text
Are helper-route bitstreams being locked too early, forcing the encoder worker
to wait in nvEncLockBitstream?
```

Depth `8` was tested with `ORANGE_NVENC_EXTRA_OUTPUT_DELAY=7`.

Result:

- full-frame and crop artifacts stayed healthy,
- `nvEncLockBitstream` mean fell from about `4.5-4.6 ms` to about
  `0.25-0.26 ms`,
- `NVENC EncodeFrame p95` improved from about `11.878 ms` to about
  `4.448 ms`,
- YOLO `cudaLaunchKernel` / `cpu_preprocess_ms` p95 did not improve,
- helper-route full-frame encode/output became burstier.

Conclusion:

- extra output depth proves Linux NVENC can be pipelined enough to avoid
  blocking most `nvEncLockBitstream` calls,
- output depth alone is not the production fix for the current YOLO p95 tail,
- GOP-sized or larger buffer counts should not be pursued unless a later
  measurement shows a specific queue-depth requirement.

The resolved value is recorded in `recording_snapshot.json` under the encoder
snapshot:

- `nvenc_extra_output_delay`
- `encoder_buffer_count`
- `resolved_config.buffers.nvenc_extra_output_delay`

The useful experiment is to split the current `EncodeFrame()` behavior into:

```text
submit encoded frame
  -> return quickly after nvEncEncodePicture

harvest encoded frame
  -> block or poll in nvEncLockBitstream on a separate thread
```

That change would move the Linux blocking point out of the hardware encoder
submit worker and make it easier to isolate NVENC output stalls from capture,
preprocess, and inference scheduling.

Important caveat:

- The repo now instruments the helper-route substeps before the split:
  `encoder_cuda_set_device`,
  `preprocess_complete_stream_wait_enqueue`,
  `source_to_helper_copy_sync_wait`,
  `source_to_helper_copy_elapsed_query`,
  `pre_encoder_reference_capture_enqueue`,
  `nvenc_get_next_input_frame`,
  `nvenc_encode_frame_total`, and
  `encoder_output_accounting`.
- If `nvEncLockBitstream` still causes YOLO to block inside libcuda after the
  lock is moved to a separate in-process harvest thread, the remaining issue is
  likely process-level CUDA/NVENC driver contention.
- In that case, process isolation for full-frame encode/output becomes the
  stronger architecture candidate.

If host-side CUDA/NVENC driver contention remains dominant after separating
submit and harvest, the stronger isolation candidate is process-level
separation for full-frame encode/output, especially for split-GOP multi-camera
recording.
