# Multi-Camera Failure Modes

Date: 2026-04-17
Branch: `exp/gop-split-a16`

## Purpose

This note summarizes the distinct multi-camera failure modes observed during
recent split-GOP and PTP-gated recording work.

It is meant to be a compact matrix of:

- what fails,
- how it presents,
- what evidence supports it,
- what is probably *not* the cause,
- and what the next diagnostic should be.

## Summary Matrix

| Scenario | Symptom | Evidence | Current Read |
| --- | --- | --- | --- |
| Dual-camera `100 fps` `free_run` recording | Throughput collapse with split-GOP output overflow | backlog exceptions, `overflow_events > 0`, `peak_backlog_gops > max_inflight_gops` | Recording-path backlog overflow under unsynchronized high-rate multi-camera load |
| Dual-camera `80 fps` `ptp_gate` recording, no stagger | Both cameras drop to about `54-57 fps` | single-camera `80 fps ptp_gate` works, dual-camera `80 fps free_run` works, dual-camera `80 fps ptp_gate` fails | Synchronized burst contention once tightly aligned arrivals interact with recording |
| Dual-camera `80 fps` `ptp_gate` recording, `2 ms` stagger | Stable at about `80 fps` on both cameras | `0` camera drops, balanced helper routing, `overflow_events = 0` | Stagger relieves the synchronized burst problem at `80 fps` |
| Dual-camera `100 fps` `ptp_gate` stream-only | Stable at about `100 fps` on both cameras | `0` camera drops in `--stream-only` | Raw synchronized acquisition is fine without recording |
| Dual-camera `100 fps` `ptp_gate` recording, nonzero stagger | One or both cameras collapse; bad camera eventually shows multi-second stale-frame lag | `latch_minus_frame_ns` jumps from `~9 ms` to seconds, `overflow_events = 0`, failure follows offset camera for larger offsets | PTP-gated offset acquisition becomes unstable at `100 fps`; this is a different mode than GOP backlog overflow |
| Invalid split-GOP config | GUI shows red validation and blocks stream start | missing helper or overlapping GPU claims are rejected by preflight | Config/policy failure, not runtime throughput failure |
| Headless PTP startup before hardening | Cameras open but local PTP gate never really engages, or host stack is absent | old post-reboot hangs and zero-participant barrier state | Operational setup failure; largely addressed by host-stack preflight/auto-start |

## Detailed Failure Modes

### 1. Split-GOP Backlog Overflow

Observed in:

- dual-camera `100 fps` `free_run` recording

Typical signal:

- terminal logs like:
  - `split_gop-ending GOP backlog exceeded configured limit`
- `recording_snapshot.json` shows:
  - `peak_backlog_gops > max_inflight_gops`
  - `overflow_events > 0`
  - `frontier_present = true`
  - `frontier_complete = false`

Interpretation:

- the pipeline is receiving work fast enough to keep creating later GOPs
- but the oldest GOP needed for ordered output is not completing fast enough
- newer GOPs pile up behind the incomplete frontier GOP

This looks like:

- recording/output path overload
- not a camera-side PTP issue

Representative artifacts:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_rerun2`
- `/home/jeremy/orange_data/exp/unsorted/2026_04_17_12_50_42`

### 2. Synchronized Burst Contention

Observed in:

- dual-camera `80 fps` `ptp_gate` recording with no stagger

Typical signal:

- both cameras degrade together to about `54-57 fps`
- preprocess/encode track the lower acquisition rate
- single-camera `80 fps ptp_gate` is healthy
- dual-camera `80 fps free_run` is healthy

Interpretation:

- tightly aligned PTP-gated arrivals create bursty instantaneous load
- average throughput is not the main limit
- some shared part of the acquisition-to-recording path cannot absorb the
  synchronized bursts cleanly

What this is probably not:

- not a wrong GPU pairing issue
- not a generic PTP misconfiguration
- not a simple average-bandwidth limit

Why we believe this:

- `2 ms` stagger restores healthy `80 fps` dual-camera recording

Representative artifacts:

- failing unstaggered run:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_80fps_gop25_dual_pix_ptp_rerun7`
- healthy staggered run:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_80fps_gop25_dual_pix_ptp_stagger2ms_rerun1`

### 3. PTP-Gated Offset Instability At 100 FPS

Observed in:

- dual-camera `100 fps` `ptp_gate` recording with any nonzero stagger tested

Typical signal:

- run does not show split-GOP backlog overflow
- `overflow_events = 0`
- one camera starts healthy, then falls badly behind
- on the bad camera:
  - `latch_minus_frame_ns` begins near `~9-10 ms`
  - later jumps to multi-second values

Interpretation:

- the camera clock continues advancing
- but the frames reaching the receiver are extremely stale relative to the
  camera's current PTP time
- that suggests buffering or gated-acquisition instability after gate open

Important discriminator:

- for larger offsets, the bad behavior follows the offset camera
- swapped `2 ms` order moved the catastrophic failure to the camera with the
  stagger

What this is probably not:

- not the old GOP backlog overflow mode
- not a wrong GPU assignment issue
- not simply "PTP is broken" in all cases

Relevant sweep outcomes:

- `25 us`
- `50 us`
- `100 us`
- `250 us`
- `2 ms`
- swapped `2 ms`

All remained unstable at `100 fps`.

Representative artifacts:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger25us_rerun1`
- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger50us_rerun1`
- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger100us_rerun1`
- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger0p25ms_rerun1`
- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_rerun2`
- `/home/jeremy/orange_data/exp/unsorted/2010096_2010095_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_swaporder_rerun1`

### 4. Operational / Setup Failures

Observed earlier in the work:

- host linuxptp stack not running after reboot
- local PTP barrier waiting with zero effective participants
- occasional `GVCP ACK error` on camera open
- invalid split-GOP camera configs

Interpretation:

- these are real failures, but they are different from the performance modes
  above
- the host-stack setup gap is now substantially reduced by headless preflight
  and auto-start support
- invalid split-GOP configs are now caught by shared preflight before stream
  start

Representative areas:

- `src/orange_headless_client.cpp`
- `src/recording_validation.cpp`
- `scripts/ptp_stack.sh`

## What We Have Mostly Ruled Out

### Wrong GPU Assignment

In the core multi-camera tests, the validated source/helper claims are:

- `2010095 -> source 1, helper 2`
- `2010096 -> source 5, helper 6`

These are disjoint and correct for the tested runs.

### Average Streaming Bandwidth Limit

This is the strongest non-cause we currently have.

Why:

- dual-camera `100 fps` `free_run` stream-only works
- dual-camera `100 fps` `ptp_gate` stream-only works

So average dual-camera acquisition bandwidth is not the whole story.

The more likely issue is burstiness and queueing pressure once recording work
is added, especially under tight PTP phase alignment.

### Generic Single-Camera PTP Failure

Single-camera `ptp_gate` is healthy at the rates we tested.

So the problem is specifically multi-camera and rate-sensitive.

## Suspicious But Not Yet Proven Root Cause

The camera-side PTP-gated acquisition programming still looks brittle:

- `TriggerSelector = AcquisitionStart`
- `TriggerSource = Software`
- `TriggerMode = On`
- `AcquisitionMode = MultiFrame`
- `AcquisitionFrameCount = 1`

This clearly works in some regimes:

- single-camera `80 fps ptp_gate`
- dual-camera `60 fps ptp_gate`
- dual-camera `80 fps ptp_gate` with `2 ms` stagger

So it is not simply broken. But it remains a plausible source of fragile
high-rate multi-camera behavior, especially once offsets are introduced.

## Current Best Next Diagnostics

1. Investigate the camera-side gated acquisition mode itself.
   - Compare the current `MultiFrame + count=1` programming against a
     controlled experimental `Continuous` mode variant.
2. Add more observability around stale-frame onset.
   - Specifically, determine whether stale frames are already queued before the
     gate or begin accumulating only after some number of good post-gate
     frames.
3. Keep `80 fps` stagger as the current validated synchronized baseline.
   - Do not treat `100 fps` nonzero stagger as usable yet.

