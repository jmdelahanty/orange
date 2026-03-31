# Camera Config Schema

## Identifier

- `schema_id = "orange.camera.config"`
- `schema_version = 1`

## Compatibility

- Legacy camera configs without schema metadata still load.
- Saving a camera config rewrites it into schema version 1.
- Explicit config metadata wins. If `camera_scan_type` or `gpio_connector_variant` is missing, Orange tries to infer it from `device_model_name`.
- The current operator-facing behavior and recipe expansions are documented in [camera_gpio_configuration_guide.md](/home/jeremy/orange-jeremy/docs/camera_gpio_configuration_guide.md).

## Top-Level Fields

Existing image and lens fields remain at the top level:

- `name`
- `width`
- `height`
- `frame_rate`
- `gain`
- `exposure`
- `pixel_format`
- `color_temp`
- `gpu_id`
- `gpu_direct`
- `focus_uart_bootstrap`
- `color`
- `focus`
- `iris`
- `offset_x`
- `offset_y`

Schema and GPIO-related fields:

- `schema_id`
- `schema_version`
- `device_model_name`
- `device_serial_number`
- `camera_scan_type`
- `gpio_connector_variant`
- `gpio_recipe`
- `sync_mode`
- `trigger`
- `ptp`
- `gpio`

## Camera Metadata

### `device_model_name`

- Free-form model string from the camera, for example `HB-7000SC`.
- This is the canonical on-disk model field in schema v1.
- Orange still maps it into the internal runtime `device_model` field.

### `device_serial_number`

- Canonical camera serial string for the physical device.
- Numeric serials should be stored without leading zeros.
- Orange preserves this field in schema v1 for compatibility with older config sets and offline tooling.
- Config lookup now prefers `device_serial_number` from the JSON body and only falls back to the filename stem if the field is missing.
- When loading legacy configs, Orange can fall back to `device_serial_number` if runtime camera metadata is not already attached.

### `camera_scan_type`

Supported values:

- `area_scan`
- `line_scan`
- `unknown`

Notes:

- This is a logical camera class, not a physical connector description.
- If omitted, Orange infers it from `device_model_name` when possible.

### `gpio_connector_variant`

Supported values:

- `area_scan_12_pin`
- `area_scan_8_pin`
- `line_scan_12_pin`
- `unknown`

Notes:

- This is the physical GPIO connector family used for recipe validation.
- If omitted, Orange infers it from `camera_scan_type` and `device_model_name` when possible.

### Current Inference Rules

Current runtime inference is intentionally conservative:

- If `device_model_name` is one of `HB-65000GM`, `HB-65000GC`, `HB-7000SC`, or `HB-7000SM`, infer `camera_scan_type = "area_scan"`.
- If `device_model_name` begins with an EVT area-scan family prefix such as `HB`, `HZ`, `HR`, `HT`, or `HE`, infer `camera_scan_type = "area_scan"`.
- If `device_model_name` begins with an EVT line-scan family prefix such as `LB`, `TLB`, `LR`, `TLR`, `LT`, `LZ`, or `TLZ`, infer `camera_scan_type = "line_scan"`.
- If `device_model_name` contains `Eros`, infer `camera_scan_type = "area_scan"` and `gpio_connector_variant = "area_scan_8_pin"`.
- If `device_model_name` begins with `HE`, infer `gpio_connector_variant = "area_scan_8_pin"`.
- If `camera_scan_type = "line_scan"` and no connector is set, infer `gpio_connector_variant = "line_scan_12_pin"`.
- If `camera_scan_type = "area_scan"` and no connector is set, infer `gpio_connector_variant = "area_scan_12_pin"` unless the model matches `Eros`.

The machine-readable mapping lives in [camera_type_pinout_map.json](/home/jeremy/orange-jeremy/docs/camera_type_pinout_map.json).

## `sync_mode`

Supported values:

- `free_run`
- `ptp_gate`
- `external_trigger`
- `software_trigger`

Current runtime behavior:

- `free_run`: uses the normal trigger-off startup path unless a `gpio_recipe` or explicit trigger block changes that behavior.
- `ptp_gate`: participates in the existing PTP gated-start flow.
- `external_trigger` and `software_trigger`: use the explicit `trigger`, `gpio_recipe`, and `gpio.nodes` settings you provide.

## `gpio_recipe`

Supported values:

- `""` or omitted
- `area_scan_hw_trigger_internal_gpi4`
- `area_scan_hw_trigger_external_gpi4`
- `line_scan_hw_frame_gpi1_internal_line`
- `line_scan_hw_frame_gpi1_encoder_line`
- `line_scan_encoder_frame_encoder_line`
- `line_scan_hw_gate_gpi1_encoder_frame_encoder_line`

Notes:

- Recipes are validated against `camera_scan_type` and `gpio_connector_variant`.
- Recipes currently reject `sync_mode = "ptp_gate"`.
- Recipes apply before `gpio.nodes`.
- `gpio.nodes` then applies afterward and can override individual recipe defaults.
- There is intentionally no area-scan 8-pin recipe yet. The vendor pinout does not expose the same `GPI_4` path used by the 12-pin area-scan trigger examples.

## `trigger`

```json
"trigger": {
  "enabled": false,
  "selector": "AcquisitionStart",
  "source": "Software",
  "activation": "RisingEdge"
}
```

Notes:

- If `enabled = false`, no generic trigger block is applied.
- If `enabled = true`, Orange applies the trigger selector, source, activation, and enables `TriggerMode`.
- If a `gpio_recipe` is active, the recipe is treated as the primary trigger programming path instead of the generic trigger block.
- `ptp_gate` manages its own runtime gating and does not use the generic trigger block as the primary control path.

## `ptp`

```json
"ptp": {
  "enabled": false
}
```

Notes:

- `sync_mode` is the authoritative mode selector.
- `ptp.enabled` is stored for readability and backward compatibility.
- `ptp.mode` is used only when `sync_mode = "ptp_gate"`.
- For non-PTP configs, `ptp.mode` may be omitted entirely.

## `gpio`

```json
"gpio": {
  "nodes": [
    {
      "name": "LineSelector",
      "type": "enum",
      "value": "Line1"
    },
    {
      "name": "LineInverter",
      "type": "bool",
      "value": false
    },
    {
      "name": "LineDebouncerTime",
      "type": "uint",
      "value": 10
    }
  ]
}
```

Supported node types in schema version 1:

- `enum`
- `bool`
- `uint`

Runtime behavior:

- Each requested node is applied by name during camera open.
- Missing nodes or failed writes are treated as configuration errors.
- `gpio.nodes` is generic and camera-specific; recipe validation does not guarantee that ad hoc node writes are valid on every model.

## Example

```json
{
  "schema_id": "orange.camera.config",
  "schema_version": 1,
  "device_model_name": "HB-7000SC",
  "device_serial_number": "2002496",
  "camera_scan_type": "area_scan",
  "gpio_connector_variant": "area_scan_12_pin",
  "gpio_recipe": "",
  "name": "Cam0",
  "width": 3208,
  "height": 2200,
  "frame_rate": 30,
  "gain": 1500,
  "iris": 0,
  "focus": 345,
  "exposure": 2500,
  "pixel_format": "BayerRG8",
  "gpu_id": 0,
  "color_temp": "CT_3000K",
  "gpu_direct": false,
  "focus_uart_bootstrap": false,
  "color": true,
  "offset_x": 0,
  "offset_y": 0,
  "sync_mode": "free_run",
  "trigger": {
    "enabled": false,
    "selector": "AcquisitionStart",
    "source": "Software",
    "activation": "RisingEdge"
  },
  "ptp": {
    "enabled": false
  },
  "gpio": {
    "nodes": []
  }
}
```
