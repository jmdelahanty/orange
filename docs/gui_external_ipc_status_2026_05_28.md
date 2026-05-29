# GUI External IPC Status - 2026-05-28

This is the current implementation snapshot for the GUI external IPC recording
work on `exp/gop-split-a16`.

## Proven Live State

- Four-camera GUI autorun works through the installed display wrapper on the
  real X11 display.
- Four-camera Orange/Citrus orchestration now works through the local-control
  sockets on the real X11 display.
- The current production-like profile is:
  `scripts/run_gui_fourcam_external_ipc_validation.sh --hidden-crop-preview`.
- The current Orange/Citrus co-run profile is:
  `scripts/run_orange_citrus_fourcam_orchestrator.sh --execute`.
- Latest strict Orange/Citrus single-clip artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_21_48_48`.
- Latest strict Orange/Citrus rolling artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_29_02_44_43`.
- Latest Orange/Citrus forced-timeout diagnostic artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_29_10_43_46`.
- Previous strict Orange/Citrus rolling artifact (orchestrator-owned stop):
  `/home/jeremy/orange_data/exp/unsorted/2026_05_29_02_26_12`.
- Earlier strict Orange/Citrus rolling artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_21_55_25`.
- The latest strict Orange/Citrus artifact passed with `0` failures and
  `0` warnings, strict main-video content validation for all four cameras, and
  persisted Citrus-owned local-control stop ACK state `executed`.
- The latest forced-timeout diagnostic passed the orchestrator's
  `--allow-orange-drain-timeout` consistency gates: Orange reported
  `ack_state=failed_timeout`, `drain_timed_out=true`,
  `forced_finalize_requested=true`, and
  `forced_finalize_stream_stop_requested=true`, then finalized
  `recording_session.json` with `last_event=finalized_after_drain_timeout`.
- Earlier GUI-only single-clip hardware artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_15_38_33`.
- Earlier GUI-only rolling-clip hardware artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_16_08_46`.
- `Cam2010093` had no lens attached for those earlier GUI-only runs.
  Validation supports an explicit opt-in for that diagnostic case:
  `--allow-main-video-content-failure 2010093`.
- With that opt-in, both earlier GUI-only validations passed with two warnings,
  both for the expected `Cam2010093` low-bitrate/black main-video content.

## Current Performance Baseline

For the current four-camera Orange/Citrus profile, the important production
baseline is:

- full-frame external IPC and crop external IPC both work in single-clip and
  rolling modes;
- all four full-frame streams encode valid `4512x4512 @ 100 fps` content at
  about `150-153 Mbps`;
- crop recorders have received/encoded every offered crop frame in the current
  strict runs, with `0` drops;
- steady YOLO p95 is about `3.95-3.96 ms` across all four cameras, and YOLO
  queue p95 is about `0.014-0.017 ms`;
- Orange's Citrus-safe GUI profile intentionally runs near `30 fps`
  (`swap_interval=1`, GUI frame cap `30`, display preview cap `10`) so it does
  not burn display-GPU budget while Citrus renders at `120 Hz`;
- the Orange-only GUI validation profile can still run near `60 fps`
  (`swap_interval=0`, GUI frame cap `60`, display preview cap `15`) when Citrus
  is not sharing the display GPU.

The larger four-camera crop external queue is a load absorber, not a throughput
fix. The current co-run profile uses crop queue depth `128`; the latest strict
rolling run peaked at `64/76/78/64`, with crop external
`enqueue_age_p95_ms` about `27/121/124/95 ms`. That is healthy for the short
validated profile because drops stayed at `0`, but long soaks should keep
gating on queue high-water, `enqueue_age_p95_ms`, recorder drops, and crop
sidecar continuity.

## Accomplished In This Slice

- Orange can now run on the real display through the installed validation
  wrapper without manual GUI interaction.
- Orange exposes a local JSON control socket with status plus opt-in recording
  start/stop and Citrus completion handling.
- The Orange/Citrus wrapper can launch both applications, wait for readiness,
  start Orange recording, start Citrus, stop/drain/finalize Orange, preserve
  logs and validation JSON, and clean up launched process groups/sockets.
- GUI exit-after-finalize now stops streaming before closing the process, which
  avoids leaving camera streams and recorder children behind after automated
  runs.
- Full-frame external IPC is validated in the GUI for both single-clip and
  rolling clip sessions.
- Crop external IPC is validated in the GUI for both single-clip and rolling
  clip sessions, including per-clip sidecars and session-level manifests.
- The four-camera Orange/Citrus profile now uses a larger crop external queue
  (`128`) to absorb short NVENC completion stalls when crop recorders and
  full-frame split-GOP shards share A16 encoder resources.
- App config now covers the stable GUI profile defaults used by validation:
  `gui.stream.downsample`, `gui.display.*`, and
  `gui.telemetry.show_speed_graphs`.
- App config also covers stable crop recording defaults:
  `recording.crop.sink_mode`, `recording.crop.frame_pool_size`, and
  `recording.crop.external_ipc.encode_queue_depth`.
- App config now also covers crop external recorder GPU placement through
  `recording.crop.external_ipc.recorder_gpu_id` and
  `recording.crop.external_ipc.recorder_gpu_ids_by_serial`; env overrides
  still win for validation one-offs.
- Orange's render helper now caches the main GLFW framebuffer size and updates
  it through the framebuffer-size callback, removing Orange's duplicate
  per-frame `glfwGetFramebufferSize(...)` query from `render_a_frame(...)`.
- Orange also compiles the Dear ImGui GLFW backend with a main-window
  size-cache shim, so `ImGui_ImplGlfw_NewFrame()` reads Orange's cached
  window/framebuffer dimensions instead of calling `glfwGetWindowSize(...)` and
  `glfwGetFramebufferSize(...)` every frame. New runs persist
  `session.gui_display_frame_rate.imgui_glfw_size_cache`, and
  `--require-imgui-glfw-size-cache` validates hits with no fallback size
  polling.
- Validation now checks source provenance, dirty tracked-worktree state,
  external recorder status/storage/protocol telemetry, CPU isolation, YOLO
  affinity, GUI display pacing, and the full crop/full-frame artifact surface.
- The four-camera Orange/Citrus wrapper now uses an operation-scoped Orange
  local-control event log by default:
  `/tmp/<operation_id>_orange_local_control.events.jsonl`. That avoids copying
  stale rows from the shared socket log while preserving socket-thread and
  GUI-thread lifecycle evidence for the current operation.

Healthy metrics from the latest Orange/Citrus rolling co-run:

- Operation id: `orange_citrus_fourcam_20260529T064431Z`.
- Command shape:
  `scripts/run_orange_citrus_fourcam_orchestrator.sh --execute --record-seconds 6 --warmup-seconds 2 --clip-seconds 2 --stop-policy citrus_completion_notify`.
- Orange artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_29_02_44_43`.
- Orchestrator summary:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_29_02_44_43/orchestrator/orchestrator_summary.json`.
- Result: `PASS (0 warnings)` with `recording_session.json` mode
  `rolling_clips`, persisted `recording.control.method="citrus_completion"`,
  `command_source="citrus"`, `grace_seconds=10`, and
  `ack_state="executed"`.
- Citrus completed `good_cop_bad_cop_demo.json` with terminal state
  `completed` and reason `protocol_finished`, then sent Orange the completion
  request itself.
- Full-frame external IPC: all four cameras wrote `3179` valid `4512x4512`
  frames at about `150.4-151.7 Mbps`.
- Crop external IPC: all four crop streams received/encoded `3179/3179` frames
  with `0` drops and `16` rolling crop clips per camera. Queue depth was `128`;
  high-water was `64/76/78/64`.
- YOLO steady detect p95 was `3.968/3.959/3.951/3.960 ms`; YOLO queue p95 was
  `0.016/0.017/0.014/0.016 ms`.
- GUI Citrus-safe profile stayed near its cap: hidden-preview p05 `29.8`,
  p50 `30.0`, mean `30.0`, with `swap_interval=1`, GUI frame cap `30`, and
  display preview cap `10`.
- ImGui/GLFW size-cache telemetry reported `941` window-size cache hits,
  `941` framebuffer-size cache hits, and `0` fallback/null-window size calls.

Healthy metrics from the previous orchestrator-owned Orange stop rolling co-run:

- Operation id: `orange_citrus_fourcam_20260529T062559Z`.
- Command shape:
  `scripts/run_orange_citrus_fourcam_orchestrator.sh --execute --record-seconds 6 --warmup-seconds 2 --clip-seconds 2`.
- Orange artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_29_02_26_12`.
- Orchestrator summary:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_29_02_26_12/orchestrator/orchestrator_summary.json`.
- Result: `PASS (0 warnings)` with `recording_session.json` mode
  `rolling_clips`, `record_for_seconds=6`, `clip_seconds=2`, and persisted
  `recording.control.ack_state="executed"`.
- Citrus completed `good_cop_bad_cop_demo.json` with terminal state
  `completed` and reason `protocol_finished`.
- Full-frame external IPC: all four cameras wrote `2199` valid `4512x4512`
  frames at about `150.3-152.4 Mbps`.
- Crop external IPC: all four crop streams received/encoded `2199/2199` frames
  with `0` drops and `11` rolling crop clips per camera. Queue depth was `128`;
  high-water was `66/54/39/65`.
- YOLO steady detect p95 was `3.965/3.931/3.962/3.966 ms`; YOLO queue p95 was
  `0.018/0.017/0.018/0.016 ms`.
- GUI Citrus-safe profile stayed near its cap: hidden-preview p05 `29.8`,
  p50 `30.0`, mean `30.2`, with `swap_interval=1`, GUI frame cap `30`, and
  display preview cap `10`.
- ImGui/GLFW size-cache telemetry reported `647` window-size cache hits,
  `647` framebuffer-size cache hits, and `0` fallback/null-window size calls.

Healthy metrics from the Orange/Citrus co-run:

- Operation id: `orange-citrus-live-010`.
- Orange artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_21_48_48`.
- Orchestrator summary:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_21_48_48/orchestrator/orchestrator_summary.json`.
- Citrus perf JSONL:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_21_48_48/citrus/2026-05-29T01-48-51Z_citrus_perf_125475.jsonl`.
- Result: `PASS (0 warnings)` with strict main-video content validation for
  all four cameras.
- Full-frame external IPC: all four cameras wrote `2202` frames, valid
  `4512x4512` MP4s, and about `150.6-152.8 Mbps` video.
- Crop external IPC: all four crop streams received/encoded `2202/2202`
  frames with `0` drops. Queue depth was `128`; high-water was
  `52/52/51/47`.
- YOLO steady detect p95 was `3.960/3.952/3.951/3.949 ms`; YOLO queue p95 was
  `0.017/0.017/0.016/0.014 ms`.
- GUI Citrus-safe profile held Orange near its cap: hidden-preview p05 `29.3`,
  p50 `30.0`, mean `30.2`, with `swap_interval=1`, GUI frame cap `30`, and
  display preview cap `10`.
- Citrus completed `good_cop_bad_cop_demo.json` with terminal state
  `completed` and reason `protocol_finished`.
- The orchestrator copied Orange/Citrus logs, validation JSON, and the combined
  summary into the recording folder, then cleaned up the launched Citrus
  process group and removed the local-control sockets.

Healthy metrics from the Orange/Citrus rolling co-run:

- Operation id: `orange-citrus-rolling-live-011`.
- Command shape:
  `scripts/run_orange_citrus_fourcam_orchestrator.sh --execute --record-seconds 6 --warmup-seconds 2 --clip-seconds 2`.
- Orange artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_21_55_25`.
- Orchestrator summary:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_21_55_25/orchestrator/orchestrator_summary.json`.
- Citrus perf JSONL:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_28_21_55_25/citrus/2026-05-29T01-55-28Z_citrus_perf_129108.jsonl`.
- Result: `PASS (0 warnings)` with `recording_session.json` mode
  `rolling_clips`, `record_for_seconds=6`, and `clip_seconds=2`.
- Full-frame external IPC: all four cameras wrote `2183` frames, rolling clip
  artifacts, and valid high-bitrate `4512x4512` MP4 content.
- Crop external IPC: all four crop streams received/encoded `2183/2183`
  frames with `0` drops and `11` rolling crop clips per camera. Queue depth was
  `128`; high-water was `22/44/40/42`.
- YOLO steady detect p95 was `3.964/3.959/3.964/3.957 ms`; YOLO queue p95 was
  `0.014/0.016/0.014/0.014 ms`.
- GUI Citrus-safe profile stayed near its cap: hidden-preview p05 `27.5`,
  p50 `29.9`, mean `30.5`.
- Citrus completed `good_cop_bad_cop_demo.json` with terminal state
  `completed` and reason `protocol_finished`; the orchestrator cleaned up the
  launched Citrus process group and local-control sockets.

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

Current Orange/Citrus single-clip run:

```bash
cd /home/jeremy/orange-gop-split-a16
scripts/run_orange_citrus_fourcam_orchestrator.sh --execute
```

Current Orange/Citrus rolling run:

```bash
cd /home/jeremy/orange-gop-split-a16
scripts/run_orange_citrus_fourcam_orchestrator.sh \
  --execute \
  --record-seconds 6 \
  --warmup-seconds 2 \
  --clip-seconds 2 \
  --citrus-run-seconds 6
```

Manual Orange GUI session with Citrus/STOP ALL local-control stop enabled:

```bash
cd /home/jeremy/orange-gop-split-a16
DISPLAY=:1 \
  XAUTHORITY=/run/user/1000/gdm/Xauthority \
  XDG_RUNTIME_DIR=/run/user/1000 \
  XDG_SESSION_TYPE=x11 \
  ./scripts/run_gui_fourcam_external_ipc_validation.sh \
    --hidden-crop-preview \
    --citrus-display-safe \
    --manual-local-control
```

This leaves recording start operator-owned in the GUI, enables Orange
`citrus_completion` / recording-stop local control, and keeps Orange open after
finalization. Use this mode for the manual Citrus STOP ALL validation; a plain
manual GUI launch leaves those stop gates disabled.

After clicking STOP ALL in Citrus and waiting for Orange finalization, validate
the stopped-terminal control metadata with:

```bash
scripts/validate_gui_ptp_recording.py --latest-complete \
  --expect-local-control-stop-method citrus_completion \
  --expect-local-control-stop-command-source citrus \
  --expect-local-control-stop-terminal-state stopped \
  --expect-local-control-stop-reason stopped_by_local_control \
  --expect-local-control-stop-ack-state executed
```

Earlier GUI-only no-lens-safe single-clip run:

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

Strict validation command for the earlier rolling no-lens artifact:

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
  --require-external-recorder-storage-preflight \
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
  --require-imgui-glfw-size-cache \
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
To repeat the short GUI-only rolling gate with strict main-video validation:

```bash
DISPLAY=:1 \
  XAUTHORITY=/run/user/1000/gdm/Xauthority \
  XDG_RUNTIME_DIR=/run/user/1000 \
  XDG_SESSION_TYPE=x11 \
  ./scripts/run_gui_fourcam_external_ipc_validation.sh \
    --hidden-crop-preview \
    --record-seconds 6 \
    --warmup-seconds 2 \
    --clip-seconds 2
```

Expected validation shape:

- `recording_session.json` mode is `rolling_clips`.
- Full-frame external recorder summaries publish rolling clips.
- Parent recording folder contains `recording_clip_index.json` and
  `recording_clip_index.csv`.
- Per-clip full-frame and crop artifact rows are continuous and sum to the
  session totals.
- No camera gaps, recorder drops, crop drops, or GUI hidden-FPS regression.

To test the direct Citrus completion notifier instead of the orchestrator-owned
Orange stop, use:

```bash
scripts/run_orange_citrus_fourcam_orchestrator.sh \
  --execute \
  --record-seconds 6 \
  --warmup-seconds 2 \
  --clip-seconds 2 \
  --stop-policy citrus_completion_notify
```

That profile enables `CITRUS_ORANGE_COMPLETION_NOTIFY=1`, passes
`CITRUS_ORANGE_COMPLETION_GRACE_SECONDS=10`, waits for Orange to finalize
without sending its own Orange stop request, and validates that
`recording.control.method` is `citrus_completion` with
`command_source=citrus`.

## Remaining Risks

- The latest strict Orange/Citrus runs did include real positive YOLO rows:
  single-clip had `2010094=2202/2202` and `2010096=716/2202` detection rows,
  and rolling had `2010094=2183/2183` and `2010096=898/2183` detection rows.
  `2010093` and `2010095` remained zero-detection cameras in those artifacts,
  so this validates positive detection-to-crop fanout on two views, not full
  four-view positive-detection coverage or pose/track quality.
- Older no-lens artifacts remain documented above, but the current strict
  Orange/Citrus artifacts passed main-video content validation for all four
  cameras with `0` warnings.
- Long soak coverage is still open. The current validation is a short hardware
  discriminator, not a 24-hour stability proof.
- The four-camera crop recorder queue depth of `128` is validated for the short
  Orange/Citrus profile. The latest strict rolling run peaked at
  `64/76/78/64` queue high-water and roughly `27/121/124/95 ms`
  `enqueue_age_p95_ms`, so this should stay under explicit high-water,
  enqueue-age, and drop gates before being treated as a long-run production
  value.
