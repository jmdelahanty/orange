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

The current queue is effectively single-consumer:

- readers call `pop(...)`
- `pop(...)` advances the shared `tail`
- if multiple readers attach, they compete for the same queue contents

This means:

- one active reader can drain frames before another reader sees them
- a reader should usually be started before or shortly after the writer

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
- `updates=0`: normal unless YOLO IPC updates are expected

See also: [frame_ipc_hardening_todo.md](./frame_ipc_hardening_todo.md)

## Note on real-time scheduling

If you need `SCHED_FIFO` without `sudo`, you can grant the capability:

```
sudo setcap cap_sys_nice=eip ./targets/.../orange
```

This is separate from shared memory permissions.
