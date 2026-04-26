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

## External Recorder Smoke

The current one-camera external-recorder smoke is:

```bash
cd /home/jeremy/orange-gop-split-a16
scripts/run_external_recorder_smoke.sh --duration 3 --warmup 1 --encode-fps 60 --output-dir /tmp
```

Default shape:

- Camera `2010096`.
- Analytics/YOLO GPU `5`.
- External recorder GPU `5`.
- `recording_sink_mode = "external_ipc"`.
- External HEVC encode capped at `60 fps`.
- Socket path uses the production default:
  `/tmp/orange_external_recorder_2010096.sock`.

Latest short smoke:

- Analytics root:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_real_yolo_external_ipc_encode_smoke_2010096_20260425_212327`
- Recorder root:
  `/tmp/orange_external_recorder_2010096_20260425_212327`
- Recorder received/ACKed `401` frames.
- Encoded `241`, skipped `160`, dropped `0`.
- Detach copy `p95 = 0.033864 ms`.
- External encode total `p95 = 1.685832 ms`.
- `nvEncLockBitstream p95 = 0.003807 ms`.
- MP4 output was valid enough for `ffprobe`: `duration = 4.017 s`,
  `size = 76,638,875 bytes`.
- The runner now writes `external_video_sanity.json` and fails if the external
  MP4 is missing, empty, undecodable, black, or flat. The latest decoded sample
  check had mean luma about `220`, luma stddev about `79.5`, and
  `black_fraction_lt8 = 0.0`, so it was not the previous black-frame failure
  mode.

Latest 30-second GPU placement comparison:

- Same-GPU command:
  `scripts/run_external_recorder_smoke.sh --duration 30 --warmup 2 --encode-fps 60 --output-dir /tmp`
- Paired-GPU command:
  `scripts/run_external_recorder_smoke.sh --duration 30 --warmup 2 --encode-fps 60 --recorder-gpu-id 6 --output-dir /tmp`
- Same-GPU artifact:
  `/tmp/orange_external_recorder_2010096_20260425_213750`
- Paired-GPU artifact:
  `/tmp/orange_external_recorder_2010096_20260425_213850`
- Both runs received/ACKed `3203` descriptors, encoded `1922`, skipped `1281`
  by the `60 fps` cap, dropped `0`, had `0` IPC failures/timeouts, and passed
  external MP4 sanity with `black_fraction_lt8 = 0.0`.
- Same GPU `5 -> 5` post-warm p95:
  `capture_to_detect_done_ms = 4.591`, `total_ms = 4.560`,
  `infer_ms = 4.104`, `sync_ms = 4.463`, external
  `encode_total_ms = 0.112`, `nvEncLockBitstream_ms = 0.0028`.
- Paired GPU `5 -> 6` post-warm p95:
  `capture_to_detect_done_ms = 3.245`, `total_ms = 3.222`,
  `infer_ms = 2.718`, `sync_ms = 3.130`, external
  `encode_total_ms = 0.126`, `nvEncLockBitstream_ms = 0.0028`.

Interpretation: external process isolation keeps the YOLO CPU launch path fast.
Moving external NVENC off the analytics GPU also reduces remaining GPU
completion pressure. This does not remove the production need for split-GOP:
one A16 encoder still cannot carry a full `4512x4512 @ 100 fps` stream, so the
future recorder should minimize analytics-GPU encode share while preserving
multi-GPU split-GOP throughput.

## Remaining Work

- Automated decoded-frame entropy/black-frame sanity checking is now part of
  headless experiment validation via `require_valid_video_content`, and the
  external recorder smoke has its own MP4 sanity gate because the external MP4
  is not the in-process video artifact checked by `orange_client`.
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
- The first live external-recorder detach prototype now exists behind
  `recording_sink_mode=external_ipc`. A one-camera `2010096` smoke ACKed `601`
  CUDA IPC frames at about `99.85 fps`, with no camera drops/frame-id gaps and
  YOLO `cpu_preprocess_ms p95 = 0.0119 ms`.
- The external IPC probe now also has a first NVENC encode slice behind
  `--encode`. It ACKs after detach copy, then encodes from recorder-owned
  device slots on a dedicated external-process encoder thread.
- First same-GPU encode smoke on `2010096` capped external HEVC encode at
  `60 fps`: `601` frames received, `501` post-warmup ACKs for `500` submitted
  frames, `0` IPC failures/timeouts, `360` externally encoded frames, and
  `0` encode queue drops. YOLO stayed on the fast CPU path
  (`cpu_preprocess_ms p95 = 0.0149 ms`, `cpu_pre_sync_ms p95 = 0.0914 ms`);
  `capture_to_detect_done_ms p95` rose to `4.5895 ms` from same-GPU completion
  pressure, not CPU launch/preprocess contention.
- The external IPC probe now writes MP4, keyframe sidecar, per-frame CSVs, and a
  summary JSON for the one-camera capped external encode smoke. It is still not
  production recording yet. Next implementation work is to make the recorder
  protocol/session metadata robust, add video-content validation to the smoke
  summary, then move toward external split-GOP/multi-GPU routing. Keep encode
  GPU placement/routing as a first-class design variable.
- The staged implementation roadmap is documented in
  `docs/external_recorder_implementation_plan.md`. The next highest-signal
  slice is hardening the single-camera MP4 smoke into a production-like
  external recorder contract, not more same-process NVENC tuning.
- The external split-GOP recorder protocol/routing design is documented in
  `docs/external_split_gop_recorder_design.md`. Use that as the starting point
  for the next implementation slice: session metadata, shard assignment,
  GOP routing artifacts, and then a one-camera two-shard diagnostic.
- The first metadata-only external recorder shard slice now exists:
  descriptors/artifacts carry session id, stream id, GOP index, frame index
  within GOP, assigned GPU, assigned shard, and `routing_policy`.
  `single_shard` remains the default recorder mode.
- The first two-shard external recorder diagnostic also exists behind
  `external_recorder_ipc_probe --shard-gpu-ids 5,6` and the smoke runner option
  `scripts/run_external_recorder_smoke.sh --shard-gpu-ids 5,6`. It routes GOPs
  by `gop_index % shard_count` and writes per-shard MP4s/encode CSVs plus
  `external_gop_routing.csv`. Multi-shard mode now also writes a merged base
  `Cam<serial>_external.mp4` through a GOP-order coordinator; per-shard MP4s
  remain diagnostic outputs.
- For full-rate one-camera split-GOP headless smoke, use queue depth at least
  one GOP burst. Validated command shape:
  `scripts/run_external_recorder_smoke.sh --duration 3 --warmup 1 --encode-fps 100 --encode-max-fps 0 --queue-depth 32 --output-dir /tmp --shard-gpu-ids 5,6`.
  The `--encode-max-fps 0` override disables the diagnostic frame-rate cap
  while keeping the merged MP4 nominal FPS at `100`.
- Two-camera PTP external-recorder smoke now has a runner:
  `scripts/run_external_recorder_two_camera_ptp_smoke.sh --duration 6 --warmup 1`.
  Default topology is `2010095 -> analytics GPU 5, shards 5,6` and
  `2010096 -> analytics GPU 7, shards 7,8`, with `100_cam4_ptp`,
  `--encode-max-fps 0`, and queue depth `32`.
- Current clean two-camera PTP external-recorder artifact:
  `/tmp/orange_external_recorder_ptp_20260425_224354`; analytics artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_20260425_224354`.
  Both cameras received/ACKed/encoded `401` frames with `0` skips/drops, both
  merged MP4s passed video sanity, and YOLO p95 stayed below the old in-process
  `11-12 ms` baseline (`2010095 capture_to_detect_done_ms p95 = 6.698`,
  `2010096 = 5.953`).
- Keep `100_cam4_ptp` as the default GUI validation folder for two-camera
  production-like runs on this host.
