# Merge plan: `agent/analytics/device-roi-20260912` into `agent/acquisition/shaman-v2-authoritative-20260824`

Date: 2026-09-21. Status: plan, not started. Owner: Jeremy; the device-roi
session prepares the integration branch, the session that holds the shared
worktree `/home/jeremy/orange-gop-split-a16` decides when production moves.

## Why now

The device-roi line has passed its gate: a 30-minute, four-camera, real-fish,
production-shape session (detect, device ROI/crop, TensorRT pose, four
full-frame split-GOP recorders, four crop recorders) with 0 camera gaps,
NIC counters unchanged, 0 recorder drops, detect p99 1.87-1.88 ms and
capture-to-pose p95 2.43 ms (artifact
`.../exp/unsorted/fourcam_fused_recorder_realfish_int8_192_native_ownerpush_ring_od8ff_s16_endurance30_20260921_142019`).
The old in-process two-camera baseline was 11-12 ms detect p95.

## Divergence (measured 2026-09-21)

- Merge base `5cf21a9` (2026-09-05). device-roi is 89 commits ahead;
  production is 7 ahead (GUI daily-registration and arena-centering work,
  2026-09-14/15) plus uncommitted GUI edits in the shared worktree.
- Files touched on both sides: `CMakeLists.txt` (+4 on production) and
  `src/acquire_frames.cpp` (production `11ab7ab` "Buffer aligned snapshot
  frames without blocking acquisition", 23 lines).
- Dry-run `git merge --no-ff` of device-roi into the production tip in a
  throwaway worktree: **clean, no conflicts**, 194 files, +34,175 / -658.
  The two shared files auto-merged; `acquire_frames.cpp` still needs a
  human read of the merged acquisition loop and a build.

## What the merge brings

Fused per-slot analytics graph (preprocess, detect, device ROI, crop, pose,
pool copy); INT8 min-max detect engine; 192 px pose head path; external
recorder owner push with per-card serialization and deadline gate; native
NV12 array input (CUDA 13 recorder build, `tools/nvenc_native_probe`);
split submit/harvest; staging-bound fix; full-frame-only
`extra_output_delay`; owner-push slot count; async line sink for hot-thread
diagnostics; MP4 writeback pacing; host sysctl file; 114 experiment specs;
analysis and diagnostic scripts; the journal-backed docs.

## What the merge does NOT deliver by itself (gaps to close)

1. **GUI parity for the validated recorder configuration.** The headless
   client sets these through spec keys and env; the GUI lifecycle only knows
   `native_local_input` and `full_frame_extra_output_delay` structurally and
   sets neither:
   - owner push on (`ORANGE_EXTERNAL_RECORDER_OWNER_PUSH=1`) and
     `ORANGE_EXTERNAL_RECORDER_OWNER_PUSH_SLOTS=16`;
   - full-frame `--extra-output-delay 8` (lifecycle option
     `full_frame_extra_output_delay`);
   - native local input on, with `recorder_tool_path` pointing at the CUDA 13
     native recorder binary (GUI today resolves the sibling
     `external_recorder_ipc_probe` next to the executable);
   - crop recorders keep default buffers (do not use the env form of
     extra_output_delay).
   Plan: add `recording.external_ipc.{owner_push, owner_push_slots,
   full_frame_extra_output_delay, native_local_input, recorder_tool_path}`
   to the app config (`~/orange_data/config/app/default.json`), wire them in
   `orange.cpp` where `prepared.external_recorder_lifecycle_options` is
   built, keep env overrides winning, and re-run
   `scripts/run_gui_fourcam_external_ipc_validation.sh` plus
   `scripts/validate_gui_ptp_recording.py --latest-complete`.
2. **Native recorder binary as an installed artifact.** Today the spec
   contracts point at `/home/jeremy/orange-device-roi-20260912/targets/native/external_recorder_ipc_probe_native`.
   Production needs: the build recipe in the tree (it is:
   `cmake -S tools/nvenc_native_probe -B targets/native -DORANGE_SOURCE_ROOT=<tree> -DCUDA_13_ROOT=~/.local/opt/cuda-13.1.1-nvenc -DNVENC_13_INTERFACE_DIR=~/.local/opt/nvenc-interface-13.1.15/Video_Codec_Interface_13.1.15/Interface -DBUILD_NATIVE_RECORDER=ON -DBUILD_NATIVE_KERNEL=ON`),
   a stable install path (proposal `/usr/local/bin/orange_external_recorder_native`,
   or the sibling-binary convention next to `orange`/`orange_client`), and the
   sudo wrappers (`/usr/local/bin/orange-local-benchmark`,
   `orange-gui-validation`) updated for the production tree. The 63 specs
   that name the device-roi path get a sed after the install path exists.
3. **Defaults versus opt-ins.** Decide per knob whether the merged code
   defaults to the validated value or the config carries it:
   - owner push: keep opt-in in code, on in the production app config and
     specs (it depends on the per-card lock, which needs the PCI sysfs
     lookup; it fails closed to the pull path);
   - full-frame extra_output_delay 8 and 16 slots: config;
   - native local input: config, because it needs the CUDA 13 binary;
   - MP4 writeback pacing: default on in code (already);
   - hot-thread diagnostics: safe to leave on in gates; off in production
     app config.
4. **Host state.** `/etc/sysctl.d/90-orange-writeback.conf` installed
   2026-09-21 (verify after the next reboot with `sysctl vm.dirty_bytes`).
   `isolcpus`, `iommu=pt`, `tsc=reliable` unchanged. PTP stack as before.
5. **Docs.** `AGENTS.md` in the production tree still describes the
   2026-05 state (in-process baselines, headless PTP runs). It needs the
   current shape, the gates, and the pointers to the journal and to
   `docs/device_roi_merge_plan_2026_09_21.md`.

## Sequence

1. Other session commits or stashes (tagged, per the worktree rules) its
   GUI edits in `/home/jeremy/orange-gop-split-a16` and pushes production.
2. Device-roi session creates `integration/device-roi-into-shaman-20260921`
   from the production tip in a **new** worktree (never the shared one),
   merges device-roi with `--no-ff`, reads the `acquire_frames.cpp` merge,
   builds `orange`, `orange_client`, `external_recorder_ipc_probe`, the
   test executables (`ctest`), and the native recorder.
3. Gap 1 (GUI parity) and gap 2 (install path) land on the integration
   branch as separate commits.
4. Gates on the integration build, in order: 60 s four-camera headless
   round (`..._od8ff_s16` spec, retargeted to the integration binaries and
   the installed native recorder), the 30-minute fish endurance, then the
   GUI four-camera validation with the Orange/Citrus completion path
   (`docs/manual_orange_citrus_completion_runbook.md`).
5. Production fast-forwards to the integration branch; `AGENTS.md`
   updated; the device-roi worktree is retired.

## Rollback

Everything new is behind config or spec keys except the async sink and the
MP4 pacing, both of which fail safe (sink drops rows and reports; pacing
disables itself if the fd cannot be opened). Production can set
`recording.sink_mode` back to the previous value and the old recorder path
is untouched.
