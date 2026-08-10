# 30 FPS, 24-Hour Recording Plan

Date: 2026-08-06

Status: bitrate/preset matrix, recording-profile contract, and duration-aware
storage preflight ready; no lower-bitrate production profile promoted

## Goal

Support a full-resolution `4512x4512`, PTP-synchronized, 30 FPS recording for
24 hours without changing the validated 100 FPS production profile or
re-enabling AQ, temporal AQ, lookahead, or a QP map.

The quality screen uses Cam2010096 and the normal IR operating point. It keeps
HEVC, low-latency tuning, GOP 30, external split-GOP recording, PTP, full-range
Mono8 semantics, and the registered-dish QP map off. The variables are:

- P1 at 150, 75, 60, 45, and 30 Mbps average VBR;
- P3 at 60 and 45 Mbps average VBR; and
- a bounded maximum bitrate/VBV value for every row.

The 45 Mbps row has the same nominal bits-per-frame budget as 150 Mbps at
100 FPS. This is a useful starting point, not a quality proof.

The base spec now declares the encoder policy under
`fixed.recording_profile`. For each generated row, the matrix runner updates
that profile and the external stream together. Orange rejects a generated spec
if the profile, singleton matrix values, or explicit full-frame recorder values
disagree.

## Storage Consequences

Fixed bitrate storage depends on bitrate and duration, not directly on frame
rate. Lowering acquisition from 100 to 30 FPS only reduces storage when the
configured bitrate is also reduced or a content-dependent controller delivers
fewer bits.

Decimal TB for 24 hours of retained full-frame video:

| Average / maximum | 1 camera average | 2 cameras average | 4 cameras average | 2 cameras at maximum | 4 cameras at maximum |
| --- | ---: | ---: | ---: | ---: | ---: |
| 150 / 150 Mbps | 1.620 TB | 3.240 TB | 6.480 TB | 3.240 TB | 6.480 TB |
| 75 / 100 Mbps | 0.810 TB | 1.620 TB | 3.240 TB | 2.160 TB | 4.320 TB |
| 60 / 80 Mbps | 0.648 TB | 1.296 TB | 2.592 TB | 1.728 TB | 3.456 TB |
| 45 / 60 Mbps | 0.486 TB | 0.972 TB | 1.944 TB | 1.296 TB | 2.592 TB |
| 30 / 45 Mbps | 0.324 TB | 0.648 TB | 1.296 TB | 0.972 TB | 1.944 TB |

These numbers cover full-frame masters only. Lossless crops, metadata,
filesystem overhead, incomplete active clips, and a reserved-free-space margin
must be added. Preflight should use the maximum bitrate rather than the average
target.

At the measured 6.1 TB available on 2026-08-06:

- two 150 Mbps masters fit arithmetically, but consume 3.24 TB before crops,
  metadata, and headroom;
- four 150 Mbps masters do not fit safely;
- 45/60 Mbps is comfortable for two cameras, but four cameras are close to the
  current capacity once the conservative lossless-crop bound, 10% margin, and
  500 GB reserve are included; and
- the chosen profile must pass the duration-aware storage preflight for the
  exact selected outputs.

## Matrix Artifacts

- Manifest:
  `experiment_specs/encoding_master_singlecam_30fps_vbr_bitrate_screen_manifest.json`
- Base experiment:
  `experiment_specs/2010096_external_ipc_vbr_quality_base_30fps_a16_gpu7_8.json`
- Isolated camera fixture:
  `config/encoding_quality_screen_2010096_30fps/2010096.json`

Plan only:

```bash
python3 scripts/run_encoding_master_experiment.py \
  --manifest experiment_specs/encoding_master_singlecam_30fps_vbr_bitrate_screen_manifest.json \
  --dry-run --print-commands
```

Validate the base spec without touching cameras:

```bash
targets/release/orange_client --mode local \
  --experiment-spec experiment_specs/2010096_external_ipc_vbr_quality_base_30fps_a16_gpu7_8.json \
  --validate-experiment-spec
```

Execute when Cam2010096 is available:

```bash
scripts/run_encoding_master_experiment.py --execute \
  --manifest experiment_specs/encoding_master_singlecam_30fps_vbr_bitrate_screen_manifest.json \
  --continue-on-failure
```

The existing benchmark wrapper validates and starts PTP when needed, then
stops it if that individual run acquired ownership. Because the matrix invokes
the wrapper once per row, the current implementation may repeat that lifecycle
between rows. Batch-scoped PTP ownership is a follow-up optimization; manual
PTP startup is not required.

The runner writes labeled MP4 paths, achieved Mbps, bytes per frame, queue and
drop telemetry, encoder timing, and projected TB/day into the stamped matrix
folder under `~/orange_data/benchmarks/encoding_quality_30fps`.

## Acceptance Order

1. Reject any row with acquisition gaps, recorder drops, encode failures,
   queue growth, failed frame-identity proof, invalid MP4 content, or failure to
   sustain 30 FPS.
2. Review identical biological events at native pixels, especially the tail,
   silhouette, fast motion, and low-confidence frames.
3. Prefer P1 unless P3 shows a meaningful detail or size improvement and its
   throughput margin remains comfortably above 30 FPS.
4. Select the lowest bitrate with no meaningful task-level regression in fish
   masks, keypoints, detections, and behavior metrics.
5. Create a separate two- or four-camera production profile from the selected
   row. Do not overwrite `100_cam4_ptp`.
6. Use rolling clips as the authoritative long-run representation and set the
   requested duration to exactly `86400` seconds.
7. Before arming, require the session-level duration-aware storage preflight to
   pass for the exact camera count and selected full-frame/crop outputs.

## Duration-Aware Preflight

Orange now performs one session-level calculation before starting any recorder.
It groups all full-frame and lossless-crop streams by destination filesystem so
four independent recorder processes cannot each pass by observing the same free
bytes. Capped streams use `max_bitrate_bps`; lossless and constant-QP streams use
a conservative raw-NV12 upper bound derived from their frame bytes and FPS.

The default policy adds:

- 1,024 metadata bytes per possible encoded frame;
- a 10% safety margin on estimated peak retained data;
- 500 GB of space that must remain free; and
- a second copy only when diagnostic shard preservation is explicitly enabled.

Normal rolling recording has a retained and peak video-copy multiplier of one.
The decision and per-stream arithmetic are written to
`duration_aware_storage_preflight.json` in the recording folder, with additional
per-recorder copies inside the external recorder artifact roots. The aggregate
required byte count is also forwarded to the recorder processes' existing
startup checks. A finite-duration plan fails before acquisition when the
aggregate does not fit. Operator-timed recordings without a declared duration
remain allowed but are explicitly reported as not carrying a hard capacity
guarantee. Headless experiments use their total planned run duration (including
warmup) as the bound when they do not declare a shorter recording-control
interval.

This preflight establishes capacity correctness, not image quality. The next
step is still the real-fish bitrate screen before promoting a lower-bitrate
24-hour profile.

## Duration Enforcement

The configured `86400` seconds is now an enforced recording lifetime rather
than recorder rollover metadata:

- the GUI arms one monotonic deadline when the run actually starts;
- expiry uses the ordinary operator drain/finalize path and records
  `record_for_seconds_elapsed` plus the observed deadline polling overrun;
- every full-frame and crop recorder independently rejects intake beyond the
  target plus `max(2 * fps, gop)` frames; and
- finalization marks the run incomplete for a greater-than-two-second
  monotonic overrun, a recorder ceiling violation, unclean descriptor intake,
  or a final `min_free_bytes` violation.

At 30 FPS and GOP 30, the recorder backstop is 2,592,060 accepted descriptors:
the requested 2,592,000 frames plus a bounded 60-frame drain allowance. This
backstop is not expected to fire during a healthy run; it exists to prevent a
lost GUI stop from becoming another multi-hour recording.
