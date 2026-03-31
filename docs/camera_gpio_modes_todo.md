# Camera GPIO Modes TODO

## Goal

Add config-driven GPIO and sync modes for camera startup so we can support more than the current PTP-gated multi-camera path.

This should let us describe how a camera is meant to acquire frames in config, instead of hardcoding behavior in separate GUI/headless paths.

## Current State

- Camera JSON config files only store image/lens settings plus a few flags such as `gpu_direct` and `focus_uart_bootstrap`.
- The app already records two timestamps per frame:
  - `timestamp`: camera/SDK frame timestamp (`frame_recv.timestamp`)
  - `timestamp_sys`: host `CLOCK_REALTIME` timestamp
- PTP does not create per-frame timestamps. It makes camera timestamps comparable across multiple cameras and adds a coordinated acquisition gate.
- GUI streaming only enables PTP when `PTP Stream Sync` is checked.
- Headless currently always forces PTP sync.
- There is no generic GPIO configuration layer in production code today.
- The only GPIO-adjacent startup behavior in normal runtime is the lens UART bootstrap, which touches `GPO_3_Mode` and `Uart*` nodes when enabled.

## Why This Matters

Right now the code mixes together:

- normal free-running acquisition
- PTP-based coordinated start
- ad hoc UART/GPO lens setup

That makes it hard to add external trigger or line-based synchronization cleanly. We need one explicit model for acquisition/sync mode and one explicit model for line/GPIO routing.

## Desired End State

Each camera config should be able to describe:

- how the camera acquires frames
- whether it uses free-run, PTP-gated start, or external GPIO trigger
- any required line/GPIO routing
- any trigger polarity / activation / selector settings
- optional UART/lens bootstrap settings that coexist with other GPIO usage

This needs to work consistently in:

- GUI streaming
- GUI recording
- headless recording
- recording snapshots and config save/load paths

## Proposed Configuration Shape

Exact field names can still change, but the config should move toward something like:

```json
{
  "sync_mode": "free_run",
  "trigger": {
    "enabled": false,
    "selector": "AcquisitionStart",
    "source": "Software",
    "activation": "RisingEdge"
  },
  "ptp": {
    "enabled": false,
    "mode": "TwoStep"
  },
  "gpio": {
    "lines": [
      {
        "name": "GPO_3_Mode",
        "type": "enum",
        "value": "Test_Gen_Uart_Txd"
      }
    ]
  }
}
```

Likely supported high-level modes:

- `free_run`
- `ptp_gate`
- `external_trigger`
- `software_trigger`

The high-level mode should drive the default acquisition/trigger behavior. A lower-level GPIO/node block can handle camera-specific line routing details.

## Implementation Tasks

### 1. Define the runtime model

- Add explicit acquisition/sync mode fields to `CameraParams`.
- Decide whether GPIO config should be:
  - a strongly typed struct for common cases
  - a generic node list
  - or a hybrid of both
- Keep the model compatible with saved JSON and recording snapshots.

### 2. Extend config load/save

- Update camera config parsing in `src/project.cpp`.
- Update config save in `save_camera_json_config()`.
- Ensure recording snapshots preserve the new fields as-is.
- Keep backward compatibility with existing config files that do not contain GPIO/sync fields.

### 3. Add a generic GenICam node apply layer

- Promote a small reusable helper into `src/` for:
  - node existence checks
  - enum writes
  - bool writes
  - uint writes
  - optional readback/logging
- Reuse patterns already present in `tools/evt_lens_probe.cpp`.
- Avoid spreading raw stringly-typed node writes throughout the app.

### 4. Refactor camera startup around explicit modes

- Split camera startup into:
  - base image/lens configuration
  - sync/trigger mode application
  - optional GPIO/UART routing
- Apply the chosen mode during `open_camera_with_params()` before streaming starts.
- Make mode application deterministic and logged.

### 5. Separate PTP mode from generic trigger mode

- Stop treating PTP as the only non-default sync path.
- Make it explicit that PTP mode configures:
  - `PtpMode`
  - gated start behavior
  - any related trigger settings
- Prevent silent conflicts between PTP and external trigger settings.

### 6. Unify GUI and headless behavior

- Stop hardcoding headless to always use PTP.
- Make headless honor camera config or an explicit requested mode.
- Keep GUI and headless semantics aligned.

### 7. Define GPIO conflict rules

- Decide what happens if:
  - UART bootstrap wants `GPO_3`
  - external trigger mode also wants that line
  - a config requests line settings incompatible with the selected sync mode
- Prefer explicit validation errors over silent overrides.

### 8. Add diagnostics and metadata

- Log the final applied sync/GPIO mode per camera at startup.
- Include applied mode details in recording metadata or `recording_snapshot.json`.
- If PTP is enabled, keep existing PTP diagnostics.
- Add startup warnings when requested nodes do not exist on a given camera model/firmware.

### 9. Add validation and tests

- Add unit-style coverage where practical for config parsing and serialization.
- Add a dry-run or probe path for validating GPIO node availability.
- Use the existing probe tool to verify target cameras expose the required nodes and enum values.
- Validate at least:
  - old config loads without new fields
  - new config saves round-trip cleanly
  - invalid mode combinations fail clearly

### 10. Document supported modes

- Add operator docs for:
  - free-run
  - PTP-gated multi-camera mode
  - external trigger mode
  - any required cabling / line routing assumptions
- Document which timestamp should be treated as authoritative in each mode.

## Open Questions

- Which exact EVT node names should define external trigger on our target cameras?
  - `TriggerSource`
  - `TriggerSelector`
  - `TriggerActivation`
  - `LineSelector`
  - `LineMode`
  - `GPI_*` / `GPO_*`
- Do all deployed camera models expose the same GPIO/line node set and enum values?
- Should GPIO config be generic per-node, or should we only support a curated subset of known-safe modes?
- Should `focus_uart_bootstrap` remain a standalone field or move under a broader GPIO/UART section?
- For single-camera recordings, should we explicitly document `timestamp` as the primary timing field and `timestamp_sys` as fallback/diagnostic?
- Do we need a UI surface for selecting sync mode, or is config-only sufficient for the first phase?

## Suggested Phase Order

### Phase 1: Schema and plumbing

- Add config fields and `CameraParams` support.
- Add load/save/snapshot support.
- Add startup logs with no behavior change yet.

### Phase 2: Mode refactor

- Refactor startup so free-run and PTP are explicit modes.
- Remove headless-only hardcoded PTP assumption.

### Phase 3: Generic GPIO application

- Add reusable node-setting helpers.
- Apply config-driven GPIO/line settings during startup.

### Phase 4: External trigger mode

- Implement validated external-trigger configuration for target cameras.
- Add docs and operational checks.

## Acceptance Criteria

- Existing configs continue to load unchanged.
- New configs can describe at least `free_run` and `ptp_gate`.
- Headless and GUI honor the same sync-mode semantics.
- Startup logs clearly show which mode and GPIO settings were applied.
- Invalid or conflicting GPIO/sync combinations fail with actionable errors.
- Recording snapshots preserve the new config fields.
