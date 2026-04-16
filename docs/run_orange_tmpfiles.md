# `/run/orange` Tmpfiles Provisioning

This note describes how to provision the live Orange IPC directory used for:

- `/run/orange/latest_recording.json`

## Why This Exists

`/run` is tmpfs. That means:

- it is a good place for live producer/consumer rendezvous state
- it is reset on reboot
- manual ownership or mode fixes do not persist across boot

So if Orange and Citrus want a stable machine-wide live pointer path, the
directory should be provisioned by the system rather than created ad hoc during
recording.

## Recommendation

Treat:

- `/run/orange/latest_recording.json`

as the canonical live IPC path for producer/consumer discovery.

Treat:

- `~/orange_data/.orange/latest_recording.json`

as the durable user-data fallback.

## Tmpfiles Rule

Repo-tracked sample:

- [config/system/orange-tmpfiles.conf.example](/home/jeremy/orange-gop-split-a16/config/system/orange-tmpfiles.conf.example)

Suggested installed path:

- `/etc/tmpfiles.d/orange.conf`

Suggested rule:

```text
d /run/orange 2775 root ipc -
```

Why `2775 root:ipc`:

- `root` owns the directory
- `ipc` group can write
- setgid keeps descendants in group `ipc`
- world can traverse/read if files are written `0644`

## Applying It

Install:

```bash
sudo install -o root -g root -m 0644 \
  /path/to/orange-tmpfiles.conf.example \
  /etc/tmpfiles.d/orange.conf
```

Apply immediately without reboot:

```bash
sudo systemd-tmpfiles --create /etc/tmpfiles.d/orange.conf
```

Verify:

```bash
ls -ld /run/orange
```

Expected shape:

- owner: `root`
- group: `ipc`
- mode: `drwxrwsr-x` (`2775`)

## Relationship To App Config

App storage config should still control:

- whether Orange writes the `/run` pointer
- what file path it writes
- the canonical durable pointer root under `~/orange_data`

But app config should not be responsible for provisioning `/run/orange` itself.
That is system policy, and `tmpfiles.d` is the right mechanism for it.
