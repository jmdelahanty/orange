# Recording Metadata (Producer -> Consumer)

This document describes where the producer writes recording metadata and how
consumers should parse it.

For a full current-runtime artifact and schema contract (including CSV, video
sidecars, IPC payloads, and known caveats), see:

- `docs/output_artifacts_contract.md`

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
  "encoders": { ... },
  "models": { ... }
}
```

`cameras` is a dictionary keyed by camera serial number (as a string), where each
value is the full camera config JSON used at recording start (or `null` if missing).

`encoders` is a dictionary keyed by camera serial number (as a string). Each value
captures resolved runtime encoder parameters for one or more outputs for that
camera.

Preferred multi-output shape (full + crop):

```json
{
  "encoders": {
    "02010093": {
      "schema_version": 2,
      "outputs": {
        "full": { "...": "encoder_info" },
        "crop": { "...": "encoder_info" }
      }
    }
  }
}
```

Output key semantics:

- `full`: full-frame recording encoder.
- `crop`: crop recording encoder (for example 256x256 detection/pose-driven ROI
  path).

Compatibility rule:

- During migration, producers may still emit legacy shape:
  - `encoders[serial] = <encoder_info>`
- Consumers should support both shapes:
  - if `encoders[serial].outputs` exists, use that;
  - otherwise treat `encoders[serial]` as the `full` encoder entry.

`models` is an optional dictionary keyed by camera serial number (as a string).
Each value captures resolved runtime model metadata (for example detect TRT model
and pose TRT model/skeleton) so downstream consumers can reproduce interpretation
of outputs.

Per-output `encoder_info` should include at least:

- current encoder fields (`backend`, `path`, `codec`, `preset`, `tuning`,
  `resolution`, `fps`, GOP/RC fields).
- RC strategy detail for reproducibility:
  - `rc.strategy` such as `vbr`, `vbr_cq`, `cqp`, or `lossless`
  - optional `rc.target_quality` for VBR+CQ recordings
  - optional `rc.const_qp.{p,b,i}` for CONSTQP/lossless recordings
- output identity (`output` = `full|crop`).
- artifact linkage (for example `video_file`, `metadata_file`, `keyframe_file`
  basenames) so downstream systems can map encoder config to generated files.

## Model Metadata Source of Truth

Recommended source-of-truth split:

- Camera config JSON stores operator intent and defaults (for example model path,
  skeleton id, enable flags, UI defaults).
- Recording snapshot stores resolved runtime values actually used for that run.

Consumer rule:

- Consumers should read detect/pose model details from
  `recording_snapshot.json` first, not directly from static camera config files.

## Detect Model Metadata

`models[serial].detect` should capture the resolved detect runtime, including:

- `enabled`
- source provenance (`camera_config_path` and/or UI-driven selection metadata)
- runtime identifiers:
  - `backend`, `engine_path`, `engine_sha256`
  - optional class mapping identifiers (for example `classes_path`,
    `classes_sha256`, `label_space`)
  - input/output interpretation fields used by downstream consumers

Suggested snapshot shape:

```json
{
  "models": {
    "02010093": {
      "detect": {
        "enabled": true,
        "source": {
          "camera_config_path": "/abs/path/config/local/02010093.json",
          "ui_selected": true
        },
        "runtime": {
          "backend": "tensorrt",
          "engine_path": "/abs/path/models/fish_jinyao.engine",
          "engine_sha256": "123abc...",
          "classes_path": "/abs/path/models/fish_classes.txt",
          "classes_sha256": "789xyz...",
          "input": {"width": 640, "height": 640, "format": "rgb8"},
          "output": {
            "bbox_layout": "x,y,w,h",
            "score_field": "prob",
            "class_count": 1
          }
        }
      },
      "pose": {
        "enabled": true,
        "source": {
          "camera_config_path": "/abs/path/config/local/02010093.json"
        },
        "runtime": {
          "backend": "tensorrt",
          "engine_path": "/abs/path/models/rat_pose.engine",
          "engine_sha256": "abc123...",
          "skeleton_id": "rat_v1_8pt",
          "skeleton_path": "/abs/path/models/rat_v1_8pt.json",
          "skeleton_sha256": "def456...",
          "input": {"width": 256, "height": 256, "format": "mono8"},
          "output": {"kps_layout": "x,y,s", "max_kps_floats": 32},
          "execution": {
            "inference_path": "cuda_graph",
            "cuda_graph_requested": true,
            "cuda_graph_captured": true
          }
        }
      }
    }
  }
}
```

## Pose Model and Skeleton Metadata

`models[serial].pose` should capture the resolved pose runtime, including
model+skeleton identifiers and output interpretation fields.

## Pose Execution Metadata (CUDA Graph / Enqueue)

For reproducibility, pose runtime metadata should also include the effective
execution mode used during that recording run.

Recommended fields under `models[serial].pose.runtime.execution`:

- `inference_path`: `cuda_graph` or `enqueue`.
- `cuda_graph_requested`: whether graph mode was requested by runtime config.
- `cuda_graph_captured`: whether graph capture succeeded for this worker/model.
- `fallback_reason` (optional): one-line reason when requested graph mode falls
  back to enqueue path.

Notes:

- If runtime overrides are applied, snapshot must record the override result.
- If detect is disabled for a camera, either omit `models[serial].detect` or set
  `"enabled": false`.
- If pose is disabled for a camera, either omit `models[serial].pose` or set
  `"enabled": false`.
- If pose graph capture is disabled or fails, snapshot should still record
  execution mode (`enqueue`) and fallback reason when available.
- If detect model, pose model, or skeleton changes during a recording,
  append/update with an effective frame range marker.

## Keyframe sidecar

Each recording also writes a keyframe index sidecar alongside the video:

```
Cam<serial>_keyframe.json
```

This file is emitted by the writer and contains:

```
{
  "codec": "hevc",
  "fps": 60,
  "total_frames": 12345,
  "keyframe_frames": [0, 60, 120, 180, ...]
}
```

The writer sets `AV_PKT_FLAG_KEY` for H.264 IDR (NAL type 5) and HEVC IDR
(NAL types 19/20), and uses that to populate the keyframe list.

Important: these camera configs are read from the static JSON config files at
recording start. The snapshot does not query live camera state from the SDK, and
does not reflect any runtime UI tweaks applied after recording starts.

Legacy single-output example (currently emitted for full-frame HW encoder):

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

Target multi-output example (full + crop):

```json
{
  "encoders": {
    "02010093": {
      "schema_version": 2,
      "outputs": {
        "full": {
          "output": "full",
          "backend": "nvenc",
          "path": "hw",
          "codec": "hevc",
          "resolution": {"width": 4512, "height": 4512}
        },
        "crop": {
          "output": "crop",
          "backend": "nvenc",
          "path": "crop",
          "codec": "hevc",
          "resolution": {"width": 256, "height": 256},
          "video_file": "Cam02010093_crop.mp4",
          "metadata_file": "Cam02010093_crop_meta.csv",
          "keyframe_file": "Cam02010093_crop_keyframe.json"
        }
      }
    }
  }
}
```
