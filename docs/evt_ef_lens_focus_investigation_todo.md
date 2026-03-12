# EVT EF Lens Focus Investigation TODO

## Scope
- Camera: `HB-20000SBM` (`2010096`)
- Lens (current validated run): `EF100mm f/2.8L Macro IS USM`
- Symptom: Iris control works, focus control does not respond as expected (or is limited to a partial range).

## Status Update (February 23, 2026)
- Confirmed on camera `2010096` that `Focus` is present but can initialize as `range=[0,0]`.
- Confirmed `Iris` is normal (`range=[0,56]`) while focus is degenerate.
- Confirmed UART nodes are present and writable (`UartEnable`, `UartBaud`, `UartDataBits`, `UartStopBits`, `GPO_3_Mode`).
- Confirmed that enabling UART path changes focus range to `range=[0,6276]`.
- Confirmed focus writes then move lens physically (`1000`, `3000`, `5000` targets tested).
- Measured slow focus transaction in one pre-bootstrap run (`~1049 ms`) versus iris (`~1 ms`), indicating focus path can be slow/stateful.

## Current Deployment State (Pre-Reboot)
- Updated live configs in `~/orange_data/config/local/60_4/`:
- `02010093.json`
- `02010094.json`
- `02010095.json`
- `02010096.json`
- All above now include `"focus_uart_bootstrap": true`.
- Next validation step after reboot: run app and confirm startup focus range is non-degenerate without manual probe bootstrap.

## Implemented In Repo
- Added `evt_lens_probe` utility in `tools/evt_lens_probe.cpp`.
- Added probe runbook in `docs/evt_lens_probe.md`.
- Added app-side focus bootstrap logic in `src/camera.cpp`.
- Added per-camera JSON flag `focus_uart_bootstrap` (default `false`) in `src/project.cpp` parsing and `CameraParams`.
- New app behavior: if focus range is degenerate, code polls briefly for natural readiness.
- New app behavior: if still degenerate and lens is present, code applies UART bootstrap (`GPO_3_Mode`, `UartEnable`, `UartBaud`, `UartDataBits`, `UartStopBits`).
- New app behavior: focus range is re-queried before applying focus values.
- Bootstrap logic is used from `update_focus_value`.
- Bootstrap logic is used from `update_camera_params`.

## Outcome Summary
- Root cause is now reproducible: focus can remain unavailable until UART-related lens path is initialized.
- Immediate mitigation is implemented in app startup/control path.
- Operational requirement for affected cameras: set `focus_uart_bootstrap: true` in the per-camera JSON config.
- Remaining validation is to confirm no regressions and acceptable behavior on other lens/camera combinations.

## Current Facts
- Repo focus writes go through `EVT_CameraSetUInt32Param(camera, "Focus", value)` in `src/camera.cpp`.
- Repo iris writes go through `EVT_CameraSetUInt32Param(camera, "Iris", value)` in `src/camera.cpp`.
- UI slider limits come from runtime node attributes (`FocusMin/Max/Inc`, `IrisMin/Max/Inc`) loaded in `update_camera_params` (`src/camera.cpp`).
- Startup path applies configured `focus` and `iris` immediately when opening a camera (`src/camera.cpp`, `open_camera_with_params`).
- Startup/default configuration does not currently set UART/GPIO nodes (`UARTEnable`, line mode, baud, etc.).
- EVT UART docs/examples indicate UART requires explicit enable/config (`UartEnable`, `UartBaud`, `UartDataBits`, `UartStopBits`) and line routing mode.
- EVT SDK headers/examples note focus-related writes can be slow enough to cause stale GVCP reply behavior if not handled carefully.

## Investigation Plan (Updated)

### 1. Reproduce and Capture Baseline
- [x] Reproduce in probe utility (`evt_lens_probe`) and record behavior.
- [ ] Reproduce in full `orange-jeremy` UI/client after app-side bootstrap change.
- [x] Record requested values and observed movement for `1000`, `3000`, `5000`.

### 2. Enumerate Actual Camera Feature Nodes
- [x] Dump relevant nodes with probe utility (`Focus`, `Iris`, `Lens*`, `Uart*`, `GPO*`, `GPI*`, `Line*`).
- [x] Capture type/value/range for discovered nodes.
- [ ] Expand to full XML node audit if needed for broader lens support.

### 3. Validate Prerequisites (Likely Missing Enable Path)
- [x] Verified runtime states (`LensMountPresent=true`, `LensPresent=true`, `LensBusy=false` during checks).
- [x] Verified focus range expands only after UART setup for this tested camera/lens.
- [x] Verified sequence includes `GPO_3_Mode=Test_Gen_Uart_Txd`.
- [x] Verified sequence includes `UartEnable=true`.
- [x] Verified sequence includes `UartBaud=B_9600`, `UartDataBits=8`, `UartStopBits=1`.
- [ ] Optional: validate UART loopback wiring test (`UartTxData`/`UartRxData`) if needed.

### 4. Instrument App for High-Signal Logging
- [ ] Add temporary logs around focus writes in `src/camera.cpp`:
- [ ] Requested value
- [ ] Runtime min/max/inc at write time
- [ ] Return code from `EVT_CameraSetUInt32Param`
- [ ] Readback value immediately after write
- [ ] Lens busy state before/after
- [ ] Add timing around focus command latency (to detect >2s behavior).
- [ ] If stale GVCP replies are observed, test SDK-recommended reply-buffer clearing after focus commands.

### 5. Isolate Startup Side Effects
- [x] Verified startup-state dependence via probe runs (fresh state can show `FocusMax=0`).
- [x] Added startup/control bootstrap mitigation in app code.
- [ ] Validate behavior with production JSON configs and UI workflows.

### 6. Execute Direct API Experiments (No UI Layer)
- [x] Built `evt_lens_probe` utility.
- [x] Supports open/probe, optional UART enable, target focus writes, latency logging.
- [x] Confirmed disabled-vs-enabled behavior difference on focus range.
- [ ] Add stepped sweep mode if deeper characterization is needed.

### 7. Decide Fix Path
- [x] Focus range is larger after prerequisite setup.
- [x] Added explicit init/re-query path in `src/camera.cpp`.
- [ ] If any residual latency issues appear, add targeted timeout/reply-buffer handling.

### 8. Validate and Lock In
- [x] Confirm usable focus travel in tested region (`1000`/`3000`/`5000`).
- [x] Confirm iris still behaves correctly.
- [ ] Confirm no regression on other camera/lens configs.
- [ ] Keep a saved diagnostic log from a passing run for future support tickets.

## Open Questions to Answer During Investigation
- Do any other Canon EF lenses on this mount require a different UART mode/baud/sequence?
- Should UART bootstrap be gated by config to avoid touching `GPO_3` on cameras that use it for other functions?
- Does any workflow still show slow focus commands after bootstrap in long-running sessions?

## References
- EVT GPIO and line control: https://docs.emergentvisiontec.com/camera-features/general-purpose-input-output-control-features
- EVT UART enable page: https://docs.emergentvisiontec.com/camera-features/uartenable
- Local EVT SDK example: `/opt/EVT/eSDK/Examples/EVT_GPIO/EVT_GPIO.cpp`
