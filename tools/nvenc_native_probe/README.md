# Isolated NVENC native-NV12 probe

This standalone target compares the existing pitch-linear CUDA-device-pointer
NV12 input with CUDA 13.1's video-native multiplanar NV12 array. It does not
modify or link any Orange production target.

The pinned NVIDIA archive supplies the API 13.1 interface definitions only.
The probe compiles those headers together with the isolated CUDA 13.1 toolkit
headers, then uses the installed NVIDIA driver's `libcuda` and
`libnvidia-encode` implementations at runtime. It also compiles the checkout's
`NvEncoder` wrapper into this standalone executable. CMake generates a private
build-tree copy of that implementation for the mechanical API 13.1 field
migration and for passing mapped format and input pitch through to picture
submission. The tracked wrapper, Orange CMake files, and production binaries
are unchanged.

The native allocation is one parent `CUarray` created with:

- `Format = CU_AD_FORMAT_NV12`
- `Width = 4608`, `Height = 4512`, `Depth = 0`, `NumChannels = 3`
- `CUDA_ARRAY3D_VIDEO_ENCODE_DECODE | CUDA_ARRAY3D_SURFACE_LDST`

NVENC registers the parent array once as
`NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY`. The probe obtains Y and UV child-array
views with `cuArrayGetPlane`; it writes Y through plane 0 and initializes plane
1 to neutral chroma (`0x80`) once per reusable surface. Child plane handles are
views owned by the parent and are not destroyed separately.

For NV12, the parent descriptor's three channels are the Y, U, and V image
components. `cuArrayGetPlane` exposes them as one-channel Y (`W x H`) and
two-channel interleaved UV (`W/2 x H/2`). The visible encode remains
4512x4512, but the native array allocation width defaults to the next 128-byte
boundary, 4608. NVENC registration pitch and
`NV_ENC_PIC_PARAMS::inputPitch` are both the Y child descriptor's 4608-byte
row width. This is a declared allocation width, not a measurement of the
driver's opaque physical array layout. Only the visible 4512 luma bytes are
updated per frame; the 96 declared padding bytes are initialized once.

This padding is an empirical requirement on the tested A16/610 stack. A 4512-wide parent with
registration/input pitch 4512 encoded and had byte-exact CUDA plane readback,
but the decoded image drifted by 64 pixels per 128 rows (frame-0 luma MAE
83.92295, PSNR 7.78881 dB). The 4608-wide parent removed that shear (MAE
0.260464, PSNR 50.42284 dB, neutral decoded chroma). The generic API-header
wording says CUDAARRAY registration pitch is parent `Width * NumChannels`
(13536 for the tested 4512-wide parent). That value allowed registration and
submission, but failed later at bitstream lock with status 8 even when mapped
format and picture input pitch were explicit. Current FFmpeg instead registers
multiplanar CUARRAY input with the first plane's byte width. The working 4608
result therefore documents observed driver behavior rather than resolving
that generic wording for every format or driver. `--native-storage-width` and
`--native-register-pitch` keep both values directly testable.

The original matched linear control uses `cuMemAllocPitch` with the same
arguments as `NvEncoderCuda::AllocateInputBuffers`; its returned pitch is 4608
bytes on this system. An early packed control produced a field-reordered image,
but that control was malformed. The corrected progressive packed-registration
control at pitch 4512 passes exact-source decoded-image checks and produces the
same encoded bytes as the pitched and native-array controls. Do not interpret
the earlier failure as a restriction against tightly packed linear input.
Packet output alone is not a sufficient correctness check.

## Live-like cached-source comparison

`--update registered-source` adds a linear-only control for the external
recorder shape. Every cached source is a complete device NV12 buffer: source Y
comes from the generated pattern or raw record and UV is initialized to 128.
The buffers are registered once with NVENC and selected directly in stable
`frame_index % source_count` order; there is no per-frame CUDA copy. The cache
must contain at least as many sources as the encoder has in-flight buffers.

The corresponding native case is `--input native-array --update per-frame`.
It selects sources in the same order, copies only visible Y into a retired
reusable native array, and keeps that array's UV plane at 128. Do not use the
native `prefilled` mode as a camera-like comparison: it binds content to
encoder slots rather than presenting a new source for every submitted frame.

Cached NV12 allocations support `--source-layout pitched` (the default,
`cuMemAllocPitch`) and `--source-layout packed` (`cuMemAlloc` with pitch equal
to visible width). The packed option models the current tight-pitch IPC pool
and is a correctness discriminator; the CUDA API does not document that layout
as invalid. Decode and compare each variant before interpreting timings. Raw
records still use `--raw-pitch` and `--raw-frame-bytes` to extract Y; cached
device UV is deliberately neutral for the monochrome comparison.

`--pacing split-gop` submits one GOP at the configured active rate, skips
`--split-gop-idle-frames` frame periods, and repeats. With the defaults below,
each local shard receives 25 frames at 100 fps followed by 25 idle periods,
for an average 50 submitted frames/s. Source order remains contiguous across
the idle interval. `timeline_frame_index` and NVENC input timestamps include
the skipped periods.

Example real-frame pair using 25 cached sources and two warmup bursts:

```bash
COMMON="--gpu-id 1 --fps 100 --frames 250 --warmup-frames 50 \
  --source-frames 25 --source-layout packed --extra-output-delay 3 \
  --pacing split-gop --split-gop-idle-frames 25 \
  --raw-file /home/jeremy/orange_data/model_sources/detect/detect_all_available_detect_training_v004_yolo11n_trt_20260520/calibration_raw_20260918_fish_preenc/Cam2010096_preenc_ref.bin \
  --raw-pitch 4608 --raw-frame-bytes 31186944"

/tmp/build-nvenc-native-probe/nvenc_native_nv12_probe $COMMON \
  --input linear --update registered-source \
  --bitstream-out /tmp/registered-source.hevc --csv /tmp/registered-source.csv

/tmp/build-nvenc-native-probe/nvenc_native_nv12_probe $COMMON \
  --input native-array --update per-frame \
  --bitstream-out /tmp/native-array-updated.hevc --csv /tmp/native-array-updated.csv
```

The CSV retains the existing warmup/measure phase and operation durations, and
adds source layout, actual cached-source pitch, submitted NVENC input pitch,
pacing mode, logical timeline frame, burst and in-burst indices, skipped
periods before a burst, absolute steady-clock timestamps for
schedule/input-ready/copy/encode/frame completion, and schedule lateness.

Use NVIDIA's official standalone interface archive, not the checkout's older
API 11.1 header:

```text
https://developer.nvidia.com/downloads/designworks/video-codec-sdk/secure/13.1/video_codec_interface_13.1.15.zip
SHA256 830180b5a4ca15a4bf99eb94ebd669609a0e5ddca44d8cfc60eff46c3ac4ea23
```

Build into a separate directory:

```bash
unzip -q /tmp/Video_Codec_Interface_13.1.15.zip -d /tmp/nvenc-interface-13.1.15
cmake -S tools/nvenc_native_probe -B /tmp/build-nvenc-native-probe \
  -DORANGE_SOURCE_ROOT=/home/jeremy/orange-nvenc-native-integration-20260919 \
  -DCUDA_13_ROOT=/home/jeremy/.local/opt/cuda-13.1.1-nvenc \
  -DNVENC_13_INTERFACE_DIR=/tmp/nvenc-interface-13.1.15/Video_Codec_Interface_13.1.15/Interface
cmake --build /tmp/build-nvenc-native-probe -j
```

The four matched comparisons use output delay 3 so the wrapper owns four
reusable slots. With four source frames, prefilled and per-frame modes then
encode the same repeating source sequence:

```bash
/tmp/build-nvenc-native-probe/nvenc_native_nv12_probe --gpu-id 5 --input linear --update prefilled --extra-output-delay 3 --bitstream-out /tmp/linear-prefilled.hevc --csv /tmp/linear-prefilled.csv
/tmp/build-nvenc-native-probe/nvenc_native_nv12_probe --gpu-id 5 --input native-array --update prefilled --extra-output-delay 3 --bitstream-out /tmp/native-prefilled.hevc --csv /tmp/native-prefilled.csv
/tmp/build-nvenc-native-probe/nvenc_native_nv12_probe --gpu-id 5 --input linear --update per-frame --extra-output-delay 3 --bitstream-out /tmp/linear-updated.hevc --csv /tmp/linear-updated.csv
/tmp/build-nvenc-native-probe/nvenc_native_nv12_probe --gpu-id 5 --input native-array --update per-frame --extra-output-delay 3 --bitstream-out /tmp/native-updated.hevc --csv /tmp/native-updated.csv
```

For the maintained 600-frame, four-mode comparison on GPU 1, run from this
directory and always provide a new absolute artifact root. The script's old
default root may already contain an earlier invalid comparison:

```bash
python3 run_comparison.py \
  --root /tmp/nvenc-native-nv12-NEW-ABSOLUTE-ROOT \
  --binary /tmp/build-nvenc-native-probe/nvenc_native_nv12_probe \
  --nsys /home/jeremy/.local/opt/nsight-systems-2025.5.2-nvenc/bin/nsys
```

Decode all 600 frames in every output and compare sampled luma planes against
their exact source files with the Python environment that provides NumPy:

```bash
/home/jeremy/miniforge3/bin/python validate_outputs.py \
  --root /tmp/nvenc-native-nv12-NEW-ABSOLUTE-ROOT
```

`per-frame` copies only device-resident Mono8 Y into the destination. UV stays
at 128. Each input slot is retired and unmapped before it is overwritten. The
CSV records CUDA-event GPU copy time, copy wall time including synchronization,
and the wrapper's map, encode-picture, completion, lock, copy, unlock, and
unmap timings separately. Encoder configuration explicitly selects progressive
frame mode (`NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME`).

For camera-like dish content, pass a raw Mono8 sequence. `--raw-frame-bytes`
is the record stride, so it can skip unused trailing planes or metadata:

```bash
... --update per-frame --raw-file /path/to/mono8.raw \
  --raw-pitch 4512 --raw-frame-bytes 20358144 --source-frames 16 \
  --reference-prefix /tmp/dish-reference
```

`--reference-prefix` writes every cached source as tightly packed Mono8 Y. For
the generated sequence this preserves every deterministic pattern variant; for
raw input it preserves the exact cropped Y rows passed to the GPU. Compare a
decoded frame with the corresponding `sourceNNNN.y8` after accounting for the
configured video-range interpretation and lossy HEVC quantization.

Decode the saved HEVC stream and verify its dimensions, frame count, and luma
pattern before interpreting profiler kernel names or timings.

The accepted matched run is
`/tmp/nvenc-native-nv12-20260919-nsys2025`. Each mode encoded 600 frames with
four sources and output delay 3. All four full streams decoded successfully,
six frames spanning each stream matched their exact source Y with neutral
UV=128, and all four bitstreams had the same SHA256
`d2fcfc11e3217ad20ad7611862d97e2858f7145ef13ed5acf9d29a18e0d49657`.

Nsight Systems 2025.5.2 recorded two `Convert_PL2BL` launches per linear frame:
1200 launches totaling about 372.1 ms, or about 0.620 ms per submitted frame,
in each linear mode. Both native modes recorded zero CUDA kernels, while the
per-frame native trace retained all 600 expected array-copy activities. The
native per-frame Y copy measured 0.4440 ms mean by CUDA event versus 0.2849 ms
for the linear device copy. The measured mean of copy wall plus encode wall
was 11.2873 ms native versus 11.5256 ms linear in this one controlled run;
that difference is a standalone observation, not a production throughput or
latency claim. See `acceptance.json`, `comparison.json`, and
`decoded_validation.json` under the artifact root.

The 2025 profiler retained a compatibility warning because the installed
driver exposes CUDA driver API 13.3 while the profiler officially supports
CUDA 13.1. The accepted trace nevertheless contains the expected copy
activities, CUDA API activity through resource teardown, and no import or
profiling errors. An earlier Nsight Systems 2023 attempt reported an unknown
CUDA driver API and `TargetProfilingFailed`; any zero-count result from that
older artifact is invalid.

## Optional external-recorder integration

The camera-free integration tools and build recipe are documented in
[`docs/nvenc_native_integration_20260919.md`](../../docs/nvenc_native_integration_20260919.md).
The separate CMake switches `BUILD_NATIVE_RECORDER`, `BUILD_CONTENTION_TOOLS`,
and `BUILD_LEGACY_RECORDER_CONTROL` keep this work outside Orange's main build.

- `external_recorder_ipc_probe_native` uses isolated CUDA 13.1 and the pinned
  NVENC 13.1 interface. Native arrays are opt-in with `--native-local-input`.
- `external_recorder_ipc_probe_legacy` checks the same recorder source against
  the existing CUDA 12.2/NVENC headers and rejects the native option.
- `trt_contention_probe` runs a required CUDA graph on the production CUDA 12.2
  and TensorRT installation. Input stays zero; it measures contention only.
- `ipc_replay_probe` exports a bounded CUDA 12.2 NV12-shaped pool with real
  cached luma, and verifies the recorder's ACK/RELEASE ownership protocol.
- `run_contention_comparison.py` interleaves three inference conditions in
  three orders, preserves commands/provenance, and uses actual time overlap.
- `run_ipc_replay_validation.py` exercises native off/on, same/split GPU,
  and submit/harvest off/on. `validate_ipc_replay_content.py` fully decodes each
  video and compares selected frames with their exact raw sources.

Use `python3 SCRIPT --help` for required paths. The content validator requires
NumPy; the orchestration runners use only the Python standard library.

## Experimental native surface-write kernel

Configure `-DBUILD_NATIVE_KERNEL=ON` to build `native_nv12_write.ptx` with the
isolated CUDA compiler. Add these flags to the native-array per-frame probe:

```bash
--native-update kernel \
--native-kernel-ptx /tmp/build-nvenc-native-integration/native_nv12_write.ptx
```

The default remains `--native-update copy`. Kernel mode writes visible Y with
vectorized surface stores and preserves neutral UV; it still moves the pixels
and consumes SM resources. The original `copy_*` CSV columns are retained as
compatibility aliases, and `native_update` plus generic `update_*` columns
identify and time the selected operation. The external recorder's native backend also supports this experimental kernel
with `--native-local-input --native-local-kernel-ptx PATH` (or
`ORANGE_EXTERNAL_RECORDER_NATIVE_LOCAL_INPUT=1` and
`ORANGE_EXTERNAL_RECORDER_NATIVE_KERNEL_PTX=PATH`). The copy remains its default.
The kernel and arrays stay inside the external recorder process. Source RELEASE
waits for the selected operation to finish, and summaries distinguish `copy`
from `kernel`. The option applies only to validated local full-frame shards;
peer shards retain linear staging. Do not enable it globally for crop recorders.

The contention runner can add a fourth condition with
`--include-native-kernel --native-kernel-ptx PATH`. For matched encoder/inference
phasing across the three repetitions, use
`--phase-offsets-ms 0,3.333333,6.666667`. It assigns future absolute monotonic
starts, keeps TensorRT warmup immediately before measurement, and records the
derived epochs in each run's artifacts. Without that option the original
naturally phased comparison is retained. The NVENC probe's optional
`--start-at-monotonic-ns T` refers to its first frame, including warmup; the
TensorRT probe's corresponding option refers to its first measured iteration.

The IPC replay matrix accepts `--native-kernel-ptx PATH` to replace its four
native-copy cases with native-kernel cases while retaining all four linear
controls. It records PTX provenance, explicitly clears inherited kernel settings
for linear/default-copy cases, and checks the resolved update and RELEASE
boundary in addition to frame ownership, packets, metadata, and decoded pixels.
