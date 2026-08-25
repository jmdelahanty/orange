# IPC Shared Memory Permissions

This project uses POSIX shared memory in `/dev/shm` for inter-process
communication. The shared memory ring buffer is defined in `src/shaman.h`.

## Why group permissions matter

Readers call `SharedBoxQueue::pop(...)`, which advances the `tail` index in
shared memory. That means readers open the shared memory object with `O_RDWR`,
not read-only. If the writer (running as root) creates the object with a
restrictive mode, non-root readers will get `Permission denied` on `shm_open`.

## Expected permissions

The writer sets the shared memory group to `ipc` and mode to `0660`.

The owning user is not required to be `root`. POSIX shared memory objects
persist until they are unlinked, and the writer only normalizes the group, not
the owning user. That means a healthy queue may appear as either:

```
-rw-rw---- 1 root   ipc ... /dev/shm/shm_cam_2010093
-rw-rw---- 1 jeremy ipc ... /dev/shm/shm_cam_2010093
```

Both are acceptable. What matters is:

- mode is `0660` (`-rw-rw----`)
- group is `ipc`
- the intended reader is either the owner or a member of `ipc`

## Setup

Run the helper script once:

```
sudo scripts/setup_ipc_group.sh
```

Then either log out/in or run:

```
newgrp ipc
```

Restart the writer so it opens or recreates `/dev/shm/shm_cam_*` with the new
group.

## Verify

```
id -nG | grep -qw ipc && echo "ipc group active"
ls -l /dev/shm/shm_cam_*
```

If you still see `root root` or `-rw-r--r--`, the writer binary you are running
does not include the group setup change.

If the owner is your user instead of `root`, that is fine as long as the group
is `ipc` and the mode is `0660`.

## Runtime behavior

Frame IPC in Orange is a per-camera shared-memory ring:

- queue name: `/shm_cam_<camera_serial>`
- writer: Orange `FrameIPCManager`
- reader: Citrus or another SHM consumer
- slot timestamps (current ABI field names):
  - `timestamp_us_epoch` = `orange_shm_publish_timestamp_us_epoch`:
    Orange enqueue/publish wall-clock microseconds written when Orange pushes a
    slot into SHM
  - `timestamp_us_monotonic` = `orange_shm_publish_timestamp_us_monotonic`:
    Orange enqueue/publish steady-clock microseconds written when Orange pushes
    a slot into SHM

These SHM timestamps are publish-time metadata for the Orange producer. They are
not the original camera/acquisition timestamp for the frame.

Orange already carries the original camera/acquisition timestamp separately in
recording metadata and YOLO event artifacts. In current Orange terminology, that
timestamp domain should be referred to as `camera_timestamp_ns`.

The current queue is effectively single-consumer:

- readers call `pop(...)`
- `pop(...)` advances the shared `tail`
- if multiple readers attach, they compete for the same queue contents

This means:

- one active reader can drain frames before another reader sees them
- a reader should usually be started before or shortly after the writer

## Citrus Live-Control Semantics

Citrus currently treats `/shm_cam_<camera_serial>` as a latest-state stream for
stimulus execution, not as a complete producer event log. It drains available
messages and keeps the latest effective tracking state.

For the current Citrus contract, Orange should preserve monotonic `frame_id`
behavior on this queue. Delayed YOLO detections for older frames should not be
published as older-frame updates because Citrus may treat any non-empty payload
as current stimulus state.

Current timestamp naming guidance for this queue:

- Orange producer SHM publish timestamps:
  - `orange_shm_publish_timestamp_us_epoch`
  - `orange_shm_publish_timestamp_us_monotonic`
- Citrus consumer-side receive timestamps in docs should be described as:
  - `citrus_ipc_receive_timestamp_*`
- Citrus stimulus/frame-output timestamps in docs should be described as:
  - `citrus_stimulus_output_timestamp_*`

Citrus should not interpret current SHM `timestamp_us_*` fields as
`camera_timestamp_ns`.

See [yolo_ipc_citrus_contract_plan.md](./yolo_ipc_citrus_contract_plan.md) for
the live-control vs Orange audit-log split.

## Future Live Camera Timestamp Path

If Citrus later needs live access to `camera_timestamp_ns`, do not change the
current `/shm_cam_<camera_serial>` queue in place.

Recommended safe path:

- keep the current `/shm_cam_<camera_serial>` behavior, field meanings, and
  binary layout unchanged
- add a versioned `/shm_cam_<camera_serial>_v2` queue
- include explicit schema/version fields in the v2 slot payload
- add `camera_timestamp_ns` to that v2 payload
- preserve the same monotonic live-state contract and stale-update suppression
  rules so delayed older-frame detections still cannot regress Citrus state

The first concrete v2 design is documented in
[shaman_v2_live_state_contract.md](./shaman_v2_live_state_contract.md). It
keeps v2 as a Citrus live-control latest-state stream, not a complete event log,
and adds explicit YOLO/pose status plus source-frame pose keypoints.

Orange can create the v2 writer from the existing `FrameIPCManager` by setting:

```bash
ORANGE_SHAMAN_V2_LIVE_STATE=1
```

This remains an opt-in validation switch, not the production default. Citrus
now has a matching authoritative reader. For an automated Shadow validation
without changing `shadow.json`, launch Citrus autorun with:

```bash
CITRUS_GUI_AUTORUN_SHAMAN_V2_AUTHORITATIVE=1
```

When enabled for a SHAMAN arena, Citrus suppresses its legacy v1 reader and
uses v2 as the sole producer of the Arena tracking queue. Without this switch
or the equivalent per-arena UI selection, current v1 behavior is unchanged.

## Headless IPC Testing

Headless local runs keep frame IPC disabled by default. Enable it explicitly when
the run is meant to exercise the shared-memory producer path:

```bash
orange_client --mode local --record-folder /abs/run --frame-ipc producer_only
orange_client --mode local --record-folder /abs/run --frame-ipc verify_drain
orange_client --mode local --record-folder /abs/run --frame-ipc verify_drain_v2
```

Experiment specs use `fixed.frame_ipc`.

- `producer_only` creates the same serial-named writers as the GUI and expects an
  external consumer to drain `/shm_cam_<camera_serial>`.
- `verify_drain` creates those writers plus one built-in reader per selected
  camera and writes `frame_ipc_summary.json` into the run folder.
- `verify_drain_v2` forces the Shaman v2 live-state writer, drains
  `/shm_cam_<camera_serial>_v2`, and records v2 reader/publisher counters in
  the same `frame_ipc_summary.json` contract.
- `verify_drain` consumes the queue contents. Do not use it for a Citrus
  integration run where Citrus must see every IPC message.
- `verify_drain_v2` is also a single-consumer test mode. Do not run it while a
  Citrus v2 reader is expected to consume the same queue.
- `--frame-ipc-unlink-existing` or
  `fixed.frame_ipc.unlink_existing_queues=true` removes stale serial-named SHM
  objects before the writers are created. In v2 mode it also removes the
  corresponding `_v2` object.

Validated 2026-04-21 smoke:

- `verify_drain` on camera `2010096`
- queue: `/shm_cam_2010096`
- frames published: `901`
- frames drained by verifier: `901`
- frame-id gaps: `0`
- push failures: `0`
- artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_frame_ipc_verify_stream_only_a16_gpu5_retry/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25/frame_ipc_summary.json`

Validated 2026-05-10 v2 smoke:

- `verify_drain_v2` on camera `2010096`
- queue: `/shm_cam_2010096_v2`
- active v2 frames published: `501`
- frames drained by verifier: `501`
- frame-id gaps: `0`
- sequence-id gaps: `0`
- active v2 push failures: `0`
- artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_frame_ipc_verify_v2_stream_only_a16_gpu5_rerun/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25/frame_ipc_summary.json`

The same v2 smoke intentionally does not drain the transitional v1 queue, so
`v1_ipc_push_failures` can be nonzero without failing the active v2 verifier.

Validated 2026-05-10 pose-to-v2 smoke:

- `verify_drain_v2` plus noop `pose_worker` on camera `2010096`
- synthetic center-box runtime detections were enabled for pose plumbing only
- v2 base states published: `410`
- v2 pose update states published: `282`
- v2 slots drained by verifier: `692`
- frame-id gaps: `0`
- sequence-id gaps: `0`
- active v2 push failures: `0`
- pose JSONL rows: `401`
- stale v2 pose suppressions: `128`
- artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010096_pose_noop_synthetic_center_box_v2_ipc_rerun2/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25__aq_off__tempaq_off__lookahead_off/frame_ipc_summary.json`

The stale suppressions are expected live-state behavior: Orange keeps the full
pose history in `Cam2010096_pose_events.jsonl`, while v2 IPC publishes only
same-frame pose state that is still current for Citrus.

## Troubleshooting

### Orange shows `init_error=shm_open failed ... Permission denied`

This usually means the existing `/dev/shm/shm_cam_*` objects are stale or were
created under an incompatible prior state.

Observed recovery procedure:

1. Stop Orange and all IPC readers.
2. Remove the stale SHM objects:

   ```
   rm -f /dev/shm/shm_cam_2010093 /dev/shm/shm_cam_2010094 /dev/shm/shm_cam_2010095 /dev/shm/shm_cam_2010096
   ```

3. Restart Orange so it recreates the queues.
4. Verify the new objects are writable by the intended reader, for example:

   ```
   -rw-rw---- 1 root ipc ... /dev/shm/shm_cam_2010093
   ```

`root:ipc` and `jeremy:ipc` are both acceptable after recreation as long as the
mode is `0660`.

If the SHAMAN slot schema changed, restarting only the reader is not enough.
Restart Orange too so the queue is recreated with the current `VectorSlot`
layout.

### Orange shows rising `push_fail` counts

`push_fail` means Orange tried to publish to SHM but the shared-memory ring was
already full.

In practice, this usually means:

- no reader is currently draining the queue, or
- a reader attached too late and the queue filled before it started, or
- the active reader is too slow for the current publish rate

This does **not** necessarily mean the writer is broken. If `base` is still
increasing in Orange's `Frame IPC Status` panel, Orange is successfully
publishing some frames and the problem is backpressure at the SHM ring.

### Citrus `dummy_reader` connects but shows no frames

Check these in order:

1. Orange `Frame IPC Status` must show `base` increasing.
2. Only one reader should be attached to a given queue.
3. If `push_fail` is already large, restart the writer and reader in a clean
   order:
   - start Orange
   - start streaming
   - start exactly one reader immediately

## Current debugging signals

Orange's `Frame IPC Status` panel is the fastest way to tell which failure mode
you are in:

- `manager unavailable` + `init_error=...`: queue creation/open failure
- `base` increasing: writer is publishing
- `push_fail` increasing: SHM ring full / not being drained
- `stale_live_suppress` increasing: delayed older-frame detection updates are
  being intentionally suppressed to preserve Citrus latest-state semantics
- `updates=0`: normal unless YOLO IPC updates are expected

See also: [frame_ipc_hardening_todo.md](./frame_ipc_hardening_todo.md)

## Note on real-time scheduling

If you need `SCHED_FIFO` without `sudo`, you can grant the capability:

```
sudo setcap cap_sys_nice=eip ./targets/.../orange
```

This is separate from shared memory permissions.
