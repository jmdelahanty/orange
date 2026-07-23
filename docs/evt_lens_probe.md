# `evt_lens_probe`

`evt_lens_probe` is a standalone diagnostic utility for EVT cameras that helps answer:
- Is the camera exposing usable GenICam `Focus`/`Iris` nodes?
- Are UART/GPIO lens communication nodes present and writable?
- Does behavior change after enabling UART-related settings?

Use this tool to gather evidence for the EF focus issue before changing app code.

Related investigation checklist:
- `docs/evt_ef_lens_focus_investigation_todo.md`

## Confirmed Findings (February 23, 2026)
- Camera `2010096` (`HB-20000SBM`) with Canon `EF100mm f/2.8L Macro IS USM` was validated.
- Fresh open can show `Focus range=[0,0]` while `Iris` remains normal.
- After UART bootstrap (`GPO_3_Mode=Test_Gen_Uart_Txd`, `UartEnable=true`, `B_9600`, `8N1`), focus range becomes `range=[0,6276]`.
- Focus target writes (`1000`, `3000`, `5000`) succeeded with readback and observed physical movement.
- One pre-bootstrap focus transaction showed ~1s latency; post-bootstrap target writes were effectively immediate in the probe run.

## Current Deployment State (Pre-Reboot)
- Live camera configs under `~/orange_data/config/local/60_4/` have been updated to include `"focus_uart_bootstrap": true`.
- Files updated:
- `02010093.json`
- `02010094.json`
- `02010095.json`
- `02010096.json`
- After reboot, validate in app runtime that focus initializes with non-degenerate range and no manual UART enable step.

## Changes Already Applied In App
- App startup/control now includes a focus-range readiness bootstrap in `src/camera.cpp`.
- If focus range is degenerate, it polls briefly, then attempts UART bootstrap when lens/mount are present, and re-queries focus range.
- This logic is used in `update_focus_value`.
- This logic is used in `update_camera_params`.
- Bootstrap is gated per camera config JSON with `focus_uart_bootstrap` (boolean).
- Operational note: when enabled, bootstrap can touch `GPO_3_Mode`.

## Per-Camera JSON Flag

Add this field to a camera config JSON:

```json
"focus_uart_bootstrap": true
```

Behavior:
- `true`: app may apply UART bootstrap (`GPO_3_Mode`, `UartEnable`, `UartBaud`, `UartDataBits`, `UartStopBits`) if focus range is degenerate.
- `false` (default): app will not apply UART bootstrap; it only polls briefly for natural readiness.

For the validated camera (`2010096`), set this to `true` in the camera config currently used by the app
(for example in your local setup under `~/orange_data/config/local/60_4/`).

Example snippet:

```json
{
  "name": "Cam4",
  "device_serial_number": "2010096",
  "focus": 5000,
  "iris": 5,
  "focus_uart_bootstrap": true
}
```

## What It Does

At runtime the utility:
- Discovers EVT cameras with `EVT_ListDevices`.
- Opens a selected camera (by `--serial` or `--index`).
- Probes key lens nodes (`Focus`, `Iris`, `Lens*`) and UART/GPIO nodes (`Uart*`, `GPO_*`, `GPI_*`, `Line*`).
- Prints node type, current value, and range/inc where available.
- Optionally exercises `Focus`/`Iris` with write-readback and timing.
- Optionally applies UART configuration and runs loopback (`UartTxData` -> `UartRxData`).
- Optionally restores `GPO_0` to the Cam2010096 active-low exposure-pulse
  strobe mode and requires matching camera readback.
- Prints a summary (`PASS`/`FAIL`/`NOT_RUN`) for each probe class.

## Build

```bash
cmake -S . -B build
cmake --build build --target evt_lens_probe -j8
```

Show full CLI options:

```bash
./build/evt_lens_probe --help
```

## Quick Start (Recommended Order)

1. List cameras and confirm serial.

```bash
LD_LIBRARY_PATH=/opt/EVT/eSDK/lib:${LD_LIBRARY_PATH} ./build/evt_lens_probe --list-only
```

2. Run read-only probe for your camera.

```bash
LD_LIBRARY_PATH=/opt/EVT/eSDK/lib:${LD_LIBRARY_PATH} \
  ./build/evt_lens_probe --serial 2010096
```

3. Exercise GenICam lens writes without changing targets.

```bash
LD_LIBRARY_PATH=/opt/EVT/eSDK/lib:${LD_LIBRARY_PATH} \
  ./build/evt_lens_probe --serial 2010096 --exercise-genicam
```

4. Apply UART setup and compare node/readback behavior.

```bash
LD_LIBRARY_PATH=/opt/EVT/eSDK/lib:${LD_LIBRARY_PATH} \
  ./build/evt_lens_probe --serial 2010096 --enable-uart
```

5. Optional: run UART loopback if hardware wiring is present.

```bash
LD_LIBRARY_PATH=/opt/EVT/eSDK/lib:${LD_LIBRARY_PATH} \
  ./build/evt_lens_probe --serial 2010096 --enable-uart --uart-loopback
```

6. Optional: attempt explicit focus write target.

```bash
LD_LIBRARY_PATH=/opt/EVT/eSDK/lib:${LD_LIBRARY_PATH} \
  ./build/evt_lens_probe --serial 2010096 --focus-target 5000
```

7. Recover the mapped NIR strobe after an interrupted calibration workflow.
   This mutates camera state and therefore requires an explicit serial:

```bash
LD_LIBRARY_PATH=/opt/EVT/eSDK/lib:${LD_LIBRARY_PATH} \
  ./build/evt_lens_probe --serial 2010096 --restore-nir-strobe-pulse
```

Success requires readback of `GPO_0_Mode = Exposure` and
`GPO_0_Polarity = false`; a successful write without matching readback is a
failure.

## Run Across Multiple Cameras

Use the helper script:

```bash
scripts/run_evt_lens_probe_all.sh -- --enable-uart
```

With additional probe flags:

```bash
scripts/run_evt_lens_probe_all.sh -- --enable-uart --exercise-genicam
```

Limit to specific serials:

```bash
scripts/run_evt_lens_probe_all.sh --serials 02010093,02010094 -- --enable-uart
```

Outputs:
- Per-camera logs in `/tmp/evt_lens_probe/` by default.
- A summary file with pass/fail for each serial.

## Important CLI Note
- Flags must be on the same command line as `evt_lens_probe`.
- If `--serial` or `--index` is entered on a separate line, the tool falls back to default index `0` and shell will print `command not found`.

## Validation Procedure After App Patch

Use this to verify the `src/camera.cpp` focus-bootstrap behavior is working as intended.

1. Build and run probe baseline on target camera:

```bash
export LD_LIBRARY_PATH=/opt/EVT/eSDK/lib:${LD_LIBRARY_PATH}
./build/evt_lens_probe --index 3
```

2. Confirm focus is usable after bootstrap path:

```bash
./build/evt_lens_probe --index 3 --enable-uart --keep-uart-config
./build/evt_lens_probe --index 3 --focus-target 1000
./build/evt_lens_probe --index 3 --focus-target 3000
./build/evt_lens_probe --index 3 --focus-target 5000
```

Expected:
- `Focus` range is non-degenerate (for validated case: `0..6276`).
- Focus target writes return `PASS` with matching readback.
- Physical focus movement is observed.

3. Validate app startup behavior (`orange_client` / full app):
- Start the app with camera `2010096` config.
- Watch startup logs from `src/camera.cpp` for focus-bootstrap messages:
- `Focus range is degenerate. Attempting UART lens bootstrap.`
- `Focus range after UART bootstrap: [min,max]`
- Confirm focus controls in app no longer stay stuck at `0..0`.

4. Regression spot-check:
- Verify iris behavior remains correct.
- Verify no unexpected side effects if `GPO_3` is needed for another workflow.

## Interpreting Output

Important sections:
- `[Lens/Focus Nodes]`: confirms whether `Focus`/`Iris` exist and what ranges are reported.
- `[UART/GPIO Nodes]`: confirms whether `UartEnable`, `UartBaud`, etc. exist for this camera/firmware.
- `[GenICam Focus/Iris Exercise]`: reports set/readback success and command latency.
- `[Summary]`: compact status view for each test category.

If `Focus` node is present but writes/readbacks fail or mismatch, treat that as strong evidence of a control-path issue (timing/state/feature gating).  
If behavior improves after `--enable-uart`, prioritize adding explicit UART setup in camera initialization.

## Safety/Behavior Notes

- By default, when `--enable-uart` is used, the tool restores original `UartEnable` and `GPO_3_Mode` on exit.
- Use `--keep-uart-config` only when you intentionally want UART settings to persist after the run.
- `--uart-loopback` assumes EVT-style external loopback wiring and sends bytes `0x30` to `0x39`.
