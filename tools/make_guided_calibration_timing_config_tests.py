#!/usr/bin/env python3

import json
import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "make_guided_calibration_timing_config.py"


def camera_payload(*, include_startup_nodes: bool) -> dict:
    nodes = []
    if include_startup_nodes:
        nodes = [
            {"name": "GPO_0_Polarity", "type": "bool", "value": False},
            {"name": "GPO_0_Mode", "type": "enum", "value": "Exposure"},
        ]
    return {
        "device_serial_number": "2010096",
        "frame_rate": 100,
        "exposure": 50,
        "sync_mode": "ptp_gate",
        "ptp": {"enabled": True, "mode": "TwoStep"},
        "gpio_pinout_access": "exposed",
        "gpio": {"nodes": nodes},
        "rig_io": {
            "connections": [
                {
                    "purpose": "nir_strobe_trigger",
                    "direction": "output",
                    "camera_line": "GPO_0",
                    "normal_output_mode": "Exposure",
                    "normal_polarity": False,
                }
            ]
        },
    }


def run_generator(
    payload: dict,
    extra_args: list[str] | None = None,
) -> tuple[subprocess.CompletedProcess[str], Path, tempfile.TemporaryDirectory[str]]:
    temp_dir = tempfile.TemporaryDirectory()
    root = Path(temp_dir.name)
    source = root / "source"
    output = root / "output"
    source.mkdir()
    (source / "2010096.json").write_text(json.dumps(payload), encoding="utf-8")
    command = [
            "python3",
            str(SCRIPT),
            str(source),
            str(output),
            "--frame-rate-hz",
            "5",
            "--exposure-us",
            "100000",
        ]
    command.extend(extra_args or [])
    result = subprocess.run(
        command,
        text=True,
        capture_output=True,
        check=False,
    )
    return result, output, temp_dir


def test_holds_mapped_strobe_low_at_open() -> None:
    """The strobe camera opens with its output in manual low, never mapped to the long
    calibration exposure (5 fps x 100 ms would be 50 % duty)."""
    result, output, temp_dir = run_generator(camera_payload(include_startup_nodes=True))
    try:
        assert result.returncode == 0, result.stderr
        generated = json.loads((output / "2010096.json").read_text(encoding="utf-8"))
        assert generated["frame_rate"] == 5
        assert generated["exposure"] == 100000
        assert generated["gpio"]["nodes"] == [
            {"name": "GPO_0_Polarity", "type": "bool", "value": False},
            {"name": "GPO_0_Mode", "type": "enum", "value": "GPO"},
        ], generated["gpio"]["nodes"]
        assert "held low at open" in result.stderr
    finally:
        temp_dir.cleanup()


def test_camera_without_strobe_keeps_its_nodes() -> None:
    payload = camera_payload(include_startup_nodes=True)
    payload["rig_io"] = {"connections": []}
    result, output, temp_dir = run_generator(payload)
    try:
        assert result.returncode == 0, result.stderr
        generated = json.loads((output / "2010096.json").read_text(encoding="utf-8"))
        assert generated["gpio"]["nodes"] == payload["gpio"]["nodes"]
    finally:
        temp_dir.cleanup()


def test_rejects_mapped_strobe_without_startup_nodes() -> None:
    result, _, temp_dir = run_generator(camera_payload(include_startup_nodes=False))
    try:
        assert result.returncode != 0
        assert "maps an exposed NIR strobe" in result.stderr
    finally:
        temp_dir.cleanup()


def test_applies_explicit_camera_iris_override() -> None:
    payload = camera_payload(include_startup_nodes=True)
    payload["iris"] = 24
    result, output, temp_dir = run_generator(
        payload, ["--camera-iris-overrides", "2010096=15"]
    )
    try:
        assert result.returncode == 0, result.stderr
        generated = json.loads((output / "2010096.json").read_text(encoding="utf-8"))
        assert generated["iris"] == 15
    finally:
        temp_dir.cleanup()


if __name__ == "__main__":
    test_holds_mapped_strobe_low_at_open()
    test_camera_without_strobe_keeps_its_nodes()
    test_rejects_mapped_strobe_without_startup_nodes()
    test_applies_explicit_camera_iris_override()
    print("make_guided_calibration_timing_config tests passed")
