# Threading Model Overview

Date: 2026-01-27
Scope: High-level threading, queueing, and event synchronization architecture.

## Overview
The codebase uses a small thread-per-stage pipeline model. Each stage is a `CThreadWorker<T>` that owns a thread, an input queue, and (optionally) an output queue. Most workers are terminal consumers that recycle entries instead of forwarding them to an output queue.

Primary stages (per camera):
- Acquisition thread (not a `CThreadWorker`): pulls frames from the camera, fills a `WORKER_ENTRY`, records a CUDA event for readiness, and dispatches to downstream workers.
- Display (`COpenGLDisplay`): consumes frames for on-screen rendering (GPU → PBO), may wait for YOLO overlays.
- YOLO (`YoloWorker`): runs inference, writes detections into the entry, may dispatch to crop/encode.
- Encoder preprocess (`EncoderPreprocessWorker`): prepares frames for HW encoding and hands off `ENCODER_WORKER_ENTRY` to the HW encoder.
- HW encoder (`EncoderHwWorker`): performs NVENC encoding and file I/O.
- Crop/encode (`CropAndEncodeWorker`): creates a 256x256 crop for both preview and optional NVENC recording.
- Image writer (`ImageWriterWorker`): background disk writes for still images.

## Threading primitives
- `COffThreadMachine` handles thread lifecycle (start/stop, join) and stores an atomic `threadOn` flag.
- `CThreadWorker<T>` provides input/output queues, a `WorkerFunction`, and a simple polling loop.
- `SafeQueue<T>` is used for free pools and recycle queues; worker queues use `std::queue` + mutex.

Files: `src/offthreadmachine.h`, `src/offthreadmachine.cpp`, `src/threadworker.h`, `src/thread.h`.

## Data flow (typical recording + detection)
1. Acquisition thread captures a frame, records a CUDA event on the acquisition stream, and populates `WORKER_ENTRY` fields.
2. Entry is enqueued to display, YOLO, and/or encoder preprocess depending on configuration.
3. Each worker waits on the per-frame CUDA event before accessing GPU data.
4. YOLO writes detection results into `WORKER_ENTRY` and sets `detections_ready` (atomic). It can forward the same entry to crop/encode by incrementing `ref_count`.
5. Crop/encode uses the same entry to produce a 256x256 crop for preview and optional recording.
6. Encoder preprocess uses a separate pool of `ENCODER_WORKER_ENTRY` buffers and notifies the HW encoder via its own queue.
7. Workers decrement `ref_count` and push entries to the recycle queue when done.

Files: `src/acquire_frames.cpp`, `src/video_capture.h`, `src/yolo_worker.cpp`, `src/crop_and_encode_worker.cpp`, `src/encoder_preprocess_worker.cpp`, `src/encoder_hw_worker.cpp`, `src/opengldisplay.cpp`.

## Event synchronization model
- A CUDA event (`entry->event_ptr`) marks when the acquisition stream has produced the frame data.
- Workers call `cudaStreamWaitEvent` on their own stream before reading the GPU buffers.
- YOLO records an additional `yolo_completion_event` for inference completion.
- The display worker optionally waits for YOLO GPU completion and spins on `detections_ready` to align CPU-side detection overlays.

## Queueing and backpressure
- Worker queues have a configurable `maxQueueSize`; enqueue (`PutObjectToQueueIn`) busy-waits with a short sleep when the queue is full.
- The display worker drains its input queue to only render the newest frame, reducing latency at the expense of dropped frames.
- Most other workers process in-order and only drop frames when disabled (e.g., recording off) or when resources are unavailable.

## Shutdown behavior
- Acquisition threads are joined first to stop new frames.
- Workers are signaled to stop, then deleted in reverse pipeline order.
- Workers are designed to drain their queues and recycle entries during shutdown.

## Performance characteristics
- GPU-heavy workloads are asynchronous on CUDA streams and synchronized by events, which keeps CPU overhead low when properly balanced.
- The per-frame event model keeps GPU/CPU coordination explicit and minimizes blocking between stages.
- NVENC is used directly for high-throughput recording paths (main and crop).
- Trade-offs:
  - Busy-wait queue backpressure can waste CPU under sustained overload.
  - Some paths (display/crop preview) synchronize streams per frame to keep OpenGL previews consistent.
  - Multi-stage pipelines rely on careful shutdown ordering to avoid late enqueues.

## Efficiency assessment (high level)
The model is generally high‑performance for multi-camera GPU pipelines: it avoids unnecessary CPU copies, uses asynchronous CUDA streams, and keeps per-stage work isolated. The main efficiency costs are in backpressure (polling), per-frame preview synchronization, and the complexity of shutdown ordering across dependent stages. These are normal trade-offs for real-time display + recording, but they should be profiled and revisited if CPU headroom or latency becomes a constraint.

## Pipeline diagram (Mermaid)
```mermaid
flowchart LR
    subgraph CR[CameraResources]
        FEQ[free_entries_queue]
        EQ[free_events_queue]
        YEQ[yolo_events_queue]
        REQ[recycle_queue]
    end

    subgraph ACQ[Acquisition Thread]
        A[Capture -> WORKER_ENTRY\nrecord event_ptr on stream]
    end

    subgraph W[Workers]
        D[Display Worker\nCOpenGLDisplay]
        Y[YOLO Worker\nYoloWorker]
        P[Encoder Preprocess\nEncoderPreprocessWorker]
        H[HW Encoder\nEncoderHwWorker]
        C[Crop/Encode Worker\nCropAndEncodeWorker]
        IW[ImageWriter Worker]
    end

    subgraph OUT[Outputs]
        GL[OpenGL Texture / PBO]
        CGL[Crop Preview Texture / PBO]
        V1[Main Video + Metadata]
        V2[Crop Video + Metadata]
    end

    IPC[Frame IPC Manager]

    FEQ --> A
    EQ --> A
    YEQ --> A

    A -->|WORKER_ENTRY + event_ptr| D
    A -->|WORKER_ENTRY + event_ptr| Y
    A -->|WORKER_ENTRY + event_ptr| P
    A -->|still image job| IW
    A -->|frame + metadata\n(optional)| IPC

    Y -->|yolo_completion_event\n+ detections_ready| D
    Y -->|detections + ref_count++\n(recording only)| C
    Y -->|detections\n(optional)| IPC

    D -->|GPU -> PBO copy| GL
    C -->|preview GPU -> PBO| CGL
    P -->|ENCODER_WORKER_ENTRY + event| H
    H -->|NVENC packets| V1
    C -->|NVENC packets| V2

    D --> REQ
    Y --> REQ
    C --> REQ
    P --> REQ
    IW --> REQ

    REQ -->|drain + return resources| A
    A --> FEQ
    A --> EQ
    A --> YEQ

    H -->|return encoder entry/event| P
```
