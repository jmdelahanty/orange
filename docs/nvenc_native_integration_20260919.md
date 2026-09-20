# Native NV12 recorder integration, 2026-09-19

This branch begins at recorder commit `152278d10659de827a2134cf18f15b439f84c680`
and carries the standalone proof as `8979ebd` (original proof `3ffa701`).
Implementation commit: `c83e10d`. The recorder agent's worktree is untouched. The opt-in implementation is in
`tools/external_recorder_ipc_probe.cpp`; its default remains the existing linear
input path. The isolated CMake project builds the experimental recorder against
CUDA 13.1 and NVENC 13.1 without changing Orange's CUDA 12.2 build or installed
application binaries.

## First integration checks

Artifacts: `/tmp/nvenc-native-integration-20260919`.

Real-frame source is the first cached frames of
`calibration_raw_20260918_fish_preenc/Cam2010096_preenc_ref.bin` under
`~/orange_data/model_sources/detect/detect_all_available_detect_training_v004_yolo11n_trt_20260520`.
These were captured every tenth live frame. Replaying them at 100 fps exercises
layout and recording, but is not consecutive 100 fps camera motion.

The 32-frame controls `pixels/linear_packed`, `pixels/linear_pitched`, and
`pixels/native_packed` all produced bitstream SHA-256
`7da135ecb2781a0e5ba383a80becb9c7eb240b91fb5508d597a7a5ca80a4c8e7`.
Full CPU decode and six selected source comparisons passed, with luma MAE
3.49–4.00 and PSNR 33.75–34.96 dB; decoded chroma stayed 128. Direct packed
registration at pitch 4512 works. This supersedes any interpretation of the
earlier malformed packed control as proof that NVENC forbids pitch 4512.

A fair 200-frame standalone trace compared packed registered linear input
(no per-frame input copy) against a copy from that same layout into native
arrays. `traces/linear_registered` contains 400 `Convert_PL2BL` calls totaling
123.587 ms (0.618 ms/frame). `traces/native_copy` contains zero CUDA kernels
and 200 device-to-array Y copies totaling 82.290 ms (0.411 ms/frame).
Both imports include teardown, and the native copy count matches submission
count. Nsight 2025.5 reports its known CUDA-driver-13.3/support-13.1 warning;
no trace-import error was reported. These activity totals establish reduced
SM work and about 33% less summed input-preparation GPU activity for this
control; they do not establish a live inference or encoder throughput gain.

`ipc_smoke_initial` is preserved negative evidence: new API initialization
rejected an unspecified HEVC chroma format. The private profile adapter now
sets 4:2:0 explicitly, alongside 8-bit input/output and progressive frames.
`ipc_smoke_chroma_fixed` passed 64 real frames from the CUDA 12.2 IPC producer
to the CUDA 13.1 recorder: every frame was ACKed, released exactly once after
copy completion, and encoded; zero drops and clean MP4 finalization.

The first TensorRT harness smoke uncovered a TensorRT 10 storage-metadata
query that is invalid for unvectorized tensors. The harness now sizes linear
tensors by dtype, and the corrected 100-iteration smoke passed with a required
CUDA graph, output D2H copies, and absolute 100 fps pacing. Zero-input graph
service p95 was 1.984 ms. This is a plumbing/contention harness, not live YOLO
preprocessing, detection quality, or capture-to-detection latency.

## Build and runtime isolation

```bash
cmake -S tools/nvenc_native_probe -B /tmp/build-nvenc-native-integration \
  -DCMAKE_BUILD_TYPE=Release \
  -DORANGE_SOURCE_ROOT="$PWD" \
  -DCUDA_13_ROOT=/home/jeremy/.local/opt/cuda-13.1.1-nvenc \
  -DNVENC_13_INTERFACE_DIR=/tmp/nvenc-interface-13.1.15/Video_Codec_Interface_13.1.15/Interface \
  -DBUILD_NATIVE_RECORDER=ON -DBUILD_CONTENTION_TOOLS=ON \
  -DBUILD_LEGACY_RECORDER_CONTROL=ON -DBUILD_NATIVE_KERNEL=ON
cmake --build /tmp/build-nvenc-native-integration -j4
```

The native recorder links the isolated `libcudart.so.13`. The TensorRT probe,
IPC producer, and legacy control recorder link the existing `libcudart.so.12`.
Neither global `/usr/local/cuda`, the installed recorder, nor system packages
are changed. Compatibility source files are generated only in the build tree;
configuration fails if reviewed API-migration patterns stop matching.

For a full-frame recorder invocation, `--native-local-input` or
`ORANGE_EXTERNAL_RECORDER_NATIVE_LOCAL_INPUT=1` selects native arrays on the
shard that shares the producer GPU. Other shards retain their existing linear
staging path. Native mode currently fails closed outside the validated
4512×4512 NV12-shaped Mono8 pool and HEVC combination. Do not enable this env
variable globally for crop recorder invocations. The legacy binary rejects it.

Source RELEASE follows completion of the Y copy into a recorder-owned native
array. Array reuse waits for the encoder's existing input-slot completion.
UV is initialized to neutral 128 once. No buffer is recycled merely because
its frame descriptor was ACKed. The new per-item ACK-ready gate prevents an
already-running worker from emitting RELEASE before intake sends ACK.

Machine-readable summaries record the requested native option, resolved
per-shard backend, native frame counts, and native source-release boundary.

## Repeated inference-contention result

`contention/` contains nine interleaved runs: inference alone, inference plus
registered packed linear NVENC input, and inference plus native-array copies.
Each inference run includes 100 paced warmup iterations and 2,000 measured
100 fps graph launches. Each encoder process submits 1,300 frames as 25-frame
100 fps bursts alternating with 25 idle periods (50 fps average for one shard).
All 2,000 inference rows in every run lie inside the actual shared interval.
There were zero deadline misses. The six encoded streams are byte-identical.

The primary metric is scheduled deadline to inference completion, including
wake/launch delay. Values below are means of three run-level percentiles,
not percentiles of a pooled trace:

| Condition | p95 | p99 |
| --- | ---: | ---: |
| Inference alone | 2.041 ms | 2.056 ms |
| Registered linear input | 2.129 ms | 2.477 ms |
| Native array copy | 2.125 ms | 2.132 ms |

The p95 difference is only 3.67 microseconds and is not a material improvement.
The p99 reduction is 0.345 ms (about 14%) and appears in all three repeats:
linear p99 2.418–2.518 ms versus native 2.129–2.135 ms. GPU graph and CPU sync
p99 show the same tail reduction; CPU submission and wake timing do not explain
it. Treat this as a repeatable camera-free tail improvement on GPU 1, with a
zero-input detection graph and cached real encoder content. It is not a claim
about full camera/pose latency, model quality, or engine encoding capacity.
The optional active-burst analysis uses host submission intervals as a proxy,
not exact GPU activity intervals; the shared-interval metric above is primary.

## Recorder correctness matrix

`ipc_matrix/matrix_summary.json` passed all eight 200-frame combinations:
producer/recorder on GPU 1 alone or GOP shards on GPUs 1 and 2; native off/on;
combined or separate submit/harvest threads. All 1,600 submitted frames were
ACKed, released exactly once after ACK, encoded, and represented in packet and
metadata counts. Full CPU decode and selected exact-source luma comparisons
passed in every case, including frames on each side of shard/GOP boundaries.
Same-GPU native runs recorded 200 native frames; split-GPU native runs recorded
100 native frames on GPU 1 and 100 linear frames on GPU 2. The finite one-GPU
cases are correctness/drain tests, not evidence of sustained 100 fps capacity.

Review additionally fixed copy quiescence, failed registered-input lease
retirement, queued ACK/release cleanup, stream ownership, registration-before-
storage destruction, and worker-before-IPC-mapping teardown. A subsequent
review moved the protocol-write mutex before the worker vector so it outlives
workers during exceptional stack unwinding. The eight-case matrix predates
that declaration-order-only correction; subsequent legacy/error/live checks
use the corrected final binary.

## Final build and live PTP acceptance

Both the API 13/CUDA 13 native target and the API 11/CUDA 12 legacy control
build successfully. The final legacy binary passed a 64-frame real-content
IPC replay and pixel validation. Native mode rejects unsupported 4500×4512
geometry with a clear error and both processes exit with failure; it also
rejects the native option in the legacy build. These are expected negative
checks, not successful recording cases.

`live/linear` and `live/native` are an eight-second-per-condition, two-camera
PTP acceptance pair using the existing application binary at
`/home/jeremy/orange-device-roi-20260912/targets/release/orange_client`.
The temporary specs and recorder launchers are preserved. No production
config or installed binary was replaced. The PTP processes already running
before the experiment were left running.

Both conditions returned 0 and passed the strict external-recorder verifier
and decoded-video sanity. Each camera recorded 802 frames with 802 ACKs,
encoded frames, packets, and metadata rows; zero skips, drops, camera gaps,
GetFrame errors, encode failures, write failures, or identity mismatches.
Queue high-water was 4. Native mode recorded 402 native frames on each local
shard (GPU 7 for 2010095, GPU 5 for 2010096); each peer shard (GPU 8/6) retained
400 linear frames.

Latency analysis joins recorder metadata to YOLO by local frame ID and checks
both camera and system timestamps. It uses recording frame IDs 101–802 only
(702 rows per camera), excluding startup and post-record inference:

| Camera | Input | Acquisition → detect p95 | p99 |
| --- | --- | ---: | ---: |
| 2010095 | Linear | 2.555 ms | 3.032 ms |
| 2010095 | Native local | 2.410 ms | 2.580 ms |
| 2010096 | Linear | 2.467 ms | 2.698 ms |
| 2010096 | Native local | 2.337 ms | 2.476 ms |

This short fixed-order pair proves integration correctness and records an
observed latency reduction. It does not establish a repeated live performance
benefit or validate long GUI/rolling/crop/pose sessions. Keep native input opt-in
until those production acceptance gates are run.

## Why the remote shard still uses linear input

Both shards can in principle use native NV12 arrays; the hardware does not
require different layouts. This first slice targets the shard sharing a GPU
with analytics. Its local device-to-array copy has now been tested with the
actual producer and recorder ownership protocol. The remote shard currently
retains its existing early peer copy into a linear buffer and NVENC's tiling.
The next extension is to make that same peer transfer land in a native array,
using the appropriate cross-context/peer array copy API, so it does not add a
second transfer. Validate peer transfer, source release, slot reuse, pixels,
and the CUDA trace before extending the option to remote shards.

Native-array writes from an existing useful preprocessing kernel remain a
possible later fusion. The bounded surface-write experiment below tests an
extra kernel; it does not yet fuse any existing producer operation.

The final integrated-recorder trace in `ipc_traces/` confirms the same behavior
inside the recorder itself: the 64-frame linear control launched 128
`Convert_PL2BL` kernels (40.020 ms total); native launched zero CUDA kernels
and made 64 Y array copies (26.559 ms total). Both producers completed all
ACK/RELEASE checks. The trace analyzer initially read the release counter from
the wrong JSON nesting; correcting it reused the completed linear trace.
No GPU experiment was repeated solely for that analyzer correction.

## Follow-up requested before handoff: external surface-write preprocessing

The user requested a bounded preprocessing experiment and explicitly kept
external IPC as the recording architecture. The candidate replaces the external
recorder's copy into the native Y plane with a vectorized CUDA surface-write
kernel. It still moves the same visible pixels and uses SM resources; this is
not a zero-copy path. Compare pixel correctness, preparation duration, and
inference contention before choosing it. The already-validated native copy
backend remains the default for the opt-in native mode during this experiment.

The first surface-write kernel passed packed and pitched 32-frame real-source
checks, including exact array readback and the same encoded bitstream SHA as
the native-copy and linear controls. The kernel uses 16-byte vector loads and
surface writes into the native Y plane; UV stays initialized to 128.
`kernel_trace/` contains 200 `orange_native_nv12_write_luma` launches totaling
51.932 ms (0.260 ms/frame), zero `Convert_*` calls, and no per-frame array-copy
activity. Its 200-frame bitstream is identical to the copy control. This proves
correctness and a shorter isolated preparation step, not yet a better choice
under inference contention.

For the follow-up comparison, both encoder and inference accept absolute
CLOCK_MONOTONIC starts. Matching conditions use the same nominal phase, and
three repetitions sample offsets across the 10 ms frame period. Inference
warmup ends immediately before its measurement epoch so the coordinated wait
does not cool the baseline GPU. This is a stronger phase control than the
initial naturally phased three-condition comparison.

The producer currently supplies unchanged full-resolution Mono8 pixels; its
YOLO preprocessing produces a resized inference tensor and cannot also serve
as the full-frame recording image. The current CUDA IPC contract shares linear
allocations, not CUDA array/surface handles. Eliminating both the producer-owned
copy and recorder-native copy would require a new shared-allocation and slot
ownership contract, with independent proof that the native video array can be
aliased safely across processes. That is a separate experiment. NVENC and its
preparation remain in the external recorder process for this implementation.

### Phase-controlled result and optional recorder kernel

The completed `kernel_contention_phased/` matrix covers four conditions and
three offsets (0, 3.333333, and 6.666667 ms). Each run uses 2,000 measured graph
launches; the recorded metric remains scheduled deadline to completion. Means
of the three run-level percentiles are:

| Condition | p95 | p99 |
| --- | ---: | ---: |
| Inference alone | 2.043 ms | 2.062 ms |
| Registered linear input | 2.132 ms | 2.493 ms |
| Native array copy | 2.125 ms | 2.148 ms |
| Native surface-write kernel | 2.125 ms | 2.134 ms |

The kernel's shorter preparation duration does not translate into a material
inference improvement over the native copy. The approximately 14 microsecond
mean p99 difference is small and not consistent in sign across phases; p95 is
essentially identical. Both native approaches retain the substantial tail
reduction relative to the original driver tiling in this controlled harness.
Saving time in recorder preparation need not shorten inference: only overlapping
resource contention affects its completion, and the kernel moves preparation
work onto SMs. This experiment does not isolate the precise overlap mechanism.

The recorder now exposes the kernel as an additional opt-in:

```text
--native-local-input --native-local-kernel-ptx /absolute/path/native_nv12_write.ptx
```

Equivalent per-recorder environment settings are
`ORANGE_EXTERNAL_RECORDER_NATIVE_LOCAL_INPUT=1` and
`ORANGE_EXTERNAL_RECORDER_NATIVE_KERNEL_PTX=/absolute/path/native_nv12_write.ptx`.
An empty/unset PTX option keeps the native copy. The existing linear backend
remains the overall default. Native initialization loads a worker-local module
and Y surface objects only on the local shard; peer shards keep linear staging.
Both update paths synchronize before source RELEASE and use the same encoder
slot-retirement rules. Summaries record `native_update=copy|kernel` and
`native_source_release_boundary=copy_complete|kernel_complete` for native
shards, and null for linear shards. Invalid option combinations and missing PTX
are rejected without fallback. Arbitrary PTX is diagnostic input and must match
the supplied kernel entry point and ABI.

The independent `kernel_contention_phased/phased_interpretation.json` and `.md`
reports verify all 24,000 measured inference rows, zero deadline misses, and
byte-identical output across all nine encoder streams (243,268,829 bytes;
SHA-256 `981f69957299e3b4b1a95f67ffd2e0926dd32e3e672ab8ea00ca428106a3fcbc`).
The phase settings control scheduled burst alignment, not exact per-frame GPU
overlap: encoder backpressure within a 100 fps burst shifts late submissions.
Only 67.1–73.9% of matched active frames were within 0.25 ms of the requested
host phase; median burst-end lag was 8.647–11.646 ms. Each phase has one run per
condition, so the precise p99 reduction remains dependent on phase and run.
The direction of the original linear-to-native tail improvement persists across
all three scheduled offsets.

`ipc_kernel_matrix/matrix_summary.json` passed the final recorder binary through
all eight 200-frame cases: four linear controls and four native-kernel cases,
same/split GPU and combined/separate submission/output threads. All 1,600 frames
passed ACK/RELEASE order and reuse checks, packet/metadata parity, full CPU decode,
and exact-source sample comparisons, with zero drops. Split native cases used
100 kernel-prepared frames locally and 100 linear frames on the peer.
PTX SHA-256 is `b5310024af940654791dd269f251002e84da5a4ac74184dd334fdb3ce4cdba6f`.

`ipc_kernel_trace/` confirms the final recorder launches exactly one
`orange_native_nv12_write_luma` per frame: 64 calls, 16.631 ms total
(0.260 ms/frame), no `Convert_*` calls, and no per-frame array-copy activity.
The eight recorded host-to-array copies are one-time Y/UV initialization of
four native slots. The trace imports through teardown with only the previously
documented profiler compatibility warning. All 64 source leases were released.

Final option-rejection checks cover kernel-without-native, missing PTX, and
legacy-build rejection. The initial CLI check omitted the required socket and
is preserved as invalid evidence under `final_option_rejections_invalid_missing_socket`;
corrected checks assert the intended error messages under `final_option_rejections`.

The user's priority is consistent low latency as camera rates increase at
similar resolution. Subsequent acceptance should emphasize p99/high-percentile
latency, deadline misses, queue residence/growth, and zero camera/recorder drops.
A lower isolated preparation time or average alone is insufficient. The native
backend's tail reduction is the principal result; no higher sustainable capture
or encoding rate is claimed by these tests.

### Final copy/kernel live acceptance

`live_kernel/copy` and `live_kernel/kernel` repeat the two-camera PTP acceptance
with the final recorder binary, eight recorded seconds per condition. Both
returned 0, passed the strict external-recorder verifier, and passed decoded
video sanity with real nonblack camera content. Each condition/camera received,
ACKed, encoded, and wrote metadata/packets for 802 frames, with zero drops,
skips, camera gaps, GetFrame errors, or encode failures. Each local shard used
402 native frames; each peer shard retained 400 linear frames. Queue high-water
was 4 for copy and 3 for kernel in this short pair.

The same metadata/timestamp join retains recording IDs 101–802 only:

| Camera | Native update | Acquisition → detect p95 | p99 |
| --- | --- | ---: | ---: |
| 2010095 | Copy | 2.411 ms | 2.589 ms |
| 2010095 | Kernel | 2.456 ms | 2.563 ms |
| 2010096 | Copy | 2.376 ms | 2.510 ms |
| 2010096 | Kernel | 2.376 ms | 2.497 ms |

This fixed-order pair supports correctness and similar inference latency,
including a small p95 increase on one view. It does not justify promoting the
kernel on the basis of inference speed. Copy remains the default native update;
both native backends remain opt-in overall. The host's pre-existing PTP stack
was preserved. No camera settings, production launch configuration, or installed
application binary was changed.

The final legacy API 11/CUDA 12 binary also passed another 64-frame replay and
full decode/exact-source sample validation under `ipc_legacy_kernel_final/`.
`final_binary_provenance.json` records the final recorder/PTX and source hashes;
older exact tested binaries are retained rather than overwritten. Implementation
is committed as `c83e10d`, following standalone proof `8979ebd`.
