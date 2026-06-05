# Encoding Master Measured Summary

Date: 2026-06-05

This document summarizes measured Orange external-IPC encoder capacity for the
current A16 branch. It is not a complete visual-quality verdict. Human review is
still required for lossy clips, but these runs define the current throughput and
storage boundaries.

## Current Answer

For one `4512x4512 @ 60 fps` camera with exactly two external encoder shards,
the highest measured sustainable HQ preset is currently:

```text
HEVC P5 HQ, VBR 250 Mbps, GOP 5 or GOP 30, two shards
```

`P7 HQ` did not sustain `60 fps` on two shards for either GOP tested. It filled
the queue and dropped encoded frames at both `150 Mbps` and `250 Mbps`.

This is a throughput answer only. The visual-quality question is still whether
`P5 HQ 150 Mbps`, `P5 HQ 250 Mbps`, or another rate-control point is sufficient
for fish detail.

## Measured Artifact Roots

```text
/tmp/orange_encoding_master/encoding_master_singlecam_60fps_stage0_20260605_223612
/tmp/orange_encoding_master/encoding_master_all_stages_smoke_20260605_224416
/tmp/orange_encoding_master/encoding_master_singlecam_60fps_gop1_single_encoder_20260605_230133
/tmp/orange_encoding_master/encoding_master_singlecam_60fps_two_encoder_p5_p7_20260605_234339
/tmp/orange_encoding_master/encoding_master_singlecam_60fps_two_encoder_lossless_20260605_235048
/tmp/orange_encoding_master/encoding_master_singlecam_60fps_two_encoder_lossless_20260605_235307
```

## NVENC Preset/Tuning Notes

NVIDIA's Video Codec SDK documentation describes preset and tuning as separate
controls:

- tuning info selects the use case, including high quality, low latency,
  ultra-low latency, and lossless;
- P1 through P7 presets are available for each tuning info and control the
  performance/quality tradeoff by selecting encoder tools;
- P1 is the highest-performance end, and P7 is the lowest-performance /
  highest-quality end for lossy encoding.

For lossless, "quality" should not be read the same way as lossy HQ tuning. If
the lossless encode is truly lossless for the prepared input format, decoded
content should already be exact; preset differences are mainly throughput,
encoder tool behavior, and possibly compressed size. NVIDIA's preset migration
guide maps older HEVC `LosslessHP` behavior to newer lossless+CQP settings near
P2 for Turing/Ampere, so P1/P2/P5/P7 is a useful future lossless curve if we
want more detail than the current P1/P5/P7 check.

References:

- https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html
- https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-preset-migration-guide/index.html
- https://developer.nvidia.com/blog/introducing-video-codec-sdk-10-presets/

## Two-Encoder P5/P7 Matrix

Manifest:

```text
experiment_specs/encoding_master_singlecam_60fps_two_encoder_p5_p7_manifest.json
```

Run root:

```text
/tmp/orange_encoding_master/encoding_master_singlecam_60fps_two_encoder_p5_p7_20260605_234339
```

All rows used:

- camera `2010096`;
- `4512x4512 @ 60 fps`;
- external IPC recorder;
- two shards on GPUs `5,6`;
- HEVC HQ tuning;
- AQ, temporal AQ, and lookahead off;
- video sanity enabled.

| Setting | Result | Encoded | Drops | Queue | Encode p95 | Mbps | TB/day/cam |
| --- | --- | ---: | ---: | --- | ---: | ---: | ---: |
| P5 HQ VBR150 GOP5 | pass | 420 | 0 | 4/32 | 0.136 ms | 151.5 | 1.64 |
| P5 HQ VBR250 GOP5 | pass | 421 | 0 | 4/32 | 0.124 ms | 255.2 | 2.76 |
| P7 HQ VBR150 GOP5 | fail | 269 | 152 | 31/32 | 68.942 ms | 155.8 | 1.68 |
| P7 HQ VBR250 GOP5 | fail | 268 | 152 | 31/32 | 68.612 ms | 258.7 | 2.79 |
| P5 HQ VBR150 GOP30 | pass | 421 | 0 | 11/32 | 24.869 ms | 152.8 | 1.65 |
| P5 HQ VBR250 GOP30 | pass | 421 | 0 | 11/32 | 24.340 ms | 251.1 | 2.71 |
| P7 HQ VBR150 GOP30 | fail | 253 | 167 | 31/32 | 68.699 ms | 154.6 | 1.67 |
| P7 HQ VBR250 GOP30 | fail | 254 | 166 | 31/32 | 67.961 ms | 250.7 | 2.71 |

Interpretation:

- P5 HQ is sustainable on two shards at both tested bitrates and GOP sizes.
- GOP 5 is much shallower and lower-latency than GOP 30 for P5 HQ.
- P7 HQ saturates two encoders for this frame size/rate, independent of the
  tested bitrate and GOP size.
- The failed P7 rows still acquired clean camera frames and wrote partial MP4s
  that passed video sanity, so the failure mode is encoder throughput, not
  camera acquisition or content validity.

## One-Encoder GOP1 Discriminator

Manifest:

```text
experiment_specs/encoding_master_singlecam_60fps_gop1_single_encoder_manifest.json
```

Run root:

```text
/tmp/orange_encoding_master/encoding_master_singlecam_60fps_gop1_single_encoder_20260605_230133
```

| Setting | Result | Encoded | Drops | Queue | Encode p95 |
| --- | --- | ---: | ---: | --- | ---: |
| P5 HQ VBR150 GOP1, one shard | fail | 327/421 | 94 | 31/32 | 22.910 ms |
| P7 HQ VBR150 GOP1, one shard | fail | 141/421 | 280 | 31/32 | 64.552 ms |

Interpretation:

- GOP 1 HQ at this resolution/rate is not sustainable on a single external
  encoder.
- Prior GOP 1 successes must be interpreted as multi-shard capacity results,
  not single-encoder capacity results.

## Two-Encoder Lossless Matrix

Manifest:

```text
experiment_specs/encoding_master_singlecam_60fps_two_encoder_lossless_manifest.json
```

Run roots:

```text
/tmp/orange_encoding_master/encoding_master_singlecam_60fps_two_encoder_lossless_20260605_235048
/tmp/orange_encoding_master/encoding_master_singlecam_60fps_two_encoder_lossless_20260605_235307
```

All rows used:

- camera `2010096`;
- `4512x4512 @ 60 fps`;
- external IPC recorder;
- two shards on GPUs `5,6`;
- HEVC lossless tuning;
- GOP 1;
- CQP quality value `0`;
- video sanity enabled.

| Setting | Result | Encoded | Drops | Queue | Encode p95 | Mbps | TB/day/cam |
| --- | --- | ---: | ---: | --- | ---: | ---: | ---: |
| P1 lossless GOP1 | pass | 420 | 0 | 4/32 | 1.545 ms | 752.1 | 8.12 |
| P5 lossless GOP1 | pass | 421 | 0 | 4/32 | 1.526 ms | 750.6 | 8.11 |
| P7 lossless GOP1 | pass | 421 | 0 | 3/32 | 2.107 ms | 753.6 | 8.14 |

Interpretation:

- Lossless is sustainable for one `60 fps` camera on two encoder shards for
  P1, P5, and P7.
- File size is essentially unchanged across these presets in this short run:
  about `750-754 Mbps` or `8.1 TB/day/cam`.
- The limiting issue for lossless is storage footprint, not encoder throughput,
  at least for this one-camera two-shard test.
- Because lossless should already preserve the prepared input exactly, do not
  treat P7 lossless as inherently "more visually correct" than P1/P5 lossless
  without a decoded-equality check against a pre-encoder reference.

## Four-Shard Single-Camera Reference

Run root:

```text
/tmp/orange_encoding_master/encoding_master_singlecam_60fps_stage0_20260605_223612
```

| Setting | Result | Encoded | Drops | Queue | Encode p95 | Mbps | TB/day/cam |
| --- | --- | ---: | ---: | --- | ---: | ---: | ---: |
| P1 lossless GOP1, 4 shards | pass | 421 | 0 | 3/32 | 1.683 ms | 754.3 | 8.15 |
| P7 lossless GOP1, 4 shards | pass | 421 | 0 | 3/32 | 1.528 ms | 752.3 | 8.12 |
| P5 HQ VBR150 GOP30, 4 shards | pass | 420 | 0 | 11/32 | 25.014 ms | 155.4 | 1.68 |
| P7 HQ VBR150 GOP30, 4 shards | pass | 420 | 0 | 24/32 | 68.608 ms | 155.5 | 1.68 |

Interpretation:

- P7 HQ can pass with four shards, but it is substantially queue-stressed.
- Lossless passes with four shards, but the storage projection is about
  `8.1 TB/day/cam`, before multiplying by camera count or experiment duration.

## Four-Camera Installed-Rig Reference

Run root:

```text
/tmp/orange_encoding_master/encoding_master_all_stages_smoke_20260605_224416
```

The installed four-camera smoke used the existing `100 fps` PTP external IPC
shape, not the new `60 fps` P5/P7 HQ matrix.

| Setting | Cameras | Result |
| --- | --- | --- |
| P1 LL VBR150 GOP25, 100 fps, 2 shards/camera | 2010093-2010096 | pass, 0 drops |

Interpretation:

- The four-camera external IPC pipeline is healthy with the validated P1 LL
  `100 fps` setup.
- It does not answer the P5/P7 HQ `60 fps` two-shard question by itself.

## Current Decision Boundary

For a practical next four-camera `60 fps` HQ test, promote only P5 HQ settings:

```text
P5 HQ VBR150 GOP5
P5 HQ VBR250 GOP5
P5 HQ VBR150 GOP30
P5 HQ VBR250 GOP30
```

The safest first four-camera candidate is:

```text
P5 HQ VBR250 GOP5, 2 shards/camera
```

It had the higher tested bitrate and the lowest queue pressure in the
two-shard single-camera matrix. If visual review shows `150 Mbps` is sufficient,
`P5 HQ VBR150 GOP5` is the more storage-friendly choice.

Do not promote P7 HQ to four cameras without changing the resource allocation
or reducing the workload, because it already fails on one camera with two
shards.
