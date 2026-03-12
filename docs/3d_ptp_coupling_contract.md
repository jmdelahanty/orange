# 3D/PTP Coupling Contract (Loose Modules, Strict Sync)

Date: 2026-02-26
Status: target contract (planned, not fully implemented)

## Purpose

Define strict runtime synchronization behavior for multi-camera 3D while keeping
the code modular.

This contract is about temporal/data coupling only. It is not a requirement to
tightly couple code modules.

## Scope

- `orange-jeremy` runtime join of per-camera 2D detections into 3D outputs.
- Interaction between 3D runtime module and PTP synchronization state.
- GUI and headless parity expectations.

## Module Boundary

- PTP module owns clock/barrier/session state.
- Detection module owns per-camera 2D detections.
- 3D module consumes detection packets and PTP/session metadata through this
  contract.
- 3D module does not directly mutate PTP state.

## Required Input Contract (Per Camera Detection Packet)

Each 2D detection packet provided to 3D join must include:

- `session_id`: run generation id.
- `camera_key`: stable camera identity (serial preferred).
- `frame_id`: camera-local frame id.
- `recording_frame_id` (optional but recommended when recording).
- `timestamp_camera`: camera timestamp in PTP-disciplined domain.
- `timestamp_sys`: system timestamp (diagnostic/fallback only).
- `detections_2d`: normalized 2D outputs for this frame.
- `calibration_ref`: calibration artifact/version identifier used for this
  camera at runtime.

Packets missing `session_id`, `camera_key`, or time fields are invalid for 3D
join.

## Join Semantics

Join key policy (ordered):

1. Use `(session_id, recording_frame_id)` when recording ids are present for all
   participating cameras.
2. Otherwise use `(session_id, timestamp_camera)` with a bounded skew window.

Join requirements:

- Minimum participating cameras: `>= 2`.
- All packets in one 3D solve must share the same `session_id`.
- All joined packets must satisfy configured skew budget.

## Timing and Skew Policy

Required config knobs:

- `max_join_skew_us`: max allowed inter-camera timestamp skew for one 3D join.
- `join_wait_timeout_ms`: max wait time for missing cameras before finalizing
  join attempt.
- `min_cameras_for_3d`: minimum camera count (default `2`).
- `strict_ptp_required`: when true, disable 3D join if PTP health is degraded.

Behavior:

- If deadline expires and fewer than `min_cameras_for_3d` arrived, drop join.
- If enough cameras arrived but skew budget is violated, drop join.
- Never block acquisition indefinitely while waiting for a join.

## PTP Health Gating

PTP module must expose to 3D module:

- current `session_id`,
- PTP health status (`healthy|degraded|failed`),
- barrier outcome for current run,
- optional per-camera offset/skew diagnostics.

3D module behavior:

- If `strict_ptp_required=true` and PTP is not healthy, do not emit 3D outputs.
- Surface explicit status in UI/logs (`3d_blocked_ptp_unhealthy`).

## Output Contract (Per 3D Result)

Each emitted 3D result must include:

- `session_id`,
- join key used (`recording_frame_id` or timestamp bucket),
- `source_cameras` list,
- `source_frame_ids`,
- `max_observed_skew_us`,
- `calibration_refs` used,
- triangulated 3D points and reprojection payloads.

This metadata is required for auditability and downstream debugging.

## Failure and Drop Semantics

3D module must classify failed join attempts with typed reasons:

- `JOIN_TIMEOUT`
- `INSUFFICIENT_CAMERAS`
- `SKEW_EXCEEDED`
- `SESSION_MISMATCH`
- `PTP_UNHEALTHY`
- `CALIBRATION_MISSING`

All drops should increment counters and be visible in run summary logs.

## Observability Requirements

At minimum emit:

- join attempts/successes/failures by reason,
- join wait latency (P50/P95/P99),
- observed skew distribution (P50/P95/max),
- active camera count per join,
- final per-run summary line with success rate and dominant failure reason.

## Evolution Rules

- Additive field changes only unless explicit version bump is coordinated.
- New fields must not break existing 3D consumer parsers.
- Contract version should be logged in 3D runtime startup line.

## Parity Intent vs `orange`

Parity target is behavioral:

- equivalent calibration loading and triangulation/reprojection correctness,
- deterministic cross-camera join behavior with bounded skew/deadlines,
- clear failure semantics under missing camera/packet conditions.

Implementation can remain modular and need not mirror `orange` file structure.
