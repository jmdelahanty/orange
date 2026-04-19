# Helper Queue Wait Explainer

Date: 2026-04-19
Branch: `exp/gop-split-a16`

Related notes:

- `docs/ptp_recording_sink_experiment_plan.md`
- `docs/multi_camera_failure_modes.md`

## Purpose

This note explains a point that is easy to misunderstand when reading the
helper-path diagnostics:

- `queue push/pop` is not the same thing as `queue wait`
- a preallocated queue can still show large `queue_wait_ms`

The goal is to make the current helper-path observations easier to interpret
without needing to reverse-engineer the code every time.

## Short Version

When we say a helper-routed frame saw `30 ms` of queue wait, we do **not**
mean:

- "the queue data structure took 30 ms to access"

We mean:

- "the frame sat around for about 30 ms before the helper worker really began
  handling it"

The queue container itself is fast. The wait is about **when service begins**,
not about the cost of `push()` or `pop()`.

## Why A Queue Can Have A Slow Startup Even When Memory Is Already Allocated

Preallocating queue storage and buffers solves only one class of cost:

- allocation / deallocation churn

It does **not** guarantee that the whole helper path is instantly in
steady-state throughput.

Startup delay can still come from things like:

- the helper worker thread not yet draining at full speed
- the first helper-routed frames arriving faster than the helper worker starts
  consuming them
- waiting for an acquisition event before helper work can begin
- waiting for helper-side resources to become available
- one-time CUDA / driver / peer-access-path effects
- the helper path becoming active suddenly rather than gradually

So "warm up" here means:

- the whole helper processing path settling into a steady rhythm

not:

- "allocate the queue"

## What The Helper Prewarm Does Now

The helper preprocess worker now has an explicit cross-GPU input prewarm step
before recording starts routing frames to that helper.

For a helper worker whose preprocess GPU is different from the camera
acquisition GPU, the prewarm:

- sets the CUDA device to the helper preprocess GPU
- enables peer access from the source acquisition GPU if needed
- allocates the helper-side input staging buffer
- records and synchronizes a lightweight CUDA event on the helper stream

That intentionally moves one-time setup costs out of the first helper-routed
recording frame. It does not push fake images through the full preprocess
path, and it does not prove that every downstream GPU kernel is warmed.

Relevant code:

- `EncoderPreprocessWorker::PrepareCrossGpuInput(...)`
- `src/modern_recording_pipeline.cpp`

## What We Measure Right Now

The current lightweight helper probe is anchored at two places:

1. helper enqueue in
   `src/recording_ingress.cpp`
2. helper worker start/done host timestamps in
   `src/encoder_preprocess_worker.cpp`

Relevant code touch points:

- helper enqueue stamp:
  - `src/recording_ingress.cpp:247`
  - `src/recording_ingress.cpp:310`
  - `src/recording_ingress.cpp:320`
- helper worker:
  - `src/encoder_preprocess_worker.cpp:420`
  - `src/encoder_preprocess_worker.cpp:581`
  - `src/encoder_preprocess_worker.cpp:588`
  - `src/encoder_preprocess_worker.cpp:625`
  - `src/encoder_preprocess_worker.cpp:709`
- frame entry fields used by the probe:
  - `src/video_capture.h:24`
  - `src/video_capture.h:36`

### `queue_wait_ms`

In the current probe, this is approximately:

- `helper worker start host time - helper enqueue host time`

So it includes:

- time sitting in the helper input queue
- worker thread scheduling delay
- time before the worker reaches the start of helper handling

It is **not** just:

- queue bookkeeping cost

### `worker_service_ms`

In the current probe, this is approximately:

- `helper done host time - helper worker start host time`

Important nuance:

- this is host-side time until the worker has queued the GPU work into the CUDA
  stream
- it is **not** the full GPU execution time to finish the copy/resize/preprocess

So a tiny `worker_service_ms` means:

- the CPU side of submission is quick

It does **not** prove:

- the GPU copy/resize path has already completed

## Step-By-Step Sketch Of The Current Helper Path

Below is the high-level sequence for a helper-routed recording frame.

### 1. Acquisition receives a frame

Acquisition fills a `WORKER_ENTRY` and gives it:

- image pointer(s)
- `recording_frame_id`
- `event_ptr`
- source GPU id

Relevant struct:

- `src/video_capture.h`

### 2. `RecordingIngress` decides where the frame goes

`RecordingIngress::SubmitFrame(...)` looks at:

- the current recording mode
- split-GOP routing policy
- `recording_frame_id`

and chooses:

- primary preprocess worker
- or helper preprocess worker

Relevant function:

- `src/recording_ingress.cpp:247`

When it chooses a helper worker, it stamps:

- `helper_enqueue_host_ns`
- helper queue depth at enqueue
- helper free buffer/event counts at enqueue

and then calls:

- `target_worker->PutObjectToQueueIn(entry)`

### 3. The helper worker eventually pops that frame

The helper worker runs in:

- `EncoderPreprocessWorker::WorkerFunction(...)`

Relevant function:

- `src/encoder_preprocess_worker.cpp:420`

At this point, the probe records:

- helper start host time
- queue depth at start
- free helper resources at start

This is the point used for the current `queue_wait_ms`.

### 4. The worker waits for acquisition readiness if needed

If the acquisition thread attached an event, the helper worker does:

- `cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0)`

Relevant line:

- `src/encoder_preprocess_worker.cpp:581`

That means "worker start" in the probe is still slightly earlier than the
moment real helper GPU work can proceed.

### 5. Cross-GPU helper setup happens

If the frame came from a different GPU than the helper preprocess GPU, the
worker:

- ensures peer access
- may allocate staging memory once
- issues `cudaMemcpyPeerAsync(...)` into helper staging

Relevant lines:

- `src/encoder_preprocess_worker.cpp:588`
- `src/encoder_preprocess_worker.cpp:625`

### 6. Debayer / resize / NV12 preparation gets queued

After the source image is ready on the helper GPU, the worker queues the
preprocess work for recording.

The worker then records the preprocess completion event and eventually stores a
host-side "done" timestamp for the probe.

Relevant lines:

- `src/encoder_preprocess_worker.cpp:633`
- `src/encoder_preprocess_worker.cpp:706`
- `src/encoder_preprocess_worker.cpp:709`

### 7. The original frame entry is released

Once the worker has queued the preprocess work, it decrements `ref_count` and
may recycle/requeue the original camera buffer if it was the last consumer.

That is why recording-path timing can affect acquisition even when no obvious
queue is "full".

Relevant lines:

- `src/encoder_preprocess_worker.cpp:720`

## How To Read The Current `helperprobe5` Numbers

From the current clean baseline:

- `free_run`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_helperprobe5`
- `ptp_gate`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_ptp_helperprobe5`

The first helper-routed frames showed:

- `free_run`: queue wait about `28.5-28.8 ms`
- `ptp_gate`: queue wait about `33.3-33.6 ms`
- worker service in both cases: only about `0.04-0.07 ms`

Then the queue wait decayed rapidly over the next few helper frames.

That pattern is best read as:

- not "the queue implementation is slow"
- but "helper routing turns on with a startup backlog, and the helper path
  catches up after the first few frames"

## What Changed With `helperprobe6`

After adding helper cross-GPU input prewarm, the same dual-camera `100 fps`
preprocess-only helper probes were rerun:

- `free_run`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_helperprobe6`
- `ptp_gate`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_ptp_helperprobe6`

The first helper-routed frame changed from:

- `free_run`: about `28.5-28.8 ms` queue wait
- `ptp_gate`: about `33.3-33.6 ms` queue wait

to:

- `free_run`: about `3.7-3.9 ms` queue wait
- `ptp_gate`: about `4.2 ms` queue wait

That is a real improvement in helper activation latency.

However, both runs remained below target:

- `free_run`: about `70.3 fps` mean acquisition rate, `400-401` camera drops
- `ptp_gate`: about `69.5 fps` mean acquisition rate, `351` camera drops

So the prewarm fixes a specific cold-start cost, but it does not fix the
remaining two-camera `100 fps` acquisition/drop behavior by itself.

## What Changed With Deferred Source Release

After helper prewarm, the next hypothesis was that Orange might be returning
the raw source buffer to EVT or the local acquisition ring before helper GPU
work had safely finished reading from it. That would be a correctness bug even
if the helper queue looked healthy.

The preprocess worker now records a CUDA event after the last queued GPU
operation that can read the source frame:

- for helper cross-GPU frames, after the peer copy into the helper staging
  buffer
- for same-GPU frames, after the preprocess work has been queued

The raw `WORKER_ENTRY` is recycled only after that event has completed. This is
enabled by default and can be disabled for comparison with:

```bash
ORANGE_PREPROCESS_DEFER_SOURCE_RELEASE=0
```

Validation probes:

- `free_run`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_helperprobe10`
- `ptp_gate`:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_ptp_helperprobe11`

Result:

- both runs completed without source-release event exhaustion
- sampled same-GPU source release waits were roughly `0.4-0.5 ms`
- sampled helper cross-GPU source release waits were roughly `3.2 ms`
- `free_run` still measured about `70.3 fps` with `400-401` camera drops
- `ptp_gate` still measured about `69.4-69.6 fps` with `351` camera drops

Interpretation:

- early raw-source reuse is now guarded against
- the normal source-release delay is small compared with the missing `100 fps`
  cadence
- deferred release is correctness hardening and useful observability, not the
  throughput fix for the current dual-camera split-GOP failure

## The Most Important Learning Point

A queue can be preallocated and still show large queue wait because:

- queue wait is really about **when the consumer becomes ready to service the
  item**

not:

- how expensive it was to store a pointer in memory

That is the main idea to carry forward when reading these diagnostics.

## What This Still Does Not Tell Us

This probe is useful, but it does not fully answer:

- why helper routing turns on with a burst instead of a smooth transition
- whether the startup backlog is caused by routing policy, worker scheduling,
  event dependency timing, or something else upstream
- exact GPU copy / preprocess execution time on the helper GPU

So the current probe is best treated as:

- a localization tool

not:

- the final root-cause proof
