# Memory Lifetime and Pooling Review, 2026-06-05

This is a read-only static review of memory lifetime, pooling, and hot-path
allocation behavior in the camera/GPU pipeline. It distinguishes repeated
per-frame allocation from desirable preallocated heap/CUDA pools.

Classification terms:

- `confirmed hot-path allocation`: allocation is directly inside a frame,
  packet, worker, or descriptor loop.
- `likely hot-path allocation`: allocation is conditional on detections,
  optional logging, queue growth, or first high-water use.
- `setup/control-plane allocation`: allocation happens during startup,
  recording rollover, diagnostics, or rare operator actions.
- `needs runtime profiling`: static review can see a possible allocation or
  growth point, but real frequency and cost depend on runtime configuration.

## Executive Summary

The modern camera/GPU pipeline is mostly following the right pattern:
large frame buffers, CUDA events, crop frames, encoder preprocess entries, and
external recorder slots are generally pooled. Recent `WORKER_ENTRY`
retain/release hardening also made ownership transfer much clearer and safer.

The legacy `GPUVideoEncoder` color path had the biggest confirmed per-frame
allocation risk, but it has since been retired from the current GUI and
`orange_client` build targets. The next most important allocation sources are
per-packet FFmpeg/NVENC packet copying, small crop job object allocation,
per-frame external IPC string construction, YOLO event/IPC allocation when
detections or logs are enabled, and queue growth from `std::queue`/`std::deque`
based hot queues.

No large fixed-size stack buffers were found in the core acquisition, display,
YOLO, recording, crop, or IPC hot loops.

## Findings

### 1. Legacy GPUVideoEncoder Color Path Allocates Per Frame

Classification: `confirmed hot-path allocation`
Severity: retired from active builds; high only if this path is revived.

`src/gpu_video_encoder.cpp` allocates a temporary RGB buffer inside
`GPUVideoEncoder::WorkerFunction`:

- `src/gpu_video_encoder.cpp:556`: `cudaMalloc(&d_rgb_temp_, width * height * 3)`
- `src/gpu_video_encoder.cpp:594`: `cudaFree(d_rgb_temp_)`

The same worker also has per-frame `std::cout` logging around frame copy,
success, and recycle decisions:

- `src/gpu_video_encoder.cpp:522`
- `src/gpu_video_encoder.cpp:532`
- `src/gpu_video_encoder.cpp:725`
- `src/gpu_video_encoder.cpp:737`

This worker is no longer used by the current GUI or `orange_client` build
targets. Shared `Writer`/`EncoderContext` types were moved to
`src/recording_writer_types.h`, and `src/gpu_video_encoder.cpp` was removed from
the active target source lists. Treat this file as archival legacy code unless a
future task explicitly revives it.

### 2. In-Process Encoded Packet Writing Allocates Per Packet

Classification: `confirmed hot-path allocation`
Severity: medium; expected behavior, but relevant to latency.

`FFmpegWriter::push_packet` allocates an `AVPacket`, copies encoded bytes into
it, and queues it for the writer thread:

- `src/FFmpegWriter.cpp:181`: `av_packet_alloc`
- `src/FFmpegWriter.cpp:187`: `memcpy(pkt->data, pData, nBytes)`
- `src/FFmpegWriter.cpp:219`: queue push
- `src/FFmpegWriter.cpp:323`: packet free in writer thread

NVENC bitstream collection also grows/copies vectors:

- `src/NvEncoder/NvEncoder.cpp:740`: grows `vPacket`
- `src/NvEncoder/NvEncoder.cpp:748`: inserts bitstream bytes
- `src/NvEncoder/NvEncoder.cpp:859`: grows `vPacket`
- `src/NvEncoder/NvEncoder.cpp:867`: inserts bitstream bytes

This is normal FFmpeg/NVENC ownership plumbing, but it is confirmed per-packet
heap work in the in-process recording path. It reinforces the current direction
of external IPC/process isolation for production recording load.

### 3. Thread Queues Are Bounded, But Not Preallocated

Classification: `likely hot-path allocation` / `needs runtime profiling`
Severity: medium.

`CThreadWorker<T>` and `SafeQueue<T>` use `std::queue`:

- `src/threadworker.h:80`: `std::queue<T*> queueIn`
- `src/threadworker.h:181`: hot-path `PutObjectToQueueIn`
- `src/thread.h:31`: `SafeQueue::push`
- `src/thread.h:53`: `std::queue<T> queue`

Queue sizes are capped by policy, and many pools are fixed-size, but the
underlying container can allocate as a queue reaches new high-water marks.
This is usually a startup/high-water cost rather than steady allocation churn,
but it should be measured on real four-camera GUI/external IPC runs.

The same pattern appears in acquisition pending requeue tracking:

- `src/acquire_frames.cpp:1165`: `std::deque<PendingRequeue>`
- `src/acquire_frames.cpp:1851`: push for ring-copy requeue
- `src/acquire_frames.cpp:1863`: push for analytics-hybrid requeue

The pending depth is bounded by consumer progress, but not by a fixed-capacity
ring in the current static structure.

### 4. Crop Job Wrappers Are Allocated Per Job

Classification: `confirmed hot-path allocation`
Severity: medium.

The crop image buffers are pooled, but job wrappers are heap allocated:

- `src/crop_producer_worker.cpp:204`: `std::make_unique<CropEncodeJob>()`
- `src/crop_producer_worker.cpp:414`: `new CropPreviewJob()`
- `src/crop_producer_worker.cpp:427`: `new CropPreviewJob()`
- `src/crop_and_encode_worker.cpp:1175`: worker takes ownership through
  `std::unique_ptr<CropEncodeJob>`
- `src/crop_preview_worker.cpp:282`: worker takes ownership through
  `std::unique_ptr<CropPreviewJob>`

Preview is cadence limited, so the preview job allocation is lower risk. The
recording crop job allocation happens at crop worker rate and is a reasonable
small hardening target after the legacy encoder path is checked.

### 5. External IPC Descriptors Allocate Per Frame

Classification: `confirmed hot-path allocation`
Severity: low to medium.

Full-frame external IPC constructs frame descriptors with `std::ostringstream`:

- `src/recording_ingress.cpp:695`: local handle string
- `src/recording_ingress.cpp:717`: descriptor `std::ostringstream`
- `src/recording_ingress.cpp:746`: `send_all(msg.str())`

Crop external IPC does the same:

- `src/crop_and_encode_worker.cpp:142`: local handle string
- `src/crop_and_encode_worker.cpp:170`: descriptor `std::ostringstream`
- `src/crop_and_encode_worker.cpp:192`: `send_all(msg.str())`

CUDA IPC handle strings are cached by pointer, which is good:

- `src/recording_ingress.cpp:695-710`
- `src/crop_and_encode_worker.cpp:142-157`

If CPU-side jitter remains a problem, replace the descriptor stream with a
fixed reusable buffer or a binary descriptor protocol. This is not the first
priority because the absolute payload is small.

### 6. YOLO Positive-Detection and Event Paths Allocate

Classification: `likely hot-path allocation`
Severity: low to medium, depending on detections/logging/IPC.

`WORKER_ENTRY::detections` is reused, but static review did not find an
up-front reserve for the expected maximum detections:

- `src/video_capture.h:70`: `std::vector<pose::Object> detections`
- `src/yolo_worker.cpp:1697`: `postprocess(entry->detections)`
- `src/yolo_worker.cpp:1723`: synthetic detection `push_back`

When SHAMAN live IPC is enabled and detections exist, conversion creates a new
vector:

- `src/yolo_worker.cpp:1843`: `std::vector<shaman::Object> shaman_objects`

YOLO event logging copies detection vectors and builds JSON on a logger thread:

- `src/yolo_worker.cpp:1901`: `record.detections = entry->detections`
- `src/yolo_event_log.cpp:156`: JSON detections array
- `src/yolo_event_log.cpp:159`: JSON keypoints array
- `src/yolo_event_log.cpp:193`: JSON root object

This is not the inference hot loop itself, but it is per-frame side-channel
work during recording. It should be profiled with real detections before being
treated as a primary bottleneck.

### 7. Debug/Diagnostic Paths Allocate Large Buffers, But Not Normally

Classification: `setup/control-plane allocation`

The scan found 4 KiB and 64 KiB stack buffers in control-plane helpers, not in
camera frame hot loops:

- `src/orange_headless_client.cpp:954`: 4 KiB command stdout buffer
- `src/orange_headless_client.cpp:979`: 64 KiB command binary buffer
- `src/orange_local_control.cpp:983`: 4 KiB local-control buffer
- `src/project.cpp:1929`: 4 KiB config/control buffer
- `src/usaf_resolution_calibration.cpp:79`: 4 KiB calibration buffer

YOLO debug image dumping allocates a large host buffer, constructs OpenCV
objects, and writes a PNG, but only when a debug frame dump is requested:

- `src/yolo_worker.cpp:1543`: `dump_this_frame`
- `src/yolo_worker.cpp:1548`: `new unsigned char[image_size_bytes]`
- `src/yolo_worker.cpp:1551`: `cv::Mat`
- `src/yolo_worker.cpp:1557`: `cv::imwrite`

These are acceptable as diagnostics, not steady-state pipeline behavior.

## Good Practices Already Present

### Acquisition Frame and Event Pools

`CameraResources` preallocates acquisition work entries, device image buffers,
and event pools:

- `src/video_capture.h:258`: `new WORKER_ENTRY[acquire_work_entries_max]`
- `src/video_capture.h:260`: per-entry `cudaMalloc`
- `src/video_capture.h:262`: analytics event creation
- `src/video_capture.h:263`: YOLO input-ready event creation
- `src/video_capture.h:283`: event pool resize
- `src/video_capture.h:291`: YOLO event pool resize

Pool exhaustion is observable through acquisition resource starvation counters
in the acquisition loop.

### WorkerEntry Ownership Hardening

The ownership model is much safer than raw `fetch_add/fetch_sub`:

- `src/worker_entry_ownership_core.h:286`: retain only when `ref_count > 0`
- `src/worker_entry_ownership_core.h:330`: guarded release
- `src/worker_entry_ownership_core.h:341`: double-release/underflow detection
- `src/worker_entry_release.h:115`: `WorkerEntryLease`
- `src/worker_entry_release.h:173`: retain-and-enqueue helper

Acquisition now establishes a base ref and guards it:

- `src/acquire_frames.cpp:2017`: base `ref_count.store(1)`
- `src/acquire_frames.cpp:2018`: `WorkerEntryRefGuard`
- `src/acquire_frames.cpp:2070`: retain-and-enqueue YOLO
- `src/acquire_frames.cpp:2091`: retain-and-enqueue display
- `src/acquire_frames.cpp:2110`: recording `WorkerEntryLease`

This is the right convention for objects that are recycled, not deleted.

### Encoder Preprocess Pools and Source Release

`EncoderPreprocessWorker` preallocates direct-input surfaces, encoder entries,
CUDA events, and source-release events:

- `src/encoder_preprocess_worker.cpp:252`: reserve direct-input surfaces
- `src/encoder_preprocess_worker.cpp:256`: `cudaMallocPitch` per direct slot
- `src/encoder_preprocess_worker.cpp:292`: encoder entry pool resize
- `src/encoder_preprocess_worker.cpp:298`: prepared frame allocation
- `src/encoder_preprocess_worker.cpp:307`: event pool resize
- `src/encoder_preprocess_worker.cpp:325`: source-release event pool resize

Source `WORKER_ENTRY` release is event-backed, with a correctness-first stream
sync fallback:

- `src/encoder_preprocess_worker.cpp:646`: `release_source_after_stream_work`
- `src/encoder_preprocess_worker.cpp:661`: fallback stream sync
- `src/encoder_preprocess_worker.cpp:669`: source entry release

### Crop Producer Pools and Event-Backed Recycle

The crop producer preallocates source-release events and crop frames:

- `src/crop_producer.cpp:109`: source-release event pool
- `src/crop_producer.cpp:115`: crop frame pool
- `src/crop_producer.cpp:117`: crop device buffer allocation
- `src/crop_producer.cpp:122`: crop-ready event
- `src/crop_producer.cpp:123`: recycle event

Pool exhaustion drops/records rather than growing:

- `src/crop_producer.cpp:391`: acquire crop frame
- `src/crop_producer.cpp:404`: crop frame pool miss counter
- `src/crop_producer.cpp:405`: drop log

Source and crop-frame reuse are tied to CUDA events:

- `src/crop_producer.cpp:577`: source-release event acquisition
- `src/crop_producer.cpp:581`: event record after crop stream work
- `src/crop_producer.cpp:588`: deferred source release
- `src/crop_producer.cpp:593`: stream sync fallback
- `src/crop_producer.cpp:731`: recycle event after consumer stream

### Display Buffer Lifetime

Display buffers are allocated during construction:

- `src/opengldisplay.cpp:65`: detection draw buffer
- `src/opengldisplay.cpp:89`: P2P host copy buffer
- `src/opengldisplay.cpp:93`: resize buffer

The worker waits on source events and synchronizes before releasing the frame:

- `src/opengldisplay.cpp:139`: source-ready event wait
- `src/opengldisplay.cpp:145`: YOLO completion event wait for overlays
- `src/opengldisplay.cpp:210`: stream sync before release
- `src/opengldisplay.cpp:229`: release entry
- `src/opengldisplay.cpp:293`: stream sync before release
- `src/opengldisplay.cpp:311`: release entry

This is safe, though it is a blocking synchronization model rather than an
event/fence-backed display handoff.

### External Recorder Slot Pooling

The external recorder probe uses a bounded slot pool:

- `tools/external_recorder_ipc_probe.cpp:2558`: slot vector sized to queue depth
- `tools/external_recorder_ipc_probe.cpp:2560`: free slot list reserve
- `tools/external_recorder_ipc_probe.cpp:2569`: detach-ready event creation
- `tools/external_recorder_ipc_probe.cpp:2574`: reusable event creation
- `tools/external_recorder_ipc_probe.cpp:2584`: slot allocation
- `tools/external_recorder_ipc_probe.cpp:2594`: empty free slot returns invalid

The allocation is lazy, but bounded. The runner supports prewarm knobs, which
should be used for production-like runs to avoid first-use tails.

## Risky or Suspicious Areas

- The old `GPUVideoEncoder` remains unsafe for color hot-path use if revived,
  but it is no longer part of the current GUI or `orange_client` build targets.
- `std::queue` and `std::deque` hot queues can allocate at high-water marks.
  This is probably not steady churn, but it can create startup or burst jitter.
- Crop job wrappers are unpooled despite the crop image buffers being pooled.
- External IPC descriptor construction is small but per-frame.
- YOLO event logging and SHAMAN conversion can allocate on positive-detection
  frames.
- In-process FFmpeg packet ownership is allocation-heavy by design; this is a
  reason to keep process-isolated/external recording as the production path.

## Recommended Next Hardening Tasks

1. Add a small fixed pool for `CropEncodeJob` and `CropPreviewJob`.
2. Add allocation profiling around a real four-camera GUI external IPC run to
   distinguish one-time queue growth from steady churn.
3. Consider fixed-capacity queues for the hottest worker handoffs and
   acquisition pending requeues.
4. Reserve/reuse YOLO detection and SHAMAN conversion buffers.
5. Replace per-frame `std::ostringstream` IPC descriptors with reusable fixed
   buffers or a binary frame descriptor if CPU jitter remains visible.
6. Keep display/YOLO synchronization correctness-first until event-backed
   source/PBO ownership is designed; do not remove syncs merely to reduce
   apparent latency.

## Best Next Implementation Target

The next best target is a small pooled job allocator for `CropEncodeJob` and
`CropPreviewJob`, because it is local, reviewable, and directly removes
confirmed crop-path heap churn.
