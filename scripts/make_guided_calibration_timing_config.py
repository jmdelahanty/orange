#!/usr/bin/env python3
"""Create an ephemeral PTP camera config at calibration timing.

The source config is never modified. Starting Orange from this derived config
ensures the shared PTP gate is established at the intended commissioning frame
period, instead of changing each live camera's timing sequentially afterward.

A camera that drives the NIR strobe (exposed `nir_strobe_trigger` output) opens
with that output held LOW in manual mode (`GPO_0_Polarity=false`,
`GPO_0_Mode=GPO`), not mapped to exposure. Calibration exposures are long
(10-100 ms at 5 fps, 5-50 % duty) and the strobe must never exceed 8 % duty;
before 2026-11-14 the output opened in `Exposure` mode and only the GUI's
calibration preflight, which runs after the stream starts, forced it low, so a
lit strobe depended on the PTP gate's start-up delay. The preflight still
captures and restores this state; `keep_or_restore_mapped_pulse` re-enables the
mapped pulse explicitly (and is refused above 8 % duty).
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path


def parse_camera_iris_overrides(value: str) -> dict[str, int]:
    overrides: dict[str, int] = {}
    if not value:
        return overrides
    for item in value.split(","):
        camera, separator, iris_text = item.strip().partition("=")
        if not separator or not camera.isdigit():
            raise argparse.ArgumentTypeError(
                "camera iris overrides must use SERIAL=VALUE[,SERIAL=VALUE...]"
            )
        try:
            iris = int(iris_text)
        except ValueError as error:
            raise argparse.ArgumentTypeError(
                f"invalid iris value for camera {camera}: {iris_text}"
            ) from error
        if iris < 0:
            raise argparse.ArgumentTypeError("camera iris values must be non-negative")
        overrides[camera] = iris
    return overrides


def validate_mapped_strobe_startup_contract(payload: dict, source_path: Path) -> None:
    if payload.get("gpio_pinout_access") != "exposed":
        return
    connections = payload.get("rig_io", {}).get("connections", [])
    mapped_strobes = [
        item
        for item in connections
        if isinstance(item, dict)
        and item.get("purpose") == "nir_strobe_trigger"
        and item.get("direction") == "output"
    ]
    if not mapped_strobes:
        return

    nodes = payload.get("gpio", {}).get("nodes", [])
    node_by_name = {
        item.get("name"): item
        for item in nodes
        if isinstance(item, dict) and isinstance(item.get("name"), str)
    }
    required = {
        "GPO_0_Polarity": ("bool", False),
        "GPO_0_Mode": ("enum", "Exposure"),
    }
    for name, (node_type, value) in required.items():
        node = node_by_name.get(name)
        if not node or node.get("type") != node_type or node.get("value") != value:
            raise ValueError(
                f"{source_path} maps an exposed NIR strobe but does not enforce "
                f"{name}={value!r} ({node_type}) in gpio.nodes"
            )


def hold_mapped_strobe_low_at_open(payload: dict) -> bool:
    """Rewrite the strobe output nodes to manual low. True when a strobe is mapped."""
    if payload.get("gpio_pinout_access") != "exposed":
        return False
    mapped = any(
        isinstance(item, dict)
        and item.get("purpose") == "nir_strobe_trigger"
        and item.get("direction") == "output"
        for item in payload.get("rig_io", {}).get("connections", [])
    )
    if not mapped:
        return False
    nodes = payload["gpio"]["nodes"]
    others = [n for n in nodes if n.get("name") not in ("GPO_0_Polarity", "GPO_0_Mode")]
    # Polarity first, so the line is low before it leaves Exposure mode.
    payload["gpio"]["nodes"] = others + [
        {"name": "GPO_0_Polarity", "type": "bool", "value": False},
        {"name": "GPO_0_Mode", "type": "enum", "value": "GPO"},
    ]
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_dir", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--frame-rate-hz", type=int, required=True)
    parser.add_argument("--exposure-us", type=int, required=True)
    parser.add_argument(
        "--camera-iris-overrides",
        type=parse_camera_iris_overrides,
        default={},
        help="optional SERIAL=VALUE[,SERIAL=VALUE...] startup iris overrides",
    )
    args = parser.parse_args()

    if args.frame_rate_hz <= 0 or args.exposure_us <= 0:
        parser.error("frame rate and exposure must be positive")
    if not args.source_dir.is_dir():
        parser.error(f"source config directory does not exist: {args.source_dir}")
    if args.output_dir.exists():
        parser.error(f"output config directory already exists: {args.output_dir}")

    camera_files = sorted(args.source_dir.glob("*.json"))
    if not camera_files:
        parser.error(f"source config contains no JSON camera files: {args.source_dir}")
    available_serials = {path.stem for path in camera_files}
    unknown_serials = set(args.camera_iris_overrides) - available_serials
    if unknown_serials:
        parser.error(
            "camera iris override serials are absent from the source config: "
            + ",".join(sorted(unknown_serials))
        )

    args.output_dir.mkdir(parents=True)
    for source_path in camera_files:
        payload = json.loads(source_path.read_text(encoding="utf-8"))
        if payload.get("sync_mode") != "ptp_gate":
            parser.error(f"{source_path} does not use sync_mode=ptp_gate")
        ptp = payload.get("ptp")
        if not isinstance(ptp, dict) or ptp.get("enabled") is not True:
            parser.error(f"{source_path} does not enable PTP")
        try:
            validate_mapped_strobe_startup_contract(payload, source_path)
        except ValueError as exc:
            parser.error(str(exc))
        payload["frame_rate"] = args.frame_rate_hz
        payload["exposure"] = args.exposure_us
        if hold_mapped_strobe_low_at_open(payload):
            print(f"{source_path.stem}: NIR strobe output held low at open (GPO_0_Mode=GPO, polarity low)",
                  file=sys.stderr)
        if source_path.stem in args.camera_iris_overrides:
            payload["iris"] = args.camera_iris_overrides[source_path.stem]
        destination = args.output_dir / source_path.name
        destination.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    for source_path in sorted(args.source_dir.iterdir()):
        if source_path.is_file() and source_path.suffix != ".json":
            shutil.copy2(source_path, args.output_dir / source_path.name)

    print(args.output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
