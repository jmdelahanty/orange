# PTP Synchronization Hardening TODO

Date: 2026-02-24
Scope: cross-camera start/stop synchronization in `orange-jeremy` (GUI + headless paths).

## Goal

Make PTP start/stop synchronization deterministic, timeout-safe, and recoverable across partial failures (camera missing, packet loss, stalled thread), while keeping current behavior for healthy runs.

## Current Gaps (Observed)

1. Start barrier uses spin-wait with no timeout and can deadlock forever.
   - Refs: `src/video_capture.cpp:34`, `src/video_capture.cpp:40`, `src/orange_headless_client.cpp:90`.
2. Stop barrier in headless also spin-waits with no timeout or escalation.
   - Refs: `src/acquire_frames_headless.cpp:140`, `src/acquire_frames_headless.cpp:144`.
3. GUI path does not have a symmetric cross-camera stop barrier.
   - Refs: `src/orange.cpp:948`, `src/orange.cpp:966`.
4. Shared synchronization fields are plain members, read/written by multiple threads.
   - Refs: `src/camera.h:100`, `src/orange_headless_client.cpp:282`, `src/video_capture.cpp:40`.
5. Reset logic is tied to success path, so stale state can leak into next session after a failure.
   - Refs: `src/orange.cpp:980`, `src/orange_headless_client.cpp:175`.
6. Headless still lacks a hardened local-PTP startup path.
   - Headless can now auto-start `scripts/ptp_stack.sh` for `ptp_gate` runs, but
     local PTP start still depends on the legacy gated-start flow and can
     underperform badly after startup.
   - Refs: `scripts/ptp_stack.sh`, `src/orange_headless_client.cpp`,
     `src/video_capture.cpp`.

## Audit Update (2026-03-16)

- Recent March 12 work improved observability, not start/stop barrier semantics:
  - `scripts/ptp_stack.sh` now manages the local linuxptp stack.
  - `scripts/compare_camera_timestamps.py` now summarizes per-camera skew from recording metadata.
  - `src/acquire_frames.cpp` now emits `[PTP_LIVE]` diagnostics once per second during GUI acquisition.
- Shared synchronization state is still the plain `PTPParams` struct in `src/camera.h`, with direct cross-thread reads/writes.
- Unbounded spin-wait barriers are still present in `src/video_capture.cpp` and `src/acquire_frames_headless.cpp`.
- GUI and headless still use different stop/session-reset paths.
- Treat this TODO as still open; recent code landed diagnostics and runbook improvements only.

## Audit Update (2026-04-17)

- Headless experiment specs now accept `fixed.sync_mode = ptp_gate`.
- Headless `ptp_gate` runs now preflight the host linuxptp stack and can
  auto-start it through `scripts/ptp_stack.sh`.
- This closed the setup gap that previously left post-reboot `ptp_gate` runs
  hanging before the cameras crossed the local PTP gate.
- Remaining problem: the local PTP-gated dual-camera `2 x 80 fps` run completes
  but collapses to roughly `54 fps` per camera with large camera drops, so the
  synchronization path is still not performant enough to treat as validated.
- Stable rerun reference:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_80fps_gop25_dual_pix_ptp_rerun7`
- Confirmed non-cause:
  - the two cameras are not accidentally sharing a source GPU in this run
    (`2010095 -> 1`, `2010096 -> 5`).
- New discriminator results:
  - single-camera `80 fps` PTP works:
    `/home/jeremy/orange_data/exp/unsorted/2010096_split_gop_hevc_80fps_gop25_ptp_rerun1`
  - dual-camera `60 fps` PTP works:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_60fps_gop25_dual_pix_ptp_rerun2`
- Current best interpretation:
  - this is likely a rate-sensitive dual-camera synchronized contention problem,
    not a general single-camera `ptp_gate` configuration failure.

## Hardening Plan

## Phase 1: Make Shared State Thread-Safe

- [ ] Replace `PTPParams` ad-hoc shared fields with a synchronization state object:
  - Option A: atomics for flags/counters + a mutex/condition variable for waits.
  - Option B: all fields mutex-guarded with explicit state transitions.
- [ ] Add `session_id` (generation counter) to prevent stale packets/flags from affecting a new run.
- [ ] Add helper APIs:
  - `reset_for_new_session(session_id)`
  - `mark_start_armed(session_id, gate_time)`
  - `mark_stop_armed(session_id, stop_time)`
  - `abort_session(session_id, reason)`
- [ ] Ensure all reader/writer sites use the helper APIs, not direct field access.
- [ ] Expose a read-only synchronization status API for 3D runtime consumers:
  - session id,
  - barrier state/outcome,
  - current health state (`healthy|degraded|failed`).
  - Contract ref: `docs/3d_ptp_coupling_contract.md`.

## Phase 2: Replace Busy-Wait Barriers with Deadline-Based Waits

- [ ] Start barrier:
  - Replace `while (!network_set_start_ptp)` and `while (ptp_counter != num_cameras)` with `wait_for` + deadline.
  - Return structured error on timeout (`PTP_START_TIMEOUT`), do not block forever.
- [ ] Stop barrier:
  - Replace `while (ptp_stop_counter != num_cameras)` with `wait_for` + deadline.
  - Define timeout policy: force local stop + mark session aborted.
- [ ] Add per-barrier jitter-safe deadlines in config (`start_barrier_ms`, `stop_barrier_ms`).

## Phase 3: Unify Start/Stop Semantics Across GUI and Headless

- [ ] Introduce one shared barrier flow used by both acquisition entry points:
  - `acquire_frames` (GUI)
  - `acquire_frames_headless` (headless)
- [ ] Enforce symmetric stop behavior:
  - all participants observe the same stop intent
  - all participants either stop cleanly or fail with the same terminal state
- [ ] Guarantee idempotent STOP handling (duplicate STOP commands are safe).

## Phase 4: Network Reliability and Protocol Hygiene

- [ ] Add command sequence numbers for START/STOP (`command_id`) and ignore out-of-order/duplicate commands.
- [ ] Add ACK/NACK for START/STOP gating commands in ENet protocol.
- [ ] Add retransmit/timeout handling on missing ACKs.
- [ ] Add validation for received `ptp_global_time` and `ptp_stop_time`:
  - reject past times
  - reject impossible deltas
  - log and abort session on invalid values.

## Phase 5: Recovery and Cleanup Guarantees

- [ ] Centralize teardown into one function that always executes on both success and failure paths.
- [ ] On any barrier failure:
  - stop acquisition safely
  - disable PTP sync mode
  - clear all session-scoped state
  - emit explicit terminal status to manager/network layers
- [ ] Ensure camera thread join/stop is bounded by timeout and logs stuck threads.

## Phase 6: Observability

- [x] Add live per-camera PTP diagnostics in GUI acquisition logs (`[PTP_LIVE]` with offset and latch/frame delta snapshots).
- [ ] Add structured logs for each barrier transition:
  - `session_id`, `camera_serial`, `command_id`, `state`, `deadline`, `elapsed_ms`.
- [ ] Add counters/metrics:
  - start barrier timeouts
  - stop barrier timeouts
  - stale command drops
  - max barrier latency.
- [ ] Add one-line final synchronization report per run.
- [ ] Add 3D-facing timing diagnostics:
  - per-camera offset/skew snapshot suitable for join-budget enforcement,
  - explicit indicator when strict-PTP gating should disable 3D joins.

## Phase 7: Host Stack UI Controls

- [x] Add a dedicated `PTP Stack` panel in Orange for host-side linuxptp control.
- [x] Call `scripts/ptp_stack.sh` directly for `start`, `stop`, `restart`, and `status`:
  - do not rely on shell aliases like `ptp-stack`.
- [x] Keep host stack state distinct from camera-side `PTP Stream Sync` state in the UI and code.
- [x] Surface at least these fields in the UI:
  - `ptp4l` running/not running,
  - `phc2sys` running/not running,
  - socket presence for `/var/run/ptp4l`,
  - latest `TIME_STATUS_NP` output.
- [ ] Define privilege behavior explicitly:
  - current implementation: full controls only when Orange has sufficient privileges,
  - remaining gap: non-root read-only status refresh instead of a disabled panel.
- [x] Prevent unsafe host-stack shutdown while streaming with PTP sync active unless explicitly confirmed.

## Phase 8: Tests

- [ ] Unit tests for barrier state machine transitions (including invalid transitions).
- [ ] Unit tests for timeout behavior and reset semantics.
- [ ] Integration tests (can be mock/simulated):
  - one camera never reaches start barrier
  - STOP packet delayed/lost
  - duplicate START/STOP packets
  - one acquisition thread exits early
  - restart immediately after failed session.
- [ ] Add a scripted fault-injection mode to exercise timeout paths in CI/dev.

## Definition of Done

- [ ] No unbounded spin-wait loops remain in PTP start/stop synchronization paths.
- [ ] All barrier waits have explicit deadlines and typed failure outcomes.
- [ ] GUI and headless use the same synchronization semantics.
- [ ] Session state cannot leak across runs (validated by tests).
- [ ] Logs and metrics are sufficient to diagnose failed sync runs without debugger attachment.
