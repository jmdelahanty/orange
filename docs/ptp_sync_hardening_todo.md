# PTP Synchronization Hardening TODO

Date: 2026-02-24
Scope: cross-camera start/stop synchronization in `orange-jeremy` (GUI + headless paths).

Related note:

- `docs/multi_camera_failure_modes.md`
- `docs/ptp_recording_sink_experiment_plan.md`

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
  - dual-camera `100 fps` PTP stream-only also works:
    no recording artifacts are emitted in this mode, but the direct local
    benchmark sustained about `100 fps` on both cameras with `0` camera drops
- Current best interpretation:
  - this is likely a rate-sensitive dual-camera synchronized contention problem,
    not a general single-camera `ptp_gate` configuration failure.
  - more specifically, the evidence now points away from an average-bandwidth
    limit and toward a burst-capacity or queueing problem that appears when
    tightly phase-aligned camera arrivals interact with the recording path.
  - next diagnostic:
    introduce a small deliberate stagger between the two PTP-gated cameras; if
    throughput returns at `2 x 80 fps`, that strongly supports the
    synchronized-burst-contention explanation.
  - that diagnostic has now succeeded with a `2 ms` stagger:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_80fps_gop25_dual_pix_ptp_stagger2ms_rerun1`
    restored both cameras to about `80 fps` with `0` camera drops in
    `runs.csv` and `overflow_events = 0` in `recording_snapshot.json`.
  - the branch now has an experimental headless stagger hook:
    `fixed.ptp_gate_stagger_ns` in experiment specs and
    `--ptp-gate-stagger-ns` in local headless CLI.
  - follow-on `100 fps` characterization shows that nonzero stagger is not a
    complete solution at the higher rate:
    - `25 us`:
      `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger25us_rerun1`
    - `50 us`:
      `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger50us_rerun1`
    - `100 us`:
      `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger100us_rerun1`
    - `250 us`:
      `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger0p25ms_rerun1`
    - `2 ms`:
      `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_rerun2`
    - swapped `2 ms` order:
      `/home/jeremy/orange_data/exp/unsorted/2010096_2010095_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_swaporder_rerun1`
  - current read after that sweep:
    - `80 fps` plus stagger is a valid mitigation
    - `100 fps` plus nonzero stagger remains unstable
    - for the larger offsets, the failure follows the camera receiving the
      offset
    - the `100 fps` failures show stale-frame behavior after gate open rather
      than the older split-GOP backlog-overflow signature
  - one more discriminator is now available:
    - experimental `Continuous` gate acquisition mode with `2 ms` stagger:
      `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_continuous_rerun1`
    - result:
      - `2010095` stays near `100 fps`
      - offset `2010096` still falls to about `7 fps`
      - `overflow_events = 0`
    - so `MultiFrame + AcquisitionFrameCount=1` may still be brittle, but it
      is probably not the entire explanation for the `100 fps` offset-camera
      instability
  - stale-onset receive-history logging is now also in:
    - threshold dump run:
      `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_staleprobe1`
    - current read:
      - at the first `latch_minus_frame_ns > 50 ms` crossing, the receive
        history still shows contiguous camera frame ids
      - `camera_dropped_frames = 0` at onset
      - `acquisition_resource_starvations = 0` at onset
      - free entry / event pools are still near full at onset
    - that pushes suspicion further upstream, toward camera/transport/SDK-side
      buffering or gated-acquisition behavior rather than obvious app-side
      queue exhaustion
  - recording-submit handoff logging is now also in:
    - probe run:
      `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_handoffprobe1`
    - current read:
      - at first stale onset on the bad camera, the recent
        `recording_ingress->SubmitFrame(...)` history still shows primary-only
        routing
      - helper dispatch has not really started yet on that camera
      - preprocess waits/drops are still `0`
      - preprocess buffers / events are still near full
      - queue depth near the recording handoff is still shallow
    - that narrows the trigger further:
      - recording must be enabled to trigger the failure
      - but the first visible onset still appears before obvious app-side
        recording queue pressure or preprocess starvation
  - sink-mode discriminator:
    - headless now supports experimental
      `recording_sink_mode=immediate_recycle|threaded_handoff_only`
    - validated runs:
      - `immediate_recycle`:
        `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_immediaterecycle_rerun2`
      - `threaded_handoff_only`:
        `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_threadedhandoff_rerun2`
    - both runs stayed near `100 fps` with `0` camera drops
    - current read:
      - the failure is not triggered by bookkeeping alone
      - the failure is not triggered by a simple cross-thread recording handoff
      - real downstream recording work is required
  - new spec-run discriminator:
    - dual-camera `100 fps` `ptp_gate` `stream_only` with no stagger:
      `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_stream_only_dual_pix_ptp`
    - dual-camera `100 fps` `ptp_gate` `stream_only` with `2 ms` stagger:
      `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_stream_only_dual_pix_ptp_stagger2ms`
    - both runs stayed near `100 fps` on both cameras with `0` camera drops
      and no `[PTP_STALE_DUMP]` output
	  - that means the pathological `100 fps` offset-camera stale-frame failure
	      is not triggered by PTP gating plus offset alone; it requires recording
	      to be active

## Audit Update (2026-04-21)

The recabled A16 topology plus the stable GPUDirect receive/requeue descriptor
patch changed the validated PTP operating point.

New validated artifacts:

- Stream-only PTP gate sanity check:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_ptp_stream_only_recabled_stable_frame_patch`
- Real PTP-gated recording, best `12 s` follow-up:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_ptp_real_recabled_stable_frame_patch_12s`

Checked-in specs:

- `experiment_specs/2010095_2010096_split_gop_hevc_100fps_ptp_stream_only_recabled_stable_frame_patch.json`
- `experiment_specs/2010095_2010096_split_gop_hevc_100fps_ptp_real_recabled_stable_frame_patch_12s.json`

The `12 s` PTP-gated real recording passed current policy:

- `2010095`: `1001` submitted frames, `0` camera frame-ID gaps,
  `0` GetFrame errors, `0` preprocess drops, `0` encode failures.
- `2010096`: `1000` submitted frames, `0` camera frame-ID gaps,
  `0` GetFrame errors, `0` preprocess drops, `0` encode failures.
- `runs.csv` reports about `100 fps` acquisition on both cameras.
- `recording_snapshot.json` reports `overflow_events = 0` and
  `peak_backlog_gops = 2`.
- `ptp_sync_summary.json` reports steady-state latch-minus-frame around
  `9.2 ms` and PTP offsets under `1 us`.

Caveat:

- The real-recording runs still emit early `PTP_STALE_DUMP` logs while encoder
  workers are starting. These dumps did not correspond to frame loss,
  `EVT_CameraGetFrame` errors, preprocess drops, or split-GOP overflow in the
  checked artifacts.

Current read:

- The older `100 fps` nonzero-stagger PTP failures remain useful historical
  failure-mode evidence, but they predate the recabled topology and stable
  receive/requeue fix.
- The currently validated synchronized recording point is no-stagger
  `ptp_gate` on the recabled A16 topology.
- Remaining PTP hardening work is now more about robustness and observability:
  suppressing or reclassifying startup-only stale dumps, adding deadline-based
  barriers, and validating longer soak runs.

## Audit Update (2026-05-04)

The latest headless external-recorder work clarified a separate PTP hot-path
cost: the expensive operation was not the PTP gate itself, but the diagnostic
per-frame `GevTimestampValueHigh/Low` camera-clock register read after
`EVT_CameraGetFrame(...)`.

Implemented since the April 21 update:

- `ORANGE_PTP_REGISTER_READ_DECIMATE=<N>`.
- Experiment-spec `fixed.ptp_register_read_decimate`.
- Default `N=1` preserves the old full per-frame diagnostic polling.
- `N>1` keeps PTP gate synchronization and embedded per-frame camera
  timestamps, while sampling the current-camera-clock register read on the
  first few frames and then every `N` frames.
- Stop/drain logic uses embedded frame timestamps when register polling is
  decimated, avoiding stale latched-clock decisions.
- Acquisition cadence probes record whether a row came from a register read
  and how old the sampled register value is.

Validated headless result:

- Run:
  `/tmp/orange_external_recorder_ptp_20260426_002618`.
- Two-camera PTP external split-GOP recording, `30 s`, decimate `100`.
- Both cameras received/ACKed/encoded `2803` frames with no drops, frame-id
  gaps, GetFrame errors, recorder failures, or pending GOP backlog.
- PTP register reads dropped to `33` per camera over `2803` frames.
- Steady `acquisition_to_ptp_done_ms p95` was effectively zero.
- Steady `acquisition_to_detect_done_ms p95` was about `4.58 ms` on both
  cameras before the A16 engine rebuild, and about `3.95 ms` with the
  high-effort A16 detect engine candidate in the later validation.
- Cross-camera embedded timestamp skew over the cadence probe stayed within
  tens of nanoseconds.

Current read:

- Full per-frame `GevTimestampValue*` polling should remain available as a PTP
  diagnostic mode.
- Decimated register polling is the right default candidate for production-like
  hot-path runs on this host.
- Headless supervised external-recorder validation with decimated polling is
  now complete. The 2026-05-07 two-camera supervised PTP run used
  `fixed.ptp_register_read_decimate = 100`, sampled `9` PTP register reads per
  camera over `401` acquired frames, encoded/ACKed `400/400` frames per camera,
  had no camera gaps/GetFrame errors, and kept cadence-probe embedded timestamp
  skew within `-18 ns` to `+22 ns`.
- GUI/session PTP validation with supervised external IPC is now complete for
  the two-camera `100_cam4_ptp` setup. The 2026-05-21 artifact
  `/home/jeremy/orange_data/exp/unsorted/2026_05_21_12_39_24` used
  `ORANGE_PTP_REGISTER_READ_DECIMATE=100`, sampled `27` PTP register reads per
  camera, recorded `1645/1645` submitted/ACKed/encoded frames per camera, and
  passed `scripts/validate_gui_ptp_recording.py --latest-complete`.
- Operational note: headless `ptp_gate` runs may auto-start `scripts/ptp_stack.sh`
  if the host PTP stack is not ready. If Orange starts it, the stack is left
  running after the run and should be stopped explicitly with
  `scripts/ptp_stack.sh stop` when no more PTP validation is planned.
- GUI `ptp_gate` now has two operational repair paths: the manual
  `Host PTP Stack` panel exposes status/start/stop/restart controls, and the
  validation launcher/wrapper can run `--ptp-stack-mode off|require|auto`.
  Autorun PTP-gated validation defaults to `auto`, so stopped
  `ptp4l`/`phc2sys` are repaired before Orange opens cameras.
- The original hardening items below are still relevant: deadline-based
  barriers, thread-safe shared state, robust reset after partial failure, and
  GUI/headless lifecycle symmetry are not solved by register-read decimation.

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
