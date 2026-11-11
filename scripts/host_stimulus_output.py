#!/usr/bin/env python3
"""Print the stimulus display output a pancakebatter host config declares for a rig.

The host config (installed at /etc/pancakebatter/host.yml) lists per rig the
GPU outputs that feed its projectors (`rigs[].display_outputs[]`: output_name,
projection_path and, since 2026-11-10, x_screen). Citrus binds its stimulus
window from the same entry (Citrus src/utils/stimulus_display_binding.h), so
Orange's launchers read it here instead of naming outputs themselves.

Prints one line: "<output_name> <x_screen>", x_screen "-" when the entry does
not declare one (the output is on the X screen of $DISPLAY). Exits 1 with a
reason on stderr when the rig or the projection path is not declared, or is
declared more than once.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import yaml


def stimulus_output(host_config: dict, rig_id: str, projection_path: str) -> tuple[str, int | None]:
    rigs = [rig for rig in host_config.get("rigs") or [] if isinstance(rig, dict) and rig.get("rig_id") == rig_id]
    if len(rigs) != 1:
        raise ValueError(f"host config declares rig {rig_id!r} {len(rigs)} times, expected once")
    outputs = [
        output
        for output in rigs[0].get("display_outputs") or []
        if isinstance(output, dict) and output.get("projection_path") == projection_path
    ]
    if len(outputs) != 1:
        raise ValueError(
            f"rig {rig_id!r} declares {len(outputs)} display outputs for projection path "
            f"{projection_path!r}, expected one"
        )
    name = outputs[0].get("output_name")
    if not isinstance(name, str) or not name:
        raise ValueError(f"rig {rig_id!r} projection path {projection_path!r} has no output_name")
    x_screen = outputs[0].get("x_screen")
    if x_screen is not None and (isinstance(x_screen, bool) or not isinstance(x_screen, int) or x_screen < 0):
        raise ValueError(f"rig {rig_id!r} output {name} x_screen must be a whole number >= 0, got {x_screen!r}")
    return name, x_screen


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host-config", type=Path, default=Path("/etc/pancakebatter/host.yml"))
    parser.add_argument("--rig-id", required=True)
    parser.add_argument("--projection-path", default="main")
    args = parser.parse_args()
    try:
        host_config = yaml.safe_load(args.host_config.read_text(encoding="utf-8")) or {}
        name, x_screen = stimulus_output(host_config, args.rig_id, args.projection_path)
    except (OSError, yaml.YAMLError, ValueError) as error:
        print(f"host_stimulus_output: {args.host_config}: {error}", file=sys.stderr)
        return 1
    print(f"{name} {'-' if x_screen is None else x_screen}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
