# App Storage Config Schema

This document proposes an app-level schema for Orange storage defaults and
latest-recording pointer behavior.

This is intentionally separate from the camera config schema:

- camera config answers:
  - how a specific camera should run
  - where its source GPU lives
  - how its recording path should behave
- app storage config answers:
  - where recordings should go by default
  - where latest-recording pointers should be written

This schema is meant for process/session defaults, not per-camera behavior.

## Why This Exists

Today the default GUI recording root is effectively derived from the current
user and hardcoded into the Orange data tree:

- [orange.cpp](/home/jeremy/orange-gop-split-a16/src/orange.cpp:2312)
- [orange.cpp](/home/jeremy/orange-gop-split-a16/src/orange.cpp:2314)

That gives us a default base folder like:

- `~/orange_data/exp/unsorted`

At the same time, latest-recording metadata is written to:

- `<base_folder>/.orange/latest_recording.json`
- `/run/orange/latest_recording.json`

See:

- [recording_metadata.md](/home/jeremy/orange-gop-split-a16/docs/recording_metadata.md:14)
- [recording_pointer_compatibility_plan.md](/home/jeremy/orange-gop-split-a16/docs/recording_pointer_compatibility_plan.md:1)

We want a first-class config for the default recording root and a cleaner model
for how the latest-recording pointers relate to it.

## Schema Identity

- `schema_id = "orange.app.config"`
- `schema_version = 1`

## Proposed Shape

```json
{
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "storage": {
    "default_recording_root": "/home/jeremy/orange_data/exp/unsorted",
    "latest_recording": {
      "write_local_pointer": true,
      "canonical_pointer_root": "/home/jeremy/orange_data/.orange",
      "write_run_pointer": true,
      "run_pointer_path": "/run/orange/latest_recording.json"
    }
  }
}
```

## Field Semantics

### `storage.default_recording_root`

Type:

- string

Meaning:

- the default base folder for recordings when the caller does not explicitly
  provide a recording folder

Examples:

- GUI default output root
- headless local mode when no `--record-folder` is provided and recording is
  enabled
- future automation defaults

This should generally point at a persistent user-data tree such as:

- `~/orange_data/exp/unsorted`

It is a base folder, not a single run folder.

### `storage.latest_recording.write_local_pointer`

Type:

- boolean

Meaning:

- whether Orange writes:
  - `<actual_base_folder>/.orange/latest_recording.json`

Recommended default:

- `true`

This should remain the required local pointer because it is colocated with the
actual output tree.

### `storage.latest_recording.canonical_pointer_root`

Type:

- string

Meaning:

- a stable user-level location where Orange also writes:
  - `<canonical_pointer_root>/latest_recording.json`

Recommended default:

- `~/orange_data/.orange`

This is the proposed durable user-data pointer root.

Unlike the local pointer, this path should remain stable even if a single run
overrides the recording folder somewhere else.

### `storage.latest_recording.write_run_pointer`

Type:

- boolean

Meaning:

- whether Orange attempts to write the compatibility pointer used by Citrus and
  other system-level consumers

Recommended default:

- `true`

This should remain enabled initially for compatibility.

### `storage.latest_recording.run_pointer_path`

Type:

- string

Meaning:

- the absolute compatibility pointer path

Recommended default:

- `/run/orange/latest_recording.json`

This is a full file path, not just a directory.

## Precedence Rules

The effective recording base folder should resolve in this order:

1. explicit per-run recording folder
   - GUI-selected folder
   - headless `--record-folder`
   - experiment-run resolved output folder
2. `storage.default_recording_root`
3. legacy fallback:
   - `~/orange_data/exp/unsorted`

The pointer outputs should then use the resolved base folder plus the configured
metadata roots.

## How `latest_recording.json` Should Behave

Assume:

- `storage.default_recording_root = /home/jeremy/orange_data/exp/unsorted`
- `storage.latest_recording.canonical_pointer_root = /home/jeremy/orange_data/.orange`
- `storage.latest_recording.run_pointer_path = /run/orange/latest_recording.json`

And assume Orange resolves:

- `actual_base_folder = /home/jeremy/orange_data/exp/unsorted`
- `recording_folder = /home/jeremy/orange_data/exp/unsorted/2026_04_15_22_19_15`

Then Orange should write:

1. local pointer:
   - `/home/jeremy/orange_data/exp/unsorted/.orange/latest_recording.json`
2. canonical user-data pointer:
   - `/home/jeremy/orange_data/.orange/latest_recording.json`
3. compatibility run pointer:
   - `/run/orange/latest_recording.json`

All of those pointers should contain the same payload:

```json
{
  "recording_id": "2026_04_15_22_19_15",
  "timestamp_utc": "2026-04-16T02:19:15Z",
  "recording_folder": "/home/jeremy/orange_data/exp/unsorted/2026_04_15_22_19_15",
  "snapshot_path": "/home/jeremy/orange_data/exp/unsorted/2026_04_15_22_19_15/recording_snapshot.json"
}
```

### Important detail

The canonical pointer should point at the actual recording folder used for the
run, even if that run did not use the configured default root.

Example:

- configured default root:
  - `/home/jeremy/orange_data/exp/unsorted`
- actual one-off run folder override:
  - `/mnt/fast/orange/exp/unsorted`

Then the canonical pointer under:

- `/home/jeremy/orange_data/.orange/latest_recording.json`

should still point at:

- `/mnt/fast/orange/exp/unsorted/<recording_id>`

That is what makes it a stable lookup location rather than just a mirror of the
default root.

## What This Schema Does Not Cover

This schema should not contain:

- per-camera recording strategy
- source GPU placement
- encoder GPU ids
- codec/preset/GOP defaults for a specific camera
- Citrus session output policy

Those belong elsewhere:

- per-camera config schema
- experiment specs
- Citrus contracts

## Relationship to Citrus

This schema does not remove `/run/orange/latest_recording.json`.

It only makes the Orange-side storage model cleaner by adding:

- a configurable default recording root
- a configurable canonical user-data pointer root

During migration, Citrus can continue preferring:

- `/run/orange/latest_recording.json`

while Orange also writes:

- `~/orange_data/.orange/latest_recording.json`

See:

- [recording_pointer_compatibility_plan.md](/home/jeremy/orange-gop-split-a16/docs/recording_pointer_compatibility_plan.md)

## Recommended Implementation Order

1. Add app-level config parsing for this schema.
2. Use `storage.default_recording_root` where Orange currently falls back to
   `~/orange_data/exp/unsorted`.
3. Keep the existing local pointer.
4. Add canonical user-data pointer writing.
5. Keep `/run/orange/latest_recording.json` as compatibility output.
6. Update docs/contracts after runtime support lands.

## What I Think

This should be an app-level schema, not a camera schema.

The most important design property is:

- one stable configured default for where recordings go
- one stable configured user-data pointer location
- continued `/run/orange` compatibility while Citrus still expects it

That gives Orange a cleaner ownership model without immediately breaking the
existing Citrus discovery flow.
