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
