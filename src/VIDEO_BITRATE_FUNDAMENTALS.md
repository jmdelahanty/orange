# Video Bitrate Fundamentals (with Corrections)

This note explains bitrate basics and clarifies a few common misconceptions for high-resolution
machine-vision recording.

## What bitrate means

Bitrate is the amount of data used per unit time to represent video. Common units are Mbps
(megabits per second). Higher bitrate usually means better quality and larger files.

The encoder's job depends on the rate-control mode:
- CBR/VBR: maximize quality at or below a bitrate target.
- CQP: hold quality steady and let bitrate vary.

## Core formula (bpp)

Bits per pixel (bpp) is a convenient way to reason about bitrate at a given resolution and FPS.

```
bpp = bitrate / (width * height * fps)
bitrate = width * height * fps * bpp
```

Example at 4512x4512 @ 60 FPS:
- Pixels/sec = 4512 * 4512 * 60 = 1,221,488,640
- 0.08 bpp -> ~97.7 Mbps
- 0.12 bpp -> ~146.6 Mbps

## Raw throughput is not always 3 bytes per pixel

Raw throughput depends on the pixel format:
- Mono8 or Bayer RAW: 1 byte/pixel
- RGB 8-bit: 3 bytes/pixel
- NV12 (YUV 4:2:0): 1.5 bytes/pixel

Example at 4512x4512 @ 60 FPS:
- Mono8: ~1.22 GB/s
- RGB 8-bit: ~3.66 GB/s
- NV12: ~1.83 GB/s

Compression ratios vary widely by content. Fine detail and motion reduce compression efficiency.

## Rate-control modes (practical view)

- CBR (Constant Bitrate): fixed rate, predictable size, can waste bits on simple scenes.
- VBR (Variable Bitrate): spends more bits on complex frames, better quality per byte.
- CQP (Constant QP): fixed quantization, consistent quality, bitrate varies.
- CRF (Constant Rate Factor): x264/x265 quality mode; not a true NVENC mode.

For NVENC, VBR (or VBR HQ) and CQP are the closest equivalents to CRF-style control.

## What drives required bitrate

1) Resolution (more pixels need more bits)
2) Frame rate (more frames need more bits)
3) Motion/detail (more complexity needs more bits)
4) Codec efficiency (HEVC usually needs fewer bits than H.264 for similar quality)
5) Preset and tuning (quality vs speed tradeoff)

## NVENC specifics (presets and tuning)

- Presets P1..P7: P1 is fastest, P7 is highest quality (but slower).
- Tuning:
  - hq: enables more quality tools (lookahead, AQ) and improves detail.
  - ll/ull: disables latency-heavy tools to boost throughput.
  - lossless: attempts near-zero loss; very large files.

If you are NVENC-bound and dropping frames, lower preset or use ll/ull. If you have headroom,
raise bitrate (bpp) or use a slower preset for more detail.

## Practical guidance for machine vision

- Start with a stable preset that holds 60 FPS (e.g., P3 HQ), then raise bpp to improve detail.
- Mono data generally needs fewer bits than color for similar perceived quality.
- If segmentation quality is the priority, aim for fewer artifacts rather than smaller files.

