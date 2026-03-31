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
- `gpio_recipe`
- `trigger.enabled`
- `trigger.selector`
- `trigger.source`
- `trigger.activation`
- `ptp.mode`
- `gpio.nodes`

The UI also shows:

- `device_model_name` as a read-only device model line
- `device_serial_number` as a read-only device serial line
- recipe compatibility warnings
- a `Recipe Node Writes` section showing the exact node writes implied by the selected `gpio_recipe`

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
