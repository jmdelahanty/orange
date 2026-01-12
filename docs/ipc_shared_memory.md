# IPC Shared Memory Permissions

This project uses POSIX shared memory in `/dev/shm` for inter-process
communication. The shared memory ring buffer is defined in `src/shaman.h`.

## Why group permissions matter

Readers call `SharedBoxQueue::pop(...)`, which advances the `tail` index in
shared memory. That means readers open the shared memory object with `O_RDWR`,
not read-only. If the writer (running as root) creates the object with a
restrictive mode, non-root readers will get `Permission denied` on `shm_open`.

## Expected permissions

The writer sets the shared memory group to `ipc` and mode to `0660`:

```
-rw-rw---- 1 root ipc ... /dev/shm/shm_cam_2010093
```

## Setup

Run the helper script once:

```
sudo scripts/setup_ipc_group.sh
```

Then either log out/in or run:

```
newgrp ipc
```

Restart the writer so it recreates `/dev/shm/shm_cam_*` with the new group.

## Verify

```
id -nG | grep -qw ipc && echo "ipc group active"
ls -l /dev/shm/shm_cam_*
```

If you still see `root root` or `-rw-r--r--`, the writer binary you are running
does not include the group setup change.

## Note on real-time scheduling

If you need `SCHED_FIFO` without `sudo`, you can grant the capability:

```
sudo setcap cap_sys_nice=eip ./targets/.../orange
```

This is separate from shared memory permissions.
