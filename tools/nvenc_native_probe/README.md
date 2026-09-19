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

The matched linear control uses `cuMemAllocPitch` with the same arguments as
`NvEncoderCuda::AllocateInputBuffers` and registers CUDA's returned pitch. On
this system the returned pitch is 4608 bytes. An earlier packed `cuMemAlloc`
control used the nominally legal 4512-byte pitch and produced packets, but its
decoded raster was invalid: NVENC emitted all even source rows followed by all
odd source rows (normal-order frame-0 MAE about 82.87; MAE about 0.377 after
that exact field reordering). CUDA's API does not document 4512 as illegal, so
the working 4608 linear pitch is also recorded as empirical behavior of this
driver/encode path. Packet output alone is not a sufficient control.

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
  -DORANGE_SOURCE_ROOT=/home/jeremy/orange-nvenc-layout-20260918 \
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
