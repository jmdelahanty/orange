# Native NVENC input handoff, 2026-09-19

The implementation stays entirely in the external IPC recorder. The native
array backend removes NVENC's visible input-tiling kernels on the local shard;
the optional surface-write kernel prepares that array faster than the copy in
the isolated A16 test. Both preserve the 4512×4512 image and neutral UV.

Worktree: `/home/jeremy/orange-nvenc-native-integration-20260919`.
Branch: `agent/encoder/nvenc-native-integration-20260919`.
Base: `152278d10659de827a2134cf18f15b439f84c680`.
The original recorder-agent worktree was left unchanged.

## Bringing the work back

The branch contains the standalone proof (`8979ebd`, cherry-picked from
`3ffa701`), implementation `c83e10d`, and the following evidence/handoff commit. Inspect
`git log --oneline 152278d..agent/encoder/nvenc-native-integration-20260919`
and bring those commits onto the recorder branch in order. Review any changes
since the base before cherry-picking; the isolated CMake adapter deliberately
fails if its API migration patterns no longer match the wrapper/profile source.

The maintained code is in `tools/external_recorder_ipc_probe.cpp` and
`tools/nvenc_native_probe/`. The main Orange build, installed executables,
application configs, and `/usr/local/cuda` remain unchanged. The native target
uses isolated CUDA 13.1 and the pinned NVENC 13.1 interface on driver 610.57.04.
Producer/analytics remain CUDA 12.2. Pancakebatter toolkit/profiler staging work
is separate (`3c42ee8`, `1829cf5`) and is not an Orange cherry-pick dependency.

## Selecting a backend

Build with the recipe in [the integration report](nvenc_native_integration_20260919.md).
`BUILD_NATIVE_KERNEL=ON` also builds the PTX dependency when the native probe or
recorder target is selected explicitly.

| Per-recorder configuration | Local shard | Peer shard |
| --- | --- | --- |
| Existing defaults | Registered linear input | Existing early linear staging |
| `--native-local-input` | Native array, Y copy | Existing early linear staging |
| Above plus `--native-local-kernel-ptx PATH` | Native array, Y surface-write kernel | Existing early linear staging |

Equivalent environment settings are `ORANGE_EXTERNAL_RECORDER_NATIVE_LOCAL_INPUT=1`
and, for the kernel, `ORANGE_EXTERNAL_RECORDER_NATIVE_KERNEL_PTX=/absolute/path/native_nv12_write.ptx`.
Apply these to full-frame recorder invocations only. Native mode rejects crop,
non-HEVC, and unvalidated local geometry; the tested geometry is 4512×4512.
The legacy build rejects native options. No option enables native input by default.

The supplied PTX must retain the `orange_native_nv12_write_luma` entry point
and ABI. Missing PTX or initialization failures are errors, not fallbacks.
Source RELEASE follows copy/kernel completion; destination reuse follows NVENC
input retirement. Machine-readable summaries report resolved per-shard backend,
native frame count, update method, and RELEASE boundary.

## Reading the result

[The integration report](nvenc_native_integration_20260919.md) contains the
commands, test matrices, live acceptance, trace counts, and limits.
[The journal](nvenc_input_layout_journal.md) preserves the original question,
driver/runtime investigation, and chronology.

The fair linear control needs no application-side per-frame copy and performs
about 0.618 ms/frame of driver tiling. Native preparation took about 0.411 ms
by array copy or 0.260 ms by the surface-write kernel in matched standalone
traces. These are preparation activity durations, not end-to-end latency or
NVENC throughput improvements. All matched encoded outputs were identical.

In the phase-controlled TensorRT comparison, inference p95 was essentially
equal for copy and kernel (2.125 ms). Their p99 values were about 2.148 and
2.134 ms, versus 2.493 ms for original linear input. The small kernel-versus-copy
difference is not a demonstrated additional inference speedup. The main result
is lower preparation work with similar measured inference contention.

The final kernel replay matrix passed all eight cases (1,600 frames total).
The final copy/kernel live pair recorded 802 frames per camera per condition,
with zero drops or camera gaps and valid decoded video. The short live pair also
showed similar inference latency; it is not a causal performance comparison.

The user's primary performance criterion is consistency as camera rates rise:
track p99 and higher percentiles, deadline misses, queue residence/growth, and
zero camera/recorder drops. Retain p95 as a useful steady-state reference.

## Follow-up scope

Keep external IPC as the recording architecture. Keep native input and the
kernel opt-in until longer production-like GUI/rolling and positive-detection
crop/pose sessions pass. The short live recordings validate healthy full-frame
recording and real inference plumbing, not long-run quality or all pose paths.
Failure after ACK has code-review coverage; comprehensive injected GPU/encoder
failure testing remains a production-hardening gate.

The next layout extension is a peer transfer directly into a native array,
with its own pixel, ownership, trace, and inference checks. The current peer
layout is a staged implementation choice, not a hardware requirement. Removing
the remaining local movement entirely would require a new cross-process native
allocation/ownership contract; current CUDA IPC exports linear allocations.
The existing YOLO preprocessing output is resized and cannot replace the
full-resolution recording image.

Durable evidence and source are archived at
`/mnt/Data2/nvenc-native-integration-20260919`, including source snapshot,
commit bundle, binaries, PTX, exact commands, traces, raw-cache/engine inputs,
and checksums. Earlier investigation and driver archives are separate and
remain unchanged.
