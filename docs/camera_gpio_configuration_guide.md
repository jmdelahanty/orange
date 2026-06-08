# Camera GPIO Configuration Guide

## Scope

This note describes the GPIO and sync configuration behavior that is implemented today in `orange-jeremy`.

It complements:

- [camera_config_schema.md](/home/jeremy/orange-jeremy/docs/camera_config_schema.md)
- [evt_gpio_pinouts.md](/home/jeremy/orange-jeremy/docs/evt_gpio_pinouts.md)

## Current Operator Workflow

The current GUI path is:

1. Open a camera in the app.
2. Open `Camera Property`.
3. Edit the schema-backed sync and GPIO fields.
4. Save the camera config JSON.
5. Re-open the camera to apply the new sync and GPIO settings.

Important runtime note:

- Image and lens controls such as width, height, gain, exposure, focus, and iris still apply live.
- Sync and GPIO settings are currently treated as config-on-save settings.
- The GUI explicitly says these settings are applied the next time the camera is opened.

## Fields Exposed In The Camera Property UI

The `Camera Property` panel currently exposes:

- `focus_uart_bootstrap`
- `sync_mode`
- `camera_scan_type`
- `gpio_connector_variant`
- `gpio_pinout_access`
- `gpio_recipe`
- `trigger.enabled`
- `trigger.selector`
- `trigger.source`
- `trigger.activation`
- `ptp.mode`
- `gpio.nodes`
- `rig_io.connections`

The UI also shows:

- `device_model_name` as a read-only device model line
- `device_serial_number` as a read-only device serial line
- recipe compatibility warnings
- a `Recipe Node Writes` section showing the exact node writes implied by the selected `gpio_recipe`
- a `Rig I/O Mapping` section for per-camera rig wiring metadata

`gpio_connector_variant` and `gpio_pinout_access` answer different questions:

- `gpio_connector_variant` is the camera-side connector family, such as
  `area_scan_12_pin`.
- `gpio_pinout_access` is whether the installed cable, power supply, or
  breakout module exposes those GPIO pins to the rig. Use `exposed`,
  `not_exposed`, or `unknown`.

A camera can be an area-scan 12-pin model while the installed power module is
power-only or otherwise hides the TTL wires. In that case keep the connector
variant as `area_scan_12_pin`, but set `gpio_pinout_access = "not_exposed"` and
do not create physical TTL mappings for that camera.

When `gpio_pinout_access = "not_exposed"`, the Camera Property UI locks:

- `gpio_recipe`
- `gpio.nodes`
- Rig I/O mapping edits and diagnostic GPO actions

Changing a camera to `not_exposed` clears operational `gpio_recipe` and
`gpio.nodes` entries. Existing Rig I/O metadata is not auto-deleted, but it is
not editable or diagnosable until access is changed back to `exposed`.
Saving a `not_exposed` config also canonicalizes stale loaded values by writing
an empty `gpio_recipe` and empty `gpio.nodes`.

## PTP Behavior

`sync_mode` is the authoritative mode selector.

Current meanings:

- `free_run`: normal trigger-off startup unless a recipe or explicit trigger block changes that behavior
- `ptp_gate`: existing PTP gated-start flow
- `external_trigger`: explicit trigger and GPIO-driven sync
- `software_trigger`: explicit software-trigger setup

`ptp.mode` is now optional.

Current behavior:

- If `sync_mode != "ptp_gate"`, the UI allows `PTP Mode = (unset)`.
- Non-PTP configs save `ptp` as `{ "enabled": false }` with no `mode`.
- If `sync_mode = "ptp_gate"` and `ptp.mode` is unset, runtime falls back to `TwoStep`.

## Recipe Expansion

Recipes are not just labels. Each recipe expands to an explicit list of GenICam node writes.

The UI now shows these writes under `Recipe Node Writes`.

### `area_scan_hw_trigger_internal_gpi4`

Requires:

- `camera_scan_type = "area_scan"`
- `gpio_connector_variant = "area_scan_12_pin"`

Expands to:

- `AcquisitionMode = MultiFrame`
- `TriggerMode = On`
- `TriggerSource = Hardware`
- `GPI_Start_Exp_Mode = GPI_4`
- `GPI_Start_Exp_Event = Rising_Edge`
- `GPI_4_Debounce_Count = 50`
- `GPI_End_Exp_Mode = Internal`

This is the current recipe for the common area-scan hardware-trigger case where exposure starts on `GPI_4` rising edge and ends internally.

### `area_scan_hw_trigger_external_gpi4`

Requires:

- `camera_scan_type = "area_scan"`
- `gpio_connector_variant = "area_scan_12_pin"`

Expands to:

- `AcquisitionMode = MultiFrame`
- `TriggerMode = On`
- `TriggerSource = Hardware`
- `GPI_Start_Exp_Mode = GPI_4`
- `GPI_Start_Exp_Event = Rising_Edge`
- `GPI_4_Debounce_Count = 50`
- `GPI_End_Exp_Mode = GPI_4`
- `GPI_End_Exp_Event = Falling_Edge`

### `line_scan_hw_frame_gpi1_internal_line`

Requires:

- `camera_scan_type = "line_scan"`
- `gpio_connector_variant = "line_scan_12_pin"`

Expands to:

- `TriggerMode = On`
- `TriggerSource = Hardware`
- `GPI_Start_Frame_Mode = GPI_1`
- `GPI_Start_Frame_Event = Rising_Edge`
- `GPI_1_Debounce_Count = 50`
- `LineTime = 1000`

### `line_scan_hw_frame_gpi1_encoder_line`

Requires:

- `camera_scan_type = "line_scan"`
- `gpio_connector_variant = "line_scan_12_pin"`

Expands to:

- `TriggerMode = On`
- `TriggerSource = Hardware`
- `GPI_Start_Frame_Mode = GPI_1`
- `GPI_Start_Frame_Event = Rising_Edge`
- `GPI_1_Debounce_Count = 50`
- `GP_ENC_MODE = true`
- `GP_ENC_LINE_Multiplier = 1`
- `GP_ENC_LINE_DIVIDER = 4`

### `line_scan_encoder_frame_encoder_line`

Requires:

- `camera_scan_type = "line_scan"`
- `gpio_connector_variant = "line_scan_12_pin"`

Expands to:

- `TriggerMode = On`
- `TriggerSource = Hardware`
- `GPI_Start_Frame_Event = Encoder_Frame_Divider`
- `GP_ENC_MODE = true`
- `GP_ENC_LINE_Multiplier = 1`
- `GP_ENC_LINE_DIVIDER = 4`
- `GP_ENC_FRAME_DIVIDER = 24000`

### `line_scan_hw_gate_gpi1_encoder_frame_encoder_line`

Requires:

- `camera_scan_type = "line_scan"`
- `gpio_connector_variant = "line_scan_12_pin"`

Expands to:

- `TriggerMode = On`
- `TriggerSource = Hardware`
- `GPI_Start_Frame_Mode = GPI_1`
- `GPI_Start_Frame_Event = Pulse_High`
- `GPI_1_Debounce_Count = 50`
- `GP_ENC_MODE = true`
- `GP_ENC_LINE_Multiplier = 1`
- `GP_ENC_LINE_DIVIDER = 4`
- `GP_ENC_FRAME_DIVIDER = 24000`

## `gpio.nodes` Override Behavior

`gpio.nodes` is applied after `gpio_recipe`.

That means:

- the recipe provides a known starting point
- explicit `gpio.nodes` entries can override individual recipe values

The UI reflects this by showing:

- `Recipe Node Writes` first
- then the editable `GPIO Nodes` list

## `rig_io.connections` Metadata

`rig_io.connections` is per-camera rig wiring metadata. It is not a GenICam
programming surface.

Use it to record facts such as:

- `purpose = "nir_strobe_trigger"`
- `direction = "output"`
- `camera_line = "GPO_0"`
- `physical_pin = 7` for the common area-scan 12-pin `GPO_0` output
- `reference_line = "GND"`
- `reference_pin = 8` or `9` for the common area-scan 12-pin ground return,
  depending on the cable wiring
- `electrical = "ttl_0_5v"`
- `active_level = "high"`
- `normal_output_mode = "Exposure"`
- `normal_polarity = false`
- `controlled_device = "near_infrared_strobe"`
- `nominal_wavelength_nm = 855.0`

Only record `physical_pin` / `reference_pin` when both are true:

- the camera-side `gpio_connector_variant` is known;
- the installed power supply, cable, or breakout exposes those pins
  (`gpio_pinout_access = "exposed"`).

If the camera has the connector but the current power module does not expose
GPIO wires, leave the physical pin mapping absent and set
`gpio_pinout_access = "not_exposed"`.

Current behavior:

- Orange loads, edits, saves, and snapshots the mapping with the camera config.
- Orange does not use `rig_io.connections` to write GenICam nodes.
- The curated `Add NIR strobe mapping` shortcut requires a known compatible
  area-scan connector and `gpio_pinout_access = "exposed"`.
- Rig I/O editing and live diagnostics are disabled when
  `gpio_pinout_access = "not_exposed"`.
- Orange has a diagnostic-only manual GPO test for output mappings. It resolves
  `camera_line = "GPO_N"` and writes `GPO_N_Mode = GPO` plus
  `GPO_N_Polarity` for active/inactive testing.
- The diagnostic UI can first capture the current `GPO_N_Mode` and
  `GPO_N_Polarity`, then restore those values after the manual test. This is
  useful for discovering and recovering the camera's exposure-synchronized
  strobe mode without power-cycling.
- For `purpose = "nir_strobe_trigger"`, the UI also exposes named controls:
  - `Suppress mapped NIR strobe`: captures the current state if needed, then
    forces manual inactive output.
  - `Restore mapped pulse mode`: restores the captured state when available,
    otherwise uses `normal_output_mode` and `normal_polarity`.
- These live output controls are disabled while recording or finalization is
  active. The broader camera property panel also locks live camera mutations in
  that state, with focus intended as the only live-adjustable exception.
- On the current `Cam2010096` wiring, oscilloscope testing showed the normal
  exposure-pulse state is `GPO_0_Mode = Exposure` and
  `GPO_0_Polarity = false`.
- That diagnostic is not a full runtime strobe controller. It may replace a
  camera-generated pulse mode with manual GPO mode, so first use should be with
  streaming stopped. Restore the captured GPO state, or re-open/reapply the
  camera config, before normal experiments if the line normally pulses.
- `verified = true` should be set only after the physical cable/pin mapping has
  been checked for that camera/rig.
- The curated `Add NIR strobe mapping` UI shortcut is enabled only when the
  selected camera has a known area-scan GPIO connector pinout. Orange should not
  invent `physical_pin` or `reference_pin` values for cameras whose connector
  pinout is unknown or not applicable. Blank mappings remain available as
  advanced metadata, but pins should be left unset unless externally verified.

This gives us a pragmatic per-camera platform for documenting actual rig wiring
now. A later rig-level schema can ingest or reference these entries without
changing the current camera config contract.

## Current Validation Rules

Current validation is intentionally strict:

- `gpio_recipe` and `sync_mode = "ptp_gate"` are treated as conflicting
- area-scan `GPI_4` recipes require `area_scan_12_pin`
- line-scan recipes require `line_scan_12_pin`
- `gpio.nodes` entries must have:
  - a non-empty `name`
  - a supported `type` of `enum`, `bool`, or `uint`
  - a valid type-matching value

Missing GenICam nodes at runtime are treated as configuration errors, not silent no-ops.

## Example: Area-Scan GPI4 Internal-End Trigger

If the goal is:

- `GPI_Start_Exp_Mode = GPI_4`
- `GPI_Start_Exp_Event = Rising_Edge`
- `GPI_End_Exp_Mode = Internal`

Then the intended configuration is:

- `sync_mode = "external_trigger"`
- `camera_scan_type = "area_scan"`
- `gpio_connector_variant = "area_scan_12_pin"`
- `gpio_recipe = "area_scan_hw_trigger_internal_gpi4"`

In the current UI this appears as:

- `Sync Mode = External Trigger`
- `Camera Scan Type = Area Scan`
- `GPIO Connector Variant = Area Scan 12-pin`
- `GPIO Recipe = Area scan HW trigger GPI4, internal end`

The `Recipe Node Writes` section should then display the exact GPI4 node writes listed above.

## Known Boundaries

- There is still no curated area-scan 8-pin GPIO recipe.
- Recipe application happens during camera open, not hot-reload while streaming.
- The UI is currently a config editor, not a live GenICam GPIO inspector.
- `gpio.nodes` remains a generic advanced escape hatch and is only as safe as the node names and values supplied by the operator.
- `rig_io.connections` is metadata only; it intentionally does not imply that
  Orange has production live control of the mapped light source or device.
