# Crop Preview Decoupling Checklist

Status: crop recording and crop-preview decoupling are implemented. Live GUI
validation showed the remaining frame pacing issue also occurs with crop
preview hidden and after main-display downsampling, so the current follow-up is
process isolation for crop video output rather than more crop preview work.

Last updated: 2026-05-27.

Latest live validation note:

- Artifact `/home/jeremy/orange_data/exp/unsorted/2026_05_27_16_17_49`
  validated the crop drain/finalization fix in the default in-process crop
  path. All four crop MP4/keyframe/meta/perf artifacts aligned at `1335` rows,
  with `0` crop drops and YOLO row counts matching crop metadata. GUI FPS still
  stayed low with crop previews hidden: p05 about `13.8` and p50 about `25.3`.
  That makes crop-video encode/write isolation the next diagnostic target.
- Artifact `/home/jeremy/orange_data/exp/unsorted/2026_05_27_15_57_00`
  used crop preview hidden, GUI stream downsample `4`, and display preview max
  FPS `30`. Full-frame external IPC recording and YOLO remained healthy on all
  four cameras, but GUI FPS still failed with hidden-preview p05 about `14.8`
  and p50 about `25.0`. This run also exposed a crop finalization race on
  `2010093`: YOLO/keyframe accounting reached `893` frames, but crop metadata
  and perf wrote only one row and the crop MP4 was invalid. The likely root is
  recording stop/finalization ordering, not crop preview display.
- Artifact `/home/jeremy/orange_data/exp/unsorted/2026_05_27_15_42_10`
  kept crop recording healthy with crop preview hidden: all four cameras wrote
  aligned crop artifacts with `1398` rows and `0` crop drops, and
  `preview_display_enabled_final=0` with `updated/offered=0/0`. GUI FPS still
  failed with `crop_preview_hidden.p05_fps` about `10.0` and p50 about `23.5`.
  That exonerates crop preview as the remaining bottleneck and points at the
  main full-frame display path.
- Artifact `/home/jeremy/orange_data/exp/unsorted/2026_05_27_15_33_06`
  proved the larger crop-frame pool fixed the crop-drop failure mode: all four
  cameras wrote aligned crop MP4/keyframe/meta/perf artifacts with `2136` rows
  and `0` crop drops. Worker-side preview sampling was also healthy at
  `15 fps` per camera. The remaining failure is GUI frame pacing with crop
  preview windows visible: `crop_preview_visible.p05_fps` was about `10.6`.
- Artifact `/home/jeremy/orange_data/exp/unsorted/2026_05_27_15_14_28`
  proved that worker-side preview sampling was active, but the visible crop
  windows still failed the GUI-FPS target (`crop_preview_visible.p05_fps` was
  about `11.2`). Crop recording also was not fully healthy: `2010095` reported
  5 `crop_frame_pool_empty` crop drops, and `2010096` wrote only one crop row.
  Follow-up changes in this slice move crop preview drawing off ImPlot and
  raise the default crop-frame pool size from `8` to `32`.

## Goal

Keep crop recording at YOLO cadence while making the GUI crop preview a sampled,
best-effort display path.

The current crop preview path can update and synchronize once per crop job. On
four-camera `100 fps` YOLO + crop runs, that can produce up to 400 preview
updates per second even though the GUI can only present a smaller number of
frames. The preview path should not reduce crop recording throughput, YOLO
throughput, or GUI responsiveness.

The implementation rule is simple: preview sampling may skip GUI/display work,
but it must never be allowed to skip crop artifact production.

## Current Follow-Up

The latest runs suggest crop preview is no longer the dominant UI-FPS problem.
Crop video encoding is tiny in pixels, but it still exercises NVENC session
setup, driver submission, bitstream/container handoff, and synchronization in
the GUI process. The next diagnostic/fix is therefore an opt-in external crop
recording sink:

- Keep YOLO ROI selection and crop production in the GUI/analytics process.
- Send the crop-owned Mono8 CUDA buffer to an external recorder over CUDA IPC.
- Let the external process own crop NVENC and MP4 writing.
- Keep in-process crop recording as the default until supervised crop recorder
  lifecycle and validation are complete.
- First supervised GUI wiring now exists behind
  `ORANGE_CROP_RECORDING_SINK_MODE=external_ipc`. It launches crop-suffixed
  external recorder processes, writes external crop contracts/plans under the
  recording folder, and indexes external crop MP4s through
  `recording_outputs[serial].crop`. This still needs live validation before it
  becomes a production recommendation.
- First four-camera GUI external-crop run:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_27_16_34_46`.
  The external crop recorders launched and wrote crop MP4/keyframe artifacts,
  but Orange did not index those paths correctly because the recorder summary
  had disabled `merged_output` paths. The run also showed crop encode drops
  under a 32-deep external encode queue. The follow-up patch makes Orange fall
  back to the non-merged output paths, keeps crop failures scoped to the crop
  sidecar descriptor, and raises the experimental external-crop encode queue to
  256. The validator now reports external crop `frames_received`,
  `frames_encoded`, and drop counters directly so queue pressure is visible
  before the MP4/keyframe row checks fail.
- Latest post-fix external-crop validation:
  `/home/jeremy/orange_data/exp/unsorted/2026_05_27_16_55_05`. With the GUI
  FPS threshold omitted, `scripts/validate_gui_ptp_recording.py` passes for all
  four cameras: full-frame external IPC videos validate, YOLO rows are healthy,
  crop metadata/perf/keyframe rows align at `1465`, external crop
  `frames_received`/`frames_encoded` both match crop metadata rows, and
  external crop drops are `0`. A current validator rerun also showed external
  crop queue high-water `23-41` and enqueue-age p95 under `67 ms`, so the
  default external crop encode queue depth was lowered from diagnostic `256`
  to `64`. Override it with `ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH=256` only
  when intentionally using the larger diagnostic shock absorber.
- That same run did not improve GUI display FPS: hidden crop preview still had
  `p05 = 10.7 fps`, `p50 = 25.2 fps`, and `mean = 24.9 fps`. Treat this as
  evidence that external crop encode alone is not sufficient for GUI pacing.
  The follow-up topology slice now separates crop production/fanout from
  preview and recording sinks so preview, crop recording, and future pose
  consumers have independent crop-frame leases and backpressure policies.
- Crop-frame fanout is now explicit. `CropProducer` tracks owned `CropFrame`
  offers/accepts/drops for recording, preview, and pose consumers, and the crop
  sidecar summary records those counters. `CropProducerWorker` releases the
  producer handoff lease after retaining any accepted consumer leases, and
  `CropPreviewWorker` is now an independent best-effort preview consumer rather
  than work hidden inside `CropAndEncodeWorker`.

## Non-Goals

- Do not reduce crop MP4 or `Cam<serial>_crop_meta.csv` cadence.
- Do not change crop ROI selection semantics.
- Do not change pose crop delivery semantics.
- Do not require external IPC recording for this improvement.
- Do not remove `ORANGE_CROP_PREVIEW_DISABLE`; keep it as the strongest
  diagnostic bypass.

## Current Hot Path

- YOLO forwards processed entries to `CropProducerWorker`:
  `src/yolo_worker.cpp`.
- `CropProducerWorker` creates crop recording jobs and, when preview cadence
  says an update is due, offers a separate crop-frame lease to
  `CropPreviewWorker`: `src/crop_producer_worker.cpp`.
- `CropAndEncodeWorker` handles crop video encoding and crop metadata only:
  `src/crop_and_encode_worker.cpp`.
- `CropPreviewWorker` owns detected-frame preview conversion/copy, optional
  cross-GPU staging, and display-stream synchronization:
  `src/crop_preview_worker.cpp`.
- The GUI uploads a crop PBO to a texture only when the preview worker publishes
  a new preview serial: `src/orange.cpp`.

Current expensive preview work:

- detected-frame preview conversion/copy:
  `CropPreviewWorker::WorkerFunction`
- cross-GPU staging and synchronization:
  `CropPreviewWorker::copy_crop_to_display_preview`
- display stream synchronization:
  `CropPreviewWorker::synchronize_display_preview`
- GUI-side texture upload:
  `upload_texture_from_pbo`

## Implementation Surface

Primary code paths:

- `src/camera.h`: persisted crop preview defaults and sanitization.
- `src/camera_config_schema.h`: `crop_pipeline.preview_max_fps` parsing and
  emission.
- `src/crop_preview_cadence.h`: pure cadence/visibility decision logic.
- `src/crop_and_encode_worker.cpp`: crop recording, crop metadata/perf output,
  sidecar emission, and external crop IPC forwarding.
- `src/crop_and_encode_worker.h`: crop recording lifecycle and sidecar summary
  accessors.
- `src/gui_display_frame_rate.h`: pure recording-scoped GUI FPS telemetry
  buckets and JSON summary helpers.
- `src/orange.cpp`: crop preview checkbox, upload-on-preview-serial change,
  crop-window draw gating, ImGui-image crop preview drawing,
  main-display upload-on-preview-serial change, GUI display downsample, and
  recording-scoped GUI FPS telemetry.
- `src/opengldisplay.cpp`: main-display preview serial and fast mono/no-overlay
  downsampled preview path.
- `src/gui/camera_properties_panel.cpp`: operator-visible resolved preview
  cadence.
- `scripts/validate_gui_ptp_recording.py`: post-run artifact checks for crop
  preview counters.

Test and validation surfaces:

- `tools/crop_preview_cadence_tests.cpp`
- `tools/gui_display_frame_rate_tests.cpp`
- `tools/camera_config_validation_tests.cpp`
- `tools/validate_gui_crop_preview_tests.py`
- `tools/compare_gui_crop_preview_validation_tests.py`
- `tools/summarize_gui_validation_tests.py`
- `tools/run_gui_aq_off_validation_tests.py`
- `scripts/validate_gui_ptp_recording.py --require-crop-preview-counters`
- `scripts/compare_gui_crop_preview_validation.py`
- `scripts/summarize_gui_validation.py`
- `scripts/run_gui_aq_off_validation.sh` launcher preflight and run
  instructions
- live four-camera GUI run with crop windows visible and hidden

## Desired Behavior

- Crop video encoding remains full YOLO cadence.
- Crop metadata and crop perf rows remain aligned with encoded crop frames.
- Crop preview updates at a bounded preview cadence, defaulting to a conservative
  value such as `15 fps`.
- Crop preview can be disabled completely with `ORANGE_CROP_PREVIEW_DISABLE=1`.
- The GUI uploads a crop texture only when the corresponding preview buffer has
  changed.
- The GUI can hide crop preview windows without disabling crop recording.
- No-detection preview clearing is rate-limited or transition-based, not
  repeated at full YOLO cadence.
- Preview drops/skips are visible in counters but are not crop recording drops.

## Definition of Done

- Four-camera `100 fps` GUI recording with YOLO and crop recording remains
  healthy with crop preview windows hidden.
- The same workload remains healthy with all crop preview windows visible.
- Crop MP4/keyframe/meta row counts stay aligned with YOLO-cadence crop output.
- Crop sidecar counters prove preview sampling happened and did not become crop
  artifact drops.
- GUI FPS no longer collapses to the previously observed about `20 fps` when
  crop preview windows are visible.
- The validator can distinguish:
  - crop recording health,
  - preview cadence telemetry,
  - final preview display visibility state,
  - GUI display FPS while crop previews were visible or hidden.

## Implementation Checklist

### Slice 1: Add Preview Cadence Configuration

- [x] Add a crop preview cadence resolver.
  - Suggested helper: `ResolveCropPreviewMaxFps(...)`.
  - Accept env override: `ORANGE_CROP_PREVIEW_MAX_FPS`.
  - Default: `15`.
  - `0` or negative: unlimited diagnostic mode.
- [x] Decide where the durable config lives.
  - Preferred durable field: `camera.crop_pipeline.preview_max_fps`.
  - Keep env override highest priority.
  - If the durable field is deferred, document the env-only first slice.
- [x] Add startup logging per crop camera:
  - effective preview max FPS,
  - whether preview is disabled,
  - whether the setting came from env or config.
- [x] Update docs that mention crop preview controls:
  - `docs/output_artifacts_contract.md`
  - `docs/crop_and_encode_reactivation_todo.md`

Acceptance:

- [x] `ORANGE_CROP_PREVIEW_DISABLE=1` still disables preview fully.
- [x] `ORANGE_CROP_PREVIEW_MAX_FPS=15` is visible in logs.
- [x] Unlimited mode can be selected for comparison.

### Slice 2: Gate Crop Preview Production

- [x] Add per-worker preview cadence state.
  - Track last preview update time.
  - Track whether the last published preview represented a detection or blank.
  - Track counters for offered, updated, skipped, and cleared preview frames.
- [x] Cover the preview cadence rules with unit tests.
  - Default bounded cadence.
  - Unlimited cadence.
  - Detection-to-blank transition clearing.
  - Hidden-preview behavior.
  - Re-enable forces the next preview update.
- [x] Gate detected-frame preview work only.
  - Skip `gpu_crop_and_resize_rgba(...)` when preview cadence says no.
  - Skip `copy_crop_to_display_preview()` when preview cadence says no.
  - Skip `synchronize_display_preview()` when no preview update was submitted.
- [x] Do not gate crop encoding.
  - `encoder_->GetNextInputFrame()`
  - crop mono-to-NV12 copy
  - `encoder_->EncodeFrame(...)`
  - crop metadata write
- [x] Rate-limit no-detection preview clears.
  - Clear on cadence tick, or
  - clear once on transition from detection to no-detection.
  - Do not clear the crop preview 100 times/sec per camera.
- [x] Preserve perf row semantics.
  - `crop_preview_cpu_ms = 0` when preview work is skipped.
  - `display_sync_ms = 0` when no preview synchronization occurs.
  - Do not mark preview-cadence skips as crop drops.

Acceptance:

- [ ] Crop recording row count is unchanged relative to a no-preview baseline.
- [ ] `Cam<serial>_crop_perf.csv` still has one row per encoded crop frame.
- [x] Preview skipped frames do not set `dropped=1`.
- [ ] Crop preview remains visibly correct at `15 fps`.

### Slice 3: Publish Preview Update Serial

- [x] Add an atomic preview serial to the live preview worker.
  - Increment only after a preview PBO update or clear has completed.
  - Expose a lightweight accessor, `CropPreviewWorker::PreviewSerial()`.
- [x] Store last uploaded preview serial in GUI state.
  - One value per camera.
  - Reset when cameras are opened/closed or textures are recreated.
- [x] Change the GUI upload loop in `src/orange.cpp`.
  - Current behavior uploads every crop texture every GUI frame.
  - New behavior uploads a crop texture only if `PreviewSerial()` changed.
- [x] Add an independent GUI crop-preview visibility control.
  - Hiding crop previews stops preview PBO update/copy/sync work.
  - Crop+Encode remains enabled and continues writing crop artifacts.
- [x] Keep drawing the existing texture every GUI frame.
  - Only the PBO-to-texture upload should be skipped.
  - The crop window should continue to display the last uploaded crop.
- [x] Avoid ImPlot for crop preview windows.
  - Crop windows now draw the texture with plain `ImGui::Image`.
  - The crop preview does not need plot axes, fit calculations, or ImPlot
    interaction handling.

Acceptance:

- [x] With a static crop preview, GUI does not call `upload_texture_from_pbo`
  every frame for every crop camera.
- [x] Hiding crop preview windows leaves crop recording active.
- [ ] Opening crop windows no longer drops the GUI to about `20 fps` on the
  four-camera workload.
- [ ] Closing/reopening crop windows does not require restarting cameras.

### Slice 4: Add Summary Counters

- [x] Extend crop sidecar summary with preview counters.
  - `preview_max_fps`
  - `preview_disabled`
  - `preview_display_enabled_final`
  - `preview_frames_offered`
  - `preview_frames_updated`
  - `preview_frames_skipped_by_cadence`
  - `preview_clears_updated`
  - `preview_queue_full_drops`
  - `preview_queue_high_water`
  - `preview_serial_final`
- [x] Keep existing crop perf fields:
  - `crop_preview_cpu_ms`
  - `display_sync_ms`
- [x] Document that preview skips are display telemetry, not recording drops.
- [x] Add focused validation-helper tests for crop preview counters.
  - Require counters only for crop-enabled cameras.
  - Fail missing required sidecars.
  - Fail preview FPS mismatches.
  - Fail impossible `updated > offered` counters.
  - Preserve legacy fallback when `crop_outputs` is absent.
- [x] Add an opt-in validator assertion for crop recording artifact alignment.
  - Flag: `--require-crop-recording-artifacts`.
  - For each crop-enabled camera, require crop MP4, metadata, keyframe, and
    perf artifacts.
  - Require crop keyframe `total_frames` to match crop metadata rows.
  - Require crop perf rows to match crop metadata rows.
  - Require crop perf and crop metadata `recording_frame_id` sequences to
    match.
  - Require zero crop-worker dropped rows.
  - Compare crop metadata rows with YOLO perf rows when YOLO rows are present.
- [x] Add an optional validator assertion for final preview visibility.
  - Flag:
    `--expect-crop-preview-display-enabled 0|1`.
  - Compare against `preview_display_enabled_final` for each crop-enabled
    camera.
  - Add tests for pass, mismatch, and legacy/no-crop-output behavior.
- [x] Add an optional validator assertion for disabled preview diagnostics.
  - Flag: `--expect-crop-preview-disabled 0|1`.
  - Use `1` for the `ORANGE_CROP_PREVIEW_DISABLE=1` baseline.
- [x] Add an opt-in validator assertion that bounded visible preview was
  actually sampled.
  - Flag: `--require-crop-preview-sampling`.
  - Require preview to be enabled, display-enabled at finalization, bounded by
    a positive `preview_max_fps`, backed by more than one crop metadata row,
    and to report positive `preview_frames_skipped_by_cadence`.
  - Fail if `preview_frames_updated >= preview_frames_offered`.
- [x] Add recording-scoped GUI FPS telemetry.
  - Store ImGui delta-time FPS stats under
    `recording_snapshot.json session.gui_display_frame_rate`.
  - Split samples into overall recording, crop-preview-visible, and
    crop-preview-hidden buckets.
  - Add validator thresholds for overall, visible, and hidden p05 FPS.
- [x] Add a comparison helper for visible/hidden validation summaries.
  - Script: `scripts/compare_gui_crop_preview_validation.py`.
  - Inputs are JSON files from
    `scripts/validate_gui_ptp_recording.py --json-out`.
  - Summarizes GUI FPS p05, crop rows/drops, preview update/offered counts,
    preview skip percentage, GUI timing buckets, texture upload counts, and
    YOLO detect p95 side by side.
  - It now also shows the dominant GUI timing p95 bucket from validator
    `timing_diagnosis`, or computes the same value from raw timing buckets for
    older validation JSON.
  - The per-run validator JSON now also includes
    `gui_display_frame_rate.timing_diagnosis`, a derived dominant-p95 timing
    bucket summary for quick triage.
- [x] Add crop-frame-pool telemetry to the crop sidecar and validator.
  - `Cam<serial>_crop_sidecar_perf.csv` records `crop_frame_pool_size`.
  - `scripts/validate_gui_ptp_recording.py --min-crop-frame-pool-size 32`
    confirms the effective pool used by a live run.
- [x] Update GUI validation launcher instructions.
  - The launcher now validates all camera JSON files present in the selected
    config folder, instead of hardcoding the two-camera folder contents.
  - Use `ORANGE_GUI_EXPECT_CAMERAS=2010093,2010094,2010095,2010096` for a
    strict four-camera preflight gate.
  - `ORANGE_CROP_PREVIEW_MAX_FPS` and `ORANGE_CROP_PREVIEW_DISABLE` are
    forwarded through the launcher's `sudo env` boundary for live diagnostics.
  - `ORANGE_CROP_FRAME_POOL_SIZE` is also forwarded for crop-drop diagnostics.
  - Launcher output includes the visible/hidden JSON validation and comparison
    commands.
  - Covered by `tools/run_gui_aq_off_validation_tests.py`.
- [x] Increase default crop-frame pool size for four-camera crop recording.
  - `CropProducer` default `ORANGE_CROP_FRAME_POOL_SIZE` fallback is now `32`
    frames, up from `8`.
  - The old default was too tight for the live four-camera crop encode load and
    produced `crop_frame_pool_empty` rows on `2010095`.
- [x] Add main-display configuration checks to GUI validation.
  - `--expect-gui-stream-downsample <N>` checks
    `recording_snapshot.json session.gui_display_frame_rate.stream_downsample`.
  - `--expect-display-preview-max-fps <N>` checks both
    `session.gui_display_frame_rate.display_preview_max_fps` and per-camera
    pipeline final `display_preview_max_fps`.
- [x] Make recording-time YOLO speed graphs opt-in.
  - `ORANGE_GUI_SHOW_SPEED_GRAPHS=0` is the validation launcher default.
  - Set `ORANGE_GUI_SHOW_SPEED_GRAPHS=1` only when live per-camera ImPlot speed
    plots are needed.
  - `session.gui_display_frame_rate.yolo_speed_graphs_enabled` records the final
    state for diagnostics.
- [x] Add GUI phase timing telemetry.
  - `session.gui_display_frame_rate.timings` records frame total, main/crop
    texture upload, camera/crop window draw, speed graph draw, and
    render/present timing buckets.
  - This is needed because the hidden-preview, speed-graph-disabled run still
    showed GUI p05 near `10.65 fps` and p50 near `25 fps`.
- [x] Add derived timing diagnosis to post-run summaries.
  - `scripts/validate_gui_ptp_recording.py --json-out` reports
    `gui_display_frame_rate.timing_diagnosis`.
  - `scripts/summarize_gui_validation.py --json` reports
    `gui_display_diagnosis`.
  - Human output prints the dominant p95 bucket, frame-total p95, and share.

Acceptance:

- [x] `Cam<serial>_crop_sidecar_perf.csv` records enough data to confirm preview
  cadence.
- [x] The GUI validator can prove crop recording row alignment separately from
  crop preview sampling telemetry.
- [x] Validators do not treat preview skips as artifact failures.

### Slice 4B: Reduce Main Full-Frame Display Load

This slice addresses the hidden-preview failure where crop recording stayed
healthy but GUI FPS still collapsed.

- [x] Default GUI stream display downsample to `4`.
  - Override with `ORANGE_GUI_STREAM_DOWNSAMPLE`.
  - Accept legacy `ORANGE_DISPLAY_DOWNSAMPLE`.
  - Keep the UI combo display-only and fixed while streaming.
- [x] Make the four-camera GUI validation launcher set
  `ORANGE_DISPLAY_PREVIEW_MAX_FPS=15` by default.
  - Core `CameraEachSelect.display_preview_max_fps` still defaults to `60`;
    the launcher uses the lower production-like four-camera validation cap.
- [x] Draw main camera previews with `ImGui::Image` instead of ImPlot.
- [x] Upload main camera textures only when `COpenGLDisplay::PreviewSerial()`
  changes.
- [x] Add a mono/no-overlay fast path in `COpenGLDisplay`.
  - Resize mono first when downsample is active.
  - Convert only the downsampled mono preview to RGBA in the PBO.
  - Fall back to the existing RGBA path when color or overlays require it.
- [x] Record `stream_downsample` and `display_preview_max_fps` in
  `session.gui_display_frame_rate`.

Acceptance:

- [x] Build and unit/validator tests pass.
- [ ] New hidden-preview four-camera GUI run reports
  `stream_downsample=4`, `display_preview_max_fps=15`, and GUI hidden p05 FPS
  at or above the validation threshold.
- [ ] Follow-up visible-preview run remains healthy with crop preview windows
  shown.
- [ ] If hidden-preview FPS is still low with speed graphs disabled, rerun with
  `ORANGE_DISPLAY_PREVIEW_MAX_FPS=15` and consider moving display preprocessing
  to the acquisition GPU so only downsampled previews cross to display GPU.

Latest observation before disabling speed graphs: artifact
`/home/jeremy/orange_data/exp/unsorted/2026_05_27_16_43_41` had healthy
full-frame external IPC and external crop recording for all four cameras, zero
external crop drops, and crop rows matching YOLO rows, but
`session.gui_display_frame_rate.crop_preview_hidden.p05_fps = 12.08` with p50
near `25 fps`. This isolates the remaining failure to GUI/display work rather
than crop artifact generation.

Follow-up hidden-preview run with speed graphs disabled still failed:
`crop-preview-hidden p05 = 10.65 fps`, p50 near `25 fps`, all external crop
received/encoded counts matched, and `yolo-speed-graphs-enabled = False`. The
next live run should use the timing telemetry to separate texture upload,
camera-window draw, and render/present costs.

### Slice 4C: Make Crop-Frame Fanout Explicit

This slice starts the worker-topology cleanup without changing crop cadence or
artifact semantics.

Status: the first preview-consumer split is implemented. `CropProducerWorker`
now evaluates preview cadence before requesting an extra preview lease,
`CropPreviewWorker` owns the PBO update/cross-GPU staging work, and
`CropAndEncodeWorker` keeps recording/metadata output only. Recording is now
also an explicit retained consumer lease instead of the special owner of the
producer's returned handoff lease. Because preview no longer depends on the crop
encoder, `CropProducerWorker` only enqueues `CropAndEncodeWorker` jobs for
recording-active frames. Run counters are reset on recording-folder rotation
rather than producer close so the crop encoder can still write sidecar summaries
after drain sentinels. The crop perf `crop_preview_cpu_ms` and
`display_sync_ms` columns are retained as legacy columns and should be `0` for
this path; preview queue and fanout telemetry now live in the sidecar summary.

- [x] Add explicit producer-side crop-frame consumer counters.
  - recording crop-frame offered/accepted/dropped
  - preview crop-frame offered/accepted/dropped
  - pose crop-frame offered/accepted/dropped
  - crop-frame pool misses and pending recycle telemetry
- [x] Make all downstream crop-frame consumers explicit leases.
  - `CropAndEncodeWorker` receives its own retained recording lease.
  - `CropProducerWorker` releases the producer's returned handoff lease after
    accepted consumers are retained or rejected.
  - `PoseWorker` still receives its own retained lease from `CropProducer`.
  - `CropPreviewWorker` receives its own retained lease only on preview-cadence
    updates.
  - rejected consumer offers release their retained lease immediately.
- [x] Persist fanout counters in `Cam<serial>_crop_sidecar_perf.csv`.
- [x] Teach the GUI validator to surface fanout counters when present while
  preserving compatibility with older sidecars.
- [x] Teach the GUI validation comparison report to aggregate fanout counters
  so live runs expose recording/preview/pose offered, accepted, and dropped
  crop-frame leases without digging through raw JSON.
- [x] Teach the GUI validator to prove recording fanout alignment.
  - `producer_recording_crop_frame_accepted` must match crop metadata
    `has_detection=1` rows.
  - Recording crop-frame fanout drops are failures.
  - Preview fanout counters must not exceed preview offer/update counters.
- [x] Split live crop preview into an independent best-effort consumer.
  - The preview consumer should receive a crop-frame lease only when the preview
    cadence says an update is due.
  - Preview queue overflow should increment preview drops, not recording drops.
  - Crop recording must continue to receive every YOLO-cadence crop output row.
- [x] Split crop recording into an explicit consumer path instead of treating the
  returned `ProduceResult.crop_frame` as an implicit recording lease.
- [x] Stop using the crop recording worker as a non-recording preview carrier.
  - Detected frames outside an active recording can still feed preview/pose.
  - `CropAndEncodeWorker` only receives recording-active crop rows.
- [x] Preserve sidecar counters through drain/finalization.
  - Producer close clears recording-folder state but does not reset run
    counters.
  - The next recording-folder rotation resets counters for the new run.
- [ ] Re-run four-camera GUI validation and compare the fanout counters with
  crop metadata rows, external crop recorder counts, and GUI timing telemetry.

### Slice 5: Validation Runs

Run these on the same four-camera setup where the crop windows were observed to
reduce GUI frame rate.

For the local four-camera PTP folder, launch with:

```bash
ORANGE_GUI_CONFIG_DIR=/home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam \
ORANGE_GUI_EXPECT_CAMERAS=2010093,2010094,2010095,2010096 \
ORANGE_GUI_RECORDING_SINK_MODE=external_ipc \
ORANGE_CROP_RECORDING_SINK_MODE=external_ipc \
ORANGE_PTP_REGISTER_READ_DECIMATE=100 \
./scripts/run_gui_aq_off_validation.sh
```

By default, each external crop recorder uses that camera's analytics/source
GPU. For placement diagnostics, set
`ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID=<gpu>` for all crop streams or
`ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_<serial>=<gpu>` for a single camera.
The validator JSON and comparison helper report the resulting
`analytics_gpu_id -> recorder_gpu_id` mapping, and
`--require-matching-crop-config` prevents visible/hidden A/B comparisons from
quietly mixing different crop recorder placements. When these env vars are
set, `scripts/run_gui_aq_off_validation.sh` also prints validation commands
with matching `--expect-external-crop-recorder-gpu*` gates.

- [ ] Baseline with crop preview disabled:

```bash
ORANGE_GUI_CONFIG_DIR=/home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam \
ORANGE_GUI_EXPECT_CAMERAS=2010093,2010094,2010095,2010096 \
ORANGE_CROP_PREVIEW_DISABLE=1 \
ORANGE_GUI_RECORDING_SINK_MODE=external_ipc \
ORANGE_CROP_RECORDING_SINK_MODE=external_ipc \
ORANGE_PTP_REGISTER_READ_DECIMATE=100 \
./scripts/run_gui_aq_off_validation.sh
```

- [ ] Preview cadence at `15 fps`:

```bash
ORANGE_GUI_CONFIG_DIR=/home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam \
ORANGE_GUI_EXPECT_CAMERAS=2010093,2010094,2010095,2010096 \
ORANGE_CROP_PREVIEW_MAX_FPS=15 \
ORANGE_GUI_RECORDING_SINK_MODE=external_ipc \
ORANGE_CROP_RECORDING_SINK_MODE=external_ipc \
ORANGE_PTP_REGISTER_READ_DECIMATE=100 \
./scripts/run_gui_aq_off_validation.sh
```

- [ ] Hidden-preview recording with crop recording still active:

```bash
ORANGE_GUI_CONFIG_DIR=/home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam \
ORANGE_GUI_EXPECT_CAMERAS=2010093,2010094,2010095,2010096 \
ORANGE_CROP_PREVIEW_MAX_FPS=15 \
ORANGE_GUI_RECORDING_SINK_MODE=external_ipc \
ORANGE_CROP_RECORDING_SINK_MODE=external_ipc \
ORANGE_PTP_REGISTER_READ_DECIMATE=100 \
./scripts/run_gui_aq_off_validation.sh
```

In the GUI, leave crop recording enabled and hide crop preview windows before
or during the recording. The crop artifacts should still be produced.

- [ ] Optional comparison at `30 fps`:

```bash
ORANGE_GUI_CONFIG_DIR=/home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam \
ORANGE_GUI_EXPECT_CAMERAS=2010093,2010094,2010095,2010096 \
ORANGE_CROP_PREVIEW_MAX_FPS=30 \
ORANGE_GUI_RECORDING_SINK_MODE=external_ipc \
ORANGE_CROP_RECORDING_SINK_MODE=external_ipc \
ORANGE_PTP_REGISTER_READ_DECIMATE=100 \
./scripts/run_gui_aq_off_validation.sh
```

For each run:

- [ ] Record GUI FPS with crop windows hidden.
- [ ] Record GUI FPS with all crop windows visible.
- [ ] Inspect external crop queue pressure:
  `external_encode_queue_high_water` should stay comfortably below
  `ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH`, and
  `external_enqueue_age_p95_ms` should stay low enough that the queue is acting
  as burst absorption rather than hiding a sustained backlog. Use
  `--max-external-crop-encode-queue-high-water <N>` and
  `--max-external-crop-enqueue-age-p95-ms <ms>` once a target is chosen. The
  GUI validation launcher can print those gates automatically when
  `ORANGE_CROP_EXTERNAL_MAX_QUEUE_HIGH_WATER` and
  `ORANGE_CROP_EXTERNAL_MAX_ENQUEUE_AGE_P95_MS` are set.
- [ ] Validate latest artifact:

```bash
scripts/validate_gui_ptp_recording.py --latest-complete \
  --expect-crop-preview-max-fps 15 \
  --expect-crop-preview-disabled 0 \
  --expect-crop-preview-display-enabled 1 \
  --min-crop-frame-pool-size 32 \
  --expect-external-crop-encode-queue-depth 64 \
  --require-external-crop-backend-metadata \
  --expect-gui-stream-downsample 4 \
  --expect-display-preview-max-fps 15 \
  --expect-yolo-speed-graphs-enabled 0 \
  --require-gui-timing-telemetry \
  --require-crop-recording-artifacts \
  --require-crop-preview-sampling \
  --require-crop-preview-counters \
  --min-gui-crop-preview-visible-fps-p05 45 \
  --json-out /tmp/orange_gui_crop_visible_validation.json
```

- [ ] Validate the hidden-preview run with:

```bash
scripts/validate_gui_ptp_recording.py --latest-complete \
  --expect-crop-preview-max-fps 15 \
  --expect-crop-preview-disabled 0 \
  --expect-crop-preview-display-enabled 0 \
  --min-crop-frame-pool-size 32 \
  --expect-external-crop-encode-queue-depth 64 \
  --require-external-crop-backend-metadata \
  --expect-gui-stream-downsample 4 \
  --expect-display-preview-max-fps 15 \
  --expect-yolo-speed-graphs-enabled 0 \
  --require-gui-timing-telemetry \
  --require-crop-recording-artifacts \
  --require-crop-preview-counters \
  --min-gui-crop-preview-hidden-fps-p05 45 \
  --json-out /tmp/orange_gui_crop_hidden_validation.json
```

- [ ] Validate the preview-disabled baseline with:

```bash
scripts/validate_gui_ptp_recording.py --latest-complete \
  --expect-crop-preview-max-fps 15 \
  --expect-crop-preview-disabled 1 \
  --min-crop-frame-pool-size 32 \
  --expect-external-crop-encode-queue-depth 64 \
  --require-external-crop-backend-metadata \
  --expect-gui-stream-downsample 4 \
  --expect-display-preview-max-fps 15 \
  --expect-yolo-speed-graphs-enabled 0 \
  --require-gui-timing-telemetry \
  --require-crop-recording-artifacts \
  --require-crop-preview-counters \
  --json-out /tmp/orange_gui_crop_disabled_validation.json
```

- [ ] Compare visible and hidden validation summaries:

```bash
scripts/compare_gui_crop_preview_validation.py \
  visible=/tmp/orange_gui_crop_visible_validation.json \
  hidden=/tmp/orange_gui_crop_hidden_validation.json \
  --require-pass \
  --require-zero-crop-drops \
  --require-visible-samples \
  --require-hidden-samples \
  --require-matching-cameras \
  --require-matching-display-config \
  --require-matching-crop-config \
  --min-gui-visible-p05-fps 45 \
  --min-gui-hidden-p05-fps 45
```

Add `--max-external-crop-queue-high-water <N>` and
`--max-external-crop-enqueue-age-p95-ms <ms>` when queue-pressure targets have
been chosen. The crop-config gate keeps visible/hidden comparisons from passing
when they used different crop backends, external crop queue depths, external
crop GPU placement, preview FPS caps, preview-disable settings, or crop frame
pool sizes.

- [ ] Inspect crop perf:
  - crop row count,
  - `dropped`,
  - `drop_reason`,
  - `crop_preview_cpu_ms`,
  - `display_sync_ms`.
- [ ] Inspect crop sidecar preview counters.
- [ ] Confirm full-frame recording, YOLO, and crop recording remain healthy.

Success criteria:

- [ ] GUI FPS no longer collapses to about `20 fps` when crop windows are shown.
- [ ] Crop recording remains YOLO-cadence/full-rate.
- [ ] Crop drops remain zero.
- [ ] YOLO latency does not regress relative to preview-disabled baseline.
- [ ] Operator-visible crop preview is smooth enough for alignment/monitoring.

## Deferred Slice: Double-Buffered Preview PBOs

Only do this if cadence-gating and upload-on-change are not enough.

- [ ] Add two crop preview PBOs per crop camera.
- [ ] Let CUDA write into the non-displayed PBO.
- [ ] Publish the completed PBO index with the preview serial.
- [ ] Let GUI upload from the latest completed PBO.
- [ ] Avoid CPU synchronization between CUDA preview copy and GUI upload where a
  CUDA/GL event or fence can safely express readiness.

Acceptance:

- [ ] Same visual behavior as Slice 3.
- [ ] Lower `display_sync_ms` and fewer GUI stalls under four-camera load.
- [ ] No new shutdown/resource lifetime issues.

## Rollback Plan

- Set `ORANGE_CROP_PREVIEW_DISABLE=1` to bypass crop preview entirely.
- Set `ORANGE_CROP_PREVIEW_MAX_FPS=0` to compare against previous unlimited
  preview behavior.
- Revert GUI upload-on-change separately from crop-worker cadence gating if
  texture update state causes display issues.
