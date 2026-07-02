#!/usr/bin/env python3
"""Backfill camera raster coordinate-frame metadata into recording snapshots."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Add camera_runtime[serial].coordinate_frame to existing "
            "recording_snapshot.json files."
        )
    )
    parser.add_argument(
        "roots",
        nargs="*",
        default=["/home/jeremy/orange_data/exp"],
        help="Recording artifact root(s) or explicit recording_snapshot.json path(s).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Report planned updates without writing files.",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Rewrite existing coordinate_frame payloads from current dimensions.",
    )
    parser.add_argument(
        "--backup-suffix",
        default="",
        help="Optional suffix for a JSON backup next to each modified snapshot.",
    )
    return parser.parse_args()


def iter_snapshot_paths(roots: list[str]) -> list[Path]:
    paths: list[Path] = []
    seen: set[str] = set()
    for root_text in roots:
        root = Path(root_text).expanduser()
        if root.is_file():
            candidates = [root] if root.name == "recording_snapshot.json" else []
        elif root.is_dir():
            candidates = sorted(root.rglob("recording_snapshot.json"))
        else:
            candidates = []
        for candidate in candidates:
            key = str(candidate.resolve(strict=False))
            if key not in seen:
                seen.add(key)
                paths.append(candidate)
    return paths


def as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def positive_int(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return None
    return parsed if parsed > 0 else None


def camera_keys(snapshot: dict[str, Any]) -> list[str]:
    keys: set[str] = set()
    for top_key in ("camera_runtime", "cameras", "recording_outputs"):
        value = snapshot.get(top_key)
        if isinstance(value, dict):
            keys.update(str(key) for key in value.keys())
    return sorted(keys)


def dimensions_for(snapshot: dict[str, Any], serial: str) -> tuple[int | None, int | None, str]:
    runtime_entry = as_dict(as_dict(snapshot.get("camera_runtime")).get(serial))
    runtime_config = as_dict(runtime_entry.get("runtime"))
    width = positive_int(runtime_config.get("width"))
    height = positive_int(runtime_config.get("height"))
    if width is not None and height is not None:
        return width, height, "camera_runtime.runtime"

    camera_config = as_dict(as_dict(snapshot.get("cameras")).get(serial))
    width = positive_int(camera_config.get("width"))
    height = positive_int(camera_config.get("height"))
    if width is not None and height is not None:
        return width, height, "cameras"

    full_output = as_dict(as_dict(as_dict(snapshot.get("recording_outputs")).get(serial)).get("full"))
    width = positive_int(full_output.get("width"))
    height = positive_int(full_output.get("height"))
    if width is not None and height is not None:
        return width, height, "recording_outputs.full"

    return None, None, "missing_dimensions"


def coordinate_frame(width: int, height: int) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "coordinate_space": "camera_native_pixels",
        "units": "pixels",
        "origin": {
            "name": "top_left_pixel",
            "x_px": 0,
            "y_px": 0,
        },
        "axes": {
            "x": {
                "positive_direction": "right",
                "index_role": "column",
            },
            "y": {
                "positive_direction": "down",
                "index_role": "row",
            },
        },
        "point_order": "xy",
        "pixel_indexing": {
            "index_base": 0,
            "valid_x_index_min": 0,
            "valid_y_index_min": 0,
            "valid_x_index_max": width - 1,
            "valid_y_index_max": height - 1,
        },
        "extent": {
            "width_px": width,
            "height_px": height,
            "x_min_px": 0,
            "y_min_px": 0,
            "x_max_exclusive_px": width,
            "y_max_exclusive_px": height,
        },
        "image_shape": {
            "height": height,
            "width": width,
        },
        "orientation_reference": "orange_live_stream",
    }


def read_snapshot(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise ValueError("snapshot root is not a JSON object")
    return payload


def write_snapshot(path: Path, snapshot: dict[str, Any], backup_suffix: str) -> None:
    original_mode = path.stat().st_mode
    if backup_suffix:
        backup_path = path.with_name(path.name + backup_suffix)
        backup_path.write_bytes(path.read_bytes())
        os.chmod(backup_path, original_mode)

    tmp = path.with_name(path.name + ".coord_frame.tmp")
    with tmp.open("w", encoding="utf-8") as handle:
        json.dump(snapshot, handle, indent=2)
        handle.write("\n")
    os.chmod(tmp, original_mode)
    os.replace(tmp, path)


def backfill_snapshot(
    path: Path,
    *,
    dry_run: bool,
    force: bool,
    backup_suffix: str,
) -> tuple[int, int, int]:
    snapshot = read_snapshot(path)
    camera_runtime = snapshot.setdefault("camera_runtime", {})
    if not isinstance(camera_runtime, dict):
        raise ValueError("camera_runtime exists but is not a JSON object")

    updated = 0
    unchanged = 0
    skipped = 0
    for serial in camera_keys(snapshot):
        width, height, source = dimensions_for(snapshot, serial)
        if width is None or height is None:
            skipped += 1
            print(f"skip {path}: camera {serial}: {source}")
            continue

        entry = camera_runtime.setdefault(serial, {})
        if not isinstance(entry, dict):
            skipped += 1
            print(f"skip {path}: camera {serial}: camera_runtime entry is not an object")
            continue

        frame = coordinate_frame(width, height)
        if not force and entry.get("coordinate_frame") == frame:
            unchanged += 1
            continue
        if entry.get("coordinate_frame") is not None and not force:
            unchanged += 1
            continue

        entry["coordinate_frame"] = frame
        updated += 1
        print(f"{'would update' if dry_run else 'update'} {path}: camera {serial}: {width}x{height} from {source}")

    if updated and not dry_run:
        write_snapshot(path, snapshot, backup_suffix)
    return updated, unchanged, skipped


def main() -> int:
    args = parse_args()
    paths = iter_snapshot_paths(args.roots)
    if not paths:
        print("No recording_snapshot.json files found.", file=sys.stderr)
        return 1

    total_files_changed = 0
    total_updated = 0
    total_unchanged = 0
    total_skipped = 0
    for path in paths:
        try:
            updated, unchanged, skipped = backfill_snapshot(
                path,
                dry_run=args.dry_run,
                force=args.force,
                backup_suffix=args.backup_suffix,
            )
        except Exception as exc:  # noqa: BLE001 - CLI should continue across files.
            total_skipped += 1
            print(f"error {path}: {exc}", file=sys.stderr)
            continue
        if updated:
            total_files_changed += 1
        total_updated += updated
        total_unchanged += unchanged
        total_skipped += skipped

    action = "would change" if args.dry_run else "changed"
    print(
        f"Summary: files_scanned={len(paths)} files_{action}={total_files_changed} "
        f"cameras_updated={total_updated} cameras_unchanged={total_unchanged} "
        f"cameras_skipped={total_skipped}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
