# GUI External IPC Status - 2026-05-28

This is the current implementation snapshot for the GUI external IPC recording
work on `exp/gop-split-a16`.

## Proven Live State

- Four-camera GUI autorun works through the installed display wrapper on the
  real X11 display.
- The current production-like profile is:
  `scripts/run_gui_fourcam_external_ipc_validation.sh --hidden-crop-preview`.
- The latest single-clip hardware artifact is:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_15_38_33`.
- The latest rolling-clip hardware artifact is:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_16_08_46`.
- `Cam2010093` had no lens attached for those runs. Validation now supports an
  explicit opt-in for this case:
  `--allow-main-video-content-failure 2010093`.
- With that opt-in, both strict validations passed with two warnings, both for
  the expected `Cam2010093` low-bitrate/black main-video content.

Healthy metrics from the single-clip run:

- Full external recorders: `1005/1005` frames received, ACKed, and encoded for
  each camera.
- Crop external recorders: `1005/1005` frames received and encoded for each
  camera.
- Camera health: `0` frame-id gaps, `0` GetFrame errors, `0` encode failures,
  `0` IPC failures, and `0` ACK timeouts for every camera.
- GUI hidden-preview FPS: p05 `58.7`, p50 `59.8`, mean `60.0`.
- YOLO steady p95: `3.882`, `3.950`, `3.970`, and `3.972 ms`.
- YOLO queue p95: `0.015-0.022 ms`.
- YOLO CPU affinity applied as `2010093->6`, `2010094->8`,
  `2010095->10`, and `2010096->12`.
- Kernel isolation telemetry recorded and validated for
  `6,8,10,12,38,40,42,44` in `isolcpus`, `nohz_full`, and `rcu_nocbs`.

Healthy metrics from the rolling run:

- Command:
  `scripts/run_gui_fourcam_external_ipc_validation.sh --hidden-crop-preview --record-seconds 6 --warmup-seconds 2 --clip-seconds 2 --allow-main-video-content-failure 2010093`.
- `recording_session.json` mode is `rolling_clips`.
- Three full-frame clips were written per camera:
  `1-200`, `201-400`, and `401-605`.
- Full external recorders: `605/605` frames received, ACKed, and encoded for
  every camera.
- Crop external recorders: `605/605` frames received and encoded for every
  camera; per-camera crop rolling clip counts are `3`.
- Camera health: `0` frame-id gaps, `0` GetFrame errors, `0` encode failures,
  `0` IPC failures, and `0` ACK timeouts for every camera.
- GUI hidden-preview FPS: p05 `51.7`, p50 `59.8`, mean `61.2`.
- YOLO steady p95: `3.773`, `3.951`, `3.966`, and `3.971 ms`.
- YOLO queue p95: `0.014-0.019 ms`.
- Strict validator result:
  `PASS (2 warnings)`, both expected `Cam2010093` no-lens warnings.

## Implemented Code Areas

- GUI autorun/profile launchers:
  - `scripts/run_gui_aq_off_validation.sh`
  - `scripts/run_gui_fourcam_external_ipc_validation.sh`
  - `scripts/orange_gui_validation_wrapper.sh`
- Runtime/session metadata:
  - `src/orange.cpp`
  - `src/project.cpp`
  - `src/session/recording_session.cpp`
  - `src/session/recording_session.h`
- External recorder supervision and status telemetry:
  - `src/external_recorder_supervisor.cpp`
  - `src/external_recorder_supervisor.h`
  - `tools/external_recorder_ipc_probe.cpp`
- YOLO dispatch diagnostics and CPU affinity:
  - `src/yolo_worker.cpp`
- Validators and summaries:
  - `scripts/validate_gui_ptp_recording.py`
  - `scripts/summarize_gui_validation.py`
  - `scripts/compare_gui_crop_preview_validation.py`

## Validation Surface

The GUI validator now checks:

- source Git provenance and sudo-invoking-user Git command mode,
- dirty tracked-worktree telemetry,
- kernel isolated CPU and boot-option CPU sets,
- PTP config, PTP register-read decimation, and per-camera PTP counters,
- full and crop external recorder status sidecars and runtime heartbeat
  snapshots,
- external recorder MP4 writer queue overflow counters, which must report no
  overflow,
- external recorder storage preflight/low-space telemetry when present, which
  must report `ok=true` and `low_space=false`,
- full-frame main videos, packet counts, and decoded content sanity,
- crop MP4, metadata, perf CSV, keyframe sidecar, and crop fanout counters,
- external crop recorder GPU placement and queue depth/high-water telemetry,
- YOLO queue and dispatch timing fields,
- requested/effective YOLO worker CPU affinity,
- GUI FPS buckets and per-phase timing telemetry.

Known intentionally opt-in exception:

- `--allow-main-video-content-failure <serials>` downgrades only main-video
  bitrate and decoded-content sanity failures to warnings for listed cameras.
  Missing videos, invalid dimensions, recorder failures, crop failures, and
  timing gates still fail.

## Commands

Current four-camera no-lens-safe single-clip run:

```bash
cd /home/jeremy/orange-gop-split-a16
DISPLAY=:1 \
  XAUTHORITY=/run/user/1000/gdm/Xauthority \
  XDG_RUNTIME_DIR=/run/user/1000 \
  XDG_SESSION_TYPE=x11 \
  ./scripts/run_gui_fourcam_external_ipc_validation.sh \
    --hidden-crop-preview \
    --record-seconds 10 \
    --warmup-seconds 2 \
    --allow-main-video-content-failure 2010093
```

Strict validation command for the current rolling no-lens artifact:

```bash
scripts/validate_gui_ptp_recording.py \
  /home/jeremy/orange_data/exp/unsorted/2026_05_28_16_08_46 \
  --expect-recording-mode rolling_clips \
  --expect-record-for-seconds 6 \
  --expect-clip-seconds 2 \
  --allow-main-video-content-failure 2010093 \
  --require-crop-recording-artifacts \
  --require-crop-preview-counters \
  --expect-crop-preview-max-fps 15 \
  --expect-crop-preview-disabled 0 \
  --expect-crop-preview-display-enabled 0 \
  --min-crop-frame-pool-size 128 \
  --expect-external-crop-encode-queue-depth 64 \
  --require-external-crop-backend-metadata \
  --require-external-crop-recorder-gpu-separate-from-analytics \
  --expect-external-crop-recorder-gpu 2010093=4 \
  --expect-external-crop-recorder-gpu 2010094=2 \
  --expect-external-crop-recorder-gpu 2010095=8 \
  --expect-external-crop-recorder-gpu 2010096=6 \
  --require-external-recorder-status \
  --require-source-version \
  --expect-source-git-command-user-mode sudo_invoking_user \
  --expect-source-dirty-tracked 0 \
  --expect-yolo-affinity 2010093=6 \
  --expect-yolo-affinity 2010094=8 \
  --expect-yolo-affinity 2010095=10 \
  --expect-yolo-affinity 2010096=12 \
  --require-isolated-cpus 6,8,10,12,38,40,42,44 \
  --require-kernel-cmdline-cpus isolcpus=6,8,10,12,38,40,42,44 \
  --require-kernel-cmdline-cpus nohz_full=6,8,10,12,38,40,42,44 \
  --require-kernel-cmdline-cpus rcu_nocbs=6,8,10,12,38,40,42,44 \
  --expect-gui-stream-downsample 4 \
  --expect-display-preview-max-fps 15 \
  --expect-gui-swap-interval 0 \
  --expect-gui-frame-max-fps 60 \
  --expect-yolo-speed-graphs-enabled 0 \
  --require-gui-timing-telemetry \
  --min-gui-crop-preview-hidden-fps-p05 45
```

## Rolling Implementation Notes

GUI external IPC rolling is now live-proven on the short four-camera hardware
run above. The implemented surface includes:

- app/env recording control plumbing for `record_for_seconds` and
  `clip_seconds`,
- full-frame external recorder rolling-summary mirroring into
  `recording_session.json`,
- `recording_clip_index.json` and `recording_clip_index.csv` pointers in
  `recording_snapshot.json`,
- per-clip full-frame manifest validation,
- GUI external crop rolling metadata/perf sidecar splitting by
  `recording_frame_id` range,
- session-aggregate crop descriptors plus clip-scoped crop descriptors,
- validator coverage for external crop rolling clip artifacts.

The four-camera profile has `--clip-seconds <seconds>` as a convenience switch.
To repeat the short rolling gate:

```bash
DISPLAY=:1 \
  XAUTHORITY=/run/user/1000/gdm/Xauthority \
  XDG_RUNTIME_DIR=/run/user/1000 \
  XDG_SESSION_TYPE=x11 \
  ./scripts/run_gui_fourcam_external_ipc_validation.sh \
    --hidden-crop-preview \
    --record-seconds 6 \
    --warmup-seconds 2 \
    --clip-seconds 2 \
    --allow-main-video-content-failure 2010093
```

Expected validation shape:

- `recording_session.json` mode is `rolling_clips`.
- Full-frame external recorder summaries publish rolling clips.
- Parent recording folder contains `recording_clip_index.json` and
  `recording_clip_index.csv`.
- Per-clip full-frame and crop artifact rows are continuous and sum to the
  session totals.
- No camera gaps, recorder drops, crop drops, or GUI hidden-FPS regression.

## Remaining Risks

- The latest run had no positive detections; crop infrastructure was validated
  with blank crop recording rows, not fish/positive detection crops.
- `Cam2010093` needs a lens before production video-content validation can be
  strict for all four cameras again.
- Long soak coverage is still open. The current validation is a short hardware
  discriminator, not a 24-hour stability proof.
