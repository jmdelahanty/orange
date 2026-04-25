# Agent Notes

## Current Branch Context

- Worktree: `/home/jeremy/orange-gop-split-a16`
- Branch: `exp/gop-split-a16`
- Do not touch or commit untracked `build-gop-split/`.
- Main performance target: reduce real two-camera `100 fps` detect/YOLO latency
  without breaking full-frame split-GOP recording throughput or introducing
  camera/pose/crop drops.

## Two-Camera Headless PTP Run

For valid two-camera headless real-YOLO plus real split-GOP recording on the
local `100_cam4` setup, use the PTP spec:

```bash
cd /home/jeremy/orange-gop-split-a16
sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  --yolo-perf-log \
  --yolo-perf-sample 1 \
  /home/jeremy/orange-gop-split-a16/experiment_specs/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp.json
```

Expected healthy run shape:

- `fixed.sync_mode = "ptp_gate"` in the experiment spec.
- Runtime/snapshot should report `sync_mode = "ptp_gate"`.
- Both cameras sustain about `100 fps`.
- `camera_frame_id_gaps = 0`.
- `enc_fail_final = 0`.
- Full-frame videos are present.
- With real dish content, both cameras should encode around `150 Mbps`; a much
  lower bitrate on one camera can indicate invalid/black content rather than an
  easier encode workload.

Recent measured PTP result:

- Artifact root:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp`
- `2010095`: `99.996 fps`, `150.8 Mbps`, `enc_slow_final = 8`,
  `enc_fail_final = 0`, `capture_to_detect_done_ms p95 = 11.009`.
- `2010096`: `99.964 fps`, `151.3 Mbps`, `enc_slow_final = 12`,
  `enc_fail_final = 0`, `capture_to_detect_done_ms p95 = 12.181`.
- YOLO queue wait stayed tiny, about `0.014-0.020 ms p95`; the detect tail was
  mostly `cpu_pre_sync_ms`, about `6.8-7.9 ms p95`.

Do not use the free-run two-camera headless run as the main content/load
comparison for this setup. A recent free-run run produced valid-looking
artifacts while `2010095` encoded all-black frames at about `24.6 Mbps`; the PTP
run produced real high-bitrate content from both cameras.

## Two-Camera GUI PTP Run

For production-like GUI validation, use the local PTP config folder:

```text
/home/jeremy/orange_data/config/local/100_cam4_ptp
```

That folder was cloned from `100_cam4` and only changes:

- `sync_mode = "ptp_gate"`
- `ptp = { "enabled": true, "mode": "TwoStep" }`

Run the GUI validation launcher:

```bash
cd /home/jeremy/orange-gop-split-a16
./scripts/run_gui_aq_off_validation.sh
```

The launcher defaults to `100_cam4_ptp`, validates schema-4 `aq = off`,
`temporal_aq = off`, and PTP fields, then starts the GUI. Use this to validate
without launching the GUI:

```bash
ORANGE_GUI_VALIDATE_ONLY=1 ./scripts/run_gui_aq_off_validation.sh
```

Recent measured GUI PTP/AQ-off result:

- Artifact:
  `/home/jeremy/orange_data/exp/unsorted/2026_04_25_18_22_25`
- Runtime config used `sync_mode = "ptp_gate"`, `ptp.enabled = true`, and
  `ptp.mode = "TwoStep"` for both cameras.
- Both full-frame MP4s were valid real content at about `150 Mbps`.
- `2010095`: `0` camera gaps, `0` GetFrame errors, `0` encode failures,
  `capture_to_detect_done_ms p95 = 11.150`.
- `2010096`: `0` camera gaps, `0` GetFrame errors, `0` encode failures,
  `capture_to_detect_done_ms p95 = 12.100`.
- YOLO queue wait p95 stayed tiny, about `0.014-0.015 ms`.
- The remaining tail was mostly `cpu_pre_sync_ms`: `6.641 ms p95` for
  `2010095`, `7.690 ms p95` for `2010096`.

Interpretation: GUI PTP matches headless PTP. The GUI/display lifecycle is not
the main remaining regression source; valid two-camera split-GOP recording load
still inflates YOLO host-side submission/sync latency.

## Current Interpretation

- Generic NVENC AQ and temporal AQ are disabled in the schema-4 `100_cam4`
  configs because focused headless tests showed they inflate YOLO detect p95.
- With AQ and temporal AQ off, valid two-camera PTP full-frame recording still
  pushes YOLO p95 back to about `11-12 ms`.
- The remaining tail is not YOLO queue backlog. It is primarily host-side
  CUDA/NVENC submission or synchronization contention around the full-frame
  recording path.
- Process isolation for full-frame encode/output remains the next high-signal
  architecture experiment.

## No-Fish Test Caveat

No-fish runs are still valid for the current CUDA/NVENC submission work because
they exercise:

- acquisition,
- ingress lease/source readiness,
- YOLO worker scheduling,
- YOLO CUDA/TensorRT submission,
- full-frame split-GOP encode/output.

No-fish runs do not validate:

- positive-detection crop ROI behavior,
- pose second-stage latency,
- tracking behavior,
- end-to-end positive detection-to-crop/pose latency.

Do not block process-isolation or encoder-contention experiments on having fish
available. Do require fish or another valid detectable subject before declaring
crop/pose/track latency solved.

## Remaining Work

- Automated decoded-frame entropy/black-frame sanity checking is now part of
  headless experiment validation via `require_valid_video_content`.
- Run the real-frame external NVENC discriminator using
  `nvenc_stress_load --pattern raw-file` against a
  `pre_encoder_reference_capture` dump. The key discriminator is whether YOLO
  `cpu_pre_sync_ms` / CUDA launch p95 stays near the preprocess-only fast path
  while Process B encodes real dish-frame content outside the analytics
  process.
- The first same-GPU real-frame external run on 2026-04-25 stayed near the
  preprocess-only fast path for YOLO CPU submission
  (`cpu_preprocess_ms p95 = 0.0149 ms`, `cpu_pre_sync_ms p95 = 0.0776 ms`)
  while Process B wrote about `150.4 Mbps`.
- That same run still increased `capture_to_detect_done_ms p95` from
  `3.4894 ms` to `4.4122 ms`. The added latency landed in GPU completion
  timing (`infer_ms` / `sync_ms`), not CPU launch/preprocess timing, so process
  isolation should be treated as solving same-process runtime-lock contention,
  not as eliminating same-GPU hardware/fabric contention.
- Next implementation work is the live external-recorder detach prototype with
  CUDA IPC or an exportable detach ring. Keep encode GPU placement/routing as a
  first-class design variable.
- Keep `100_cam4_ptp` as the default GUI validation folder for two-camera
  production-like runs on this host.
