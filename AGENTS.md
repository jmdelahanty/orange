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
`temporal_aq = off`, PTP fields, `ORANGE_PTP_REGISTER_READ_DECIMATE=100`, and
the configured default detect engine, then starts the GUI. Use this to validate
without launching the GUI:

```bash
ORANGE_GUI_VALIDATE_ONLY=1 ./scripts/run_gui_aq_off_validation.sh
```

During the next GUI validation, also check the visible timing status:

- before recording: `Stream` elapsed time counts up and recording is idle
- while recording: `Recording` elapsed time counts independently
- after pause/stop recording while streaming remains on: `Finalizing` counts
  drain/finalization time, then the UI should show the last recording duration

After recording, validate the artifact with:

```bash
scripts/validate_gui_ptp_recording.py --latest
```

`--latest` intentionally validates the newest attempted GUI artifact, including
metadata-only/fail-fast folders. If that lands on an incomplete attempt and the
goal is to inspect the newest real recording, use:

```bash
scripts/validate_gui_ptp_recording.py --latest-complete
```

The validator defaults to the current production-like expectation:
`sync_mode = ptp_gate`, `ptp.enabled = true`, `ptp.mode = TwoStep`,
`ptp_register_read_decimate = 100`, zero camera gaps/GetFrame errors/encode
failures, valid decoded full-frame videos, a GUI-written
`recording_session.json`, real per-camera packet counts, and low YOLO queue
wait.

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
  summary JSON for the one-camera capped external encode smoke. The diagnostic
  probe is still not the production recorder backend, but the protocol/session
  metadata, video-content validation, split-GOP routing artifacts, and
  supervised headless lifecycle slices now exist. Keep encode GPU
  placement/routing as a first-class design variable.
- The staged implementation roadmap is documented in
  `docs/external_recorder_implementation_plan.md`. The next highest-signal
  slice is GUI/session external-recorder supervision and finalization, not more
  same-process NVENC tuning or more headless-only lifecycle work.
- The external split-GOP recorder protocol/routing design is documented in
  `docs/external_split_gop_recorder_design.md`. The session metadata, shard
  assignment, GOP routing artifacts, one-camera two-shard diagnostic, and
  two-camera PTP supervised headless validation are now complete diagnostic
  slices.
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
- The large max YOLO tails in that run are startup effects, not steady-state:
  frame `1` on both cameras spent about `49 ms` in `cpu_pre_sync_ms`, then
  frames `2-6` waited in the YOLO queue behind that first live frame. After
  frame `50`, steady-state `capture_to_detect_done_ms` was much tighter:
  `2010095 p95 = 6.651 ms, max = 6.986 ms`; `2010096 p95 = 5.882 ms,
  max = 6.343 ms`.
- There is also a recorder first-use signal: frame `26`, the first frame routed
  to secondary shard GPUs `6`/`8`, showed `copy_ms` around `180 ms` on both
  cameras. Queue depth `32` absorbed it with no drops.
- Recorder pre-listen prewarm now exists in `external_recorder_ipc_probe`:
  use `--prewarm-slots`, `--prewarm-bytes`, and `--prewarm-peer-copy`. The
  smoke runners default to `--prewarm-slots 4`; the two-camera PTP runner
  derives `--prewarm-bytes` from the local Mono8 camera config. Latest
  diagnostic run:
  `/tmp/orange_external_recorder_ptp_20260425_231526`. It encoded/ACKed
  `401/401` frames per camera with no drops and moved recorder detach-copy
  steady-state max below `1 ms` (`2010095 = 0.910 ms`, `2010096 = 0.261 ms`).
  YOLO service stayed about `4.6 ms p95`; broad detect steady p95 stayed about
  `6.4-6.5 ms`, so the next latency work is YOLO warmup/worker dispatch, not
  recorder detach.
- Headless YOLO synthetic prewarm now exists via
  `fixed.yolo_worker.prewarm_iterations` and `YoloWorker::Warmup()`. The
  external-recorder smoke runners default to `--yolo-prewarm-iterations 3`.
  Latest diagnostic run:
  `/tmp/orange_external_recorder_ptp_20260425_233134`. It encoded/ACKed
  `401/401` frames per camera with no drops and removed the old first-live-frame
  `cpu_pre_sync_ms ~49 ms` tail. All-frame `capture_to_detect_done_ms max`
  dropped to `8.272 ms` for `2010095` and `7.108 ms` for `2010096`; YOLO
  steady p95 stayed about `4.57-4.59 ms`. Remaining work is steady-state
  acquisition-to-worker-start / dispatch latency.
- YOLO dispatch/acquisition split instrumentation now exists in
  `Cam*_yolo_perf.csv`. Latest diagnostic run:
  `/tmp/orange_external_recorder_ptp_20260425_235542`; analytics artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_20260425_235542`.
  The corrected headless smoke path uses the intended hybrid flags by default:
  `ORANGE_ANALYTICS_EARLY_OWNED_FRAME=1`,
  `ORANGE_YOLO_READY_EVENT_FASTPATH=1`, and
  `ORANGE_YOLO_DETACH_INPUT=1`. The run stayed healthy (`401/401` ACKed and
  encoded per camera, `0` drops, `gpu_direct=100`, `ring=0`). The YOLO queue is
  not the remaining bottleneck (`yolo_queue_wait_ms p95` about `0.02 ms`);
  recording submit is also negligible (`p95 < 0.01 ms`). The dominant
  pre-enqueue cost is per-frame PTP timestamp checking:
  `acquisition_to_ptp_done_ms p95 = 1.968 ms` for `2010095` and `2.135 ms`
  for `2010096`.
- PTP clarification: the hot-path cost is the diagnostic
  `EVT_CameraGetUInt32Param("GevTimestampValueHigh/Low")` current-camera-clock
  read after `EVT_CameraGetFrame`, not the PTP gate itself. The frame's embedded
  `received_frame->timestamp` remains the per-frame timing truth. TwoStep PTP
  is the clock-sync mode seen in logs; it does not require polling
  `GevTimestampValue*` every frame. Do not disable PTP; decimate or move these
  control-plane reads off the YOLO hot path and keep full polling as a
  diagnostic mode.
- Experimental PTP register-read decimation is implemented via
  `ORANGE_PTP_REGISTER_READ_DECIMATE=<N>` and experiment-spec
  `fixed.ptp_register_read_decimate`. Default `N=1` keeps old full polling.
  `N=100` smoke run `/tmp/orange_external_recorder_ptp_20260426_001408`
  stayed healthy (`401/401` ACKed/encoded per camera, `0` drops), sampled
  `ptp_register_reads=9` per camera, and reduced steady
  `acquisition_to_detect_done_ms p95` from `6.244/6.678 ms` to
  `4.612/4.580 ms` for `2010095/2010096`. Use embedded frame timestamps for
  per-frame cadence/skew in decimated mode; use `N=1` for direct latch
  diagnostics.
- Longer `N=100` validation
  `/tmp/orange_external_recorder_ptp_20260426_002618` stayed healthy for
  `2803/2803` frames per camera with `0` drops, `33` PTP register reads per
  camera, steady `acquisition_to_detect_done_ms p95 = 4.580/4.585 ms`, and
  cadence-probe cross-camera embedded timestamp skew within `-28 ns` to
  `+22 ns`.
- The clean two-camera supervised headless PTP external-recorder validation on
  2026-05-07 used a stamped spec and artifact root
  `/tmp/orange_external_recorder_supervised_ptp_20260507_222657`; analytics
  artifact:
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_supervised_20260507_222657`.
  `orange_client` supervised both recorder processes directly. Both cameras
  received/ACKed/encoded `400/400` frames, had `0` drops, `0` frame-id gaps,
  `0` GetFrame errors, and both merged MP4s passed video sanity. Post-frame-50
  `acquisition_to_detect_done_ms p95 = 4.488/4.598 ms` for
  `2010095/2010096`; YOLO queue wait p95 stayed `0.018/0.020 ms`.
- Supervised headless external IPC rolling is now implemented in the
  diagnostic recorder. The one-camera checked-in repeatable spec is
  `experiment_specs/2010096_headless_real_yolo_external_ipc_rolling_smoke_a16_gpu5_6.json`;
  the two-camera PTP checked-in spec is
  `experiment_specs/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp_external_ipc_rolling_smoke_a16.json`.
  The latest two-camera PTP index validation used recorder artifact
  `/tmp/orange_external_recorder_ptp_rolling_20260509_index_external`
  and analytics artifact
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_headless_external_ipc_rolling_index_20260509_index_external`.
  It used `record_for_seconds = 6`, `clip_seconds = 2`, `encode_fps = 100`,
  `encode_max_fps = 0`, and GOP shards `2010095 -> 5,6` and
  `2010096 -> 7,8`. Both cameras received/ACKed/encoded `601/601` frames, had
  `0` encode drops, wrote four rolling clips covering frame ranges `1-200`,
  `201-400`, `401-600`, and `601`, passed merged MP4 video sanity, and passed
  `scripts/verify_external_recorder_session.py`.
- For supervised headless external IPC rolling, Orange now rewrites the shared
  analytics `recording_session.json` from external recorder summaries after
  recorder finalization. That manifest reports `mode = "rolling_clips"`,
  `producer = "orange_headless_external_ipc"`, `recording_backend.mode =
  "external_ipc"`, and per-camera clip video/metadata/keyframe paths, frame
  counts, and packet counts under `clips[].camera_artifacts`. The external
  verifier now requires that manifest to match the external summaries for
  rolling runs.
- Rolling sessions now also write `recording_clip_index.json` and
  `recording_clip_index.csv` in the parent recording folder, and
  `recording_snapshot.json` carries `session.recording_session_index` absolute
  pointers. Latest native index validation:
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_rolling_clip_index_20260509_index_native`.
- Latest packet-count validation:
  native in-process rolling artifact
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_rolling_packet_counts_20260509/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25__aq_off__tempaq_off__lookahead_off`
  passed `scripts/verify_timed_recording.py` with
  `packet_count_source = "ffprobe_nb_read_packets"`. Two-camera external IPC
  rolling analytics artifact
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_external_ipc_rolling_packet_counts_20260509_021533`
  with recorder artifact
  `/tmp/orange_external_recorder_ptp_rolling_packet_counts_20260509` passed
  `scripts/verify_external_recorder_session.py`; both cameras
  received/ACKed/encoded `601/601` frames and index rows used
  `packet_count_source = "external_recorder_summary.packets_written"`.
- Latest terminal-tail coalescing validation:
  two-camera external IPC rolling analytics artifact
  `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_external_ipc_rolling_tail_coalesce_20260509_120903`
  with recorder artifact `/tmp/orange_external_recorder_ptp_rolling` passed
  `scripts/verify_external_recorder_session.py`. Both cameras
  received/ACKed/encoded `601/601` frames, but the former 1-frame fourth clip
  was coalesced into the final clip: `1-200`, `201-400`, `401-601`.
  Recorder summaries reported `target_frame_count = 600`,
  `terminal_tail_coalesced_frames = 1`, and
  `terminal_tail_coalesce_frames = 25`. Native in-process rolling also
  revalidated at
  `/home/jeremy/orange_data/exp/unsorted/2010096_headless_rolling_tail_coalesce_20260509_121015/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25__aq_off__tempaq_off__lookahead_off`
  with three clips and `scripts/verify_timed_recording.py` passing.
- GUI in-process recordings now write a shared single-clip
  `recording_session.json` after the recording drain completes, and
  `recording_snapshot.json` points at it. This is build-verified but still
  needs the next GUI validation run.
- GUI app storage can now set `recording.ptp_register_read_decimate`; the env
  var `ORANGE_PTP_REGISTER_READ_DECIMATE` still takes precedence.
- In the latest native index-validation run, the host PTP stack was initially
  not ready, so headless startup repaired it via `scripts/ptp_stack.sh` and
  left `ptp4l`/`phc2sys` running on exit. The stack was later stopped manually
  and verified stopped: no
  `ptp4l|phc2sys` processes and no `/var/run/ptp4l` socket. For future PTP
  runs, remember that auto-started host PTP should be explicitly stopped with
  `scripts/ptp_stack.sh stop` when validation is done.
- Next operator check is a GUI PTP/AQ-off in-process recording with
  `ORANGE_PTP_REGISTER_READ_DECIMATE=100`, visible stream/recording/finalizing
  timer confirmation, and `scripts/validate_gui_ptp_recording.py --latest` or
  `--latest-complete` artifact validation.
- GUI/session external-recorder supervision and finalization remain the next
  larger architecture slice after the in-process GUI path is revalidated.
- Keep `100_cam4_ptp` as the default GUI validation folder for two-camera
  production-like runs on this host.
