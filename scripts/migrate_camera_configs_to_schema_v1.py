#!/usr/bin/env python3

import argparse
import json
import shutil
from datetime import datetime
from pathlib import Path


SCHEMA_ID = "orange.camera.config"
SCHEMA_VERSION = 1

VALID_SCAN_TYPES = {"area_scan", "line_scan", "unknown"}
VALID_CONNECTOR_VARIANTS = {"area_scan_12_pin", "area_scan_8_pin", "line_scan_12_pin", "unknown"}
VALID_SYNC_MODES = {"free_run", "ptp_gate", "external_trigger", "software_trigger"}
VALID_RECIPES = {
    "",
    "area_scan_hw_trigger_internal_gpi4",
    "area_scan_hw_trigger_external_gpi4",
    "line_scan_hw_frame_gpi1_internal_line",
    "line_scan_hw_frame_gpi1_encoder_line",
    "line_scan_encoder_frame_encoder_line",
    "line_scan_hw_gate_gpi1_encoder_frame_encoder_line",
}


def lower_ascii(value):
    return str(value).strip().lower()


def normalize_string_field(value):
    if value is None:
        return ""
    return str(value)


def canonicalize_serial_number(value):
    text = normalize_string_field(value).strip()
    if not text:
        return ""
    if not text.isdigit():
        return text
    stripped = text.lstrip("0")
    return stripped or "0"


def normalize_scan_type(value):
    lowered = lower_ascii(value)
    if lowered in VALID_SCAN_TYPES:
        return lowered
    return "unknown"


def normalize_connector_variant(value):
    lowered = lower_ascii(value)
    if lowered in VALID_CONNECTOR_VARIANTS:
        return lowered
    return "unknown"


def normalize_sync_mode(value):
    lowered = lower_ascii(value)
    if lowered in VALID_SYNC_MODES:
        return lowered
    return "free_run"


def normalize_recipe(value):
    lowered = lower_ascii(value)
    if lowered in VALID_RECIPES:
        return lowered
    return ""


def starts_with_case_insensitive(value, prefix):
    return lower_ascii(value).startswith(lower_ascii(prefix))


def contains_case_insensitive(value, needle):
    return lower_ascii(needle) in lower_ascii(value)


def model_is_area_scan_family(model):
    return (
        starts_with_case_insensitive(model, "HB")
        or starts_with_case_insensitive(model, "HZ")
        or starts_with_case_insensitive(model, "HR")
        or starts_with_case_insensitive(model, "HT")
        or starts_with_case_insensitive(model, "HE")
    )


def model_is_line_scan_family(model):
    return (
        starts_with_case_insensitive(model, "LB")
        or starts_with_case_insensitive(model, "TLB")
        or starts_with_case_insensitive(model, "LR")
        or starts_with_case_insensitive(model, "TLR")
        or starts_with_case_insensitive(model, "LT")
        or starts_with_case_insensitive(model, "LZ")
        or starts_with_case_insensitive(model, "TLZ")
    )


def model_uses_area_scan_8_pin_connector(model):
    return contains_case_insensitive(model, "eros") or starts_with_case_insensitive(model, "HE")


def infer_camera_gpio_metadata(device_model, camera_scan_type, gpio_connector_variant):
    scan_type = normalize_scan_type(camera_scan_type)
    connector_variant = normalize_connector_variant(gpio_connector_variant)

    if scan_type == "unknown":
        if model_is_area_scan_family(device_model):
            scan_type = "area_scan"
        elif model_is_line_scan_family(device_model):
            scan_type = "line_scan"

    if connector_variant == "unknown":
        if scan_type == "line_scan":
            connector_variant = "line_scan_12_pin"
        elif scan_type == "area_scan":
            if model_uses_area_scan_8_pin_connector(device_model):
                connector_variant = "area_scan_8_pin"
            else:
                connector_variant = "area_scan_12_pin"

    return scan_type, connector_variant


def normalize_trigger(trigger):
    if not isinstance(trigger, dict):
        trigger = {}
    return {
        "enabled": bool(trigger.get("enabled", False)),
        "selector": normalize_string_field(trigger.get("selector", "AcquisitionStart")) or "AcquisitionStart",
        "source": normalize_string_field(trigger.get("source", "Software")) or "Software",
        "activation": normalize_string_field(trigger.get("activation", "RisingEdge")) or "RisingEdge",
    }


def normalize_ptp(ptp, sync_mode):
    if not isinstance(ptp, dict):
        ptp = {}
    normalized = {
        "enabled": bool(ptp.get("enabled", sync_mode == "ptp_gate")),
    }
    if normalized["enabled"]:
        normalized["mode"] = normalize_string_field(ptp.get("mode", "TwoStep")) or "TwoStep"
    return normalized


def normalize_gpio(gpio):
    if isinstance(gpio, list):
        nodes = gpio
    elif isinstance(gpio, dict) and isinstance(gpio.get("nodes"), list):
        nodes = gpio["nodes"]
    else:
        nodes = []
    return {"nodes": nodes}


def reorder_config(original, migrated_fields):
    output = {}
    original_without_redundant_fields = dict(original)
    original_without_redundant_fields.pop("device_model", None)
    preferred_order = [
        "schema_id",
        "schema_version",
        "device_model_name",
        "device_serial_number",
        "camera_scan_type",
        "gpio_connector_variant",
        "gpio_recipe",
        "name",
        "width",
        "height",
        "frame_rate",
        "gain",
        "iris",
        "focus",
        "exposure",
        "pixel_format",
        "gpu_id",
        "color_temp",
        "gpu_direct",
        "focus_uart_bootstrap",
        "color",
        "offset_x",
        "offset_y",
        "sync_mode",
        "trigger",
        "ptp",
        "gpio",
    ]

    for key in preferred_order:
        if key in migrated_fields:
            output[key] = migrated_fields[key]

    for key, value in original_without_redundant_fields.items():
        if key not in output:
            output[key] = value

    return output


def compute_target_path(path, rename_files):
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)

    device_serial_number = canonicalize_serial_number(data.get("device_serial_number")) or canonicalize_serial_number(path.stem)
    if rename_files and device_serial_number:
        return path.with_name(f"{device_serial_number}.json")
    return path


def migrate_file(path):
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)

    device_model = normalize_string_field(data.get("device_model") or data.get("device_model_name"))
    device_serial_number = canonicalize_serial_number(data.get("device_serial_number")) or canonicalize_serial_number(path.stem)
    camera_scan_type = normalize_scan_type(data.get("camera_scan_type", "unknown"))
    gpio_connector_variant = normalize_connector_variant(data.get("gpio_connector_variant", "unknown"))
    camera_scan_type, gpio_connector_variant = infer_camera_gpio_metadata(
        device_model,
        camera_scan_type,
        gpio_connector_variant,
    )

    sync_mode = normalize_sync_mode(data.get("sync_mode", "free_run"))
    gpio_recipe = normalize_recipe(data.get("gpio_recipe", ""))

    migrated_fields = dict(data)
    migrated_fields.pop("device_model", None)
    migrated_fields["schema_id"] = SCHEMA_ID
    migrated_fields["schema_version"] = SCHEMA_VERSION
    migrated_fields["device_model_name"] = device_model
    migrated_fields["device_serial_number"] = device_serial_number
    migrated_fields["camera_scan_type"] = camera_scan_type
    migrated_fields["gpio_connector_variant"] = gpio_connector_variant
    migrated_fields["gpio_recipe"] = gpio_recipe
    migrated_fields["sync_mode"] = sync_mode
    migrated_fields["trigger"] = normalize_trigger(data.get("trigger"))
    migrated_fields["ptp"] = normalize_ptp(data.get("ptp"), sync_mode)
    migrated_fields["gpio"] = normalize_gpio(data.get("gpio"))
    migrated_fields["focus_uart_bootstrap"] = bool(data.get("focus_uart_bootstrap", False))

    ordered = reorder_config(data, migrated_fields)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(ordered, handle, indent=4)
        handle.write("\n")

    return {
        "path": str(path),
        "device_model": device_model,
        "device_serial_number": device_serial_number,
        "camera_scan_type": camera_scan_type,
        "gpio_connector_variant": gpio_connector_variant,
    }


def main():
    parser = argparse.ArgumentParser(description="Back up and migrate camera configs to schema v1.")
    parser.add_argument("--input-root", required=True, help="Root directory containing per-scenario config folders.")
    parser.add_argument(
        "--backup-dir",
        help="Backup directory to create before migration. Defaults to a timestamped sibling directory.",
    )
    parser.add_argument(
        "--rename-files",
        action="store_true",
        help="Rename files so the filename stem matches the canonical device_serial_number.",
    )
    args = parser.parse_args()

    input_root = Path(args.input_root).resolve()
    if not input_root.is_dir():
        raise SystemExit(f"Input root does not exist: {input_root}")

    if args.backup_dir:
        backup_dir = Path(args.backup_dir).resolve()
    else:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        backup_dir = input_root.parent / f"{input_root.name}_backup_pre_schema_v1_{stamp}"

    if backup_dir.exists():
        raise SystemExit(f"Backup directory already exists: {backup_dir}")

    json_files = sorted(input_root.glob("*/*.json"))
    if not json_files:
        raise SystemExit(f"No JSON files found under: {input_root}")

    if args.rename_files:
        target_map = {}
        for path in json_files:
            target = compute_target_path(path, rename_files=True)
            existing = target_map.get(target)
            if existing and existing != path:
                raise SystemExit(f"Rename conflict: {existing} and {path} would both map to {target}")
            target_map[target] = path

    shutil.copytree(input_root, backup_dir)

    results = []
    for path in json_files:
        result = migrate_file(path)
        final_path = path
        if args.rename_files:
            final_path = path.with_name(f"{result['device_serial_number']}.json")
            if final_path != path:
                path.rename(final_path)
        result["final_path"] = str(final_path)
        results.append(result)

    print(f"Backed up {input_root} to {backup_dir}")
    print(f"Migrated {len(results)} JSON files")
    for result in results[:10]:
        print(
            f"{result['final_path']}: "
            f"model={result['device_model'] or 'unknown'} "
            f"serial={result['device_serial_number']} "
            f"scan={result['camera_scan_type']} "
            f"connector={result['gpio_connector_variant']}"
        )
    if len(results) > 10:
        print(f"... {len(results) - 10} more files")


if __name__ == "__main__":
    main()
