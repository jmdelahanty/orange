# Recording Metadata (Producer -> Consumer)

This document describes where the producer writes recording metadata and how
consumers should parse it.

For a full current-runtime artifact and schema contract (including CSV, video
sidecars, IPC payloads, and known caveats), see:

- `docs/output_artifacts_contract.md`

For current and future `recording_session.json` structure, including the
single-video layout and planned rolling-clip layout, see:

- `docs/recording_session_manifest_contract.md`

For field-level spatial-calibration schema details, see:

- `docs/spatial_layout_schema.md`

## Where to look

The producer writes pointer files at recording start:

```
<base_folder>/.orange/latest_recording.json
```

and a canonical user-data pointer:

```
<canonical_pointer_root>/latest_recording.json
```

and a global pointer:

```
/run/orange/latest_recording.json
```

`<base_folder>` is the same save folder used by the recorder UI (the configured
output path, e.g. `encoder_config->folder_name` or `input_folder`). If the user
changes the save folder in the UI, the pointer file location changes with it.

`<canonical_pointer_root>` is configured by app storage config and currently
defaults to `~/orange_data/.orange`.

The pointer file references the full snapshot file path for that recording.
The global pointer is written by the producer process (runs as root) and is
readable by all users. `/run` is tmpfs, so it resets on reboot.
The local, canonical, and global pointers are written atomically (temp file +
rename) after the snapshot file is successfully written, so consumers should
never see partial JSON.

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

## PTP Sync Summary sidecar

Orange also writes a per-recording PTP summary sidecar:

```
<recording_folder>/ptp_sync_summary.json
```

This file is intended as a low-rate per-camera timing summary for the recording
session. It is not a per-frame event log. When camera sync is disabled, the
sidecar still exists and records `mode = "none"`.

## Snapshot schema

Top-level fields:

```
{
  "schema_version": 2,
  "recording_id": "...",
  "timestamp_utc": "...",
  "producer_version": "...",
  "source_version": { ... },
  "recording_outputs": { ... },
  "sync": { ... },
  "cameras": { ... },
  "camera_runtime": { ... },
  "calibrations": { ... },
  "gpu_inventory": { ... },
  "gpu_monitoring": { ... },
  "encoders": { ... },
  "pipeline_metrics": { ... },
  "models": { ... }
}
```

`producer_version` is the short git commit when Orange can resolve its source
worktree at recording start, otherwise `"unknown"`. `source_version` is a
schema-1 provenance block with `vcs = "git"`, `worktree`, `branch`, `commit`,
`commit_short`, `describe`, `git_command_user`, `git_command_available`,
`dirty_tracked_available`, `dirty_tracked`, and `status_porcelain_tracked` when
available. In sudo/root GUI runs, Orange runs git provenance commands in a
forked child after dropping to `SUDO_UID`/`SUDO_GID`; that lets git evaluate the
worktree as the invoking user instead of root. Orange still reads `.git/HEAD`
directly as a fallback so the commit is captured even if git itself is not
available. This is intended to make performance comparisons, especially YOLO
queue/regression comparisons, traceable to the exact Orange checkout that
produced the recording.

`cameras` is a dictionary keyed by camera serial number (as a string), where each
value is the original camera config JSON loaded at recording start (or `null` if missing).

`recording_outputs` is a schema-2 dictionary keyed by camera serial, then output
kind. `full` describes the ingest-authoritative full-frame MP4 path and sidecars;
`crop` describes optional YOLO crop sidecar videos. Legacy locations such as
`encoders[serial]`, `camera_artifacts`, and `crop_outputs[serial]` are still
emitted for compatibility, but schema-2 consumers should prefer
`recording_outputs` when it is present.

`camera_runtime` is an optional dictionary keyed by camera serial number (as a
string). It records the resolved runtime camera config actually used for that
run, including runtime placement overrides such as a different `gpu_id` than
the one stored in `config/local/...`.

`gpu_inventory` is an optional dictionary keyed by GPU id string. It records the
resolved runtime device identity for the GPUs used in that recording, so a later
consumer can tell that `gpu_id = 0` mapped to a concrete device such as
`NVIDIA RTX A6000` on that host.

`gpu_monitoring` is an optional dictionary keyed by monitor name. It currently
records best-effort host-level GPU sidecars such as `nvidia-smi dmon` for
headless benchmark runs.

When pre-encoder reference capture is enabled, the recording folder may also
contain:

```text
Cam<serial>_preenc_ref.bin
Cam<serial>_preenc_ref_index.csv
Cam<serial>_preenc_ref.json
```

These are bounded benchmark artifacts representing the prepared NV12 surface
after preprocess and before encode submission. They are not sensor-native raw
recordings.

`sync` is an optional session-level synchronization snapshot captured when the
recording starts. It is intended to capture durable run provenance, not every
internal synchronization flag transition.

`calibrations` is an optional dictionary keyed by camera serial number (as a
string). It carries resolved per-recording spatial calibration outputs such as
`dish_mask`, canonical `arena_layout` references, registration metadata, and
resolved camera-pixel zone overlays. GUI recordings now emit this block when a
per-camera saved spatial artifact directory is supplied with
`ORANGE_SPATIAL_CALIBRATION_ARTIFACT_<serial>`.

Current emitted `sync` shape:

```json
{
  "schema_version": 1,
  "captured_at_utc": "YYYY-MM-DDTHH:MM:SSZ",
  "camera_sync_enabled": true,
  "mode": "none|ptp_local|ptp_network",
  "network_sync": false,
  "num_cameras_expected": 4,
  "gate_times": {
    "start_ns": 1234567890123,
    "stop_ns": 1234567999999
  },
  "barriers": {
    "start": {
      "participants_reached": 4,
      "all_reached": true
    },
    "stop": {
      "participants_reached": 0,
      "all_reached": false
    }
  },
  "signals": {
    "start_observed": false,
    "stop_observed": false
  }
}
```

Current headless `gpu_monitoring` shape:

```json
{
  "gpu_monitoring": {
    "nvidia_smi_dmon": {
      "schema_version": 1,
      "tool": "nvidia-smi dmon",
      "status": "running|completed|failed_to_start|stopped_with_signal|killed|exited_with_error",
      "sample_period_seconds": 1,
      "gpu_ids": [0],
      "artifact_path": "/abs/path/to/recording/nvidia_smi_dmon.csv",
      "stderr_path": "/abs/path/to/recording/nvidia_smi_dmon.stderr.log",
      "started_at_utc": "YYYY-MM-DDTHH:MM:SSZ",
      "stopped_at_utc": "YYYY-MM-DDTHH:MM:SSZ",
      "command": [
        "nvidia-smi",
        "dmon",
        "-i",
        "0",
        "-s",
        "putcm",
        "-d",
        "1",
        "-o",
        "DT"
      ]
    }
  }
}
```

Notes:
- This sidecar is host-level, not Orange-process-only.
- It is currently emitted by the headless recording path used for experiments.
- On older `nvidia-smi` versions, the captured file is `dmon`'s native
  whitespace-delimited text output even though the artifact filename currently
  ends in `.csv`.
- `artifact_path` and `stderr_path` are useful even when the monitor fails to
  start, because the stderr log often explains driver or CLI issues.

Current encoder snapshot extension for pre-encoder reference capture:

```json
{
  "encoders": {
    "2010096": {
      "pre_encoder_reference_capture": {
        "capture_mode": "pre_encoder_reference",
        "enabled": true,
        "max_frames": 120,
        "max_seconds": 0,
        "status": "budget_reached|completed|error",
        "frames_captured": 120,
        "bytes_written": 1469660160,
        "budget_reached": true,
        "artifacts": {
          "raw_dump": "/abs/path/Cam2010096_preenc_ref.bin",
          "index": "/abs/path/Cam2010096_preenc_ref_index.csv",
          "metadata": "/abs/path/Cam2010096_preenc_ref.json"
        }
      }
    }
  }
}
```

Notes:
- `path_type` inside this block reflects whether the capture ran on the current
  copy path or a future direct-input path.
- Consumers should verify that the three artifact paths actually exist before
  treating a reference-capture run as valid.

Current `camera_runtime` shape:

```json
{
  "camera_runtime": {
    "02010093": {
      "source": {
        "camera_config_path": "/abs/path/config/local/default/02010093.json",
        "configured_gpu_id": 0,
        "gpu_id_runtime_overridden": true
      },
      "runtime": {
        "name": "cam02010093",
        "width": 2256,
        "height": 2256,
        "frame_rate": 400,
        "pixel_format": "BayerRG8",
        "gpu_id": 8,
        "gpu_direct": true
      }
    }
  }
}
```

Notes:
- `mode` reflects the current recording-time sync mode:
  - `none`: no camera-side synchronized acquisition was enabled.
  - `ptp_local`: camera-side PTP sync enabled without network-managed gate state.
  - `ptp_network`: network-managed gate state present.
- `gate_times.start_ns` / `gate_times.stop_ns` are only present when the
  corresponding PTP gate times were populated at snapshot time.
- `barriers` and `signals` are a best-effort capture of current synchronization
  state at recording start; they are not a complete event log.

## Spatial Calibration Extension

This section documents the `recording_snapshot.json` surface for per-recording
spatial calibration outputs consumed by Citrus and other downstream tools. The
first implemented slice loads Orange spatial layout artifacts saved under
`calibrations/artifacts/<artifact_id>/` and writes the single-circle-compatible
`dish_mask` plus `arena_layout` runtime subset described below.

GUI recording hook:

```bash
ORANGE_SPATIAL_CALIBRATION_ARTIFACT_2010095=/home/jeremy/orange_data/calibrations/artifacts/<artifact_id>
ORANGE_SPATIAL_CALIBRATION_ARTIFACT_2010096=/home/jeremy/orange_data/calibrations/artifacts/<artifact_id>
./scripts/run_gui_aq_off_validation.sh
```

The artifact directory must contain `measurement.json`,
`arena_layout_runtime.json`, and `dish_mask_runtime.json` as written by the
Spatial Layout Registration UI. `measurement.json` supplies the canonical
`arena_layout` calibration ref. The current saved artifact does not yet include
a standalone canonical `dish_mask` artifact file, so Orange records a stable
runtime-derived `dish_mask` ref whose artifact id is
`<arena_artifact_id>.dish_mask_runtime`.

Recommended top-level placement:

```json
{
  "calibrations": {
    "02010093": {
      "dish_mask": { "...": "resolved per-recording dish geometry" },
      "arena_layout": { "...": "canonical ref plus resolved per-recording overlays" }
    }
  }
}
```

Recommended rules:

- key by camera serial string, matching `cameras`, `encoders`, and `models`
- keep canonical identity in artifact refs, not only in runtime geometry
- keep resolved runtime overlay geometry in `camera_native_pixels`
- allow `dish_mask` without `arena_layout`
- treat `layout_id` + `zone_id` as authoritative identity when `arena_layout` is
  present

Immediate single-circle Citrus-fed slice:

- Citrus remains the canonical owner of the selected experimental-area template
- Orange imports the selected Citrus config for the selected camera serial
- Orange should emit the observed circular fit as `dish_mask.runtime`
- Orange may also emit a trivial one-zone `arena_layout.runtime` with
  `zone_id = "z0"` so downstream Citrus/H5 consumers can already use the
  general runtime contract shape
- if a Citrus homography seed was accepted, `registration.source` should be
  `imported`

Suggested shape:

```json
{
  "calibrations": {
    "02010093": {
      "dish_mask": {
        "calibration_ref": {
          "artifact_id": "dishmask_...",
          "artifact_schema_id": "orange.calibration.dish_mask",
          "artifact_schema_version": 1,
          "fingerprint": "fnv1a64:..."
        },
        "runtime": {
          "schema_version": 1,
          "enabled": true,
          "geometry": {
            "coordinate_space": "camera_native_pixels",
            "outer_geometry": {
              "type": "circle",
              "cx": 2254.0,
              "cy": 2256.0,
              "r": 2060.0
            },
            "valid_geometry": {
              "type": "circle",
              "cx": 2254.0,
              "cy": 2256.0,
              "r": 1920.0
            },
            "edge_margin_px": 140.0
          },
          "source": "manual"
        }
      },
      "arena_layout": {
        "calibration_ref": {
          "artifact_id": "arenalayout_...",
          "artifact_schema_id": "orange.calibration.arena_layout",
          "artifact_schema_version": 1,
          "fingerprint": "fnv1a64:..."
        },
        "runtime": {
          "schema_version": 1,
          "enabled": true,
          "layout_id": "bank4_circle_v1",
          "coordinate_space": "camera_native_pixels",
          "registration": {
            "type": "similarity",
            "layout_coordinate_space": "layout_mm",
            "source": "manual_fit",
            "layout_to_camera_matrix": [
              52.0, 0.0, 100.0,
              0.0, 52.0, 120.0,
              0.0, 0.0, 1.0
            ],
            "camera_to_layout_matrix": [
              0.0192307692, 0.0, -1.9230769231,
              0.0, 0.0192307692, -2.3076923077,
              0.0, 0.0, 1.0
            ],
            "fit_point_count": 8,
            "residual_px": 1.8
          },
          "visible_zone_ids": ["z0", "z1", "z2", "z3"],
          "zones": [
            {
              "zone_id": "z0",
              "zone_index": 0,
              "visibility_status": "full",
              "geometry": {
                "type": "circle",
                "cx": 1120.0,
                "cy": 1120.0,
                "r": 420.0
              }
            },
            {
              "zone_id": "z1",
              "zone_index": 1,
              "visibility_status": "full",
              "geometry": {
                "type": "circle",
                "cx": 3392.0,
                "cy": 1120.0,
                "r": 420.0
              }
            }
          ]
        }
      }
    }
  }
}
```

Notes:

- `dish_mask` and `arena_layout` runtime payloads are convenience outputs for
  this recording; the corresponding `calibration_ref` objects identify the
  canonical calibration artifacts.
- `registration.layout_to_camera_matrix` maps canonical layout coordinates into
  camera pixels. If `camera_to_layout_matrix` is emitted, it should be the
  inverse convenience transform for mapping detections back into canonical
  layout space.
- Consumers may draw or log `runtime.zones[*].geometry` directly, but should use
  `layout_id` + `zone_id` as the stable identity.
- For the immediate single-circle Citrus-fed path, `layout_id` may be
  synthesized from the imported Citrus canvas/config pair, and `zone_id` should
  remain the trivial stable identity `z0`.
- The current schema does not yet define an explicit `citrus_template_ref`. If
  downstream Citrus/H5 consumers need exact config/homography provenance in the
  snapshot, add that field before freezing the contract.
- For the broader design rationale and canonical artifact contract, see
  `docs/spatial_layout_contract.md`.
- For field-by-field spatial payload definitions, see
  `docs/spatial_layout_schema.md`.

Suggested single-circle Citrus-fed example:

```json
{
  "calibrations": {
    "2010093": {
      "dish_mask": {
        "calibration_ref": {
          "artifact_id": "dishmask_...",
          "artifact_schema_id": "orange.calibration.dish_mask",
          "artifact_schema_version": 1,
          "fingerprint": "fnv1a64:..."
        },
        "runtime": {
          "schema_version": 1,
          "enabled": true,
          "source": "imported",
          "geometry": {
            "coordinate_space": "camera_native_pixels",
            "outer_geometry": {
              "type": "circle",
              "cx": 1128.0,
              "cy": 1125.0,
              "r": 1004.0
            },
            "valid_geometry": {
              "type": "circle",
              "cx": 1128.0,
              "cy": 1125.0,
              "r": 992.0
            },
            "edge_margin_px": 12.0
          }
        }
      },
      "arena_layout": {
        "calibration_ref": {
          "artifact_id": "arenalayout_...",
          "artifact_schema_id": "orange.calibration.arena_layout",
          "artifact_schema_version": 1,
          "fingerprint": "fnv1a64:..."
        },
        "runtime": {
          "schema_version": 1,
          "enabled": true,
          "layout_id": "citrus_casper_arena_1",
          "coordinate_space": "camera_native_pixels",
          "registration": {
            "type": "similarity",
            "layout_coordinate_space": "layout_units",
            "source": "imported",
            "fit_point_count": 96,
            "residual_px": 2.1,
            "layout_to_camera_matrix": [
              0.893, 0.0, 121.4,
              0.0, 0.893, 118.2,
              0.0, 0.0, 1.0
            ]
          },
          "visible_zone_ids": ["z0"],
          "zones": [
            {
              "zone_id": "z0",
              "zone_index": 0,
              "visibility_status": "full",
              "geometry": {
                "type": "circle",
                "cx": 1128.0,
                "cy": 1125.0,
                "r": 1004.0
              }
            }
          ]
        }
      }
    }
  }
}
```

## PTP Sync Summary schema

Current emitted `ptp_sync_summary.json` shape:

```json
{
  "schema_version": 1,
  "recording_id": "YYYY_MM_DD_HH_MM_SS",
  "recording_folder": "/abs/path/to/recording",
  "created_at_utc": "YYYY-MM-DDTHH:MM:SSZ",
  "updated_at_utc": "YYYY-MM-DDTHH:MM:SSZ",
  "sync": { "...": "same session-level sync snapshot shape as recording_snapshot.json" },
  "cameras": {
    "02010093": {
      "camera_serial": "02010093",
      "camera_id": 0,
      "gpu_id": 1,
      "sync_camera_enabled": true,
      "finalized": true,
      "updated_at_utc": "YYYY-MM-DDTHH:MM:SSZ",
      "frame_count": 12345,
      "frame_count_semantics": "legacy_alias_for_acquisition_frames_received_total",
      "acquisition_frames_received_total": 12345,
      "sync_observed_frames_total": 12345,
      "sync_observed_frame_count_source": "successful_EVT_CameraGetFrame",
      "recording_frames_assigned_total": 12000,
      "last_recording_frame_id": 12000,
      "recording_ingress_submitted_frames": 12000,
      "encoded_frames_total": null,
      "encoded_frame_count_source": "not_available_in_ptp_sync_summary",
      "encoded_frame_count_authoritative_artifacts": {
        "metadata_csv": "Cam02010093_meta.csv",
        "keyframe_json": "Cam02010093_keyframe.json",
        "recording_session_json": "recording_session.json"
      },
      "frames_received": 12345,
      "dropped_frames": 0,
      "last_frame_timestamp_ns": 1234567890,
      "last_latched_ptp_time_ns": 1234567999,
      "ptp_offset_ns": {"samples": 20, "min": 500, "max": 900, "last": 700, "mean": 710.5},
      "latch_minus_frame_ns": {"samples": 20, "min": 9000000, "max": 9300000, "last": 9123456, "mean": 9160000.0},
      "frame_delta_ns": {"samples": 20, "min": 16666650, "max": 16666680, "last": 16666665, "mean": 16666665.2},
      "latch_delta_ns": {"samples": 20, "min": 16500000, "max": 16800000, "last": 16670000, "mean": 16666670.1},
      "delta_samples": 12344,
      "avg_frame_delta_ns_running": 16666665,
      "avg_latch_delta_ns_running": 16666670
    }
  }
}
```

Notes:
- Per-camera stat blocks are updated at low rate from the live acquisition loop.
- `finalized=true` indicates the last update written for that camera during the
  active recording folder lifetime.
- `frame_count` is a legacy alias for
  `acquisition_frames_received_total`. It is the count of successful
  `EVT_CameraGetFrame` returns observed by the acquisition loop for that camera
  stream, not an encoded-video frame count.
- `sync_observed_frames_total` is the same acquisition-frame population when
  camera sync/PTP is enabled. It can include frames outside the recording-local
  encoded video interval, such as stream pre-roll before recording is toggled on
  or frames observed while recording is stopping/finalizing.
- `recording_frames_assigned_total` / `last_recording_frame_id` are the
  recording-local IDs assigned while `record_video` is active. They are closer
  to the recording interval but still are not the authoritative encoded count.
- `encoded_frames_total` is intentionally `null` in this sidecar. Use
  `recording_session.json` camera artifacts, `Cam*_meta.csv` row count, or
  `Cam*_keyframe.json.total_frames` for encoded/video ingest counts.
- The summary is intended for session-level diagnostics and cross-camera timing
  comparisons, not for reconstructing exact per-frame order.

## Headless Experiment Run Metrics

Current headless experiment outputs (`runs.json` / `runs.csv`) also export
main-video artifact metrics derived from `Cam<serial>.mp4`:

- `importance_map_mode`
- `importance_map_roi_size_px`
- `importance_map_enabled`
- `importance_map_active_mode`
- `importance_map_block_size`
- `importance_map_grid_width`
- `importance_map_grid_height`
- `video_present`
- `video_path`
- `video_file_size_bytes`
- `video_duration_s`
- `video_achieved_bitrate_bps`
- `video_content_checked`
- `video_content_valid`
- `video_content_status`
- `video_first_frame_luma_mean`
- `video_first_frame_luma_stddev`
- `video_first_frame_black_fraction`
- `video_first_frame_decoded_bytes`

Notes:

- `importance_map_mode` is the requested headless mode; `static_roi` is the
  current plumbing-validation mode that emits a synthetic centered square
  delta-QP map.
- `importance_map_roi_size_px` is the requested square ROI size in pixels for
  `static_roi`; the current default is `512`.
- `importance_map_active_mode` reflects what the encoder snapshot actually used
  after initialization and should match the requested mode on a successful run.
- `video_duration_s` reflects the actual container duration, not the scored
  post-warmup window.
- In current headless runs, recorded file duration includes warmup because
  recording starts immediately and warmup is applied only during evaluation.
- `video_achieved_bitrate_bps` reflects the written MP4 output and may differ
  from requested bitrate overrides or resolved NVENC target bitrate settings.
- `video_content_*` fields summarize the first decoded full-frame video frame.
  They are intended to catch invalid black/blank output that can otherwise pass
  throughput counters.

## Headless Recording Session Manifest

Headless experiment specs that use `fixed.recording_control` write:

```text
<recording_folder>/recording_session.json
```

Current schema:

```json
{
  "schema_id": "orange.recording_session",
  "schema_version": 1,
  "producer": "orange_headless",
  "mode": "single_clip",
  "status": "completed",
  "recording_folder": "/abs/path/to/run",
  "cameras": ["2010096"],
  "camera_artifacts": {
    "2010096": {
      "video": "Cam2010096.mp4",
      "metadata": "Cam2010096_meta.csv",
      "keyframes": "Cam2010096_keyframe.json",
      "frame_count": 600,
      "first_recording_frame_id": 1,
      "last_recording_frame_id": 600,
      "recording_frame_id_gaps": 0,
      "packet_count": 600,
      "packet_count_source": "ffprobe_nb_read_packets"
    }
  },
  "recording_outputs": {
    "2010096": {
      "full": {
        "schema_version": 1,
        "camera_serial": "2010096",
        "output_kind": "full",
        "role": "ingest_authoritative",
        "backend": "in_process",
        "status": "completed",
        "video": "Cam2010096.mp4",
        "metadata": "Cam2010096_meta.csv",
        "keyframes": "Cam2010096_keyframe.json",
        "container": "mp4",
        "coordinate_space": "full_frame_pixels",
        "frame_count": 600,
        "packet_count": 600,
        "packet_count_source": "ffprobe_nb_read_packets"
      }
    }
  },
  "stream": {
    "requested_duration_seconds": 20,
    "actual_elapsed_s": 20.1
  },
  "recording_control": {
    "record_for_seconds": 6,
    "clip_seconds": 0
  },
  "recording": {
    "started": true,
    "stop_reason": "record_for_seconds_elapsed",
    "control": {
      "source": "orange_gui_local_control",
      "method": "stop_recording",
      "request_id": "run-42:orange:stop_recording",
      "operation_id": "run-42"
    },
    "drain_completed": true,
    "actual_recording_duration_s": 6.0,
    "drain_duration_s": 0.1
  },
  "clips": [
    {
      "clip_index": 0,
      "clip_id": "clip_0000",
      "requested_duration_s": 6,
      "actual_duration_s": 6.0,
      "timed_stop_hit": true,
      "drain_completed": true,
      "artifacts": {
        "videos": {
          "2010096": "Cam2010096.mp4"
        },
        "metadata": {
          "2010096": "Cam2010096_meta.csv"
        },
        "keyframes": {
          "2010096": "Cam2010096_keyframe.json"
        }
      },
      "recording_outputs": {
        "2010096": {
          "full": {
            "...": "same descriptor shape as session-level recording_outputs"
          }
        }
      }
    }
  ]
}
```

Notes:

- The broader session/clip contract is documented in
  `docs/recording_session_manifest_contract.md`.
- The manifest is built by the shared `src/session/recording_session.*` helper
  so headless, GUI/session, and future external-recorder paths can converge on
  one `orange.recording_session` contract.
- GUI in-process recordings now use the shared helper too. After the recording
  drain completes, the GUI writes `recording_session.json` with
  `producer = "orange_gui"` and updates `recording_snapshot.json` with
  `session.recording_mode = "single_clip"` plus
  `session.recording_session_manifest_path`.
- GUI external IPC recordings use the same `single_clip` manifest shape with
  `producer = "orange_gui_external_ipc"` and
  `recording_backend.mode = "external_ipc"`. In that layout,
  `camera_artifacts.<serial>.video` points at
  `external_recorder/Cam<serial>_external.mp4`, and
  `camera_artifacts.<serial>.metadata` points at the external recorder summary
  JSON instead of a per-frame `Cam*_meta.csv`. Consumers should compare
  `camera_artifacts.<serial>.frame_count` to external summary fields such as
  `frames_received`, `acks_sent`, and `frames_encoded`.
- If the supervised GUI full-frame external recorder contract requests
  `clip_seconds > 0`, GUI finalization writes an
  `orange_gui_external_ipc` `rolling_clips` manifest instead of the single-clip
  shape. The GUI mirrors external `rolling_output.clips[]` into per-clip
  `clip_manifest.json`, `recording_clip_index.json/csv`, and
  `recording_snapshot.json` session pointers. The external recorder summaries
  remain the per-stream truth for encoded clip counts and packet counts.
- GUI external crop recordings add `recording_backend.crop_recording` and keep
  the crop output under `recording_outputs[serial].crop`. The crop backend
  block includes per-camera maps for recorder `stream_config`,
  `frames_received`, `frames_encoded`, `encode_dropped`,
  `external_frames_dropped`, `encode_queue_depth`, `encode_queue_high_water`,
  and `enqueue_age_p95_ms`, copied from the supervised external crop recorder
  plan and summaries. In `stream_config`, `analytics_gpu_id` is the
  source/crop-production GPU and `recorder_gpu_id` is the external process
  encode GPU; they can differ when crop recorder GPU placement is intentionally
  overridden for NVENC load-routing diagnostics.
- GUI external crop recorder contracts now declare `require_status = true` and
  `require_status_runtime = true`, matching the full-frame supervised external
  recorder health contract. Strict GUI validation checks the crop recorder
  status sidecar, parsed supervisor runtime status, and summary count agreement.
- GUI external crop recorder artifacts also declare their current rollover
  state explicitly. With `clip_seconds = 0`, crop outputs remain single-clip
  sidecars with `rollover.status = "not_requested"`. When GUI external
  full-frame rolling is active, crop external IPC receives the same
  `recording_control`, declares
  `rollover.implementation =
  "external_recorder_gop_boundary_writer_rotation"`, keeps crop GOP size `1`,
  and uses a terminal-tail coalesce window matched to the full-frame GOP so
  crop clips do not create orphan final-tail clips. GUI finalization splits the
  root Orange-written
  `Cam*_crop_meta.csv` and `Cam*_crop_perf.csv` into per-clip crop sidecars by
  continuous `recording_frame_id` ranges and records those paths under
  `recording_backend.crop_recording.rolling_clips` and each rolling clip's crop
  `recording_outputs` entry. The top-level
  `recording_outputs[serial].crop` descriptor is also finalized as an
  external-IPC session aggregate for the merged crop MP4 and root crop CSVs,
  with `details.scope = "session_aggregate"` and the recorder `stream_id`,
  socket, GPU placement, queue depth, summary, and status paths. Offline GUI
  validation now checks the per-clip crop sidecars and encoded crop videos; live
  GUI soak coverage is still pending.
- GUI recordings also update `recording_snapshot.json`
  `session.gui_display_frame_rate` after finalization. This is GUI display
  telemetry from ImGui delta time, split into `overall`,
  `crop_preview_visible`, and `crop_preview_hidden` buckets with
  `sample_count`, `min_fps`, `p05_fps`, `p50_fps`, `p95_fps`, `max_fps`, and
  `mean_fps`. It is intended to prove whether crop preview windows affected UI
  refresh during a recording; it is not camera acquisition FPS. Newer snapshots
  also include `session.gui_display_frame_rate.timings` for frame-total,
  texture-upload, camera/crop-window draw, speed-graph draw, and render/present
  timing buckets. `scripts/validate_gui_ptp_recording.py --json-out` and
  `scripts/summarize_gui_validation.py --json` derive a dominant-p95 GUI timing
  diagnosis from those buckets; that diagnosis is script output, not a persisted
  snapshot field. Runs after the ImGui GLFW size-cache shim also include
  `session.gui_display_frame_rate.imgui_glfw_size_cache` with recording-scoped
  cache-hit, fallback, and null-window counters for the backend display-size
  path.
- `clip_seconds = 0` means no rollover and keeps the current flat folder
  layout.
- `clip_seconds > 0` is implemented for headless experimental specs as
  seamless GOP-boundary rolling clips. Each clip writes a
  `clip_manifest.json` plus per-camera MP4/metadata/keyframe files under
  `clips/clip_000000`, `clips/clip_000001`, etc.
- The next clip writer is preopened and the active writer switches at a GOP
  first-frame boundary. Each new clip starts with an IDR/SPS/PPS picture and
  keyframe frame `0`.
- Rolling clip metadata keeps `frame_id` session-continuous across clips, while
  each MP4 uses a clip-local timeline starting at zero.
- Rolling sessions also write `recording_clip_index.json` and
  `recording_clip_index.csv` in the parent recording folder. These are
  session-level `(clip, camera)` indexes with status, rollover reason, frame
  range/count, artifact paths, and `clip_manifest.json` pointers, so consumers
  do not need to walk every clip directory.
- Native in-process rolling indexes include real packet counts measured after
  finalization with ffprobe (`ffprobe_nb_read_packets`). Supervised external
  IPC rolling indexes use recorder summary `packets_written`
  (`external_recorder_summary.packets_written`).
- `recording_snapshot.json` records `session.recording_mode =
  "rolling_clips"`, `session.recording_session_manifest_path`, and
  `session.recording_session_index` with absolute paths to the index JSON/CSV.
  The latest-recording pointer also includes `recording_session_manifest_path`.
- For `fixed.recording_control`, the recording-duration clock is anchored to
  the first observed recording frame, not camera-thread launch. This keeps PTP
  gate startup countdown time from shortening the requested video duration.
- GOP-boundary alignment can make individual clip durations vary by up to one
  GOP; consumers should use continuous `frame_id` coverage and total ffprobe
  duration for whole-recording validation.
- The manifest records the control-plane timing of the requested stop/drain.
  Consumers should use each `Cam<serial>.mp4` container duration when they need
  exact encoded media duration.
- Use `scripts/verify_timed_recording.py <experiment_root>` to check the
  current timed-recording contract against `recording_session.json`,
  `recording_clip_index.{json,csv}`, `recording_snapshot.json`, `runs.json`,
  and `ffprobe`.

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
- `crop`: crop recording encoder (for example configurable square
  detection/pose-driven ROI path).

Compatibility rule:

- During migration, producers may still emit legacy shape:
  - `encoders[serial] = <encoder_info>`
- Consumers should support both shapes:
  - if `encoders[serial].outputs` exists, use that;
  - otherwise treat `encoders[serial]` as the `full` encoder entry.

`pipeline_metrics` is an optional dictionary keyed by camera serial number (as a
string). Each value captures a condensed per-recording summary of the live
pipeline telemetry written by acquisition, plus a link to the corresponding
per-camera periodic CSV artifact.

Current emitted shape:

```json
{
  "pipeline_metrics": {
    "02010093": {
      "schema_version": 1,
      "camera_serial": "02010093",
      "camera_id": 0,
      "gpu_id": 1,
      "gpu": {
        "id": 1,
        "name": "NVIDIA RTX A6000",
        "pci_bus_id": "0000:65:00.0"
      },
      "updated_at_utc": "YYYY-MM-DDTHH:MM:SSZ",
      "artifact_path": "/abs/path/to/recording/Cam02010093_pipeline_perf.csv",
      "period_seconds": 1,
      "samples": 57,
      "finalized": true,
      "last_sample_at_utc": "YYYY-MM-DDTHH:MM:SSZ",
      "last_frame_id": 12345,
      "last_recording_frame_id": 12000,
      "fps": {
        "acquisition": {"samples": 57, "min": 58.9, "max": 60.1, "last": 60.0, "mean": 59.8},
        "preprocess": {"samples": 57, "min": 58.1, "max": 60.0, "last": 59.7, "mean": 59.1},
        "encode": {"samples": 57, "min": 54.3, "max": 60.0, "last": 58.8, "mean": 57.9}
      },
      "queue_depth": {
        "display": {"samples": 57, "min": 0, "max": 2, "last": 0, "mean": 0.3},
        "yolo": {"samples": 57, "min": -1, "max": -1, "last": -1, "mean": -1.0},
        "preprocess": {"samples": 57, "min": 0, "max": 8, "last": 2, "mean": 2.1},
        "encode": {"samples": 57, "min": 0, "max": 4, "last": 1, "mean": 1.2},
        "pending_requeues": {"samples": 57, "min": 0, "max": 3, "last": 0, "mean": 0.1}
      },
      "resource_availability": {
        "acquire_entries": {"samples": 57, "min": 18, "max": 32, "last": 29, "mean": 27.6},
        "acquire_entries_low_watermark": {"samples": 57, "min": 12, "max": 30, "last": 21, "mean": 20.4},
        "acquire_events": {"samples": 57, "min": 20, "max": 64, "last": 59, "mean": 51.0},
        "acquire_events_low_watermark": {"samples": 57, "min": 14, "max": 60, "last": 42, "mean": 39.7},
        "yolo_events": {"samples": 57, "min": 64, "max": 64, "last": 64, "mean": 64.0},
        "yolo_events_low_watermark": {"samples": 57, "min": 64, "max": 64, "last": 64, "mean": 64.0},
        "preprocess_buffers": {"samples": 57, "min": 85, "max": 120, "last": 112, "mean": 108.7},
        "preprocess_events": {"samples": 57, "min": 85, "max": 120, "last": 112, "mean": 108.7}
      },
      "totals": {
        "acquisition_resource_starvations": 0,
        "preprocess_resource_waits": 0,
        "preprocess_frames_dropped": 0,
        "encode_failures": 0,
        "encode_slow_frames": 3,
        "gpu_direct_frames": 0,
        "gpu_ring_copy_frames": 57,
        "gpu_copy_frames": 0
      }
    }
  }
}
```

Notes:
- Stats objects use the same `{samples,min,max,last,mean}` shape already used by
  other summary sidecars.
- `artifact_path` points at the per-camera periodic CSV artifact for that
  recording.
- The summary is finalized when the acquisition worker rotates away from the
  recording folder or shuts down.

`models` is an optional dictionary keyed by camera serial number (as a string).
Each value captures resolved runtime model metadata (for example detect TRT model
and pose TRT model/skeleton) so downstream consumers can reproduce interpretation
of outputs.

Per-output `encoder_info` should include at least:

- current encoder fields (`backend`, `path`, `codec`, `preset`, `tuning`,
  `resolution`, `fps`, GOP/RC fields).
- source/output geometry detail:
  - `source_resolution`
  - `output.mode`
  - `output.resize_enabled`
  - `output.downsample_factor` or `output.requested_output_size`
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

`models[serial].detect` captures the resolved detect runtime when the GUI starts
a recording. Current GUI fields include:

- `enabled`
- source provenance (`camera_config_path` and/or UI-driven selection metadata)
- runtime identifiers:
  - `worker`, `backend`, `engine_path`, `model_id`, `gpu_id`

Future fields should add:

  - `engine_sha256`
  - optional class mapping identifiers (for example `classes_path`,
    `classes_sha256`, `label_space`)
  - input/output interpretation fields used by downstream consumers

Validation note:

- The GUI YOLO smoke artifact
  `/home/jeremy/orange_data/exp/unsorted/2026_04_22_15_07_45` confirmed that
  the snapshot records `models[2010096].detect.enabled=true` with worker
  `YoloWorker`, backend `tensorrt`, engine path, model id, and GPU id.
- The same snapshot records `models[2010095].detect.enabled=false` with backend
  `none`, which is useful for consumers distinguishing disabled model state from
  missing metadata.
- The GUI YOLO + crop smoke artifact
  `/home/jeremy/orange_data/exp/unsorted/2026_04_22_21_47_28` confirmed that
  `recording_snapshot.json` captured the runtime camera
  `crop_pipeline.crop_size_px = 328`, matching the persisted camera config and
  the `328x328` crop video dimensions.
- Current GUI crop recordings also write `crop_outputs[serial]` with crop
  enablement, effective geometry, codec/container, selection/blank-frame policy,
  and expected crop artifact file names. Consumers should use this block as the
  legacy crop-output contract and cross-check it against the crop video, crop
  metadata CSV, and crop perf CSV. Newer schema-2 snapshots also mirror the same
  crop output under `recording_outputs[serial].crop` and
  `encoders[serial].outputs.crop`.
- The GUI YOLO + crop observability smoke artifact
  `/home/jeremy/orange_data/exp/unsorted/2026_04_22_22_53_43` confirmed
  `crop_outputs[2010096]` matched the emitted crop artifacts, including
  `Cam2010096_crop_keyframe.json` and `Cam2010096_crop_perf.csv`.

Crop output snapshot shape:

```json
{
  "crop_outputs": {
    "02010093": {
      "schema_version": 1,
      "enabled": true,
      "mode": "yolo_centered_square",
      "source": {
        "ui_selected": true,
        "requires_yolo": true,
        "requires_recording": true,
        "camera_config_path": "/abs/path/config/local/02010093.json"
      },
      "runtime": {
        "worker": "CropAndEncodeWorker",
        "source_gpu_id": 5,
        "crop_size_px": 328,
        "preview_max_fps": 15,
        "crop_frame_pool_size": 128,
        "width": 328,
        "height": 328,
        "coordinate_space": "full_frame_pixels",
        "selection_policy": "largest_detection_by_confidence",
        "blank_frame_policy": "encode_black_frame_when_no_detection",
        "codec": "hevc",
        "container": "mp4",
        "tuning": "lossless",
        "frame_rate": 100,
        "files": {
          "video": "Cam02010093_crop.mp4",
          "metadata": "Cam02010093_crop_meta.csv",
          "keyframes": "Cam02010093_crop_keyframe.json",
          "perf": "Cam02010093_crop_perf.csv",
          "sidecar_perf": "Cam02010093_crop_sidecar_perf.csv"
        }
      }
    }
  }
}
```

`runtime.crop_frame_pool_size` records the effective crop-frame pool size used
by the GUI process for that run. It is the default crop producer pool unless
`ORANGE_CROP_FRAME_POOL_SIZE` is set by the launcher or operator; GUI external
crop IPC validation currently auto-sizes this above the external crop encode
queue depth.

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
          "worker": "YoloWorker",
          "backend": "tensorrt",
          "engine_path": "/abs/path/models/fish_jinyao.engine",
          "model_id": "fish_jinyao",
          "gpu_id": 5,
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
      "gpu": {
        "id": 0,
        "name": "NVIDIA RTX A6000",
        "pci_bus_id": "0000:65:00.0"
      },
      "color": true,
      "resolution": {"width": 2256, "height": 2256},
      "source_resolution": {"width": 4512, "height": 4512},
      "output": {
        "mode": "factor",
        "resize_enabled": true,
        "resolved_resolution": {"width": 2256, "height": 2256},
        "downsample_factor": 2
      },
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
        "vbv_buffer_size": 250000000,
        "qp_map_mode": {"value": 2, "name": "delta"},
        "multi_pass": {"value": 0, "name": "disabled"}
      },
      "aq": {"enable_aq": 1, "enable_temporal_aq": 1},
      "requested_overrides": {
        "aq": -1,
        "temporal_aq": -1,
        "lookahead": -1,
        "lookahead_depth": -1,
        "target_bitrate_bps": -1,
        "max_bitrate_bps": -1,
        "vbv_buffer_size": -1,
        "importance_map_mode": "static_roi",
        "importance_map_roi_size_px": 512
      },
      "importance_map": {
        "requested_mode": "static_roi",
        "active_mode": "static_roi",
        "enabled": true,
        "shape": "square",
        "roi_size_px": 512,
        "block_size": 32,
        "grid_width": 71,
        "grid_height": 71,
        "inside_delta_qp": -3,
        "outside_delta_qp": 3,
        "qp_map_size_bytes": 5041
      },
      "lookahead": {"enable": 0, "depth": 0},
      "low_delay_keyframe_scale": 1,
      "strict_gop_target": 0,
      "enable_non_ref_p": 0,
      "repeat_sps_pps": 1,
      "enable_ptd": 1,
      "resolved_config": {
        "initialize": {
          "frame_rate_num": 60,
          "frame_rate_den": 1,
          "enable_ptd": 1,
          "enable_weighted_prediction": 0,
          "enable_output_in_vidmem": 0,
          "max_encode_width": 2256,
          "max_encode_height": 2256,
          "tuning_info": {"value": 2, "name": "low_latency"}
        },
        "buffers": {
          "encoder_buffer_count": 4,
          "encoder_input_pitch": 2304,
          "direct_input_enabled": false
        },
        "common": {
          "gop_length": 120,
          "frame_interval_p": 1,
          "mono_chrome_encoding": 0
        },
        "rc": {
          "mode": "vbr",
          "mode_value": 1,
          "average_bitrate": 244297728,
          "max_bitrate": 250000000,
          "vbv_buffer_size": 250000000,
          "target_quality": 0,
          "target_quality_lsb": 0,
          "enable_aq": 1,
          "enable_temporal_aq": 1,
          "enable_lookahead": 0,
          "lookahead_depth": 0,
          "multi_pass": {"value": 0, "name": "disabled"},
          "low_delay_keyframe_scale": 1
        },
        "codec": {
          "name": "hevc",
          "idr_period": 120,
          "max_num_ref_frames_in_dpb": 1,
          "repeat_sps_pps": 1
        }
      }
    }
  }
}
```

For camera config schema 4 and newer, `recording.encode.aq` and
`recording.encode.temporal_aq` are persistent tri-state encoder config fields:
`auto`, `off`, or `on`. The resolved values are reflected in the encoder
snapshot under `aq.enable_aq` and `aq.enable_temporal_aq`; `requested_overrides`
records the effective request after combining camera config and runtime
experiment overrides.

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
