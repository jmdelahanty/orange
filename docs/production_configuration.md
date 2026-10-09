# Production configuration review (2026-10-05)

Question: are the best-known settings on by default, and are they recorded as
configured parameters rather than left to code defaults? Sources compared:
the validated production shape (`AGENTS.md`, 30-minute fish endurance gate of
2026-09-21 on the headless spec
`experiment_specs/fourcam_fused_recorder_realfish_int8_192_native_ownerpush_ring_od8ff_s16*.json`),
the host app config `~/orange_data/config/app/default.json`, the code defaults
(`src/project.cpp` loader, `src/recording_ingress.cpp`, `src/FFmpegWriter.h`,
`tools/external_recorder_ipc_probe.cpp`), and the resolved flags sealed in the
GUI recordings under `~/orange_data/exp/unsorted`.

Evidence runs with the GUI shape below: ten 10-minute soaks (2026-09-22/23),
the 60-minute recording `2026_09_23_13_58_31` (360,005 frames received =
encoded per full-frame recorder, 0 `encode_dropped`, 0 camera frame-id gaps,
0 ingress rejections on all four cameras), and the 2026-10-01 fish run
`2026_10_01_21_07_02` (NPP `_Ctx` build, 0 gaps, detect p95 1.84 ms). Every
one of them ran with `fused_frame`, `device_roi` and `device_crop` on.

## Verdict

Everything the gate validated is on for the GUI, and since this review every
one of those values is written in the app config (not inherited from a code
default) or lives in the camera JSON / host config named below. Two changes
were made: `recording.external_ipc.owner_push_chunk_bytes` and
`recording.external_ipc.mp4_writeback_pace_bytes` are new app-config keys
(bridge to `ORANGE_EXTERNAL_RECORDER_OWNER_PUSH_CHUNK_BYTES` and
`ORANGE_MP4_WRITEBACK_PACE_BYTES`), set to the values the GUI soaks used. One
difference between the headless gate and the GUI remains, flagged below
(owner-push chunk size).

## Recorder shape (full frame, two shards, split GOP)

| Parameter | Validated | App config key (value) | Code default | Headless gate spec |
| --- | --- | --- | --- | --- |
| External recorder sink | external_ipc | `recording.sink_mode` = `external_ipc` | app config, else camera preference, else real | spec contract |
| In-process recorder refused | policy on | `recording.require_external_ipc` = true (GUI; exported as `ORANGE_REQUIRE_EXTERNAL_IPC=1`); spec `require_external_ipc` true | off (a bad sink-mode string silently fell back to the in-process writer) | spec key |
| Owner push (per-card serialization + deadline gate) | on | `recording.external_ipc.owner_push` = true | off | `external_recorder_owner_push` true |
| Owner-push slots | 16 | `owner_push_slots` = 16 | 8 | 16 |
| Owner-push chunk bytes | GUI 2 MiB; headless gate 64 MiB | `owner_push_chunk_bytes` = 2097152 (new) | 2 MiB | `external_recorder_owner_push_chunk_bytes` 67108864 |
| Owner-push freshness / deadline / transfer allowance | 8 / 10 / 3.6 ms | none (env only) | 8.0 / 10.0 / 3.6 | spec does not set them (defaults) |
| Full-frame `extra_output_delay` | 8 | `full_frame_extra_output_delay` = 8 | recorder default | 8 |
| Native NV12 array input (CUDA 13 recorder) | on | `native_local_input` = true | off | `external_recorder_native_local_input` true |
| Recorder binary | native build | `recorder_tool_path` = `/opt/orange/bin/external_recorder_ipc_probe_native` | sibling binary (CUDA 12) | same path |
| MP4 writeback pacing | `sync_file_range` every 32 MiB | `mp4_writeback_pace_bytes` = 33554432 (new) | 32 MiB on (`FFmpegWriter.h`, shared by the recorder binaries) | default |
| Registered NV12 source in the recorder | on | none (recorder default) | on since 2026-09-04 (`ORANGE_EXTERNAL_RECORDER_REGISTERED_SOURCE=0` disables) | `external_recorder_registered_source` true |
| Recorder direct input | off | none | off | false |
| Recorder detect priority | on | none | on | `external_recorder_detect_priority` true |
| Full-frame encode | HEVC p1 ll vbr q20 GOP 25 150 Mb/s, queue 32, prewarm 4 slots + peer copy | camera JSON `recording.*` (sealed per recording in `cameras[serial]`) | camera JSON | identical in the spec contract |
| PREPARE/PREPARED strict readiness gate | strict | none (`ORANGE_GUI_RECORDER_PREPARE_STRICT`) | strict | n/a (GUI only) |
| Owner-push ready timeout | 6 s | none (`ORANGE_GUI_OWNER_PUSH_READY_TIMEOUT_S`) | 6.0 | n/a |
| PTP register-read decimation | 1/100 | `recording.ptp_register_read_decimate` = 100 | 1 | 100 |
| PTP latch after fan-out | on | none | on | `ptp_latch_after_fanout` true |

Crop recorders: `recording.crop.sink_mode` = external_ipc, `frame_pool_size`
256, `external_ipc.encode_queue_depth` 128, `recorder_gpu_ids_by_serial`
2010093→4, 2010094→2, 2010095→8, 2010096→6 (the paired die),
`interleave` true (GOP-parity two shards). Crop recorders keep the default
extra output delay and run on the same CUDA 13 native binary as the
full-frame recorders (since 2026-10-08; the CUDA 12 recorder build is retired).

**Owner-push chunk size.** The 30-minute headless endurance pushed each
20 MB frame as one 64 MiB-bounded chunk; every GUI soak pushed 2 MiB chunks
(the client default, since the wrapper allow-list cannot set the variable).
Both pass their gates; the GUI value is kept, now written explicitly, until a
GUI A/B says otherwise. `Cam*_owner_push.csv` records per-push wait and age
either way.

## Analytics shape (fused per-slot graph)

| Parameter | Validated | App config key (value) | Code default |
| --- | --- | --- | --- |
| Device ROI | on | `analytics.device_roi` = true | off |
| Device crop (384 px) | on | `analytics.device_crop` = true | off |
| Fused per-slot graph | on | `analytics.fused_frame` = true (resolved 2026-09-23 after the preview PBO fix; `AGENTS.md` had said `false`) | off |
| Copy after pose | on | `analytics.copy_after_pose` = true | on |
| Early owned frame | on | `analytics.early_owned_frame` = true | on |
| Copy stream | off | `analytics.copy_stream` = false | off |
| YOLO sync event / GPU timing / detach input / ready-event fast path | on | `analytics.yolo.*` = true | on |
| YOLO decimate / prewarm | 1 / 3 | `analytics.yolo.decimate` = 1, `prewarm_iterations` = 3 | 1 / 3 |
| Detect engine | INT8 min-max `..._int8mm_bo5_avg32.engine` | `models.default_detect_engine` | none |
| Pose | 192 px head model fp16, device stage + device graph, 8 slots, queue 32, prewarm 3 | `models.pose_*`, `analytics.pose.*` | device stage on, graph on, slots 8, queue 32, prewarm 0 |
| CPU results path (postprocess, tracking, IPC) | never skipped | none (`ORANGE_YOLO_SKIP_CPU_RESULTS` stays unset) | on |

The INT8 min-max detect engine (chosen 2026-09-18 over entropy calibration,
which blinds the detector) has been the configured engine in every GUI
recording since 2026-09-23, including the 2026-10-01 fish run.

## GUI display and gates

| Parameter | Validated | App config key (value) | Code default |
| --- | --- | --- | --- |
| Display profile | citrus_safe (preview 10 fps, swap 1, frame 30) | `gui.display.profile` = citrus_safe, plus the three explicit keys | default profile |
| Preview source downsample on the acquisition GPU | on | none (`ORANGE_DISPLAY_SOURCE_DOWNSAMPLE=0` restores the old path) | on |
| Skip pushed GOPs in the preview | 0 (off since the downsample fix) | `gui.display.skip_pushed_gops` = 0 | 0 |
| Pose overlay | on | `gui.display.pose_overlay` = true | off |
| Stream downsample | 4 | `gui.stream.downsample` = 4 | 4 |
| Optical exposure check with iris re-home | on, ±15 %, expected means per serial | `gui.exposure_check.*` | off |
| Local control | Citrus completion stop only, drain 60 s | `gui.local_control.*` | all off |
| Readiness gate: recorders prepared, crop recorders prepared before enable | strict | code (`ORANGE_GUI_RECORDER_PREPARE_STRICT` default strict) | strict |

## Host settings (outside the app)

| Setting | Validated | Where | State 2026-10-05 |
| --- | --- | --- | --- |
| `vm.dirty_background_bytes` / `vm.dirty_bytes` | 64 MB / 512 MB | `/etc/sysctl.d/90-orange-writeback.conf` (from `config/sysctl/`) | installed and live (sysctl reads 67108864 / 536870912) |
| `vm.compaction_proactiveness` | 0 | same file | installed |
| Clocksource | tsc (`tsc=reliable`) | kernel cmdline | `current_clocksource` = tsc |
| `isolcpus/nohz_full`, `iommu=pt` | per `AGENTS.md` | kernel cmdline | unchanged since 2026-09-19 |
| Recorder binary | CUDA 13.1 native | `/opt/orange/bin/external_recorder_ipc_probe_native` | installed 2026-10-01 |

## How to verify a run used this shape

Each recording's sealed `recording_snapshot_start.json` holds
`session.effective_configuration` (app config path, sha256 and contents;
every `ORANGE_*` variable with its source) and
`session.yolo_worker.runtime_flags` (the resolved analytics flags).
`scripts/validate_gui_ptp_recording.py --latest-complete` prints both.

## Still env-only (documented, not configurable in the app config)

Owner-push freshness, deadline, transfer allowance; recorder deferred release
and ack timeout; NVENC extra-output-delay and harvest knobs of the in-process
encoder; PTP latch after fan-out; spatial mask mode; YOLO stream priority,
affinity and RT priority; Citrus binding mode, socket and timeout; prepare
strictness; owner-push ready timeout. Each has its validated value as the code
default, and the sealed effective configuration shows when one was overridden.


## Recording root, clip length and full-frame bitrate (2026-10-09)

- Recording root: `/mnt/Data1/orange_data/exp/unsorted` (app config
  `storage.default_recording_root`, spec `fixed.output_root`). Data1 is the
  NVMe with controller serial ending 297 (6.7 TB free), on its own x4 root
  port of host bridge 20; the OS drive (`/`, where `~/orange_data` lives)
  stays for configs, engines and tools. Identify drives by serial or
  filesystem UUID, never by `nvmeN` (the numbering changed at the
  2026-10-06 reboot).
- Clip length (Jeremy): 15 min at 100 fps, 30 min at 60 fps or below. The
  Palette-ready specs carry `recording_control.clip_seconds 900` and the app
  config `recording.recording_control.clip_seconds 900`. Since 2026-10-09
  rolling clips no longer need a timed recording: an open-ended GUI
  recording rolls every `clip_seconds` until it is stopped (the panel's
  checkbox default is 900 s; `record_for_seconds` stays the optional timed
  limit). Proven by the GUI validation `2026_10_09_02_28_15` (60 s clips,
  autorun stop at 150 s: 3 clips, final stop reason autorun_stop, status
  completed, 0 gaps, NIC unchanged, strict validator PASS).
- Full-frame bitrate default is 45 Mbit/s average / 60 Mbit/s max since
  2026-10-09 (Jeremy, after the A/B below): the Palette-ready specs, the
  supervisor and plan defaults and the contract fallback for camera configs
  that say "auto" (`src/external_recorder_contract_utils.cpp`). The latency
  gate specs (`..._od8ff_s16*`) keep 150/150 as the historical baseline.
- Full-frame bitrate A/B (spec `..._rolling_crops_shadow_ff45`, 45 Mbit/s
  average / 60 Mbit/s max, crops unchanged and lossless): measured 53.9
  Mbit/s against 150.2; PSNR / SSIM of the 384 px fish window cut from the
  full frame against the lossless crop of the same frame (first 1000
  frames, every 25th, `scripts/compare_full_frame_vs_lossless_crop.py`):
  33.3 to 35.0 dB and 0.947 to 0.991 at 150 Mbit/s, 33.3 to 34.8 dB and
  0.947 to 0.983 at 45/60; the montage of the windows is indistinguishable
  by eye. Capacity: four cameras at 45/60 plus lossless crops is about
  5.5 TB per day (fits Data1), against about 9.4 TB at 150. A clean 45/60
  run passed every gate with NIC counters unchanged
  (`..._ff45_20261009_013649`); an earlier 45/60 run lost frames on card A
  while an ffmpeg decode of the previous run was running on the host, which
  is the same rule as for builds: nothing heavy on the host during a run.
