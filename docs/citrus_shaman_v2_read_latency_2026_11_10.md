# Citrus SHAMAN v2 read lag: reader polling, not IPC (2026-11-10)

## What was measured

Recording `2026_11_10_15_55_37` (30 min, four arenas, Citrus 66aabf8 at 240 Hz,
Orange edc5450 at 100 Hz). Citrus's report gives the SHAMAN v2 read lag as
`bounding_boxes` received time minus payload time, the same in all four arenas:

| median | p95 | p99 | max |
|---|---|---|---|
| 0.54 ms | 1.02 ms | 1.06 ms | 18.3 ms |

- Payload time is Orange's publish time: `orange_publish_timestamp_us_*` in the
  SHAMAN v2 state, copied by `ExternalTrackingDataFromShamanV2State`
  (Citrus `src/ipc/ShamanV2IPCReader.cpp:772`).
- Received time is stamped by `ShamanV2SharedSource::PublishAcceptedState`
  (`src/ipc/ShamanV2SharedSource.cpp:347`) after the reader accepts the state.
- The lag therefore adds to Orange's ~11 ms from exposure to box
  (~9.2 ms camera transfer + ~1.8 ms detect).

## Cause

Reading a state from the shared-memory queue takes microseconds. The time is
spent waiting for the reader to look. The Citrus reader loop
(`src/ipc/ShamanV2IPCReader.cpp:1198`) sleeps 1 ms whenever a poll accepts
nothing, and yields otherwise:

```cpp
if (accepted == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
} else {
    std::this_thread::yield();
}
```

- Orange publishes one state per camera every 10 ms. Each state waits for the
  reader's next wake, which is uniform over 0 to about 1.05-1.1 ms: the 1 ms
  sleep plus the default 50 us timer slack and wake latency. A uniform wait over
  that range has a median of about 0.54 ms and a p99 of about 1.06 ms, which is
  what was measured.
- The 18 ms maximum is the reader thread not getting the CPU when it woke. It
  runs at normal priority on shared (non-isolated) cores, so one burst of other
  work is enough: recording start or stop, a GUI stall, or compositor work.

## Proposed fix (Citrus code, Citrus owner's decision)

1. Run the SHAMAN v2 reader thread on a Citrus-owned isolated core: 1 or 2,
   with siblings 33/34 (pancakebatter `core_roles`). It can optionally run
   SCHED_FIFO at a modest priority.
2. Replace the 1 ms sleep with one of:
   - a short spin, which is cheap on an isolated core at 4 arenas x 100 Hz;
   - `clock_nanosleep` of about 50 us after `prctl(PR_SET_TIMERSLACK, 1)`.

   The expected median is about 0.03-0.05 ms, with no multi-millisecond outliers.
3. Optional, longer term: Orange signals each publish (an eventfd, or a futex
   on a generation word in the segment), so the reader blocks instead of
   polling. This changes the SHAMAN v2 protocol (shaman-ipc). It needs both
   sides and a contract note.

## Why it matters

At 240 Hz one display period is 4.17 ms.
- The polling wait adds up to about 1 ms to the box age when the chaser uses it.
- The rare 18 ms stalls cost more than four refreshes.

Sent to citrus-agent on 2026-11-10. Add the measured lag here after a change
is installed.
