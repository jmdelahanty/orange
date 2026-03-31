# Emergent GPIO Pinouts

## Scope

This note summarizes the official Emergent Vision Technologies GPIO connector docs and maps the physical pins to the trigger and encoder concepts we care about in `orange-jeremy`.

## Source Docs

- GPIO ports and schematics:
  - https://docs.emergentvisiontec.com/cameras/gpio-ports-schematics
  - Vendor page updated on March 3, 2025.
- Area-scan trigger modes:
  - https://docs.emergentvisiontec.com/cameras/trigger-modes-for-area-scan-cameras
  - Vendor page updated on November 19, 2024.
- Line-scan trigger modes:
  - https://docs.emergentvisiontec.com/cameras/trigger-modes-for-line-scan-cameras
  - Vendor page updated on October 27, 2025.
- GPIO feature/node overview:
  - https://docs.emergentvisiontec.com/camera-features/general-purpose-input-output-control-features
  - Vendor page updated on July 24, 2025.
- `GPI_Start_Frame_Mode`:
  - https://docs.emergentvisiontec.com/camera-features/gpi-start-frame-mode
  - Vendor page updated on October 22, 2024.

## Connector Variants

The official vendor GPIO page currently lists three connector variants:

- Line-scan camera GPIO with 12 pins
- Area-scan camera GPIO with 12 pins
- Area-scan camera GPIO with 8 pins

Important correction:

- As of March 3, 2025, the vendor page lists an 8-pin area-scan connector, not a 7-pin connector.
- If we use "7-pin" anywhere in our own planning notes, that should be treated as outdated shorthand or a mistake unless we verify a separate camera-specific source.

## 12-Pin Comparison

| Pin | Line-scan 12-pin | Area-scan 12-pin |
| --- | --- | --- |
| 1 | `VEXT` | `VEXT` |
| 2 | `VEXT` | `VEXT` |
| 3 | `GPO_0` opto output | `GPO_3` opto output |
| 4 | `ENC_A_P` RS422+ | `GPI_2` opto input |
| 5 | `ISO_GND` | `ISO_GND` |
| 6 | `ENC_A_N` RS422- | `GPO_1` opto output |
| 7 | `GPI_1` opto input | `GPO_0` TTL output |
| 8 | `GND` | `GND` |
| 9 | `GND` | `GND` |
| 10 | `ENC_B_P` RS422+ | `DNC` |
| 11 | `GPI_2` opto input | `GPI_5` opto input |
| 12 | `ENC_B_N` RS422- | `GPI_4` TTL input |

Notes:

- Pins 1 and 2 are internally tied on the 12-pin connector.
- Pins 8 and 9 are internally tied on the 12-pin connector.
- The official line-scan page identifies the encoder pins as differential RS422 encoder inputs.

## Area-Scan 8-Pin Connector

| Pin | Area-scan 8-pin |
| --- | --- |
| 1 | `VEXT` |
| 2 | `GPI_2` opto input |
| 3 | `GPO_1` opto output |
| 4 | `GPO_0` TTL output |
| 5 | `GPI_3` TTL input |
| 6 | `VEXT_OUT` 12 V output only |
| 7 | `ISO_GND` |
| 8 | `GND` |

## Integration Notes

The following points are inferences from the vendor pinout and trigger-mode docs, not direct statements from one single page.

- Area-scan hardware-trigger examples use `GPI 4` for frame start and exposure start/end examples.
  - On the 12-pin area-scan connector, `GPI_4` is pin 12 and is a TTL input.
- Line-scan frame-trigger examples use `GPI_Start_Frame_Mode = GPI_1` as the example trigger input.
  - On the 12-pin line-scan connector, `GPI_1` is pin 7.
- The `GPI_Start_Frame_Mode` feature doc says line-scan cameras can select at least `GPI_1` or `GPI_2`.
  - On the 12-pin line-scan connector, `GPI_2` is pin 11.
- Line-scan encoder integration uses the dedicated encoder pins, not the generic GPI pins:
  - `ENC_A_P` pin 4
  - `ENC_A_N` pin 6
  - `ENC_B_P` pin 10
  - `ENC_B_N` pin 12
- Area-scan 8-pin cameras do not expose the same physical signals as area-scan 12-pin cameras.
  - In particular, the connector does not have `GPI_4`, `GPI_5`, `GPO_3`, or the differential encoder pins.
  - Config code should not assume those names or pin numbers exist across all area-scan models.
- The vendor GPIO feature overview explicitly says feature/node availability varies by camera model.

## Electrical Notes

From the vendor GPIO schematics page:

- TTL I/O is intended for lower delay and lower jitter than opto-isolated I/O.
- Exceeding `+5.5 VDC` on TTL I/O can damage the camera.
- For opto-isolated outputs, the user load resistor depends on the external supply voltage and desired current.

## Why This Matters For Orange

- The config layer should separate:
  - connector/pinout documentation
  - logical node names such as `GPI_4` or `GPO_1`
  - mode-specific recipes such as area-scan hardware trigger vs line-scan encoder mode
- `sync_mode` and `gpio.nodes` should remain logical-camera configuration.
- Any future hardware-trigger presets should be expressed in terms of the logical nodes first, then documented against the physical connector map in this file.
- The config-side inference from `device_model` and `camera_scan_type` to connector variant is tracked separately in [camera_type_pinout_map.json](/home/jeremy/orange-jeremy/docs/camera_type_pinout_map.json).
