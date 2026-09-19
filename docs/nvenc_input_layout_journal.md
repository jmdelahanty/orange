# NVENC NV12 input-layout investigation journal

## Objective and current status

Find out whether the Linux NVENC interface can consume a device-resident NV12
surface without launching the per-frame `Convert_PL2BL` or `Convert_BL2BL`
kernels seen on the A16. If a supported input path exists, determine whether
Orange can produce that surface during an existing copy/preparation operation.

Current status on 2026-09-19: the isolated native-NV12 CUDA-array probe passes
on A16 with driver 610.57.04. Four matched 600-frame runs produce identical
encoded video; linear input launches two tiling kernels per frame (about
0.620 ms combined), while native input traces contain no CUDA kernels. Native
Y updates still cost a transfer. Inference benefit and production integration
remain unvalidated. Production Orange stays on CUDA 12.2; the side-by-side
CUDA 13.1 toolkit and newer profiler have maintained pancakebatter recipes.
See the final dated entry for accepted evidence, failed controls, limitations,
and the checksummed Data2 archive. Earlier dated entries preserve the sequence
of investigation, backup, driver migration, and qualification.

## Reproducible starting point

- Worktree: `/home/jeremy/orange-nvenc-layout-20260918`
- Branch: `agent/encoder/nvenc-input-layout-20260918`
- Base commit: `dfcc344d5d0be73ac030503a1c60c23e9d8511ad`
- Base subject: `Recorder: phase-locked encode submission and output-delay options (A/B, tail not closed)`
- Reviewed source worktree: `/home/jeremy/orange-device-roi-20260912`
- Source branch: `agent/analytics/device-roi-20260912`
- Original prompt: `/home/jeremy/nvenc_nv12_input_layout_prompt_2026_09_18.md`

The source branch advanced to `99ad07499117859124c91a7c2791783d65f4eb25`
before this worktree was created. We deliberately branched from the reviewed
`dfcc344` baseline, not that newer tip. The original worktrees were not switched
or modified. Do not touch the original untracked `build-gop-split/` directory.

The new checkout contains the tracked stress-tool source, recorder probe, and
NVENC wrapper. It does not inherit the original worktree's untracked binaries;
`targets/release/` was absent at creation. Build provenance must be recorded
before future experiments rather than assuming a binary matches this checkout.

Environment reported by the original prompt: Ubuntu/kernel 6.5, NVIDIA driver
535.183.06, API 11.1, two A16 cards with eight GA107 dies total, and an RTX
A6000. Camera images are 4512 x 4512 Mono8 at 100 fps. Hardware and driver state
were not re-probed during the read-only review.

## Terms we clarified

| Term | Meaning in this investigation |
| --- | --- |
| NV12 | Pixel format: a full-resolution Y plane and a subsampled, interleaved UV plane. For grayscale, Y contains the camera image and UV is neutral `0x80`. |
| Image dimensions | The visible image remains 4512 x 4512 pixels. Layout conversion does not resize or rotate it. |
| Pitch | Byte distance between successive row starts in linear storage. For 8-bit Y, pitch 4608 means 4512 image bytes plus 96 padding bytes per row. Padding is outside the logical image; its contents are not additional image pixels. |
| Pitch-linear layout | Pixels are stored in rows, addressed as `base + y * pitch + x` for 8-bit Y. |
| Block-linear layout | Pixels are rearranged into a hardware-specific tiled arrangement. Exact native NVENC tile geometry has not been established here. |
| Surface | The concrete image allocation/resource, together with its format, dimensions, and layout information. An NV12 surface can have different memory layouts. |
| CTU | HEVC's coding-tree unit organizes compression work. It is distinct from a memory tile; no one-to-one mapping has been established. |

The observed conversion is conceptually:

```text
NV12 in linear rows -> NV12 in tiled storage -> HEVC compression
```

NVENC accepting the NV12 pixel format does not promise that its hardware reads
the application's allocation unchanged. Likewise, a CUDA array being tiled
does not prove it matches the internal surface the encoder consumes.

NV12's additional UV plane is separate from row padding. In the wrapper, its
offset for linear NV12 is `pitch * height`. Changing the registered pitch
without allocating and writing the matching layout would misaddress both rows
and the UV plane.

Conceptual references:

- [NVIDIA's PL/BL and NV12 explanation](https://docs.nvidia.com/pva/solutions/0.3.0/impl/operator/bltopl.html)
- [NVIDIA's surface-layout explanation](https://developer.nvidia.com/docs/drive/drive-os/archives/6.0.3/linux/sdk/oxy_ex-1/common/topics/nvmedia_understand/NVM_SURF_ATTR_LAYOUTAttribute15.html)

These explain terminology and spatial locality on NVIDIA platforms. They do
not establish the exact A16 NVENC layout or a conversion-free allocation API.

## Why roughly 0.6 ms is plausible, and why it matters

A tightly packed frame occupies:

```text
Y:                 4512 * 4512       = 20,358,144 bytes
NV12, including UV: 4512 * 4512 * 3/2 = 30,537,216 bytes
One read + write:                   = 61,074,432 bytes
```

If conversion read and wrote every NV12 byte once, 0.617197 ms would correspond
to roughly 99 GB/s of effective traffic. This is an explanatory estimate, not
a measured bandwidth result: the kernels' actual traffic, caching, occupancy,
internal padding, and bottleneck have not been profiled. A rearrangement can
be expensive even with little arithmetic because it touches tens of MB.

The [A16 specification's](https://images.nvidia.com/content/Solutions/data-center/vgpu-a16-datasheet.pdf)
approximately 200 GB/s is local GPU memory bandwidth per die, not PCIe
bandwidth. PCIe 4.0 x16 is approximately 31.5 GB/s per direction
before transaction overhead. At 200 GB/s, the estimated 61 MB read/write has an
ideal lower bound near 0.305 ms. At 100 frames/s it averages only 6.1 GB/s, but
its sub-millisecond bursts can overlap latency-sensitive inference. Neither
average bandwidth nor peak bandwidth alone predicts that interference.

The prompt attributes a remaining inference tail to conversion work in a
different CUDA context on the same die: an approximately 1.6 ms analytics graph
can start late when recorder GPU work overlaps its intended start. Reported
capture-to-pose p50/p95 is roughly 2.3/3.1 ms. Those end-to-end figures are prior
results, not rerun in this investigation. Removing conversion could help, but
does not yet prove the entire tail would disappear.

Process isolation still has value: earlier experiments reduced same-process
CUDA/NVENC host contention. Separate processes on one GPU continue to share
compute and memory resources.

## Existing trace evidence

Artifact directory:

```text
/tmp/claude-1000/-home-jeremy-orange-gop-split-a16/c376764a-e684-430d-829c-97d0469b618a/scratchpad
```

Existing `.sqlite` exports were opened with SQLite `mode=ro&immutable=1`.
No new profile, export, or GPU workload was run. The matching `.nsys-rep`
artifacts are named in the original prompt. This scratchpad is an external,
temporary dependency and has not been copied into this repository.

Each row below describes 600 submitted frames and 1200 conversion launches.
The last column is summed conversion-kernel duration divided by 600. These
are GPU elapsed durations, not host call time or occupancy-adjusted SM usage.

| Existing SQLite artifact | Input | Conversion kernels | Mean conversion time/frame |
| --- | --- | --- | --- |
| `nvenc_registered_prod.sqlite` | Width-pitch device pointer, production feature flags | `Convert_PL2BL`, 2/frame | 0.617197 ms |
| `nvenc_array_prod.sqlite` | One 8-bit CUDA array | `Convert_BL2BL`, 2/frame | 0.574675 ms |
| `nvenc_registered_cqp.sqlite` | Device pointer, constant QP | `Convert_PL2BL`, 2/frame | 0.608688 ms |
| `nvenc_solid.sqlite` | Wrapper-owned pitched allocation, old tool feature defaults | `Convert_PL2BL`, 2/frame | 0.617642 ms |

Corrections and limitations:

- The prompt's approximately 0.31 ms per launch is an average across the two
  launches. The production pointer trace has two duration groups near
  0.207244 and 0.409953 ms. We have not established each kernel's precise job.
- The pitched `solid` path already shows conversion. This weighs against
  padding alone removing it. It is not a controlled production-flags A/B of
  externally registered pitch 4512 versus 4608; it also enables extra analysis
  kernels through the old tool defaults. The actual pitch should be logged
  explicitly in the controlled comparison.
- AQ/temporal-AQ defaults add `Subsample2x2_NV12BL`, `calculateCost`, reductions,
  and other kernels; the old-default registered trace totals about 1.170 ms of
  kernel duration/frame. Production-flags and constant-QP traces still show
  conversion without those analysis kernels.
- Existing output-delay traces distinguish queued time from execution time:
  median launch-API-end to GPU-start delay is about 20.143 ms for output delay
  3, versus 0.222/0.225 ms for delays 0/1. This delay is not conversion runtime.
- CUDA-only traces do not definitively attribute conversion to map versus
  encode, because NVENC API ranges were not captured.
- `--duration 6 --fps 100` submits 600 frames; it is not a strict six-second
  wall-clock limit. These runs achieved roughly 95-96 fps.
- Registered test buffers are uniform synthetic content. They do not validate
  real dish-content bitrate, production ROI behavior, or pixel-layout fidelity.
  Their initialization does not independently reproduce neutral UV, although
  the tool defaults to monochrome encoding.
- The named run logs do not show saved bitstream output. Packet counts alone
  do not establish successful decode or correct pixel placement.

## Code findings at the base commit

### Input contracts and wrapper

- The active header is `nvenc_api/include/nvEncodeAPI.h`, selected by
  `CMakeLists.txt:107-108,187-188`. It is byte-identical to the prompt's
  `third_party/NvEncoder/include/nvEncodeAPI.h`; both identify API 11.1.
- `nvEncodeAPI.h:728-735` exposes DIRECTX, CUDADEVICEPTR, CUDAARRAY, and
  OPENGL_TEX. CUDAARRAY requires a 2D array with surface-load/store enabled.
- `nvEncodeAPI.h:2144-2155` documents returned allocation pitch, or byte width
  for a linear allocation, with a multiple-of-four rule for CUDADEVICEPTR.
  It does not promise a 256-byte alignment condition that suppresses kernels.
- `NvEncoderCuda.cpp:53-71` already allocates the internal ring with
  `cuMemAllocPitch` and registers its returned pitch. A pitched allocation is
  still linear storage.
- `NV_ENC_BUFFER_FORMAT_NV12_PL` is an alias of NV12 (`nvEncodeAPI.h:413`), not
  an independently selectable native tiled format. Planar alternatives in
  this header are IYUV and YV12, not a generic `NV_ENC_BUFFER_FORMAT_YUV420`.
- Mapping yields an NVENC input handle; the API does not specify private
  conversion behavior. The wrapper maps immediately before encode
  (`NvEncoder.cpp:417,500`). The header requires using `mappedBufferFmt` for
  submission, whereas the wrapper uses configured `GetPixelFormat()`
  (`nvEncodeAPI.h:2072`; `NvEncoder.cpp:617`). Current tests register NV12
  consistently, but this distinction matters for future format experiments.
- The registration contract has one resource handle, not separate Y and UV
  array handles. Layered/mipmapped objects are not documented substitutes for
  the required ordinary 2D CUDA array. CUDA VMM's tile-pool usage is sparse
  array backing, not a documented NVENC-native surface allocation option.

### Where preparation happens

```text
In-process real sink:
  capture -> EncoderPreprocessWorker -> EncoderHwWorker -> encoded output

Current fused external_ipc sink:
  capture -> analytics graph / shared NV12 pool -> IPC -> recorder -> output
```

`ModernRecordingPipeline` constructs the external sink with a null preprocess
worker (`modern_recording_pipeline.cpp:129-141`). The diagnostic
`preprocess_only` mode separately creates the preprocess worker without an
encoder. Preprocessing is a responsibility, not a requirement to use one
particular worker class in every architecture.

The external pool uses linear `cudaMalloc` storage with UV prefilled once
(`video_capture.h:325-351`). The fused graph's terminal indirect copy writes
camera Y into it (`yolo_worker.cpp:1277,2063`). The same-GPU registered-source
recorder then registers the pool pointer with pitch equal to width
(`external_recorder_ipc_probe.cpp:4917`). Its staging alternatives also register
buffers once, but **do perform per-frame Y copies**; registration reuse does
not itself make those paths copy-free (`external_recorder_ipc_probe.cpp:5149-5185`).

If a genuinely compatible destination surface is found, the terminal analytics
pool-copy operation or recorder staging copy is the natural place to combine
copying and layout preparation. Changing only `EncoderPreprocessWorker` would
not affect the active external path.

Do not manually tile bytes and register them as ordinary pitch-linear NV12.
NVENC needs a supported resource with the correct interpretation. Merely adding
our own tiling kernel can move or duplicate work without removing the driver's
conversion. Any benefit must include producer/copy and interop costs, not just
the disappearance of a kernel name.

The external buffer contract currently assumes tightly packed Mono8. A future
layout change must explicitly carry pitch/plane offsets and preserve readiness
and lifetime: the recorder must finish consuming a source before Orange reuses
it. The present deferred-release path retires sources through output harvest
and unmap before sending RELEASE.

### Submission versus collection

The prompt's phrase "synchronous EncodePicture" is imprecise. `EncodeFrame`
couples map/submit with packet collection (`NvEncoder.cpp:500-540`), and
`nvEncLockBitstream(doNotWait=false)` is the explicit blocking collection step
(`NvEncoder.cpp:846-852`). Input-slot waits can also block.

`SubmitFrameOnly` and `HarvestEncodedPackets` already exist. The in-process
path has a separate harvest thread for non-direct input. The external
registered-source path still uses coupled `EncodeFrame`; a future recorder
split can reuse existing primitives while preserving source retirement. That
is a scheduling experiment, separate from eliminating layout conversion.

## Candidate experiments, not yet executed

| Priority | Candidate | Question and current assessment |
| --- | --- | --- |
| 1, compatible stack required | Video-native NV12 CUDA array | SDK 13.1 explicitly documents conversion bypass. First isolate encode from a prefilled native array, then include per-frame camera-like writes. Published Linux requirements are driver 610+ and CUDA 13.1+. No upgrade or runtime test has been performed. |
| 2, current-stack control | Controlled `registered-pitched` input | Use the actual returned pitch, correct UV offset, and production feature flags. Cheap discriminator; existing pitched `solid` evidence makes a complete bypass look unlikely. |
| 3 | A true OpenGL-session texture input | Supported input type, but no documented conversion bypass. Include CUDA-GL write/map/unmap cost. Requires an OpenGL encode session and verified headless EGL device selection. |
| Lower priority | Driver-created input buffer diagnostic | Its documented CPU-accessible lock/fill interface does not satisfy the desired device-resident producer path; useful chiefly as a discriminator. |
| Separate integration project | Vulkan video encode images | Native video images are a plausible alternative, but require another encode API, capability checks, resource sharing, and synchronization. Not evidence of a free conversion bypass on the present stack. |

The newer-API research changes the original experiment ranking: a documented
native-video array should take precedence over guessing at generic array or
pitch variants when a compatible test stack is available. The older header's
ordinary CUDAARRAY support does not establish this newer allocation contract.
Exact GA107 layout and measured end-to-end savings remain unestablished.

Each candidate should first be isolated in `tools/nvenc_stress_load.cpp`, with
no camera dependency or production routing change. Proposed acceptance:

1. Record commit, binary provenance, driver/GPU, complete command, actual pitch,
   format/resource type, feature flags, and allocation geometry.
2. Keep AQ, temporal AQ, and lookahead explicitly off for the main comparison;
   control output delay, content, dimensions, and submission rate.
3. Save and decode the bitstream. Use a structured luma pattern that exposes
   row/plane/tile mistakes, neutral UV, and verify frame count and dimensions.
   Uniform content alone is too weak for this check.
4. Confirm no `Convert_*` kernels, and inspect all replacement GPU work and
   transfers. Measure steady-state cost, startup behavior, and achieved fps.
5. Only after a valid standalone result, consider combining preparation with
   the existing pool/staging operation and revalidating lifetime/IPC behavior.

If no suitable input path is found, retain the distinction between scheduling
the work (submit/harvest separation) and moving it off the inference die
(encoder placement). Neither proves the conversion was eliminated.

Do not repeat the already reported copy-stream, alternate-source, MPS, or
phase-lock-only tests without a new hypothesis. No hardware experiments are
part of the worktree/journal setup step.

## Journal entries

### 2026-09-18 — Read-only review and isolated worktree

- Located the dated prompt, verified the original `dfcc344` source baseline,
  and reviewed API/wrapper, stress tool/traces, and recorder dataflow in parallel.
- Explained format versus surface versus memory layout; padding does not
  enlarge the image, and memory tiles are distinct from HEVC CTUs.
- Confirmed that the external path bypasses `EncoderPreprocessWorker`; any
  eventual preparation fusion belongs at the actual pool/staging producer.
- Corroborated approximately 0.617 ms pointer conversion and 0.575 ms array
  conversion from saved SQLite traces. Found conversion in the pitched path
  as well, with the feature-flag comparison caveat recorded above.
- Created this branch/worktree from the exact reviewed commit after observing
  that the source branch had advanced. Added this journal only.
- Open question: is there an API-recognized surface that avoids conversion
  and can be filled efficiently by the existing device-resident producer?

### 2026-09-18 — External research: a documented native-video surface

NVIDIA's [SDK 13.1 announcement](https://developer.nvidia.com/blog/nvidia-video-codec-sdk-13-1-zero-copy-transcode-av1-b-frames-and-frame-accurate-seek/)
describes `AppTransZeroCopy`: allocate frame arrays using `cuArray3DCreate`
with `CUDA_ARRAY3D_VIDEO_ENCODE_DECODE`, then register the whole array through
`NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY`. NVIDIA explicitly says this permits
NVENC to read the input without conversion. Its demonstration uses NVDEC as
the producer; it does not measure a camera-to-array preparation step.

The [SDK 13.1 release notes and requirements](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/read-me/index.html)
identify application-allocated arrays as a new feature and list Ampere among
supported GPU families. Linux requirements are driver 610+ and CUDA Toolkit
13.1+. [NVIDIA's requirements announcement](https://forums.developer.nvidia.com/t/system-requirements-for-video-codec-sdk-v13-1/371379)
independently confirms the driver minimum. This is not a demonstrated fix for
driver 535/API 11.1, and family-level support is not a completed A16 test.
The blog's Docker paragraph mentions CUDA 12.3.2, inconsistent with the SDK's
published build requirements; use the latter for a supported test configuration.
No exact minimum driver for every underlying CUDA array primitive is inferred.

**Why the existing CUDA-array test does not reject this lead:**
`tools/nvenc_stress_load.cpp:707-746` allocates a single unsigned-byte array
of size 4512 x 6768 with surface-load/store enabled. The proposed allocation
is a true multi-plane NV12 video array representing a 4512 x 4512 frame.
The same resource enum can refer to different allocation contracts. The
observed generic-array `Convert_BL2BL` kernels are evidence about that tested
contract, not proof that video-native arrays also require them.

The current [FFmpeg CUDA frame implementation](https://github.com/FFmpeg/FFmpeg/blob/master/libavutil/hwcontext_cuda.c)
provides useful independent implementation evidence: its CUarray path selects
the native NV12 array format, sets both video-encode/decode and surface-load/store
flags, and obtains individual planes using `cuArrayGetPlane`. Its transfer path
copies between linear device buffers and array planes with `cuMemcpy2DAsync`.
This supports a camera-like producer path rather than requiring NVDEC to
generate every frame. It does not establish the cost or execution engine of
those transfers on our A16. A custom producer kernel writing directly through
plane surface objects is a further candidate, not a validated NVIDIA camera
sample result. Record the exact FFmpeg revision if reusing code;
the inspected master URL is a moving reference, not a pinned dependency.

**Where the remaining cost goes:**

```text
Current conceptual path:
  camera Y -> shared linear NV12 slot -> internal tiled NV12 -> encode

First isolated native-array proof:
  prefilled native NV12 array -> encode

Camera-like performance test:
  linear camera Y -> native NV12 Y plane -> encode
  neutral UV is initialized once per reusable slot
```

The second path only tests NVENC's bypass. The third includes the work that
the real producer must do. If the native-array write replaces an existing
copy, one complete intermediate read/write pass can potentially disappear.
If it is added after all current copies, it may mostly move the conversion
cost elsewhere. Even moving work away from inference SMs can help, but that
requires measuring all GPU transfers, producer time, synchronization, and
inference latency; a missing `Convert_*` name alone is insufficient.

For a local copy of Mono8 Y, one read plus one write is about 40.7 MB, versus
the estimated 61.1 MB for a full NV12 read/write. This arithmetic is a candidate
advantage of reusing constant UV, not a measured bandwidth or latency saving.

Current process isolation adds an integration constraint. The existing CUDA
IPC descriptor exports linear allocation handles; it does not transport a
CUarray. A recorder-owned native array fed from the imported pointer is the
smallest proof. Direct producer writes into a cross-process native surface
would need an independently supported sharing/lifetime contract. Replacing
the staging copy is a more immediate fusion opportunity than replacing the
direct registered-source path, which currently has no recorder-side copy.

The [official OpenGL wrapper](https://github.com/NVIDIA/video-sdk-samples/blob/master/Samples/NvCodec/NvEncoder/NvEncoderGL.cpp)
uses a single `GL_R8` rectangle texture sized W x 1.5H for NV12 and an OpenGL
encode session. That is an officially supported input but not a documented
conversion bypass. [Headless EGL](https://developer.nvidia.com/blog/egl-eye-opengl-visualization-without-x-server/)
is available on NVIDIA drivers of this generation; actual A16 device exposure
would need checking. CUDA-GL mapping, writes, and unmapping add ownership and
synchronization costs. Keep this below the documented native-array route.

### 2026-09-18 — Mono8, RGB, and NV12

Using NV12 for monochrome recording remains a sensible choice. Replicating
Mono8 into RGB stores each brightness value three times; common packed RGB
encoder interfaces additionally use four bytes per pixel. NV12 instead uses
a full-resolution brightness plane and constant neutral chroma, averaging
1.5 bytes per pixel. Reusable slots can initialize neutral UV once. Correct
video-range signaling or deliberate range mapping must preserve the intended
camera brightness interpretation.

NVIDIA's [encoder guide, features using CUDA](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html#encoder-features-using-cuda)
explicitly includes RGB encoding. RGB input is supported; the colleague's
implementation was not inherently invalid. But expanding grayscale into RGB
does not create useful color information and can add traffic and conversion
work before a 4:2:0 video encode. No claim is made that every RGB pipeline is
slower: input origin, API, fusion, and driver implementation matter.

NV12 answers which pixel values/planes are present. A native video array
answers how and where those values are stored for the hardware. Optimizing
the first does not automatically optimize the second; the current tiling
penalty does not invalidate the earlier NV12 choice.

### 2026-09-18 — Read-only driver/CUDA upgrade feasibility audit

User asked whether the new feature could be tested without disrupting existing
dependencies and identified the machine provisioning repository. Its actual
path is `/home/jeremy/pancakebatter`, HEAD
`5468194770d3d9ce0f89524db386d996cb3482de`. Three parallel Sol agents reviewed
the provisioner, live camera/module dependencies, and NVIDIA compatibility
documentation. No setup scripts, installers, camera runs, module changes, or
GPU workloads were executed. Only this journal was updated.

Conclusion: a controlled upgrade is feasible in principle. A new toolkit can
be isolated; the host GPU driver cannot be isolated as an ordinary application
dependency. The exact Emergent/OFED/Rivermax combination on R610 is not yet
qualified. Proceeding with a live upgrade is a separate maintenance decision.

| Component | Observed working installation | Initial qualification policy |
| --- | --- | --- |
| OS/kernel | Ubuntu 22.04.2, `6.5.0-44-generic` | Preserve kernel and avoid a simultaneous OS/HWE upgrade. |
| GPU driver | Proprietary NVIDIA 535.183.06, installed by NVIDIA runfile | Driver-only uplift is the system-wide change to qualify first. |
| CUDA | Toolkit 12.2, runtime/compiler 12.2.140 | Keep installed and selected by production. |
| TensorRT | `/usr/local/TensorRT-10.0.1.6` | Preserve libraries, plugins, engines, and existing binaries. |
| Camera | Emergent eSDK 2.55.02, Rivermax 1.41.11 | Preserve and explicitly validate GPU-direct acquisition. |
| NIC/RDMA | ConnectX-7, MLNX_OFED 24.01, `nvidia_peermem` 535.183.06 | Rebuild/verify the NVIDIA peer-memory module against existing OFED. |
| Experimental toolkit/SDK | Not installed | Use separate CUDA 13.1 and SDK 13.1 paths for a standalone probe. |

NVIDIA documents [new-driver/old-runtime backward compatibility](https://docs.nvidia.com/deploy/cuda-compatibility/latest/why-cuda-compatibility.html)
and [side-by-side CUDA toolkits](https://docs.nvidia.com/cuda/archive/13.1.0/cuda-installation-guide-linux/index.html#package-upgrades).
These support retaining the current application stack. They do not certify
third-party kernel modules, camera acquisition, inference outputs, or latency.

[R610.57.04 release notes](https://docs.nvidia.com/datacenter/tesla/tesla-release-notes-610-57-04/index.html)
identify a released Linux driver supporting A16, RTX A6000, and Ubuntu 22.04.
This is a concrete candidate for the SDK 13.1 requirement, not a selected or
installed upgrade. Exact kernel 6.5.0-44 plus eSDK 2.55.02/OFED 24.01 acceptance
remains unverified. R610 offers both module flavors; preserve the proprietary
flavor initially rather than adding an open-module transition to the test.

Concrete local constraints:

- The inspected Orange binary is
  `/home/jeremy/orange-device-roi-20260912/targets/release/orange_client`.
  Its ELF RUNPATH uses `/usr/local/cuda/lib64`; `ldd` resolves CUDA/NPP 12,
  TensorRT 10, EVT, and the existing `/opt/orange/lib` FFmpeg/OpenCV libraries.
  `/usr/local/cuda` currently resolves to `/usr/local/cuda-12.2`. Preserve this
  link, global loader configuration, and production environment paths.
- `pancakebatter/system_config.yml:9-20` records the matching version pins.
  `install_nvidia_drivers.sh:4,63` invokes the exact 535 runfile. This is not
  an apt-owned NVIDIA installation; changing installation ownership adds a
  separate migration variable.
- Existing installers are bootstrap helpers, not a rollback framework.
  `install_nvidia_cuda.sh` deletes its downloaded installer and has inconsistent
  12.2.2 versus 12.2 path assumptions. `uninstall_nvidia_cuda.sh:57-75` removes
  CUDA 12.2, its default symlink, and shell exports. Do not use it for this
  driver-only experiment. TRT/FFmpeg/OpenCV installers also modify global
  libraries and environment; do not rerun them as part of the upgrade.
- No old NVIDIA driver runfile was found in the inspected home, `/opt`,
  `/usr/local/src`, and apt-cache locations. Acquire and verify the exact
  535.183.06 rollback payload before any upgrade; a provisioner in git does
  not preserve excluded installer binaries.
- Only the current kernel image is installed. `/boot/vmlinuz.old` and
  `/boot/initrd.img.old` resolve to the same current kernel, not an independent
  fallback. Root is ext4; no bootable recovery image was established by this
  audit. Secure Boot is disabled.
- `dkms status` reports a missing source configuration for stale
  `nvidia-fs/2.17.5`. Audit/reconcile that entry before relying on DKMS
  automation; this is not evidence that current camera streaming is broken.
- The installed EVT GPUDirect notes at
  `/opt/EVT/eSDK/Examples/EVT_BenchmarkHS_GpuDirect/readme_cuda_12.txt:70-128`
  explain the driver/OFED installation order and peer-memory dependency.
  Loaded `nvidia_peermem` depends on both `nvidia` and `ib_core`.
  NVIDIA's [GPUDirect guidance](https://docs.nvidia.com/cuda/gpudirect-rdma/#using-nvidia-peermem)
  requires building it against the installed OFED interface. An ordinary CUDA
  inference test or `nvidia-smi` alone cannot validate this acquisition path.
- The older local guide `docs/install_linux_cuda_eSDK.md:14-22` documents why
  kernel 6.5 was retained for EVT; its historical limit is not a current vendor
  support matrix. Keep it fixed during this experiment unless qualification
  establishes a separate need to change it.

Proposed maintenance sequence, not executed:

1. Choose a recovery plan. A full drive image or bootable clone is a recommended
   precaution for this specialized rig, not a technical prerequisite. At minimum,
   retain verified current/target driver installers, the current kernel and
   headers, saved module/config/binary inventories, and usable console/recovery
   access. A tested system image gives a broader fallback if targeted driver
   restoration proves insufficient. Establish a current real-camera baseline.
2. During downtime, qualify only the new host driver on the existing kernel,
   with current OFED present and the expected module flavor explicitly chosen.
   Preserve CUDA 12.2, TensorRT, camera SDK, and application libraries.
3. After reboot, verify all nine GPUs, module versions, peer-memory and NIC
   operation; then test one-camera GPU-direct capture and the existing PTP
   multi-camera recording/inference/GUI path. Require real decoded content,
   expected throughput, zero camera gaps/encode drops, and comparable latency.
   Positive-detection tests are still needed for full crop/pose acceptance.
4. Only after the unchanged production application passes, use an isolated
   CUDA 13.1/SDK 13.1 probe to measure native-array input. Vendor its headers;
   do not replace the global old NVCodec headers used by FFmpeg. Keep CUDA
   major-version integration out of the production Orange process initially.
5. If qualification fails, restore the verified old driver or, if prepared,
   the bootable system image and revalidate acquisition. Reverting the CUDA symlink alone cannot
   roll back a kernel-driver change. A container also uses the host driver and
   cannot remove that compatibility requirement.

### 2026-09-18 — Concrete Data2 image and NVENC investigation sequence

The user's RAID/layout discussion was a side comment. Preserve all current
storage layouts. The immediate objective is a recoverable system image on
Data2 followed by the isolated NVENC investigation, not a storage migration.
This entry is a prepared plan; no backup, USB write, reboot, installer download,
driver installation, or performance test has been performed.

Disk inventory rechecked read-only:

| Role | Current device | Physical serial | Filesystem identity |
| --- | --- | --- | --- |
| System disk, complete image source | `/dev/nvme1n1` | `48816072200222` | Root p2: `c60c38ed-2574-4698-be88-7c06aeef8582`; EFI p1: `77E2-AB0D` |
| Data2, image repository filesystem | `/dev/nvme0n1p2` | Disk serial `48816072200219` | `bb4ea7ac-11f3-4f20-80b1-5e3cb0927711` |

Both physical drives are Sabrent SB-RKT4P-8TB, 8,001,563,222,016 bytes.
Current root used space is 2,367,097,540,608 bytes (about 2.37 TB); Data2 has
3,935,621,046,272 bytes available (about 3.94 TB). This should fit one used-block
system image without depending on compression. Recheck capacity immediately
before imaging. Device numbers may change in the live environment: identify
the source and repository by serial and filesystem UUID, not numbering alone.

Preparation while Ubuntu is running:

- Obtain an erasable USB flash drive; 4 GB or larger provides comfortable
  room for an amd64 UEFI Clonezilla Live boot image. Identify its exact device
  before writing it. A local console or remote KVM is needed across reboots;
  the current assistant session is not a recovery console.
- Download and verify the recovery media, exact old 535.183.06 installer,
  chosen R610 installer, and SDK/toolkit artifacts needed for the test. Keep
  recovery instructions and the old installer accessible from Data2 and an
  independent device. No payloads have yet been acquired in this investigation.
- Save package/module versions, driver settings, UEFI boot entries, networking
  configuration, GPU UUID/PCI placement, current binaries/engines, and source
  revisions. Establish the current PTP camera/recording/inference baseline.
- Reserve a new backup directory on Data2, for example
  `/mnt/Data2/system-backups/pre-nvenc-native-surface-20260918`. This path is
  proposed, not created. Keep post-image test artifacts and source patches on
  Data2 so they survive a possible system-disk restore.

Offline image sequence:

1. Stop acquisition/recording and shut down Ubuntu cleanly; boot Clonezilla
   Live in UEFI mode. The system source filesystems must remain unmounted.
2. Use the device-image workflow, choose Data2's existing filesystem as the
   image repository, and save the complete system disk, including EFI/root
   partitions and the disk partition table. Store an image directory; do not
   select device-to-device cloning or format the repository.
3. Enable the saved-image integrity check and retain its result. Confirm the
   recovery environment can find the image before proceeding. This checks
   readability/integrity; it is not a full restore-and-boot rehearsal.
4. Reboot Ubuntu normally and verify baseline operation before the upgrade.

These steps follow the [Clonezilla save-image workflow](https://clonezilla.org/show-live-doc-content.php?topic=clonezilla-live/doc/01_Save_disk_image).
The [USB instructions](https://clonezilla.org/liveusb.php) support UEFI boot.
Allow several hours for the maintenance window; actual backup and verification
time depends on sustained NVMe throughput and compression and has not been
benchmarked here. Reserve additional time for driver qualification or recovery.

Driver qualification and experiment:

1. Upgrade only the selected NVIDIA driver during downtime, preserving the
   current kernel/OFED/eSDK/CUDA 12.2/TensorRT stack and rebuilding peer-memory
   support as described in the previous entry. Reboot.
2. Validate the original applications first: nine GPU identities, GPU-direct
   camera ingress, PTP cadence, expected recording throughput/content, no gaps
   or encode failures, YOLO/pose latency, and GUI behavior. Do not equate a
   successful `nvidia-smi` call with application acceptance.
3. Install/select CUDA 13.1 in an isolated versioned location; leave production's
   `/usr/local/cuda` link intact. Build a separate SDK 13.1 encode probe in the
   investigation worktree. Implementation and builds remain to be done.
4. On the SAME new driver, compare the original linear-NV12 baseline against
   video-native NV12 arrays with matched content, dimensions, flags, and rate.
   Test prefilled input first to isolate NVENC, then include per-frame Mono8
   writes. Measure all replacement copies/kernels, validate decoded images,
   and finally measure interference with inference. A before/after comparison
   spanning two drivers alone would confound the allocation and driver changes.
5. If the old production path regresses, restore the old driver first or use
   the saved image. A [whole-disk image restore](https://clonezilla.org/show-live-doc-content.php?topic=clonezilla-live/doc/02_Restore_disk_image)
   overwrites the system disk and returns its files to the image date. Reidentify
   disks by serial and keep Data2 as the image source, never the restore target.

The backup and compatibility work enable the experiment; they do not guarantee
that the camera-to-native-array path saves the observed 0.6 ms. That remains
the performance question the controlled test must answer.

### 2026-09-18 — Clonezilla UEFI USB prepared

The user supplied a new nominal 32 GB SanDisk USB. Read-only identification
found `/dev/sda`, model `SanDisk 3.2Gen1`, 30,784,094,208 bytes, removable USB,
with one existing FAT partition UUID `1624-1203`, mounted at
`/media/jeremy/1624-1203`. Its serial is
`0401c6eede1b9176c828855daff0c7e687878f35015fa83c8b5db8d0bb91b4a6d54b0000000000000000000014dbcb2200952a188155810789b3455d`.
The only existing root entries were the SanDisk software installers/PDF and
`System Volume Information`; these were preserved.

Downloaded stable `clonezilla-live-3.3.3-15-amd64.zip` through the official
download page's SourceForge link. Verified the detached checksum signature
against the published DRBL fingerprint
`54C0821A48715DAFD61BFCAF667857D045599AFD`, verified all ZIP CRCs, and matched
SHA-256 `00cee7700433e63017e2ea9eb40519108829710132364a8028a6c039a6046304`.
The archive is 561,478,648 bytes. Sources:
[release and checksums](https://clonezilla.org/downloads.php),
[verification procedure](https://clonezilla.org/gpg-verify.php).

Followed the official [UEFI USB method](https://clonezilla.org/liveusb.php):
extract all ZIP entries, including `.disk`, onto the existing FAT filesystem.
No partitioning, formatting, raw disk writing, or legacy boot installer was
needed. The script rechecked the exact USB serial, capacity, transport,
filesystem UUID, mount path, free space, and absence of destination collisions
before writing. It flushed the files and checked every copied file by SHA-256.
Confirmed the fallback UEFI loader, GRUB configuration, kernel, initrd, and
live filesystem were present. This is file verification, not a boot test.

The USB's `Orange-Recovery/` directory holds the handoff, this journal, signed
download checksums, signing key, signature-verification output, and copied-file
verification manifest. The staging archive/script/report remain under
`/tmp/clonezilla-usb-20260918` on the rig. No reboot, system image, driver
installation, or GPU/camera test has been performed. The user already copied
the earlier handoff to an independent device; the USB copy now records the
completed media preparation.

### 2026-09-19 — System image saved, checked, and normal Ubuntu boot confirmed

Jeremy returned from the offline Clonezilla session and explicitly confirmed
that its image check passed. This verification result is user-reported; the
assistant independently inspected the saved metadata/logs and the running
system, without rereading the entire 2 TB compressed root image or performing
a restore-and-boot rehearsal.

Actual image directory:
`/mnt/Data2/pre-nvenc-native-surface-20260918`.
This is directly under Data2, not the earlier proposed `system-backups/`
subdirectory. The directory has 29 files totaling 2,126,669,165,232 bytes
(about 2.13 TB). Data2 had 1,808,951,586,816 bytes free after reboot.

The `clonezilla-img` log records Clonezilla Live 3.3.3-15, a save operation
starting 2026-09-18 22:11:42 EDT, and image creation at 22:53:52 EDT. The
`savedisk` source is explicitly physical serial `48816072200222`, the system
disk. Both the EFI partition (`nvme1n1p1`, FAT32) and root partition
(`nvme1n1p2`, ext4) report successful Partclone saves and 100% completion.
The root copy reports 35 minutes 30 seconds of copy time. Both compressed
partition images, primary/secondary GPT data, MBR data, and UEFI NVRAM dump
are present. Saved filesystem UUIDs match the pre-backup inventory.
`Info-img-id.txt` matches SHA-512 of `clonezilla-img`; this identifier checks
the metadata log only, not the partition-image payloads. The separate full
Clonezilla image-check success was confirmed by Jeremy.

Post-reboot read-only host checks:

- System still boots from disk serial `48816072200222`, root UUID
  `c60c38ed-2574-4698-be88-7c06aeef8582`.
- Data2 remains disk serial `48816072200219`, filesystem UUID
  `bb4ea7ac-11f3-4f20-80b1-5e3cb0927711`.
- Kernel remains `6.5.0-44-generic`.
- All nine GPUs (one RTX A6000, eight A16 devices) are visible through
  `nvidia-smi` outside the tool sandbox and report driver `535.183.06`.
- `/usr/local/cuda` still resolves to `/usr/local/cuda-12.2`.

The sandbox's restricted GPU device view initially prevented `nvidia-smi`
from communicating; the same read-only query outside the sandbox succeeded.
A small EFI compressed-stream test was also denied access to the image file
and was not repeated after Jeremy confirmed the full Clonezilla check. No
backup files were modified. No driver/toolkit installation or live camera/GPU
workload was run. Camera/application acceptance remains separate from these
boot and driver-enumeration checks.

The recovery checkpoint is established. Before changing the driver, acquire
and verify the exact old and selected new installers, finish the kernel/OFED/
peer-memory compatibility checks, and establish the current live application
baseline. Continue to preserve the original CUDA/TensorRT/application stack
while qualifying the driver and the isolated native-array probe.

### 2026-09-19 — Preserve conversation context across a possible disk restore

The current thread has a local JSONL transcript beneath
`/home/jeremy/.codex/sessions/2026/09/18/`. Codex also maintains local history
and a state database in `~/.codex`. Official documentation describes local
session persistence and saved-chat resume; it does not establish a guaranteed
cloud recovery mechanism for this particular client/session:
[local state and history](https://learn.chatgpt.com/docs/config-file/config-advanced#history-persistence),
[resuming saved chats](https://learn.chatgpt.com/docs/projects).

Inference from the whole-system backup: restoring its root filesystem should
also restore the local session data that existed at imaging time. It would
roll back later local chat history and worktree edits. The image is not a
continuously updated backup.

A separate recovery bundle was saved at
`/mnt/Data2/nvenc-session-recovery-20260919`, outside the Clonezilla image
directory, with the current complete JSONL records, journal, handoff, and
resume/context instructions. It is a timestamped snapshot, not continuous
synchronization; refresh it after later work and before any rollback. The
directory is restricted to the current user. It excludes authentication files
and does not claim to be a complete app-state backup or guarantee automatic
client session import. If reopening the original thread is unavailable, use
the journal and handoff to provide context to a new chat.

### 2026-09-19 — Pancakebatter ownership, DKMS, and maintained upgrades

The user wants the upgrade maintained through `/home/jeremy/pancakebatter`
and asked whether runfiles are preferable to avoid unintended DKMS driver
selection. Parallel read-only reviews inspected the provisioner, actual host
state, and current NVIDIA package documentation. No provisioner or host
configuration was changed; the provisioner already contains unrelated local
changes.

Important correction: the current 535.183.06 driver is a runfile installation
WITH DKMS registration. `/var/log/nvidia-installer.log:549-552` records the
accepted DKMS registration; targeted `dkms status -m nvidia -v 535.183.06`
reports installation for 6.5.0-44. All five NVIDIA module paths resolve under
that kernel's `updates/dkms/`. APT does not own the installed driver. OFED is
also DKMS-managed. Global `dkms status` stops on the orphaned
`nvidia-fs/2.17.5/source/dkms.conf`, hiding later healthy registrations.
This stale GPUDirect Storage entry is separate from the loaded, working
`nvidia_peermem` camera path and needs deliberate maintenance before relying
on a global rebuild.

DKMS builds locally registered source for a target kernel; it does not fetch
new driver releases from a repository. Its autoinstall logic can select the
latest registered module version, so retaining multiple registered NVIDIA
versions is not a sound rollback strategy. Keep rollback installers/packages
as archives and enforce a deliberate installed version and consistent
kernel/user-space driver components. Available repository versions and files
in a download cache are different from installed/registered versions.
Runfile versus Debian packages and DKMS versus manual/prebuilt modules are
separate choices; NVIDIA's [610 runfile documentation](https://download.nvidia.com/XFree86/Linux-x86_64/610.57.04/README/installdriver.html)
explicitly supports DKMS registration.

For a maintained Ubuntu deployment, the preferred target is one package
owner, exact version pins, and a captured dependency transaction. NVIDIA
[recommends distribution packages where practical](https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/choose-an-installation-method.html)
and documents [branch and exact-version pins](https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/version-locking.html#apt).
An exact runfile remains a reasonable option for the first isolated driver
experiment if package migration adds unwanted changes; avoiding DKMS alone
is not a reason to select it. Any package migration must explicitly remove
the runfile-owned driver before installing its package replacement.

The official [Ubuntu 22.04 repository metadata](https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/Packages.gz)
contains proprietary and open R610.57.04 packages, plus
`nvidia-driver-pinning-610.57.04`. Both R610 DKMS packages require DKMS >=3.1.8;
this host has 2.8.7. The transaction therefore includes a DKMS framework
upgrade affecting the existing OFED module workflow, which must be qualified
and explicitly versioned. `cuda-drivers` in this repository selects driver
packages; it does not itself install a new CUDA toolkit. Do not use broad
CUDA metapackages or assume a driver-only request has no dependencies.

Keep proprietary kernel modules for the first R610 qualification to minimize
changes to the proven camera path. This is an experiment-control choice:
[R610 documentation](https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/610/kernel-modules.html)
still offers both flavors and generally recommends open modules on supported
GPUs. The existing kernel/OFED/eSDK combination has not been qualified with
R610. Build NVIDIA peer-memory support against the existing OFED and validate
real camera DMA; see [NVIDIA peer-memory guidance](https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/optional-components.html#using-nvidia-peermem).

Recommended Pancakebatter work before applying an upgrade:

- A separate experimental target manifest with exact driver version, installer
  method, module flavor, DKMS version, kernel/headers, OFED version, expected
  GPU identities, artifact hashes, and rollback artifacts. Keep the validated
  baseline separate until acceptance passes.
- A read-only plan/preflight, artifact download/verification stage, explicit
  apply stage, and post-reboot verification. Reuse the existing preview/`--apply`
  convention in `camera_net_config.py:604-642`. Capture an APT simulation for
  a package migration and reject unexpected kernel/OFED/toolkit changes.
- Resolve orphan registrations; require clean global and targeted DKMS state,
  one intended NVIDIA driver owner/version, and an explicit peer-memory build
  order. Do not interpret a successful installer exit as rig acceptance.
- Preserve CUDA 12.2, TensorRT, the production CUDA symlink, and global library
  paths. Treat the isolated 13.1 toolkit as a separate step. Retain exact old
  installers and the checked Clonezilla image.
- Verify all nine GPUs and driver/library parity, then real PTP acquisition,
  recording content/throughput, drops, inference timing, and GUI behavior.

The current `install_nvidia_drivers.sh:4,63` hard-codes the runfile and invokes
it silently without this contract. `check_system.sh:328-403` accepts any GPU
count above zero and a very broad driver minimum; it does not validate the
configured exact driver/kernel/OFED stack. These are the concrete maintenance
gaps to address, whichever installation method is ultimately selected.

### 2026-09-19 — Maintained migration planner implemented and exercised

Added nine new files in `/home/jeremy/pancakebatter`: the
`nvidia_driver_upgrade.py` entry point, two read-only collector modules, an
experimental `configs/nvidia_driver_upgrade.pancake0.json`, three test files,
and `docs/nvidia_driver_upgrade_runbook.md`. The existing validated baseline,
installers, and 109 pre-existing repository files remain unchanged by checksum.
No commit was made.

The planner defaults to an offline host/recovery audit. An explicit
`--refresh-metadata` plus a new private output directory fetches signed NVIDIA
metadata and simulates APT against the actual installed package database.
It has no install/apply mode. Host APT configuration/hooks, lists, cache and
logs are isolated; the system dpkg status is only read. The manifest pins
proprietary driver `610.57.04-1ubuntu1`, DKMS `1:3.2.1-1ubuntu1`, and the NVIDIA
EGL framework dependencies, while preserving the existing kernel and CUDA 12.2.

Final command (executed with normal host GPU/network visibility):

```bash
cd /home/jeremy/pancakebatter
PYTHONDONTWRITEBYTECODE=1 python3 nvidia_driver_upgrade.py   --output-dir /mnt/Data2/nvenc-driver-plan-20260919 --refresh-metadata
```

The private APT simulation passed policy checks with 22 package transitions:
21 additions and DKMS 2.8.7 -> 3.2.1. No removals, kernel/OFED replacements,
CUDA-toolkit changes, TensorRT changes, or open-module substitutions. The earlier
agent-only preview had 25 transitions because optional recommendations were
included; the final manifest uses `install_recommends = false`. NVIDIA settings,
libxnvctrl0 and screen-resolution-extra are therefore not selected.

All nine expected GPU UUIDs remain visible on 535.183.06. Kernel/headers,
NVIDIA module flavor/version, loaded `nvidia_peermem`, OFED package and DKMS
module versions, and CUDA/TensorRT paths match the recorded baseline. The
Data2 image metadata matches the saved system disk serial; its large payload
was not reread. Successful Clonezilla image checking remains user-attested.

The overall report exits 2 / `blocked` for two real preparation issues:

- Stale `nvidia-fs/2.17.5` DKMS source registration with missing `dkms.conf`.
- Missing preserved 535.183.06 rollback runfile; its manifest SHA-256 is null
  until independently verified recovery material is saved.

Results are under `/mnt/Data2/nvenc-driver-plan-20260919`, including
`summary.txt`, `report.json`, `packages.tsv`, manifest and checksums, private
APT metadata/commands/output, and a source snapshot. Ubuntu dependency metadata
was copied from the host cache, not refreshed; package payload availability and
runtime compatibility are not established by this simulation. No `.deb` payloads
were fetched. A matching rollback installer and frozen target payloads, clean
DKMS state, and a reviewed maintenance procedure are still needed before
executing the migration. Afterward, host/GPUDirect/camera/inference acceptance
must pass before attempting the isolated CUDA 13.1 native-NV12 experiment.

Validation: 33 unit tests pass, final real-host APT simulation passes, and
checksums of 30 system dpkg/APT configuration files plus all 109 pre-existing
Pancakebatter files are unchanged. No driver, kernel, toolkit, service,
boot configuration, or DKMS state was changed, and no reboot was performed.

### 2026-09-19 — Rollback installer verified; orphan cleanup prepared

The user authorized completing the two preparation items. Downloaded the exact
535.183.06 x86-64 runfile from NVIDIA's official HTTPS Tesla release URL. The
341,920,517-byte artifact is now preserved at:

`/mnt/Data2/nvenc-session-recovery-20260919/NVIDIA-Linux-x86_64-535.183.06.run`

SHA-256: `c7bb0a0569c5347845479ed4e3e4d885c6ee3b8adf068c3401cdf754d5ba3d3b`.
It matches the independently published Google gVisor checksum for precisely
535.183.06. A commit-pinned Google COS manifest independently matches the
filename, size, SHA-512 and BLAKE2B. Saved source records and
`rollback-verification.json` sit beside the runfile. NVIDIA's adjacent SHA-256
URLs returned 404; no claim is made that NVIDIA published that checksum.
The distinct NVIDIA redist tarball has a different checksum and was not used.
The rollback installer was never executed. Pancakebatter now pins its SHA-256
and evidence location in the experimental manifest.

Independent read-only review confirmed that the stale `nvidia-fs` directory
contains only a 5.19.0-32 module and build debris plus its old active-kernel
symlink. Its `/usr/src` source and old installed-module tree are missing.
No GDS kernel module is loaded or installed under the present kernel tree.
The loaded `nvidia_peermem` camera bridge remains distinct and healthy.
CUDA 12.2's unowned cuFile/GDS user-space files remain in place.

A guarded cleanup script is saved on Data2:
`/mnt/Data2/nvenc-session-recovery-20260919/quarantine_stale_gds.py`.
It verifies the exact orphan shape and lack of an active GDS module/package,
verifies a byte/metadata-preserving tar backup on Data2, then atomically moves
the whole registration outside `/var/lib/dkms`. It checks for unexpected
xattrs/ACLs before proceeding and verifies unchanged NVIDIA/OFED module files
and dpkg status after the move. It does not invoke DKMS removal, rebuild
initramfs, unload anything, or touch CUDA user space. The old build's embedded
`dkms.conf` cannot safely be substituted: its package version is inconsistent,
and DKMS removal can invoke uninstall/depmod/initrd paths.

The attempted privileged invocation stopped at `sudo: a password is required`
before running Python. The session has passwordless camera/benchmark wrappers,
but none authorizes generic maintenance; those wrappers were not repurposed.
The user was given the exact terminal command through the chat. At this entry,
cleanup is pending the user's authenticated execution, not completed:

```bash
sudo python3 /mnt/Data2/nvenc-session-recovery-20260919/quarantine_stale_gds.py
```

The refreshed report is
`/mnt/Data2/nvenc-driver-plan-recovery-verified-20260919`. It verifies the saved
rollback runfile and again passes the same 22-transition APT simulation. Its
only current blocker is the stale `nvidia-fs` registration. All nine GPUs remain
on 535.183.06; production kernel/CUDA/TRT/OFED and dpkg/APT configuration hashes
are unchanged. No installer, driver upgrade, or reboot has run. After the
terminal cleanup succeeds, inspect its saved report and rerun the preflight.

### 2026-09-19 — Orphan quarantined and preflight clear

The user executed the guarded cleanup through an authenticated terminal. The
saved record confirms an atomic quarantine of `/var/lib/dkms/nvidia-fs` to
`/var/lib/dkms-quarantine/nvidia-fs-2.17.5-20260919`, with the exact inventory
verified and critical NVIDIA/peer-memory/OFED module files plus dpkg status
unchanged. The Data2 backup is
`/mnt/Data2/nvenc-dkms-cleanup-20260919/nvidia-fs-registration.tar.gz`;
SHA-256 `485c262e12808c4e86310950ae0aa65d0879be554288d98b9139c95f1d6c5f95`
was independently recomputed and matches the cleanup record. The live orphan
registration is absent. Global DKMS status now exits 0 with empty stderr and
lists the expected NVIDIA 535.183.06 and OFED registrations for kernel 6.5.0-44.

The subsequent read-only planner run saved to
`/mnt/Data2/nvenc-driver-plan-preflight-clean-20260919` exits 0 with
`status = review_required` and zero blockers. It verifies all nine expected
GPU UUIDs on 535.183.06, loaded proprietary NVIDIA plus matching nvidia_peermem,
the original kernel/OFED/CUDA/TensorRT baseline, the preserved rollback
installer, and the same exact 22-transition APT simulation. Critical module
files and dpkg/APT configuration checksums remain unchanged after cleanup.

Both preparation blockers are cleared. No driver upgrade, package install,
module unload, CUDA change, service stop, or reboot occurred. The planner still
sets installation_ready=false: target package payloads and a concrete
maintenance/rollback procedure remain to be prepared, followed by actual
post-upgrade GPU/GPUDirect/camera/inference validation. The runbook now records
the cleanup as complete rather than presenting it as pending.

### 2026-09-19 — Exact driver packages frozen; offline build and install simulation pass

The user authorized downloading the reviewed packages and preparing a concrete
maintenance/rollback procedure. Added a download-only companion to the
Pancakebatter planner, `nvidia_driver_prefetch.py`. It validates the saved plan,
reverifies NVIDIA/Ubuntu repository signatures and signed package-index hashes,
then downloads exact versions into private APT state. Every payload is checked
for SHA-256, size, package name, version, and architecture before entering the
lock. The native target is still exactly 22 transitions, with no recommendations.

The durable bundle is `/mnt/Data2/nvenc-driver-packages-20260919`. Its 22 target
payloads total 405,137,980 bytes. The previous DKMS package
`2.8.7-2ubuntu2` is saved separately under `packages/rollback/`; it is not part
of the target install. Package locks, signed source metadata, checksums, exact
APT preferences, source snapshots, and the maintenance/rollback document are
preserved with the files. The verified 535 runfile remains in the separate
recovery directory. No package has been installed.

An offline APT simulation using all 22 explicit local files, empty repository
configuration, normal locking, `--no-download`, `--no-remove`, and
`--no-install-recommends` selected exactly the approved transitions. APT needed
copies of the files in its private archive cache; an empty archive cache with
local file arguments was insufficient under `--no-download`. The successful
command and output are saved in the bundle.

Extracted proprietary 610.57.04 kernel sources built successfully in `/tmp`
with GCC 12 against kernel `6.5.0-44-generic`. All five resulting modules report
the expected version and kernel. `nvidia-peermem` used the explicit current
kernel's OFED headers and Module.symvers, links to `nvidia,ib_core`, and references
the peer-memory registration symbols. This avoids the stale OFED `default`
symlink, which points to 5.19. The scratch modules were not installed, registered
with DKMS, or loaded. Compilation is evidence of build compatibility; camera,
TensorRT, NVENC, and desktop runtime compatibility remain to be tested later.

Maintainer-script review found that nvidia-dkms can invoke a surviving runfile
uninstaller and ignore failure. The reviewed procedure explicitly removes 535
and checks ownership before APT. It archives DKMS, boot, and runfile backup
state before the DKMS 3 migration; preserves OFED; temporarily masks only
nvidia-persistenced; verifies module versions and initramfs; and keeps the saved
graphical boot target. Rollback removes the target modules while DKMS 3 remains
available, restores old DKMS, then reinstalls the verified 535 runfile. Extracted
DKMS 3.2.1 also read the existing registrations cleanly without modifying them.

The existing installation has 32-bit NVIDIA libraries and NVIDIA settings/xconfig
utilities that the native 22-package set omits. Jeremy confirmed no known
32-bit GPU applications; keep the reviewed native profile. An optional i386 dependency probe
did not resolve because of the current libdrm2 mirror/version mismatch; this is
not missing NVIDIA i386 support. The optional settings/xconfig tools remain
outside the transaction. Production CUDA 12.2, TensorRT 10.0.1.6, OFED, EVT, Rivermax, and the
kernel are outside the driver transaction. The separate CUDA 13.1 native-NV12
experiment follows successful driver acceptance.

Validation: all 40 Pancakebatter tests pass; maintenance shell blocks parse;
bundle checksums and package identities verify; offline APT selects exactly 22
transitions; and the five-module compile succeeds. System dpkg/APT files and
all unrelated pre-existing Pancakebatter files remain unchanged. All nine GPUs
remain on 535.183.06. No service was stopped and no reboot was performed.

### 2026-09-19 — Driver 610 migration and initial post-reboot acceptance

The user ran the reviewed Pancakebatter console-entry and apply helpers from
`/home/jeremy/pancakebatter`. The entry record at
`/mnt/Data2/nvenc-driver-console-entry-20260919T215605Z-lrljphtf/status.json`
reports a verified 1,102,755,840-byte live-state backup with SHA-256
`7e4478f43a611a4d7d8e511f4c7a0fb1a7ad6394aa404539742457aaaa35dabd` before
driver unload. During removal the user answered No to restoring the installer's
X configuration, preserving the current X configuration. The apply record at
`/mnt/Data2/nvenc-driver-console-entry-20260919T215605Z-lrljphtf/apply-status.json`
reports `ready_for_reboot`: exactly the frozen 22-package offline transaction
installed the proprietary 610.57.04 driver and DKMS 3.2.1. The maintained
helper suite passed all 59 tests.

The user temporarily set the default target to `multi-user.target` and
rebooted. On boot ID `b9bc5853-4478-4219-b890-b8bc9e09d1db`, kernel
`6.5.0-44-generic` remains in use. All nine GPU UUID/index/PCI-address tuples
match the saved 535 baseline and report driver 610.57.04. The proprietary
NVIDIA modules and `nvidia_peermem` 610.57.04 are loaded, OFED/openibd is
active, and the existing non-NVIDIA DKMS registrations remain installed.
`/usr/local/cuda` still selects CUDA 12.2 and TensorRT 10.0.1.6 remains present.

The installed CUDA 12.2 `deviceQuery` passes and reports driver API 13.3 with
runtime 12.2. `vectorAdd` passes independently on each of the nine GPUs when
selected by UUID. The inspected boot log contains no NVRM Xid or unknown-symbol
report. The two failed units, `rpc-svcgssd.service` and
`systemd-networkd-wait-online.service`, failed with the same causes on the
preceding 535 boot and are not new migration regressions.

Durable post-boot evidence is in
`/mnt/Data2/nvenc-driver-postboot-20260919-b9bc5853`; all 16 entries in its
`SHA256SUMS` verify. GDM remains inactive and the default target remains
`multi-user.target`; restoring `graphical.target` and starting GDM is the next
manual operator step. Real-camera GPUDirect, real TensorRT, NVENC recording and
throughput, display/GUI acceptance, and the isolated CUDA 13.1 native-NV12 probe
remain pending. These host and basic CUDA checks establish no NVENC tiling-cost,
latency, or performance improvement.

### 2026-09-19 — Driver 610 final runtime acceptance and next experiment

Final runtime acceptance passed on NVIDIA driver `610.57.04` with the existing
`6.5.0-44-generic` kernel. CUDA 12.2 remains the production toolkit. All nine
GPUs had already passed the installed CUDA 12.2 `deviceQuery` and per-device
`vectorAdd` checks. The workstation is restored to `graphical.target`; GDM is
running, and the desktop reports direct NVIDIA OpenGL rendering on the RTX
A6000 with driver 610.57.04. An EVT acquisition check delivered `501` frames at
`100.017 fps` with zero timeouts and zero frame-ID gaps.

The acceptance evidence root is
`/tmp/nvenc-driver-acceptance-20260919T222953Z`. The final archive is
`/mnt/Data2/nvenc-driver-runtime-acceptance-20260919`. The authoritative timed
headless run is under `supervised-timed/`. It used
`supervise_processes = true` and `record_for_seconds = 6`; both cameras
(`2010095` and `2010096`) received, acknowledged, and encoded `602/602` frames.
Both had zero skipped or dropped frames, camera frame-ID gaps, GetFrame errors,
preprocess drops, IPC failures, and encode failures. Each output has `602`
metadata rows and packets and is a `4512x4512` HEVC stream with `6.02 s` of
decoded real content. For the `552` rows with `recording_frame_id > 50`,
`acquisition_to_detect_done_ms` p95 was `2.649047 ms` for `2010095` and
`2.599986 ms` for `2010096`; YOLO queue-wait p95 was `0.015048 ms` and
`0.017964 ms`, respectively.

That run used camera config `100_cam4_ptp_fourcam`: `2010095` analytics source
GPU `7` with recorder shards `7,8`, and `2010096` source GPU `5` with shards
`5,6`. Acquisition and external HEVC recording ran at `100 fps` with PTP gating,
TwoStep PTP, register-read decimation `100`, and spatial and temporal AQ off.
Two preliminary attempts remain in the acceptance root as negative evidence:
the old manual runner produced a session-identity mismatch, and the first
supervised spec omitted `fixed.recording_control`. The accepted run corrected
both conditions.

GUI autorun acceptance also passed. The recording artifact is
`gui/recordings/2026_09_19_18_35_17` below the acceptance root. Each camera
received, acknowledged, and encoded `1002/1002` frames with zero drops, skips,
or recorded errors. Both `4512x4512` HEVC videos are `10.02 s` long, carry real
content, and measured `152.232 Mbps` for `2010095` and `152.121 Mbps` for
`2010096`. `gui/validation.json` passed with zero warnings. A separate
`external-verification-after-video-sanity-result.json` exited `0`; the real
`scripts/external_video_sanity.py` checks generated the expected content
sidecars. Real-YOLO `acquisition_to_detect_done_ms` p95 was `2.948369 ms` and
`2.845787 ms`, with queue-wait p95 `0.017803 ms` and `0.018174 ms`; the GUI ran
at about `60 fps`.

Camera `2010095` produced positive detections while `2010096` produced none.
Crop processing was disabled, so this run makes no claim about crop, pose,
tracking, or detection quality. Two existing headless metadata inconsistencies
also remain explicit: the manifest reports `actual_recording_duration_s = 0`
despite about `6.018 s` elapsed and `6.02 s` of video, and `runs.csv` counters
can lag the authoritative terminal recorder counts by one. The authoritative
counts for this acceptance are the `602` terminal recorder, metadata, and
packet totals above.

This acceptance establishes that the maintained driver migration preserved the
production desktop, camera, CUDA 12.2/TensorRT, real-YOLO, PTP, external IPC,
and split-GOP recording paths. It does not establish a causal performance
improvement from driver 610 or a reduction in the NVENC input-tiling cost.

The old driver-535 trace remains the comparison hypothesis: it showed two
conversion launches per frame, approximately `207 us` and `410 us`, for about
`617 us` of GPU elapsed time. Their exact roles remain unconfirmed. CUDA Graphs
could reduce launch overhead but would not remove the underlying memory traffic,
and no documented NVENC graph-capture path was found. The next measurement is
therefore to reprofile the ordinary linear-input path on driver 610 before
changing layouts.

After that baseline, the next isolated experiment is a side-by-side CUDA 13.1
toolkit plus a native-NV12 CUDA-array probe. CUDA 13.1 is not installed yet.
The comparison must measure the complete camera Mono8-to-native-array
preparation and encode path against the complete current linear preparation and
encode path; a native array can move conversion work without making it vanish.
Keep `/usr/local/cuda` and the Orange production toolchain on CUDA 12.2. A
toolkit-only side-by-side install is expected to require neither a reboot nor a
desktop shutdown, so the user should not need to be physically present. Do not
change a global CUDA symlink or switch the production Orange build for this
probe. Public references: the
[Video Codec SDK 13.1 NVENC programming guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html)
and the
[CUDA 13.1 Linux installation guide](https://docs.nvidia.com/cuda/archive/13.1.0/cuda-installation-guide-linux/index.html).

### 2026-09-19 — Isolated CUDA 13.1 toolkit and native-array input experiment

The driver-only retest did not remove the existing conversion. On idle A16 GPU
1 (UUID `GPU-c37c3690-5fbc-361c-5a77-8c525a47840f`), the unchanged API-11.1/CUDA-12.2
stress binary submitted 600 frames on driver 610.57.04 and produced 1,200
`Convert_PL2BL` launches. Their combined GPU elapsed time was 370.829924 ms,
or 0.618050 ms/frame, essentially unchanged from the old 535 trace's
0.617197 ms/frame. The two launch geometries averaged 0.207515 and 0.410535 ms.
The registered buffers were uniform synthetic content; this is conversion-path
evidence, not decoded-content or real-camera throughput validation. Exact
commands, binary hash, report and SQLite are under
`/tmp/nvenc-driver610-linear-baseline-20260919/gpu1_registered_prod_600f`.

A minimal CUDA 13.1.1 toolkit was staged at
`/home/jeremy/.local/opt/cuda-13.1.1-nvenc` using official NVIDIA redistributable
archives with pinned manifest/component SHA-256 values. The recipe, lock,
runtime smoke and 12 passing focused stager tests are maintained in
`/home/jeremy/pancakebatter` (`stage_cuda_probe_toolkit.py`,
`configs/cuda_probe_toolkit_13_1_1.json`, and `docs/cuda_probe_toolkit_13_1.md`).
Compiler version is 13.1.115, runtime component 13.1.80. An independent kernel
and result check passed with `runtime=13010 driver=13030 values=4096` and
`libcudart.so.13` resolved inside that prefix. No system packages, global CUDA
symlink, shell environment, or production Orange target were changed; no reboot
or desktop shutdown was needed.

The standalone probe in `tools/nvenc_native_probe` uses CUDA 13.1 Driver API
headers and the official Video Codec SDK 13.1.15 interface archive. The archive
provides headers; installed driver 610 supplies the actual `libcuda` and
`libnvidia-encode` implementations. The existing wrapper is compiled through a
private generated build-directory copy with API field migrations and mapped
format/input-pitch propagation. This probe uses the Driver API directly and
does not itself depend on the separately tested new CUDA runtime.

The working native allocation is a `CU_AD_FORMAT_NV12` parent `CUarray` with
`NumChannels=3`, declared width 4608, height 4512, and flags
`VIDEO_ENCODE_DECODE | SURFACE_LDST`. The Y child is U8x1, 4608x4512; the UV
child is U8x2, 2304x2256. The encoder still produces visible 4512x4512 frames.
NVENC registration is CUDAARRAY/NV12 with visible width/height 4512 and pitch
4608; picture inputPitch is 4608. Visible Y is copied from a device Mono8
source; neutral UV and the extra declared Y columns are initialized once per
reusable slot. Declared array width is not a measurement of its opaque physical
row pitch, and this test does not establish a universal 128-byte alignment rule.

Negative cases remain preserved in `/tmp/nvenc-native-array-smoke-20260919`.
Parent `NumChannels=1` fails CUDA allocation. With a 4512-wide parent, CUDA
readback exactly matches Y and neutral UV, but NVENC output is sheared (MAE
83.92295, PSNR 7.78881 dB). A 4608-wide parent removes that error (first-frame
MAE 0.260464, PSNR 50.42284 dB, decoded chroma exactly 128). Generic header
wording says CUDAARRAY pitch is Width*NumChannels; 13536 for the 4512-wide
parent registers/submits but later fails at bitstream lock, status 8. Current
FFmpeg instead uses the first plane's byte width. The passing configuration is
an observed contract on this stack; it does not settle the generic wording for
all formats or drivers.

The first four-mode 600-frame matrix at `/tmp/nvenc-native-nv12-20260919` is
explicitly invalid as a performance comparison. Nsight Systems 2023.2.3 exited
successfully while reporting `TargetProfilingFailed`, unknown CUDA driver API
index 780, and incomplete import. Its zero kernel counts are not evidence.
Additionally, the initial tightly packed linear control used pitch 4512 and
produced field-separated rows: decoded frame 0 closely matches concatenated
even source rows followed by odd source rows (MAE 0.377), rather than normal
raster order (MAE 82.87). Both update modes produced identical bad output, while
both padded native modes passed the same exact-source pixel checks. The
corrected linear control uses `cuMemAllocPitch` and its returned pitch, matching
the production wrapper's allocation method. The API documentation nominally
allows tight pitch divisible by four, so the failed control is recorded as a
layout/driver-path issue rather than a documented prohibition. These original
CSV timings must not be used to claim a native-input improvement.

The corrected matrix at `/tmp/nvenc-native-nv12-20260919-nsys2025` passed.
It used the standalone binary SHA-256
`b046b58edb2e79280b76e16d1f241501d554e1b3bc24d64326751a78a3c2ce08`,
returned linear pitch 4608, native declared width/pitch 4608, explicit frame
mode, HEVC P1/low-latency/VBR 150 Mbps/GOP 25, AQ/temporal-AQ/lookahead off,
output delay 3, four reusable slots, four changing structured Mono8 sources,
and 600 submitted frames per mode. The first 50 submissions were warmup.
Prefilled mode reuses already populated surfaces; per-frame mode updates Y on
every submission, approximating preparation from an already device-resident
Mono8 camera frame. This does not include acquisition, peer-GPU transfer,
recorder IPC, split-GOP coordination, or concurrent inference.

Nsight Systems 2025.5.2.266 was staged separately at
`/home/jeremy/.local/opt/nsight-systems-2025.5.2-nvenc` from the pinned official
CUDA redistribution archive. Reproduction is maintained in pancakebatter's
`stage_nsight_systems.py` and `docs/nsight_systems_2025_5_local.md`.
The profiler retains a compatibility warning: driver API 13.3 is newer than its
supported driver version, so it uses its CUDA 13.1 tracing libraries. Unlike the
old profiler, all four imports complete without errors. Integrity checks find
exactly 4 prefilled or 600 per-frame Y transfers, CUDA API activity through
resource teardown, and the expected 1,200 kernels in each linear control. No
trace error or unknown-API import failure appears. Preserve that warning with
the evidence rather than implying the profiler officially supports driver 13.3.

| Mode | Convert_PL2BL launches | Conversion GPU ms/frame | Per-frame Y-copy event ms, mean | Copy+encode host ms, mean |
| --- | ---: | ---: | ---: | ---: |
| Linear, prefilled | 1200 | 0.620156 | 0 | 11.354673 |
| Native array, prefilled | 0 | 0 | 0 | 11.300253 |
| Linear, Y updated each frame | 1200 | 0.620451 | 0.284855 | 11.525645 |
| Native array, Y updated each frame | 0 | 0 | 0.444014 | 11.287290 |

Native traces contain no CUDA kernels at all. Their per-frame preparation
appears as device-to-array transfer activity; the linear controls retain two
`Convert_PL2BL` kernels per frame plus device-to-device transfer activity.
CUDA-event timing brackets each Y update and is slightly larger than the
profiler's transfer-activity duration. All-kernel totals include the whole trace;
copy and host means use 550 post-warmup CSV rows. `copy_wall_ms` is not isolated
copy cost: the linear path's stop-event synchronization also waits for previous
NVENC work on the shared stream. Output-stage metrics filter submission rows
53 onward to exclude delayed warmup output, covering retired frames 50–596;
three final outputs drain in EndEncode outside per-frame CSV timings.

All four runs submitted and emitted 600 frames. Each output fully decodes as
4512x4512 HEVC. Exact-source Y comparisons at frames 0,1,2,3,300,599 pass, with
MAE 0.038632–0.260464 and PSNR 50.42284–61.19792 dB across those samples; decoded
UV is exactly 128. More strongly, all four complete elementary streams are
byte-identical, SHA-256
`d2fcfc11e3217ad20ad7611862d97e2858f7145ef13ed5acf9d29a18e0d49657`.
`acceptance.json`, `comparison.json`, per-mode `decoded_validation.json`, and
`trace_integrity` record these gates. Scripts `run_comparison.py` and
`validate_outputs.py` in the probe directory preserve the matched run and
validation method; use an explicit new `--root` for reruns. The validator needs
NumPy (available in `/home/jeremy/miniforge3/bin/python`).

This establishes a working native-input path that removes approximately
0.62 ms/frame of visible CUDA tiling kernels while preserving the encoded
result. Preparing that destination is still work: the tested device-to-array
Y copy is about 0.159 ms longer than the ordinary Y copy. The camera-like source
is linear storage and the native destination is a separate array, so changing
an input-format label cannot eliminate that transfer. CUDA places the data in
the array layout during the transfer. Neutral UV is reused rather than rebuilt
per frame.

Do not subtract or add those GPU activity durations to claim an equal
end-to-end latency improvement: transfers, kernels, and NVENC can overlap,
and waits shift between host API calls. The observed host difference is only
about 0.238 ms/frame in the updating modes, from one fixed-order pass, and the
prefilled difference is smaller. Whole-run achieved throughput is essentially
unchanged: 85.9698–86.0531fps across all four modes. These measured API spans
exclude other loop work, file output, slot waiting, and pacing, and must not be
labeled whole-loop latency. This single-A16 synthetic run is not a 100fps
split-GOP acceptance. No thermal/order-controlled throughput gain or
inference-tail reduction is established. The important architectural result is
that the tiling CUDA kernels can be removed from the SM workload.

A suitable next discriminator is concurrent real TensorRT inference plus these
matched linear/native recorder loads on the same A16, followed by a production
integration that writes the recorder's existing preparation or ownership-copy
directly into a native surface while preserving slot lifetime and release rules.
If an existing preprocessing kernel is needed, it may write the Y array directly;
adding a new kernel solely to replace a working transfer is not automatically a
win. Native-array peer transfer, live camera lifetime, split-GOP throughput,
multicamera recording and production quality still need their own validation.
No production target, wrapper source, camera config or running service was
changed by this experiment.

The durable investigation archive is
`/mnt/Data2/nvenc-native-nv12-investigation-20260919`. It includes the accepted
matrix, invalid earlier matrix, allocation/correctness failures, original
linear baseline, tool/header downloads, probe source/binary, maintenance
recipes, journal snapshot and SHA256SUMS. It is separate from the frozen
driver-upgrade and runtime-acceptance archives.

### Template for the next entry

- Date, hypothesis, code/binary commit, and exact command:
- Input allocation/layout and controlled settings:
- Artifact paths and content/decode checks:
- Kernels, per-frame GPU duration, transfers, and achieved fps:
- Interpretation, limitations, and next decision:
