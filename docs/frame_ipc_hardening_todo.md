# Frame IPC Hardening TODO

Context: March 2026 debugging exposed two recurring operator-facing problems:

- stale `/dev/shm/shm_cam_*` objects could cause Orange writer init failures
  (`shm_open ... Permission denied`)
- starting Orange streaming before any reader was attached allowed the SHM ring
  to fill, driving `push_fail` upward even though base publish was otherwise
  healthy

The current behavior is diagnosable, but not yet self-healing or guarded.

## Goals

- make stale-queue failures easier to recover from
- make "no reader attached" backpressure obvious
- avoid silently creating misleading SHM state

## Phase 1: Better Operator Recovery

- [ ] Add a `Recreate frame IPC queues` action in Orange.
- [ ] Require streaming to be stopped before queue recreation.
- [ ] Unlink `/shm_cam_<serial>` only for the selected/active Orange cameras.
- [ ] Show the exact queue names that will be removed before the action runs.
- [ ] After recreation, re-run the existing writer init and refresh the UI state.

## Phase 2: Stronger Initialization Diagnostics

- [ ] Distinguish `permission denied`, `size mismatch`, and generic `shm_open`
      failures explicitly in the UI.
- [ ] Consider probing existing queue metadata (`owner`, `group`, `mode`,
      `size`) when writer init fails and show that inline.
- [ ] Add a startup warning when stale queue files exist before streaming begins.

## Phase 3: Reader / Backpressure Visibility

- [ ] Add a warning state when `push_fail` increases continuously while `base`
      also increases.
- [ ] Add a short explanation in the UI that the queue is single-consumer and
      requires an active reader to keep draining.
- [ ] Consider a per-camera "likely not being drained" indicator based on
      repeated SHM push failures over a time window.

## Phase 4: Contract Hardening

- [ ] Keep Orange and Citrus SHM layouts explicitly versioned or shared from one
      source of truth to avoid header drift.
- [ ] Add a tiny compatibility/version marker in the SHM header if the current
      layout continues to evolve.
- [ ] Make non-writer readers fail loudly if the queue does not already exist;
      do not silently create empty queues from reader code paths.

## Non-Goals For The First Pass

- multi-reader fan-out from the same SHM ring
- guaranteed delivery when no reader is attached
- replacing SHM with a different IPC transport
