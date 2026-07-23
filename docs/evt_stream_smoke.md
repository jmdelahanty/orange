# EVT Stream Smoke Diagnostic

`evt_stream_smoke` is an explicit hardware diagnostic for camera bring-up. It is
not a normal unit test or CI gate. It loads the same Orange camera config files
used by the GUI, opens selected EVT cameras, applies configured parameters,
opens the stream, optionally receives frames, then closes the stream and camera.

Build it explicitly:

```bash
cd /home/jeremy/orange-gop-split-a16
cmake --build targets/release --target evt_stream_smoke -j 8
```

Install the no-typed-sudo wrapper:

```bash
cd /home/jeremy/orange-gop-split-a16
sudo scripts/install_orange_evt_stream_smoke_wrapper.sh --install-sudoers
```

After installation, use `orange-evt-stream-smoke` instead of invoking the target
with `sudo -n` directly. The installed wrapper re-execs itself through a narrow
NOPASSWD sudoers rule, validates arguments, restores the EVT SDK environment,
and only runs:

```text
/home/jeremy/orange-gop-split-a16/targets/release/evt_stream_smoke
```

`--config-dir` defaults to `ORANGE_GUI_CONFIG_DIR`. `--serials` defaults to
`ORANGE_GUI_EXPECT_CAMERAS`, but an explicit `--serial` or `--serials` replaces
the inherited environment list.

Use `--dry-run` to validate wrapper arguments and inherited defaults without
starting the hardware diagnostic. Dry-run mode does not require the
`evt_stream_smoke` target to have been built.

List discovered cameras without opening them:

```bash
orange-evt-stream-smoke --list-only
```

List discovered cameras with config match state:

```bash
orange-evt-stream-smoke \
  --config-dir /home/jeremy/orange_data/config/local/Fred \
  --list-only
```

Open/apply-config/start-stream/close for one camera:

```bash
orange-evt-stream-smoke \
  --config-dir /home/jeremy/orange_data/config/local/Fred \
  --serial 2012632
```

Also acquire one frame before closing:

```bash
orange-evt-stream-smoke \
  --config-dir /home/jeremy/orange_data/config/local/Fred \
  --serial 2012632 \
  --frames 1
```

For a Mono8 illumination check, request host buffers and brightness statistics:

```bash
orange-evt-stream-smoke \
  --config-dir /home/jeremy/orange_data/config/local/Fred \
  --serial 2012632 \
  --frames 3 \
  --gpu-direct 0 \
  --frame-stats
```

The per-frame summary reports mean/min/max intensity, the fraction below 8,
and the fraction at or above 250. This distinguishes successful camera/GPO
configuration from a filtered scene that remains dark because the physical
strobe, cable, or power path is not illuminating the view.

Measure raw EVT acquisition FPS outside the GUI/recorder/display path:

```bash
orange-evt-stream-smoke \
  --config-dir /home/jeremy/orange_data/config/local/Fred \
  --serial 2012632 \
  --measure-seconds 5 \
  --buffer-count 64
```

Try diagnostic overrides without editing the camera config:

```bash
orange-evt-stream-smoke \
  --config-dir /home/jeremy/orange_data/config/local/Fred \
  --serial 2012632 \
  --frame-rate 150 \
  --measure-seconds 5 \
  --buffer-count 64

orange-evt-stream-smoke \
  --config-dir /home/jeremy/orange_data/config/local/Fred \
  --serial 2012632 \
  --gpu-direct 0 \
  --measure-seconds 5 \
  --buffer-count 64
```

Capture link counters around the same smoke run:

```bash
scripts/orange_evt_stream_link_health.sh \
  --camera-ip 192.168.130.2 \
  --config-dir /home/jeremy/orange_data/config/local/Fred \
  --serial 2012632 \
  --measure-seconds 5 \
  --buffer-count 64
```

That writes a timestamped directory under `/tmp` containing the stream-smoke log
plus `ip`/`ethtool` before/after snapshots. For `Cam2012632` on
`mlnx1_p3_25g`, raw stream smoke accepted `FrameRate=250` but received only
about `160 fps` with frame-id gaps. The NIC was negotiated at `25000Mb/s`, MTU
`9000`, active FEC `RS`, but had PHY integrity counters such as
`rx_crc_errors_phy`, `rx_pcs_symbol_err_phy`, and lane-0 corrected/error
counters. That points at the physical/link path before Orange preprocessing,
recording, YOLO, or GUI display.

Confirmed `Cam2012632` outcome:

- With the original camera-side transceiver, the raw stream smoke received
  about `165 fps` from a camera configured/read back at `250 fps`, with hundreds
  of frame-id gaps and increasing PHY errors.
- Replacing the camera-side transceiver fixed the stream path. The validation
  artifact `/tmp/orange_evt_link_health_20260529_192407` reported
  `received=1250`, `fps=249.996`, `frame_id_gaps=0`, and zero deltas for
  `rx_crc_errors_phy`, `rx_pcs_symbol_err_phy`, `rx_corrected_bits_phy`,
  `rx_err_lane_0_phy`, `rx_fragments_phy`, and `rx_out_of_buffer`.
- Treat this failure mode as physical link quality first, not preprocessing,
  GPUDirect, GUI display, recording, or YOLO.

The diagnostic prints the loaded config path and key runtime fields including
`gpu_direct`, `source_gpu_id`, `sync_mode`, `lens_control_enabled`, and
`FrameRate configured/requested/applied_readback`. If the GUI stream reports about
`150 fps` while the config requests `250`, check this readback line first, then
run the timed acquisition measurement. Together they distinguish a camera-side
frame-rate limit or rejected setting from a later GUI/recording/display
bottleneck.

For cameras with no controllable lens attached, set:

```json
"lens_control_enabled": false
```

That avoids startup `Focus`/`Iris` node writes in `open_camera_with_params`.
`Cam2012632` opened its stream after those lens writes were disabled, so a
previous `EVT_ERROR_GVCP_ACK` at stream-open time was likely caused by lens-node
traffic during camera startup rather than by 25GbE bandwidth, GPUDirect, or
HEVC recording.
