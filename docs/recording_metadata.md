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
  "cameras": { ... },
  "encoders": { ... }
}
```

`cameras` is a dictionary keyed by camera serial number (as a string), where each
value is the full camera config JSON used at recording start (or `null` if missing).

`encoders` is a dictionary keyed by camera serial number (as a string). Each value
captures resolved encoder parameters for that camera at recording start (HW NVENC
path only).

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
  },
  "encoders": {
    "02010093": {
      "backend": "nvenc",
      "path": "hw",
      "codec": "hevc",
      "preset": "p1",
      "tuning": "ll",
      "gpu_id": 0,
      "color": true,
      "resolution": {"width": 4512, "height": 4512},
      "fps": 60,
      "gop_length": 120,
      "frame_interval_p": 1,
      "idr_period": 120,
      "refs": {"max_num_ref_frames_in_dpb": 1},
      "rc": {
        "mode": "vbr",
        "mode_value": 1,
        "average_bitrate": 244297728,
        "max_bitrate": 250000000,
        "vbv_buffer_size": 250000000
      },
      "aq": {"enable_aq": 1, "enable_temporal_aq": 1},
      "lookahead": {"enable": 0},
      "low_delay_keyframe_scale": 1,
      "strict_gop_target": 0,
      "enable_non_ref_p": 0,
      "repeat_sps_pps": 1,
      "enable_ptd": 1
    }
  }
}
```
