# Color Recording Pipeline

Date: 2026-03-31
Scope: document the current full-frame color recording path, the current
performance bottleneck hypothesis, the mitigation already landed, and the next
optimization options.

## Current Runtime Path

Active GUI recording uses the `EncoderPreprocessWorker -> EncoderHwWorker ->
FFmpegWriter` path.

For color cameras, the current preprocess pipeline is:

1. Debayer Bayer RAW to RGBA with NPP (`nppiCFAToRGBA_8u_C1AC4R`).
2. Optionally resize RGBA with NPP when recording downsample is enabled.
3. Convert RGBA directly to NV12 using the local fused CUDA kernel.
4. Hand the prepared NV12 buffer to `EncoderHwWorker`.
5. Copy the prepared NV12 frame into the NVENC input frame.
6. Call `EncodeFrame()`.
7. Push encoded packets to the asynchronous FFmpeg writer thread.

For monochrome cameras, the path is much cheaper:

1. Copy luma.
2. Fill UV with a constant neutral plane.
3. Hand the result to NVENC.

## Current Conclusion

The current evidence points to the color preprocess path being the main
bottleneck, not a missing-buffer-return bug and not NVENC saturation.

Reasons:

- The prepared-frame buffers and CUDA events are returned from the hardware
  encoder worker back to the preprocess pool after encode.
- The camera-side log showed high SM utilization while the hardware encoder did
  not appear saturated.
- Recording became stable when output size was reduced by `2x`, which is
  consistent with a pixel-throughput bottleneck.
- Preview being enabled makes the situation worse because the display path also
  debayers and resizes color data.

The practical reading is:

- full-resolution color recording can be preprocess-bound,
- downsample reduces enough work to restore steady throughput,
- mono should retain significantly more headroom than color.

## Important Caveat

The `enc_buf` / `enc_evt` debug counters are useful for trend reading but should
not be treated as exact pool occupancy. The retry path in the preprocess worker
can temporarily overstate those counters when partially acquired resources are
returned during retry.

That means the current logs are good enough to detect starvation and backpressure,
but not good enough to prove exact resource counts.

## Optimization Already Landed

The previous color path used two full-frame conversion passes after debayer:

- `RGBA -> RGB`
- `RGB -> NV12`

That has now been reduced to a single pass:

- `RGBA -> NV12`

This removes:

- one full intermediate RGB buffer,
- one full-frame kernel launch,
- one extra read/write pass over the color image.

This is the lowest-risk improvement because it preserves the encoder contract:
NVENC still receives NV12 exactly as before.

## What We Have Now

- Versioned camera config schema with GPIO/sync configuration.
- UI controls for encoder codec, preset, tuning, rate control, quality value,
  GOP length, and recording resize.
- Estimated bitrate readout in the recording UI.
- Recording resize support in the active GUI full-frame path.
- Fused `RGBA -> NV12` conversion in the active color recording preprocess path.

## Recommended Next Steps

Priority order:

1. Test the current fused-kernel build on the same external-trigger case.
   Success criteria:
   - fewer preprocess drops,
   - lower SM pressure,
   - stable full-resolution color recording or at least better margin.

2. If color is still preprocess-bound, switch the active NVENC input path to a
   4-channel RGB format (`ARGB` or `ABGR`) and let NVENC perform the final color
   conversion internally.
   Why this is attractive:
   - the local NVENC wrapper already supports 4-channel input formats,
   - it could remove the local NV12 conversion entirely,
   - it is much lower risk than writing a custom Bayer-to-NV12 path.
   Main risk:
   - channel-order validation is required so colors do not come out swapped.

3. Add proper per-stage telemetry.
   Specifically:
   - preprocess GPU time,
   - encoder GPU time,
   - encoder queue depth,
   - real pool occupancy instead of approximate counters,
   - packet-writer backlog.
   This should happen before any larger rewrite so future tuning is evidence-based.

4. Revisit preview interaction.
   If preview remains enabled during recording, it competes for GPU work on the
   same source frames. If needed, add a clearer operator note or reduce preview
   work while recording.

5. Only consider a direct Bayer-to-NV12 path after the steps above.
   This is the most aggressive optimization, but also the highest validation cost.
   It would require custom color conversion logic and more careful image-quality
   verification.

## Non-Recommendations Right Now

- Do not start by increasing pool sizes. Larger pools may hide the symptom but
  do not fix a preprocess throughput limit.
- Do not assume there is a confirmed buffer leak. Current evidence does not
  support that.
- Do not jump straight to a custom Bayer-to-NV12 path before testing the fused
  kernel and the simpler ARGB/ABGR-to-NVENC option.

