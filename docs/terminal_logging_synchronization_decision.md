# Terminal Logging Synchronization Decision

Date: 2026-03-24

## Problem

Several runtime diagnostics are emitted from multiple threads at roughly the same
time:

- `src/acquire_frames.cpp`
  - `[PTP_LIVE]`
  - `[GPU_DIRECT]`
  - `[PIPELINE]`
- `src/opengldisplay.cpp`
  - `[DISPLAY]`
- `src/threadworker.h`
  - `Child Thread Start`
  - `Child Thread DONE`

Today these logs mix:

- `std::cout`
- `printf`

and they are assembled from multiple `<<` insertions in different threads.

Observed result:

- terminal output is visually smeared
- lines appear with large left padding or odd offsets
- messages are hard to read even when the underlying data is correct

This is a terminal formatting problem, not evidence of bad PTP timing.

## Decision

Do not immediately add a broad global logging mutex around every stdout/stderr
write in the process.

Instead, implement a narrow synchronized line logger for the specific low-rate,
multi-threaded diagnostics listed above.

## Why

### Why not leave it as-is

- operator-facing diagnostics become hard to trust at a glance
- PTP/stream/debug sessions are harder to interpret during live runs

### Why not globally serialize all logging

- it would create unnecessary coupling across unrelated logging paths
- it could add blocking in places where the log rate is higher or less well
  understood
- it would be easy to accidentally hold the lock while formatting or while
  doing expensive work

### Why a narrow synchronized line logger is acceptable

The main noisy diagnostics are already low-rate:

- approximately once per second per camera for `[PTP_LIVE]`
- approximately once per second per camera for `[GPU_DIRECT]`
- approximately once per second per camera for `[PIPELINE]`
- approximately once per second per display worker for `[DISPLAY]`

That means line-level synchronization on these messages should have negligible
runtime impact if implemented correctly.

## Implementation Rules

1. Build the full log line before taking the lock.
2. Hold the lock only while emitting the final line.
3. Keep the synchronized logger limited to known low-rate diagnostic paths.
4. Do not introduce synchronized logging inside per-frame hot-path debug spam.
5. Do not hold the log lock across camera SDK calls, CUDA calls, or queue ops.

## Scope

Apply the synchronized logger to:

- `src/acquire_frames.cpp`
  - `[PTP_LIVE]`
  - `[GPU_DIRECT]`
  - `[PIPELINE]`
  - end-of-thread summaries
- `src/opengldisplay.cpp`
  - `[DISPLAY]`
- `src/threadworker.h`
  - worker lifecycle lines

Out of scope for the first pass:

- every legacy `printf` in the repo
- startup probe prints from camera configuration code
- third-party library logging
- a full async logging subsystem

## Expected Runtime Impact

Some serialization is intentional:

- log emission for the selected diagnostic lines will become line-atomic

But the blocking window should be very small:

- formatting happens outside the lock
- the lock covers only a single final write

Given the current low emission rate, this is expected to be operationally safe.

## Escalation Path

If line-level synchronization still proves too invasive or if log volume grows,
the next step is:

- an async logger thread with a queue

That is not the default plan for now.
