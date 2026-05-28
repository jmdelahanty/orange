# Recording Segment Rollover TODO (30-Minute Chunks, Continuous Capture)

Date: 2026-02-24
Scope: implement automatic recording rollover (for example every 30 minutes) so long runs (for example 24h) produce manageable files without intentional frame drops.

## Is This Possible?

Yes. `orange-jeremy` already has:
- continuous acquisition while recording is toggled on,
- encoder workers with drain/finalize states,
- async packet writing thread (`FFmpegWriter` queue + writer thread).

Refs:
- `src/orange.cpp:1094`
- `src/video_capture.h:76`
- `src/encoder_hw_worker.cpp:322`
- `src/FFmpegWriter.h:35`
- `src/FFmpegWriter.cpp:157`

## Goal

When recording is active, automatically start a new segment every N minutes (default 30) while keeping capture/encode pipelines running.

## Non-Goal Clarification

- “Without dropping frames” here means no intentional drops introduced by rollover logic.
- Hardware faults, disk exhaustion, or OS-level I/O stalls are still possible and must be detected/reported explicitly.

## Current Behavior (Baseline)

- Recording is a global toggle (`record_video`) with drain state (`recording_draining`) and active recorder counting.
- Each recorder opens one output set and finalizes when recording turns off.
- Headless-only seamless rolling clips now exist behind
  `fixed.recording_control.record_for_seconds > 0` and
  `fixed.recording_control.clip_seconds > 0`.
- The headless runner now preopens the next clip writer, switches at a GOP
  first-frame boundary, forces IDR/SPS/PPS on the first frame in the new clip,
  writes `clip_manifest.json`, and keeps recording active across rollover.
- The session/clip artifact contract is documented in
  `docs/recording_session_manifest_contract.md`.
- The manifest builder and validation live in the shared
  `src/session/recording_session.*` module so future GUI, headless, and
  external-recorder implementations do not fork the session contract.

Refs:
- `src/orange.cpp:1127`
- `src/video_capture.h:76`
- `src/encoder_hw_worker.cpp:466`
- `src/crop_and_encode_worker.cpp:202`
- `src/gpu_video_encoder.cpp:383`

## Audit Update (2026-05-09)

- Headless seamless GOP-boundary rolling was validated with one-camera 2010096
  smokes:
  - short artifact:
    `/tmp/orange_seamless_rolling_bt1/2010096_headless_seamless_rolling_clip_smoke_bt1`
    with `record_for_seconds = 18`, `clip_seconds = 6`, three clip folders,
    continuous frames `1-1800`, and total ffprobe duration `18.000 s`.
  - longer artifact:
    `/tmp/orange_seamless_rolling_long_bt2/2010096_headless_seamless_rolling_clip_long_bt2`
    with `record_for_seconds = 36`, `clip_seconds = 6`, six clip folders,
    continuous frames `1-3600`, and total ffprobe duration `36.000 s`.
- Both runs passed with `0` camera frame-ID gaps, `0` GetFrame errors,
  `0` encode failures, and `0` preprocess drops.
- A two-camera PTP real-YOLO rolling smoke also passed after anchoring the
  timed-recording clock to first recorded frame instead of camera-thread launch:
  `/tmp/orange_two_camera_ptp_rolling_bt2/2010095_2010096_headless_ptp_seamless_rolling_bt2`.
  It used `record_for_seconds = 18`, `clip_seconds = 6`,
  `ptp_register_read_decimate = 100`, wrote three clip folders per camera,
  covered frames `1-1801` continuously on both cameras, and produced
  `18.010 s` of total ffprobe video per camera.
- The first PTP attempt before this timing-anchor fix produced continuous
  frames but only `15.000 s` of media for an `18 s` request because the PTP
  gate startup countdown consumed part of the wall-clock recording timer.
- `scripts/verify_timed_recording.py` now checks the seamless rollover
  contract, per-clip manifests, cross-clip `recording_frame_id` continuity, and
  keyframe frame `0` at the start of each clip.
- Rollover boundary decode is protected by forcing the first submitted NVENC
  picture in each new clip to IDR with SPS/PPS, so a clip does not depend on
  headers or reference frames from the previous clip.
- During debugging we also fixed an existing headless thread lifetime bug:
  the camera thread lambda now captures camera/control pointer values instead
  of references to `start_camera_thread` stack parameters.

## Remaining Gap

The headless in-process full-frame encoder path now satisfies the first
"without intentional drops between clips" requirement in one-camera smokes and
a short two-camera PTP real-YOLO smoke. Supervised headless external IPC rolling
also writes verified clip manifests, parent indexes, and packet counts. A short
four-camera live GUI external IPC rolling run on 2026-05-28 passed strict
validation at
`/home/jeremy/orange_data/exp/unsorted/2026_05_28_16_08_46`: full-frame and
crop external recorders each wrote `605/605` frames per camera, three clips per
camera (`1-200`, `201-400`, `401-605`), with no camera gaps, recorder drops,
crop drops, or hidden-FPS regression. Remaining production gaps are broader
failure policy, positive-detection crop coverage, and long soak testing. GUI
external crop IPC now follows the full-frame external
IPC rolling control when that path is active: generated crop contracts and
`recording_backend.crop_recording` carry the same `recording_control`, declare
`external_recorder_gop_boundary_writer_rotation`, use crop GOP size `1`, and
coalesce short terminal tails using the full-frame GOP window so crop clips
align with the parent full-frame manifest. GUI finalization splits Orange-written
crop metadata/perf CSVs into per-clip sidecars by continuous
`recording_frame_id` ranges and publishes them under
`recording_backend.crop_recording.rolling_clips` and per-clip crop
`recording_outputs`. The offline GUI validator checks those per-clip crop
artifacts, and the short live GUI rolling artifact above validates the no-drop
recording path without positive detections.
GUI full-frame external-recorder contract materialization now preserves a
configured `recording_control`/`rollover` object instead of silently overwriting
it with `clip_seconds = 0`. GUI full-frame external-recorder finalization now
mirrors external `rolling_output.clips[]` summaries into the GUI
`recording_session.json`, per-clip `clip_manifest.json`, session
`recording_clip_index.json/csv`, and `recording_snapshot.json` session
pointers. GUI full-frame app/env recording-control plumbing and in-memory
Recording panel controls now exist for external IPC. The GUI also surfaces live
rolling recorder status: current clip, next rollover frame, and last completed
clip outcome. The normal GUI validator now supports full-frame rolling manifest
discovery, per-clip continuity checks, and rolling status sidecar/runtime
consistency checks when external summaries report rolling output. The remaining
GUI full-frame work is positive-detection coverage and longer soak validation.
External recorder MP4 writer queue overflow is now surfaced as recorder failure:
single-shard workers promote `FFmpegWriter` queue overflow to `worker_failed`,
per-shard summaries expose MP4 queue overflow counters, and
`scripts/verify_external_recorder_session.py` fails if any full-frame recorder
summary reports MP4 writer queue overflow.
External recorder storage safety now has a first implementation slice:
`external_recorder_ipc_probe` accepts `--min-free-bytes` and
`--low-space-warning-bytes`, reports `storage_preflight` in status/summary
JSON, fails before listening when the hard minimum is not met, and the external
session/GUI validators reject summaries or status sidecars that report failed
storage preflight or low-space warnings.

## Implementation Plan

## Phase 0: Requirements and Semantics

- [ ] Add config options:
  - `segment_rollover_enabled` (bool)
  - `segment_duration_minutes` (default 30, min 1)
  - `segment_align_to_wall_clock` (bool; optional)
- [ ] Define “segment boundary” policy:
  - by monotonic run-time duration, or
  - wall-clock aligned boundaries (e.g., `:00`, `:30`).
- [ ] Define cross-camera policy:
  - all cameras use the same segment index and boundary schedule.

## Phase 1: Session and Segment State Model

- [ ] Add a recording session state object shared by encoder workers:
  - `recording_session_id`
  - `segment_index`
  - `segment_start_ns`
  - `next_rollover_ns`
  - `rollover_generation` (for one-time transition per boundary)
- [ ] Add deterministic folder layout:
  - `<recording_root>/<recording_id>/segments/seg_000000/...`
- [ ] Add session manifest file with per-segment index entries.

## Phase 2: Non-Blocking Writer Switch

- [x] Implement headless full-frame writer rollover without stopping
  acquisition threads.
- [x] Pre-create/open the next segment writer before the boundary.
- [x] Switch the active writer target at a GOP first-frame boundary.
- [ ] Retire the old writer on a background cleanup path; current headless
  slice finalizes the old writer in the switch path after the prior GOP has
  been emitted.
- [ ] Ensure rollover path does not block encode worker hot path on I/O under
  multi-camera production load.
- [x] Force IDR/SPS/PPS at the first frame of each new clip.

## Phase 3: Recorder Path Coverage

- [x] Implement supervised headless external IPC rolling clips in
  `external_recorder_ipc_probe` with GOP-boundary writer rotation and verifier
  coverage.
- [x] Coalesce tiny terminal tails for external IPC rolling so a timed stop just
  after a clip boundary does not create a standalone 1-frame final clip.
- [ ] Apply rollover implementation consistently to:
  - `EncoderHwWorker` main recording path (headless full-frame path is now
    implemented; GUI external IPC path validated in a short live rolling run),
  - external recorder production/GUI supervision path (headless diagnostic
    external IPC and GUI finalization bridge are now implemented and short
    live GUI rolling validation passed),
  - `CropAndEncodeWorker` crop recording path (first prerequisite helper and
    GOP descriptor alignment exist; GUI external crop IPC finalization/index
    wiring passed a short no-positive-detection rolling run),
  - `GPUVideoEncoder` path (headless / legacy path where used).
- [x] Keep headless full-frame `recording_frame_id` continuity across segments.

Refs:
- `src/encoder_hw_worker.cpp:560`
- `src/crop_and_encode_worker.cpp:306`
- `src/gpu_video_encoder.cpp:641`

## Phase 4: Metadata and Discoverability

- [x] Write per-clip metadata and keyframe sidecars for headless seamless
  rolling clips.
- [x] Write parent `recording_session.json` and per-clip
  `clip_manifest.json` for headless seamless rolling clips.
- [x] Mirror supervised headless external IPC rolling clips into the shared
  analytics `recording_session.json` and verify it against external summaries.
- [x] Write session-level frame/status indexes for seamless rolling:
  - segment file paths
  - first/last `recording_frame_id`
  - start/end timestamps
  - frame counts
  - rollover reason (time/manual/recovery).
- [x] Update pointer/snapshot metadata so consumers can discover multi-segment sessions cleanly.
- [x] Add reliable per-clip packet counts to the session index:
  - native in-process clips use ffprobe `nb_read_packets` after finalization
  - external IPC clips use recorder summary `packets_written`

Refs:
- `docs/recording_metadata.md`
- `src/project.cpp:530`
- `src/project.cpp:611`

## Phase 5: Backpressure and Failure Policy

- [ ] Add explicit overflow/backpressure metrics at writer queue level.
  External IPC full-frame summaries now expose and verify MP4 writer queue
  overflow for aggregate, merged, and per-shard outputs; broader native writer
  and GUI-facing policy remains open.
- [ ] Add disk-space preflight and low-space runtime alarms.
  External IPC recorders now have configurable hard-minimum storage preflight
  plus status/summary low-space telemetry and validator gates. Remaining work
  is choosing production thresholds, surfacing the alarm in the GUI/operator
  status, and adding equivalent native in-process writer policy.
- [ ] Define failure fallback:
  - if new segment open fails, keep writing to current segment and retry,
  - emit critical health state/telemetry.
- [ ] Add bounded retries and escalation path (UI/log/network status).

## Phase 6: UX and Controls

- [x] Add first GUI controls for full-frame external IPC rolling:
  - enable/disable full-frame rolling
  - record duration seconds
  - clip seconds
- [ ] Add broader UI controls:
  - segment duration minutes with production defaults
  - align-to-wall-clock
  - “rollover now” debug button.
- [x] Show runtime status:
  - current segment index
  - next rollover ETA
  - last rollover outcome.

## Phase 7: Validation and Soak Testing

- [ ] Unit tests:
  - boundary time calculations
  - generation/state transitions
  - manifest correctness.
- [ ] Integration tests:
  - rollover at exact boundary under load
  - repeated rollovers (e.g., every 1 minute in stress mode)
  - writer-open failure during rollover.
- [ ] Long-run soak:
  - 24h run with rollovers
  - verify contiguous `recording_frame_id` with no gaps per camera
  - verify no acquisition-thread stalls tied to rollover events.

## Definition of Done

- [ ] Auto rollover produces new segment files at configured interval without requiring stream restart.
- [ ] Acquisition stays live during rollover; no intentional frame drops introduced by rollover logic.
- [ ] Segment index/metadata is complete and machine-readable for downstream processing.
- [ ] 24h soak run passes with stable memory, stable queues, and continuous frame-id coverage.
