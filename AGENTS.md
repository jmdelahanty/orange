# Agent Notes

## Current Branch Context

- Integration worktree: `/home/jeremy/orange-integration-20260921`, branch
  `integration/device-roi-into-shaman-20260921` = production line
  `agent/acquisition/shaman-v2-authoritative-20260824` + the analytics line
  `agent/analytics/device-roi-20260912` (merged 2026-09-21) + GUI parity for
  the validated recorder shape. Plan and gates:
  `docs/device_roi_merge_plan_2026_09_21.md`.
- The shared worktree `/home/jeremy/orange-gop-split-a16` belongs to the GUI
  registration session; do not edit it from an integration session.
- Running journal (Claude Doc, every explanation and result since
  2026-09-16): see the memory note "Pose journal doc"; add an entry per
  session.
- Do not touch or commit untracked build directories (`targets/`,
  `build-*`).

## Rig (pancake0)

- Four EVT HB-20000SBM (Sony IMX531, 2.74 um pixels), 4512 x 4512 Mono8 at
  100 fps, 100 us exposure, PTP-gated (`sync_mode = "ptp_gate"`), one camera
  per 25G Mellanox port, GPUDirect into one A16 die per camera. Only camera
  2010096 drives the IR illuminators, so a lit capture must be PTP-gated.
- Two NVIDIA A16 cards (dies 1-4 = card A, 5-8 = card B), RTX A6000 on GPU 0.
  Host bridges: 00 A6000; 20 card A plus all three NVMe drives; 40 card B
  plus the mlnx2 NIC (cameras 95/96); 60 the mlnx1 NIC (cameras 93/94).
- The SDK's GPUDirect ring holds 24 frames (240 ms); camera frame loss is
  NIC-side back-pressure into the landing card (`rx_discards_phy` and
  `tx_global_pause` on the port), never an empty ring.
- Host settings that matter: `isolcpus/nohz_full` for the hot threads,
  `iommu=pt`, `tsc=reliable`, and `/etc/sysctl.d/90-orange-writeback.conf`
  (`vm.dirty_background_bytes 64 MB`, `vm.dirty_bytes 512 MB`; without it the
  kernel flushes ~3 GB of recorder pages every 31 s at 6 GB/s on bridge 20
  and costs camera frames).
- Passwordless sudo exists only for the wrappers
  `/usr/local/bin/orange-local-benchmark`, `orange-gui-validation`,
  `orange-evt-stream-smoke`; they allow specific binaries and spec
  directories (reinstall with `scripts/install_*_wrapper.sh`). Any other sudo
  command is run by the user.

## Production shape (validated 2026-09-21)

Per camera: acquisition -> fused per-slot CUDA graph (preprocess, INT8
detect, device ROI, 384 px crop, 192 px TensorRT pose, pool copy) on the
landing die -> external full-frame split-GOP recorder (two shards: native
NV12 array input on the landing die, owner-pushed staging slots on the
paired die) and an external crop recorder. Recorder knobs the gate used:
owner push with per-card serialization and the deadline gate, 16 push
slots, full-frame `extra_output_delay 8` (crop recorders keep the default),
native local input from the CUDA 13 recorder build, MP4 writeback pacing
(`sync_file_range` every 32 MB, default on).

Headless spec: `experiment_specs/..._native_ownerpush_ring_od8ff_s16*.json`
(60 s round and `_endurance30`, 30 minutes). GUI: the same shape comes from
`~/orange_data/config/app/default.json` `recording.external_ipc` (owner
push, slots, delay, native input, recorder tool path); env overrides win.

Gate result (30 min, four cameras, real fish, everything recording):
0 camera frame-id gaps, NIC counters unchanged, 179,901 frames received =
encoded per full-frame recorder, 0 recorder drops, every crop encoded,
detect p95 1.85 / p99 1.88 ms flat over the session, capture-to-pose-done
p95 2.43 ms. Old two-camera in-process baseline: 11-12 ms detect p95.

## How to run the gates

Pre-flight: `sudo -n /usr/local/bin/orange-evt-stream-smoke --config-dir
/home/jeremy/orange_data/config/local/100_cam4_ptp_fourcam --all --frames 5`
(a camera answering "GVCP ACK error" needs `evt_force_reboot`). Config folders
under `/tmp` regenerate after a reboot from
`~/orange_data/config/local/100_cam4_ptp_fourcam`.

```bash
scripts/run_detect_latency_spec.sh --orange-client <tree>/targets/release/orange_client <spec name>
```

Check, in this order: `runs.json` camera_frame_id_gaps; NIC
`rx_discards_phy`/`tx_global_pause` before and after (the run script's pass
line does not show these); recorder `Cam*_external_summary.json`
`encode_dropped` and `early_stage_exhausted` (the pass line does not show
these either) or `scripts/verify_external_recorder_session.py <recorder dir>`;
`scripts/analyze_gop_boundary_phase.py <run>` for the per-frame detect
phases; `scripts/analyze_writeback_stalls.py <run> <sampler.csv>` with
`scripts/host_writeback_sampler.sh` for host stalls; pose counts from
`Cam*_pose_events.jsonl`. Spec keys that bite: an endurance spec needs
`recording_control.record_for_seconds`, not just `duration_s`; owner-push
slots must exceed the peer shard's encoder buffers plus queued frames; the
acquisition-thread diagnostics (`acq_cadence_probe_all`,
`acq_ring_release_log`) are safe to leave on since the async line sink.

GUI validation: `scripts/run_gui_fourcam_external_ipc_validation.sh` then
`scripts/validate_gui_ptp_recording.py --latest-complete`; the Citrus
completion path is in `docs/manual_orange_citrus_completion_runbook.md`.

GUI caveat (2026-09-21, eleven 60 s bisect runs): with
`analytics.fused_frame = true` the GUI loses 22-122 frames/min on 2010093
and 4-39 on 2010094 (mlnx1 `rx_discards_phy` +10k per run, PCIe write
back-pressure into card A; headless is clean with the same graph). With
`fused_frame = false` and device ROI, device crop and the pose device stage
still on, the GUI loses 8/2/0/1 with the same a2d p95 (1.87 ms) and
capture-to-pose-done p95 (2.44 ms). The host app config therefore keeps
`analytics.fused_frame = false` for GUI use; headless specs may keep it on.
Root cause found later the same evening: the preview PBOs stayed CUDA-mapped
while OpenGL read them (undefined behaviour per the CUDA interop contract);
fixed in `src/gui/texture_resources.cpp` + `gui/preview_staging_lock.h`
(map, copy, unmap, sync, then upload; exclusive staging ownership). With the
fix, fused on with live previews lost 0/0 steady-state frames on
2010093/2010094 (mlnx1 +843/+0). Re-enable `fused_frame` in the app config
only after a repeat 60 s gate and a longer GUI soak with the fix.
Startup loss (a few frames on 2010093 in the first seconds of every
recording) was the recorders' first-use CUDA IPC imports: fixed by the
PREPARE/PREPARED handshake (33189a2; both recorder builds), crop recorders
prepared before recording is enabled, and a strict readiness gate. First
strict GUI gate pass: `2026_09_21_22_28_51` (0 drops, 0 NIC discards on
all four ports, live previews). With the full-slot gate and the recorder's
preparation peer-pull warm-up (f8a8067), three consecutive fresh starts
(`2026_09_21_22_55_55`, `_22_56_51`, `_22_57_47`) lost no frames and no
NIC port recorded a discard. If a start is refused by the strict gate the
GUI closes itself when exit-after-finalize is on; a root-owned instance that
is truly hung needs `sudo pkill -f targets/release/orange`.
Steady-state card-A loss (three paired frames per 10 min at the end of
pushed GOPs) was the full-frame preview: its 20 MB device-to-host read of the
owned copy slows the next frame's owner push 2.6x, the other camera's push
then falls back to the recorder's pull, and that pull reads the landing die
while the NIC writes into it. `gui.display.skip_pushed_gops = 25` in the app
config keeps the preview off peer-routed GOPs (10-min soak: 0 losses, 0 NIC
discards, preview about 5 fps in bursts). The proper fix is to downsample the
preview on the landing die before the transfer. Diagnostics for any regression:
`scripts/recording_startup_audit_report.py <folder>` (audit is always on)
and `ethtool -S mlnx{1,2}_p{1,2}_25g` deltas around the run.
Judge GUI runs by the mlnx1 `rx_discards_phy` delta as well as the drop
count, since drop counts vary 5x between identical runs. The residual GUI
loss (8/2 per minute) and the `no_result` pose rows on the non-device crop
path (no-analytics run) are open.

## Native recorder build

CUDA 13.1 toolkit at `~/.local/opt/cuda-13.1.1-nvenc`, Video Codec SDK 13.1
interface at `~/.local/opt/nvenc-interface-13.1.15`, driver 610.57.04:

```bash
cmake -S tools/nvenc_native_probe -B targets/native -DORANGE_SOURCE_ROOT=$PWD \
  -DCUDA_13_ROOT=$HOME/.local/opt/cuda-13.1.1-nvenc \
  -DNVENC_13_INTERFACE_DIR=$HOME/.local/opt/nvenc-interface-13.1.15/Video_Codec_Interface_13.1.15/Interface \
  -DBUILD_NATIVE_RECORDER=ON -DBUILD_NATIVE_KERNEL=ON
cmake --build targets/native --target external_recorder_ipc_probe_native -j 16
scripts/install_orange_native_recorder.sh   # -> /opt/orange/bin (sudo)
```

Specs and the app config point `recorder_tool_path` at it. The regular
`external_recorder_ipc_probe` (CUDA 12) still serves the crop recorders and
the non-native path.

## Standing rules

- Never skip the CPU results path (postprocess, tracking, IPC) in production
  or in a gate.
- No synchronous file I/O on the acquisition, YOLO, pose or handoff threads;
  per-frame diagnostics go through `src/async_line_sink.h`.
- Keep owner push serialized per card; the GOP routing offset stays 0.
- Read `encode_dropped`, `early_stage_exhausted` and the NIC counters before
  calling a run clean.
- Real fish are needed to validate crop/pose/track quality; no-fish runs
  validate the pipeline only.

## History

The pre-2026-09 notes (in-process recorder baselines, headless PTP runs,
Orange/Citrus commissioning) are in git history of this file and in
`docs/gui_external_ipc_status_2026_05_28.md`; the analytics campaign from
2026-09-03 onward is in the journal and in `docs/device_roi_merge_plan_2026_09_21.md`.

## GUI Parity Status, 2026-09-22

- All card-A GUI frame-loss mechanisms are fixed and soaked: the CUDA/OpenGL
  preview PBO ownership bug, the recorder first-use imports and cold peer
  pull (PREPARE/PREPARED handshake, full-slot gate, warm-up), and the display
  worker's full-frame read (the preview is now box-downsampled on the
  acquisition GPU before the transfer, `ORANGE_DISPLAY_SOURCE_DOWNSAMPLE=0`
  restores the old path). `gui.display.skip_pushed_gops` is back to `0`.
  Ten-minute four-camera soaks with both previews live pass the strict
  validator with 0 gaps, 0 SDK drops and 0 NIC discards.
- A second in-application recording works: the readiness gate resets its
  budget per start (`supervisors_done_at`), the full-frame client releases
  the previous session's imported staging slots, and autorun can keep the
  stream on after its finalize (`ORANGE_GUI_AUTORUN_KEEP_STREAMING_AFTER_FINALIZE=1`).
- SIGTERM/SIGINT/SIGHUP are blocked in every thread and consumed by the main
  loop, which stops the recording, then the stream, then closes. A signal
  delivered to an acquisition thread otherwise breaks the Rivermax stream.
  A hard-killed streaming GUI leaves the cameras refusing `EVT_CameraOpen`
  (GVCP ACK error); `targets/release/evt_force_reboot <serial> <ip>` clears
  that, and `orange-evt-stream-smoke` is the light comms check.
- The GUI validation wrapper runs `scripts/host_stall_monitor.py` beside
  every run (heartbeat + 1 Hz kernel counters; SMI counter is Intel-only);
  `scripts/host_stall_correlate.py --latest` says per stall cluster whether
  the host or Orange paused. The wrapper guard watches the launcher's parent
  and grandparent and waits 60 s before KILL. Reinstall the wrapper after
  editing it.
- Card-B camera traffic arrives on `mlnx2_p3_25g`/`mlnx2_p4_25g`; `p1`/`p2`
  are down. Card A uses `mlnx1_p1_25g`/`mlnx1_p2_25g`.
- Open: the `analytics.fused_frame` decision (app config keeps `false`).
