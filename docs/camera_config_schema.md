# Camera Config Schema

## Identifier

- `schema_id = "orange.camera.config"`
- `schema_version = 4`

## Compatibility

- Legacy camera configs without schema metadata still load.
- Saving a camera config rewrites it into schema version 4.
- `source_gpu_id` is the schema-v4 GPU placement field. Legacy configs may use
  `gpu_id`; Orange accepts it as an alias while loading, but saved configs use
  `source_gpu_id`.
- Explicit config metadata wins. If `camera_scan_type` or `gpio_connector_variant` is missing, Orange tries to infer it from `device_model_name`.
  `gpio_pinout_access` is not inferred because it depends on the installed
  cable, power supply, or breakout module rather than the camera model.
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
- `source_gpu_id`
- `gpu_direct`
- `focus_uart_bootstrap`
- `lens_control_enabled`
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
- `gpio_pinout_access`
- `gpio_recipe`
- `sync_mode`
- `trigger`
- `ptp`
- `gpio`
- `rig_io`
- `optics`
- `recording`
- `crop_pipeline`

## Camera Metadata

### `device_model_name`

- Free-form model string from the camera, for example `HB-7000SC`.
- This is the canonical on-disk model field in schema v3.
- Orange still maps it into the internal runtime `device_model` field.

### `device_serial_number`

- Canonical camera serial string for the physical device.
- Numeric serials should be stored without leading zeros.
- Orange preserves this field in schema v3 for compatibility with older config sets and offline tooling.
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

- This is the camera-side GPIO connector family used for recipe validation.
- If omitted, Orange infers it from `camera_scan_type` and `device_model_name` when possible.
- This does not mean the installed power supply, cable, or breakout exposes the
  GPIO pins to the operator.

### `gpio_pinout_access`

Supported values:

- `exposed`
- `not_exposed`
- `unknown`

Notes:

- This records whether the installed rig wiring exposes the camera GPIO pins.
- `gpio_connector_variant` describes what the camera supports.
  `gpio_pinout_access` describes what this physical installation can actually
  access.
- Use `exposed` only when the cable, power supply, or breakout module exposes
  the relevant GPIO pin and reference/ground pin.
- Use `not_exposed` when the camera has the connector family but the installed
  power module/cable is power-only or otherwise hides the GPIO wires.
- Use `unknown` until the installed wiring has been checked.
- Curated rig-I/O shortcuts, such as the default NIR strobe mapping, require
  both a known compatible connector and `gpio_pinout_access = "exposed"`.
- When `gpio_pinout_access = "not_exposed"`, Orange treats physical GPIO
  recipe/node configuration as invalid. The UI locks GPIO recipe selection,
  explicit `gpio.nodes`, and Rig I/O editing/diagnostics in that state.
- Saving a `not_exposed` camera config canonicalizes operational GPIO writes by
  writing an empty `gpio_recipe` and empty `gpio.nodes`. Orange does not
  silently delete `rig_io.connections`, because those entries are descriptive
  metadata that may document a previous or alternate cable/breakout setup.

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

Supported node types in schema version 3 and later:

- `enum`
- `bool`
- `uint`

Runtime behavior:

- Each requested node is applied by name during camera open.
- Missing nodes or failed writes are treated as configuration errors.
- `gpio.nodes` is generic and camera-specific; recipe validation does not guarantee that ad hoc node writes are valid on every model.

## `rig_io`

```json
"rig_io": {
  "schema_id": "orange.camera.rig_io",
  "schema_version": 1,
  "connections": [
    {
      "purpose": "nir_strobe_trigger",
      "direction": "output",
      "camera_line": "GPO_0",
      "physical_pin": 7,
      "reference_line": "GND",
      "reference_pin": 8,
      "electrical": "ttl_0_5v",
      "active_level": "high",
      "inactive_level": "low",
      "normal_output_mode": "Exposure",
      "normal_polarity": false,
      "controlled_device": "near_infrared_strobe",
      "nominal_wavelength_nm": 855.0,
      "verified": false,
      "notes": "Per-camera metadata only; does not actuate the strobe."
    }
  ]
}
```

Notes:

- `rig_io` is optional per-camera rig wiring metadata.
- This is intentionally separate from `gpio.nodes`.
- `gpio.nodes` describes GenICam node writes that Orange applies during camera
  open. `rig_io.connections` describes how a camera line is physically wired in
  the rig.
- Orange currently loads, edits, saves, and snapshots this metadata, but does
  not actuate GPIO or strobe state from it.
- Store one entry per meaningful per-camera connection, such as a camera
  `GPO_0` output wired to an NIR strobe trigger.
- `physical_pin` is the connector pin number for the configured
  `gpio_connector_variant` when known and exposed by the installed wiring.
- `reference_line` and `reference_pin` describe the return/reference side of
  the same physical connection. For the common 12-pin area-scan `GPO_0` TTL
  output, `GPO_0` is pin `7` and normal `GND` is available on pins `8` and
  `9`; record the one actually used by the cable.
- Do not invent `physical_pin` or `reference_pin` for cameras whose connector
  pinout is unavailable or whose installed power/cable module does not expose
  those wires. Leave those fields absent or set the mapping aside until the
  wiring is externally verified.
- `normal_output_mode` and `normal_polarity` describe the state Orange should
  restore after temporary manual suppression. For the current `Cam2010096`
  NIR-strobe wiring, oscilloscope testing showed normal exposure-synchronized
  pulses use `GPO_0_Mode = Exposure` and `GPO_0_Polarity = false`.
- `nominal_wavelength_nm` is optional and is intended for light sources such as
  NIR strobes. Visible broadband sources may omit it and describe the source in
  `controlled_device` or `notes`.
- `verified = true` should mean an operator or rig maintainer has checked the
  mapping against the actual cable/pinout.

## `recording`

```json
"recording": {
  "preferred_sink_mode": "external_ipc",
  "profile_name": "fred_single_session_hevc_250fps_p1",
  "encode": {
    "codec": "hevc",
    "preset": "p1",
    "gop_length": 25
  }
}
```

Notes:

- `preferred_sink_mode` is a camera-profile preference for GUI recording
  sessions. Supported values are `real`, `external_ipc`, `default`, `auto`,
  `app`, or an empty string.
- `default`, `auto`, `app`, and empty string mean no camera-level preference.
- Environment still wins: `ORANGE_GUI_RECORDING_SINK_MODE` overrides camera and
  app config.
- Explicit app config still wins: `recording.sink_mode` in the app config
  overrides camera preferences.
- If no environment or app-level sink is set, the GUI resolves the session sink
  from the selected cameras. Any selected recording camera that prefers
  `external_ipc` makes the session use `external_ipc`.
- Use `preferred_sink_mode = "external_ipc"` for high-throughput camera
  profiles where process-isolated recording is the normal performance path,
  including single-camera profiles such as `Cam2012632 @ 250 fps`.

## `crop_pipeline`

```json
"crop_pipeline": {
  "crop_size_px": 256,
  "preview_max_fps": 15
}
```

Notes:

- `crop_size_px` is the square crop size used by the transitional GUI crop
  preview/recording path.
- The value is sanitized on load and save: default `256`, even integer,
  clamped to `32..2048`.
- The GUI uses one session crop size while streaming because GL textures and
  NVENC dimensions are allocated at stream start. If multiple open cameras have
  different configured crop sizes, the GUI uses the first open camera value for
  the session and marks the in-memory open camera configs with that value.
- `preview_max_fps` limits only the live GUI crop-preview PBO update path. It
  does not reduce crop recording cadence, crop metadata row count, YOLO cadence,
  or pose crop delivery. Default is `15`; `0` means unlimited diagnostic mode.
- `ORANGE_CROP_PREVIEW_MAX_FPS` overrides `crop_pipeline.preview_max_fps` for
  the current process. `ORANGE_CROP_PREVIEW_DISABLE=1` bypasses crop preview
  entirely.
- Use `Save to config` from the camera properties panel to persist the currently
  selected session crop size back to a camera JSON.

## Lens Control

```json
"lens_control_enabled": true,
"focus_uart_bootstrap": false,
"focus": 345,
"iris": 0
```

Notes:

- `lens_control_enabled` defaults to `true` for legacy configs.
- Set `lens_control_enabled = false` for lensless cameras or cameras whose
  mounted optics should not receive EVT `Focus`/`Iris` node writes.
- When disabled, Orange skips startup focus/iris writes in
  `open_camera_with_params` and ignores GUI focus/iris slider writes for that
  camera.
- `focus_uart_bootstrap` only controls the optional UART bootstrap path for
  focus range discovery. It does not, by itself, disable ordinary focus/iris
  writes.

## `optics`

`optics` is optional per-camera physical optics metadata. It is descriptive: it
does not write EVT `Focus`/`Iris` nodes and does not change camera runtime
behavior by itself. Use the top-level `lens_control_enabled`,
`focus_uart_bootstrap`, `focus`, and `iris` fields for hardware control.

Recommended shape:

```json
"optics": {
  "schema_id": "orange.camera.optics",
  "schema_version": 1,
  "lens": {
    "present": true,
    "manufacturer": "Canon",
    "model": "EF 100mm f/2.8L Macro IS USM",
    "serial": "",
    "mount": "EF",
    "focal_length_mm": 100.0,
    "aperture_f_number": 2.8,
    "focus_control": "camera_focus",
    "iris_control": "camera_iris",
    "notes": ""
  },
  "filter_stack": [
    {
      "id": "hoya_r72_67mm",
      "manufacturer": "HOYA / Kenko Tokina Co., Ltd.",
      "model": "Creative Filter Infrared R72",
      "label": "67",
      "type": "infrared_longpass_filter",
      "thread_size": "67 mm",
      "state": "installed",
      "runtime_role": "normal_experiment_filter",
      "cutoff_wavelength_nm": 720.0,
      "notes": "Normal runtime camera filter; remove only for visible-projector calibration captures."
    }
  ]
}
```

Notes:

- `optics.lens.present` describes whether a physical lens is attached. It is
  separate from `lens_control_enabled`, which only controls whether Orange
  attempts focus/iris writes.
- `focus_control` and `iris_control` should describe how those properties are
  controlled for this setup, for example `none`, `manual`, `camera_focus`,
  `camera_iris`, `uart`, or `unknown`.
- `filter_stack` describes normal runtime optics in front of the camera. It is
  the right place to record that a HOYA/Kenko Tokina R72 67 mm infrared filter
  is normally installed.
- Per-capture fields such as `filter_state` and `runtime_filter_state` still
  belong in calibration artifacts. For example, a visible-projector homography
  capture may say the filter was removed for that capture while `optics`
  records that the filter is installed during normal runtime.
- Calibration and recording artifacts should snapshot or reference the camera
  config so downstream analysis can determine which lens/filter stack was
  attached when the image or video was acquired.

## Example

```json
{
  "schema_id": "orange.camera.config",
  "schema_version": 4,
  "device_model_name": "HB-7000SC",
  "device_serial_number": "2002496",
  "camera_scan_type": "area_scan",
  "gpio_connector_variant": "area_scan_12_pin",
  "gpio_pinout_access": "exposed",
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
  "source_gpu_id": 0,
  "color_temp": "CT_3000K",
  "gpu_direct": false,
  "focus_uart_bootstrap": false,
  "lens_control_enabled": true,
  "optics": {
    "schema_id": "orange.camera.optics",
    "schema_version": 1,
    "lens": {
      "present": true,
      "manufacturer": "Canon",
      "model": "EF 100mm f/2.8L Macro IS USM",
      "mount": "EF",
      "focal_length_mm": 100.0,
      "focus_control": "camera_focus",
      "iris_control": "camera_iris"
    },
    "filter_stack": [
      {
        "id": "hoya_r72_67mm",
        "manufacturer": "HOYA / Kenko Tokina Co., Ltd.",
        "model": "Creative Filter Infrared R72",
        "label": "67",
        "type": "infrared_longpass_filter",
        "thread_size": "67 mm",
        "state": "installed",
        "runtime_role": "normal_experiment_filter",
        "cutoff_wavelength_nm": 720.0
      }
    ]
  },
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
  },
  "rig_io": {
    "schema_id": "orange.camera.rig_io",
    "schema_version": 1,
    "connections": [
      {
        "purpose": "nir_strobe_trigger",
        "direction": "output",
        "camera_line": "GPO_0",
        "physical_pin": 7,
        "reference_line": "GND",
        "reference_pin": 8,
        "electrical": "ttl_0_5v",
        "active_level": "high",
        "inactive_level": "low",
        "normal_output_mode": "Exposure",
        "normal_polarity": false,
        "controlled_device": "near_infrared_strobe",
        "nominal_wavelength_nm": 855.0,
        "verified": false
      }
    ]
  },
  "crop_pipeline": {
    "crop_size_px": 256
  }
}
```
