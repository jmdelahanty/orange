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
- First headless-only conservative rolling clips now exist behind
  `fixed.recording_control.record_for_seconds > 0` and
  `fixed.recording_control.clip_seconds > 0`.
- This first slice is not seamless: the headless runner stops recording at a
  clip boundary, waits for encoder/writer drain, writes `clip_manifest.json`,
  arms the next clip folder, then resumes recording while acquisition continues.
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

- Headless drain/rearm rolling was validated with a one-camera 2010096 smoke:
  `record_for_seconds = 12`, `clip_seconds = 6`, stream duration `20 s`.
- Latest artifact:
  `/tmp/orange_rolling_spec_validation_bt11/2010096_headless_rolling_clip_smoke_a16_gpu5_bt11`.
- The run passed with `0` camera frame-ID gaps, `0` GetFrame errors,
  `0` encode failures, and `0` preprocess drops.
- `scripts/verify_timed_recording.py` passed: two clip manifests, continuous
  `recording_frame_id` across clips, and total ffprobe duration `12.000 s`
  for a requested `12.000 s`.
- The latest validation also fixed the rollover boundary decode issue: each new
  clip now forces the first submitted NVENC picture to IDR with SPS/PPS, so the
  second clip does not depend on headers or reference frames from the previous
  clip.
- During debugging we also fixed an existing headless thread lifetime bug:
  the camera thread lambda now captures camera/control pointer values instead
  of references to `start_camera_thread` stack parameters.

## Remaining Gap

The conservative headless implementation is useful for short bounded
experiments and contract validation, but it does not satisfy the original
"without ever dropping frames between clips" requirement. That still needs
seamless GOP-boundary writer switching or process/external-recorder rollover.

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

- [ ] Implement writer rollover without stopping acquisition threads.
- [ ] On boundary:
  - pre-create/open next segment writer,
  - atomically switch active writer target for new packets,
  - retire old writer asynchronously (finalize on background cleanup path).
- [ ] Ensure rollover path does not block encode worker hot path on I/O.
- [ ] If codec path supports it, request a keyframe near boundary to improve segment independence.

## Phase 3: Recorder Path Coverage

- [ ] Apply rollover implementation consistently to:
  - `EncoderHwWorker` main recording path,
  - `CropAndEncodeWorker` crop recording path,
  - `GPUVideoEncoder` path (headless / legacy path where used).
- [ ] Keep per-path frame-id continuity (`recording_frame_id`) across segments.

Refs:
- `src/encoder_hw_worker.cpp:560`
- `src/crop_and_encode_worker.cpp:306`
- `src/gpu_video_encoder.cpp:641`

## Phase 4: Metadata and Discoverability

- [x] Write per-clip metadata and keyframe sidecars for headless drain/rearm
  rolling clips.
- [x] Write parent `recording_session.json` and per-clip
  `clip_manifest.json` for headless drain/rearm rolling clips.
- [ ] Write session-level frame/status indexes for seamless rolling:
  - segment file paths
  - first/last `recording_frame_id`
  - start/end timestamps
  - packet/frame counts
  - rollover reason (time/manual/recovery).
- [ ] Update pointer/snapshot metadata so consumers can discover multi-segment sessions cleanly.

Refs:
- `docs/recording_metadata.md`
- `src/project.cpp:530`
- `src/project.cpp:611`

## Phase 5: Backpressure and Failure Policy

- [ ] Add explicit overflow/backpressure metrics at writer queue level.
- [ ] Add disk-space preflight and low-space runtime alarms.
- [ ] Define failure fallback:
  - if new segment open fails, keep writing to current segment and retry,
  - emit critical health state/telemetry.
- [ ] Add bounded retries and escalation path (UI/log/network status).

## Phase 6: UX and Controls

- [ ] Add UI controls:
  - enable/disable auto rollover
  - segment duration minutes
  - align-to-wall-clock
  - “rollover now” debug button.
- [ ] Show runtime status:
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
