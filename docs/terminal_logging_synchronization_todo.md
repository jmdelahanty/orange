# Terminal Logging Synchronization TODO

Date: 2026-03-24
Related:

- `docs/terminal_logging_synchronization_decision.md`

## Goal

Make live terminal diagnostics line-stable and readable during multi-camera runs
without introducing meaningful blocking into acquisition or display work.

## Phase 1: Shared Logger Primitive

- [ ] Add a small shared helper for synchronized line logging.
- [ ] Prefer one API that accepts a fully formatted `std::string`.
- [ ] Keep the lock scope to the final write only.
- [ ] Support both stdout and stderr targets if needed.

## Phase 2: Convert Known Noisy Diagnostics

- [ ] Convert `src/acquire_frames.cpp`:
  - `[PTP_LIVE]`
  - `[GPU_DIRECT]`
  - `[PIPELINE]`
  - end-of-thread summary lines
- [ ] Convert `src/opengldisplay.cpp`:
  - `[DISPLAY]`
- [ ] Convert `src/threadworker.h`:
  - `Child Thread Start`
  - `Child Thread DONE`

## Phase 3: Verify Behavior

- [ ] Run a 4-camera streaming session and verify:
  - no left-padded/smeared lines
  - each diagnostic appears as one complete terminal line
  - stop/shutdown summaries remain readable
- [ ] Confirm no visible effect on:
  - streaming stability
  - PTP diagnostics cadence
  - display cadence

## Non-Goals

- [ ] Do not replace every `printf` in the codebase.
- [ ] Do not introduce a full async logging system unless Phase 1/2 is not good enough.
- [ ] Do not change the meaning or frequency of the existing diagnostics in the first pass.

## Definition of Done

- [ ] The main runtime diagnostics from acquisition/display threads are line-atomic.
- [ ] The terminal no longer shows large left-padding/offset artifacts during normal streaming.
- [ ] The implementation keeps formatting outside the lock and only synchronizes final emission.
