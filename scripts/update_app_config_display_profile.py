#!/usr/bin/env python3
"""Update Orange app-config GUI display pacing defaults."""

from __future__ import annotations

import argparse
import json
import os
import tempfile
from pathlib import Path
from typing import Any


PROFILE_DEFAULTS = {
    "default": {
        "display_preview_max_fps": None,
        "swap_interval": None,
        "frame_max_fps": None,
    },
    "fast": {
        "display_preview_max_fps": 15,
        "swap_interval": 0,
        "frame_max_fps": 60,
    },
    "citrus_safe": {
        "display_preview_max_fps": 10,
        "swap_interval": 1,
        "frame_max_fps": 30,
    },
}


def default_config_path() -> Path:
    return Path.home() / "orange_data" / "config" / "app" / "default.json"


def nonnegative_int_in_range(max_value: int):
    def parse(value: str) -> int:
        try:
            parsed = int(value)
        except ValueError as exc:
            raise argparse.ArgumentTypeError("must be an integer") from exc
        if parsed < 0 or parsed > max_value:
            raise argparse.ArgumentTypeError(f"must be in [0,{max_value}]")
        return parsed

    return parse


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--config",
        type=Path,
        default=default_config_path(),
        help="App config path. Default: %(default)s",
    )
    parser.add_argument(
        "--profile",
        choices=sorted(PROFILE_DEFAULTS),
        required=True,
        help="Display pacing profile to store in gui.display.profile.",
    )
    parser.add_argument(
        "--display-preview-max-fps",
        type=nonnegative_int_in_range(10000),
        default=None,
        help="Optional explicit full-frame display preview cap.",
    )
    parser.add_argument(
        "--swap-interval",
        type=nonnegative_int_in_range(4),
        default=None,
        help="Optional explicit GLFW swap interval.",
    )
    parser.add_argument(
        "--gui-frame-max-fps",
        type=nonnegative_int_in_range(1000),
        default=None,
        help="Optional explicit GUI frame cap; 0 disables the cap.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the updated JSON without writing it.",
    )
    return parser.parse_args()


def load_config(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {
            "schema_id": "orange.app.config",
            "schema_version": 1,
        }
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"failed to parse {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise SystemExit(f"app config root must be a JSON object: {path}")
    return payload


def update_display_config(payload: dict[str, Any], args: argparse.Namespace) -> dict[str, Any]:
    out = dict(payload)
    out.setdefault("schema_id", "orange.app.config")
    out.setdefault("schema_version", 1)

    gui = out.get("gui")
    if gui is None:
        gui = {}
    if not isinstance(gui, dict):
        raise SystemExit("gui must be a JSON object")

    display = gui.get("display")
    if display is None:
        display = {}
    if not isinstance(display, dict):
        raise SystemExit("gui.display must be a JSON object")

    display = dict(display)
    display["profile"] = args.profile
    for key, value in PROFILE_DEFAULTS[args.profile].items():
        display[key] = value

    overrides = {
        "display_preview_max_fps": args.display_preview_max_fps,
        "swap_interval": args.swap_interval,
        "frame_max_fps": args.gui_frame_max_fps,
    }
    for key, value in overrides.items():
        if value is not None:
            display[key] = value

    gui = dict(gui)
    gui["display"] = display
    out["gui"] = gui
    return out


def atomic_write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    rendered = json.dumps(payload, indent=2) + "\n"
    fd, tmp_name = tempfile.mkstemp(
        prefix=f".{path.name}.",
        suffix=".tmp",
        dir=path.parent,
        text=True,
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(rendered)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp_name, path)
    except Exception:
        try:
            os.unlink(tmp_name)
        except FileNotFoundError:
            pass
        raise


def main() -> int:
    args = parse_args()
    payload = load_config(args.config)
    updated = update_display_config(payload, args)
    rendered = json.dumps(updated, indent=2) + "\n"
    if args.dry_run:
        print(rendered, end="")
    else:
        atomic_write_json(args.config, updated)
        print(f"updated {args.config}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
