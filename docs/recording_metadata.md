# Recording Metadata (Producer -> Consumer)

This document describes where the producer writes recording metadata and how
consumers should parse it.

## Where to look

The producer writes pointer files at recording start:

```
<base_folder>/.orange/latest_recording.json
```

and a global pointer:

```
/run/orange/latest_recording.json
```

`<base_folder>` is the same save folder used by the recorder UI (the configured
output path, e.g. `encoder_config->folder_name` or `input_folder`). If the user
changes the save folder in the UI, the pointer file location changes with it.

The pointer file references the full snapshot file path for that recording.
The global pointer is written by the producer process (runs as root) and is
readable by all users. `/run` is tmpfs, so it resets on reboot.
Both pointer files are written atomically (temp file + rename) after the snapshot
file is successfully written, so consumers should never see partial JSON.

## Pointer file schema

`latest_recording.json` fields:

```
{
  "recording_id": "YYYY_MM_DD_HH_MM_SS",
  "timestamp_utc": "YYYY-MM-DDTHH:MM:SSZ",
  "recording_folder": "/abs/path/to/<base_folder>/<recording_id>",
  "snapshot_path": "/abs/path/to/<base_folder>/<recording_id>/recording_snapshot.json"
}
```

Notes:
- `recording_id` uses local time formatting for folder naming.
- `timestamp_utc` is UTC ISO-8601.

## Snapshot file location

```
<recording_folder>/recording_snapshot.json
```

## Snapshot schema

Top-level fields:

```
{
  "recording_id": "...",
  "timestamp_utc": "...",
  "producer_version": "...",
  "cameras": { ... }
}
```

`cameras` is a dictionary keyed by camera serial number (as a string), where each
value is the full camera config JSON used at recording start (or `null` if missing).

Important: these camera configs are read from the static JSON config files at
recording start. The snapshot does not query live camera state from the SDK, and
does not reflect any runtime UI tweaks applied after recording starts.

Example:

```
{
  "cameras": {
    "02010093": { ... },
    "02010094": { ... },
    "02010095": null
  }
}
```
