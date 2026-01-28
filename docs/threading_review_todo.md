# Threading Review TODO (Crop/Encode Pipeline)

Date: 2026-01-27
Scope: CropAndEncodeWorker and pipeline shutdown coordination.

## Findings / TODOs
1. Shutdown ordering can drop crop work or leak ref-counts.
   - Risk: YOLO can enqueue to CropAndEncodeWorker after its thread has already stopped; if the queue is full, PutObjectToQueueIn can spin forever because the consumer is gone.
   - Refs: `src/orange.cpp:986`, `src/orange.cpp:991`, `src/yolo_worker.cpp:778`, `src/threadworker.h:143`.
   - Suggested fix: stop producers (YOLO) first, wait for drain, then stop consumers (crop/encode); or add stop tokens / queue close semantics; or guard PutObjectToQueueIn with IsMachineOn to avoid enqueue-after-stop.

2. `flush_and_close()` pushes packets without guarding `writer_.video`, and it is called more than once.
   - Risk: can deref null or double-close if `flush_and_close()` runs before recording started, or after it already ran.
   - Refs: `src/crop_and_encode_worker.cpp:114`, `src/orange.cpp:1005`, `src/crop_and_encode_worker.cpp:93`.
   - Suggested fix: make `flush_and_close()` idempotent and only call `EndEncode()` / `push_packet()` when `writer_.video` is valid and recording is active.

3. Crop bounds assume frames are at least 256x256.
   - Risk: if a crop-enabled camera has smaller dimensions, `std::clamp(..., 0, entry->width - 256)` is invalid and can underflow.
   - Refs: `src/crop_and_encode_worker.cpp:212`.
   - Suggested fix: enforce minimum camera dims for crop mode or branch to a smaller crop size when needed.

## Perf / efficiency notes
- Good: GPU-heavy work is async on CUDA streams; NVENC is used directly.
- Potential stall: preview path does a per-frame `cudaStreamSynchronize(m_display_stream)`; confirm it is required for the OpenGL PBO handoff.
- Backpressure uses a spin/sleep loop in `PutObjectToQueueIn`; consider a condition-variable queue if CPU usage becomes an issue.

## Open questions
- Are crop-enabled cameras always guaranteed to be >= 256x256?
- Is `EndEncode()` guaranteed to return zero packets if no frames were ever encoded?
