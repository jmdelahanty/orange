# Headless Sudo Wrapper

Use this when the Emergent/NIC stack requires root for GPU-direct streaming and
you want to run local headless Orange experiments without typing a sudo
password each time.

## Wrapper

The repo includes a narrow wrapper script:

- `scripts/orange_local_benchmark_wrapper.sh`

It only allows:

- `orange_client --mode local --experiment-spec <spec>`
- `orange_client --mode local --stream-only ...`

It only accepts spec files under:

- `/home/jeremy/orange-jeremy/experiment_specs`
- `/home/jeremy/orange-gop-split-a16/experiment_specs`
- `/tmp`

It only accepts `orange_client` binaries at:

- `/home/jeremy/orange-jeremy/build/orange_client`
- `/home/jeremy/orange-gop-split-a16/targets/release/orange_client`

The wrapper also exposes a narrow allowlist of experiment env passthroughs:

- `--acquire-work-entries-max <n>` exports `ORANGE_ACQUIRE_WORK_ENTRIES_MAX`.
- `--encoder-entry-pool-size <n>` exports `ORANGE_ENCODER_ENTRY_POOL_SIZE`.
- `--yolo-perf-log` exports `ORANGE_YOLO_PERF_LOG=1`.
- `--no-yolo-perf-log` exports `ORANGE_YOLO_PERF_LOG=0`.
- `--yolo-perf-sample <n>` exports `ORANGE_YOLO_PERF_SAMPLE=<n>` and enables
  YOLO perf logging unless logging was explicitly disabled.

After the run, it chowns the experiment output folder back to the invoking
user so artifacts are not left root-owned.

## Install

Install the wrapper as a root-owned executable:

```bash
/home/jeremy/orange-gop-split-a16/scripts/install_orange_local_benchmark_wrapper.sh
```

Add a narrow sudoers entry for user `jeremy`:

```bash
printf 'jeremy ALL=(root) NOPASSWD: /usr/local/bin/orange-local-benchmark\n' | \
  sudo tee /etc/sudoers.d/orange-local-benchmark >/dev/null
sudo chmod 0440 /etc/sudoers.d/orange-local-benchmark
sudo visudo -cf /etc/sudoers.d/orange-local-benchmark
```

## Run

Smoke test:

```bash
sudo /usr/local/bin/orange-local-benchmark \
  /home/jeremy/orange-jeremy/experiment_specs/2010096_smoke_a6000.json
```

Full Block A:

```bash
sudo /usr/local/bin/orange-local-benchmark \
  /home/jeremy/orange-jeremy/experiment_specs/2010096_block_a_a6000.json
```

GOP-split branch headless real-YOLO smoke with explicit perf CSVs:

```bash
sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  --yolo-perf-log \
  --yolo-perf-sample 1 \
  /home/jeremy/orange-gop-split-a16/experiment_specs/2010096_headless_real_yolo_preprocessonly_a16_gpu5.json
```

GOP-split two-camera headless real-YOLO plus real split-GOP recording with PTP:

```bash
cd /home/jeremy/orange-gop-split-a16
sudo -n /usr/local/bin/orange-local-benchmark \
  --orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client \
  --yolo-perf-log \
  --yolo-perf-sample 1 \
  /home/jeremy/orange-gop-split-a16/experiment_specs/2010095_2010096_headless_real_yolo_aq_off_100_cam4_ptp.json
```

Use the PTP spec for two-camera `100_cam4` headless validation. The free-run
variant can produce misleading artifacts on this setup; one recent free-run run
encoded `2010095` as an all-black stream while `2010096` encoded real content.
The PTP version should show both cameras near `100 fps`, zero camera gaps, zero
encode failures, and roughly similar high bitrates around `150 Mbps` per camera
for real dish content.

GOP-split two-camera headless real-YOLO plus external process-isolated
split-GOP recording with PTP:

```bash
cd /home/jeremy/orange-gop-split-a16
scripts/run_external_recorder_two_camera_ptp_smoke.sh \
  --duration 6 \
  --warmup 1
```

This starts one external recorder process per camera, using the local A16
pairing `2010095 -> 5,6` and `2010096 -> 7,8`, then runs the headless PTP
benchmark with `recording_sink_mode = external_ipc`.

Temporary retry spec:

```bash
sudo /usr/local/bin/orange-local-benchmark /tmp/2010096_smoke_a6000_retry.json
```

## Notes

- This is intended for benchmark/experiment runs, not as a general root wrapper
  for arbitrary Orange commands.
- If the SDK ever stops requiring root, prefer running `orange_client` directly
  as the normal user again.
