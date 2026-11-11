#!/usr/bin/env python3
"""Display resolution of scripts/run_gui_arena_centering_commissioning.sh, dry-run only.

The launcher takes the stimulus output and its X screen from the pancakebatter
host config (scripts/host_stimulus_output.py) and the operator monitor from
xrandr's primary output on Orange's X screen. A fake xrandr serves per-screen
inventories, so no real display, Citrus or camera is touched (no --execute).
"""

from __future__ import annotations

import json
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LAUNCHER = REPO / "scripts" / "run_gui_arena_centering_commissioning.sh"
sys.path.insert(0, str(REPO / "scripts"))

import host_stimulus_output  # noqa: E402

FAKE_XRANDR = """#!/usr/bin/env python3
import json, os, sys
screens = json.loads(os.environ["FAKE_XRANDR_SCREENS"])
args = sys.argv[1:]
screen = int(args[args.index("--screen") + 1]) if "--screen" in args else 0
if screen >= len(screens):
    print(f"Invalid screen number {screen} (display has {len(screens)})", file=sys.stderr)
    sys.exit(1)
print(f"Screen {screen}: minimum 8 x 8, current 1920 x 1080, maximum 32767 x 32767")
for line in screens[screen]:
    print(line)
"""

TWO_SCREENS = [
    ["DP-0 connected primary 3840x2160+0+0 (normal left inverted right x axis y axis) 597mm x 336mm",
     "DP-1 disconnected (normal left inverted right x axis y axis)"],
    ["DP-2 connected primary 1920x1080+0+0 (normal left inverted right x axis y axis) 0mm x 0mm"],
]
ONE_SCREEN = [[
    "DP-0 connected primary 3840x2160+0+0 (normal left inverted right x axis y axis) 597mm x 336mm",
    "DP-2 connected 1920x1080+3840+0 (normal left inverted right x axis y axis) 0mm x 0mm",
]]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def host_yaml(output: str | None, x_screen: int | None, rig: str = "omnifin0") -> str:
    if output is None:
        return "rigs: []\n"
    entry = f"      - output_name: {output}\n        projection_path: main\n"
    if x_screen is not None:
        entry += f"        x_screen: {x_screen}\n"
    return f"rigs:\n  - rig_id: {rig}\n    display_outputs:\n{entry}"


def plan(tmp: Path, screens: list, host: str, *extra: str) -> dict[str, str]:
    root = tmp / "citrus"
    (root / "targets" / "rigs" / "omnifin0" / "shadow").mkdir(parents=True, exist_ok=True)
    (root / "protocols").mkdir(exist_ok=True)
    (root / "targets" / "rigs" / "omnifin0" / "shadow" / "shadow.json").write_text("{}")
    (root / "protocols" / "good_cop_bad_cop_demo.json").write_text("{}")
    for exe in (root / "targets" / "citrus", tmp / "orange", tmp / "xrandr"):
        exe.write_text(FAKE_XRANDR if exe.name == "xrandr" else "#!/bin/sh\nexit 0\n")
        exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
    (tmp / "host.yml").write_text(host)
    env = {
        **os.environ,
        "CITRUS_ROOT": str(root),
        "ORANGE_GUI_BINARY": str(tmp / "orange"),
        "XRANDR_BIN": str(tmp / "xrandr"),
        "FAKE_XRANDR_SCREENS": json.dumps(screens),
    }
    result = subprocess.run(
        ["bash", str(LAUNCHER), "--bootstrap-centers-only", "--foreground-gray-u8", "80",
         "--display", ":1", "--host-config", str(tmp / "host.yml"), *extra],
        capture_output=True, text=True, env=env, timeout=60,
    )
    require(result.returncode == 0, f"dry-run failed: {result.stderr}")
    require("Dry-run only" in result.stdout, "dry-run never executes")
    fields: dict[str, str] = {}
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith(("displays=", "citrus_output=", "display_check=")):
            for token in line.split(" "):
                if token.startswith("stimulus:"):
                    fields["stimulus"] = token.split(":", 1)[1]
                elif "=" in token:
                    key, value = token.split("=", 1)
                    fields[key] = value
            if line.startswith("display_check="):
                fields["display_check"] = line.split("=", 1)[1]
    return fields


def test_launcher() -> None:
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        # The 2026-11-10 layout: projector on its own X screen, declared in host.yml.
        f = plan(tmp, TWO_SCREENS, host_yaml("DP-2", 1))
        require(f["displays"] == "operator:DP-0@screen0", f"operator from xrandr primary: {f}")
        require(f["stimulus"] == "DP-2@screen1" and f["layout"] == "separate_x_screens", f"{f}")
        require(f["stimulus_source"] == "host_config" and f["citrus_output"] == "host_config", f"{f}")
        require(f["orange_reserved_monitor"] == "none_separate_x_screen", f"no reserve: {f}")
        require(f["citrus_monitor_check"] == "DP-2" and f["display_check"] == "ok", f"{f}")

        # One X screen with both outputs: no x_screen in host.yml, Orange reserves DP-2.
        f = plan(tmp, ONE_SCREEN, host_yaml("DP-2", None))
        require(f["stimulus"] == "DP-2@screen0" and f["layout"] == "shared_x_screen", f"{f}")
        require(f["orange_reserved_monitor"] == "DP-2" and f["citrus_output"] == "host_config", f"{f}")
        require(f["display_check"] == "ok", f"{f}")

        # host.yml says screen 1 but the output is on screen 0.
        f = plan(tmp, ONE_SCREEN, host_yaml("DP-2", 1))
        require(f["display_check"].startswith("Stimulus monitor DP-2 is not connected on :1.1"), f"{f}")
        require(f["layout"] == "unresolved", f"{f}")

        # A renamed output (the 2026-11-10 DP-3 -> DP-2 swap) is refused, not guessed.
        f = plan(tmp, TWO_SCREENS, host_yaml("DP-3", 1))
        require(f["display_check"].startswith("Stimulus monitor DP-3 is not connected"), f"{f}")

        # The rig is not declared.
        f = plan(tmp, TWO_SCREENS, host_yaml(None, None))
        require("declares rig 'omnifin0' 0 times" in f["display_check"], f"{f}")

        # Explicit override: found on whichever screen has it; Citrus told by name.
        f = plan(tmp, TWO_SCREENS, host_yaml(None, None), "--stimulus-monitor", "DP-2")
        require(f["stimulus"] == "DP-2@screen1" and f["stimulus_source"] == "option", f"{f}")
        require(f["citrus_output"] == "output_name_override" and f["display_check"] == "ok", f"{f}")

        # No primary output on Orange's screen and none named.
        no_primary = [[line.replace(" primary", "") for line in TWO_SCREENS[0]], TWO_SCREENS[1]]
        f = plan(tmp, no_primary, host_yaml("DP-2", 1))
        require(f["display_check"].startswith("OPERATOR_MONITOR is not resolved"), f"{f}")
        f = plan(tmp, no_primary, host_yaml("DP-2", 1), "--operator-monitor", "DP-0")
        require(f["display_check"] == "ok", f"named operator monitor: {f}")

        # The operator monitor must be on Orange's screen.
        f = plan(tmp, TWO_SCREENS, host_yaml("DP-0", 0), "--operator-monitor", "DP-2")
        require(f["display_check"].startswith("Operator monitor DP-2 is not connected on :1.0"), f"{f}")


def test_host_stimulus_output() -> None:
    config = {"rigs": [{"rig_id": "omnifin0", "display_outputs": [
        {"output_name": "DP-2", "projection_path": "main", "x_screen": 1}]}]}
    require(host_stimulus_output.stimulus_output(config, "omnifin0", "main") == ("DP-2", 1), "declared")
    config["rigs"][0]["display_outputs"][0].pop("x_screen")
    require(host_stimulus_output.stimulus_output(config, "omnifin0", "main") == ("DP-2", None), "no x_screen")
    for bad, why in (
        ({"rigs": []}, "rig missing"),
        ({"rigs": [config["rigs"][0], config["rigs"][0]]}, "rig twice"),
        ({"rigs": [{"rig_id": "omnifin0", "display_outputs": []}]}, "no output"),
        ({"rigs": [{"rig_id": "omnifin0", "display_outputs": [
            {"output_name": "DP-2", "projection_path": "main", "x_screen": -1}]}]}, "negative x_screen"),
        ({"rigs": [{"rig_id": "omnifin0", "display_outputs": [
            {"output_name": "DP-2", "projection_path": "main", "x_screen": True}]}]}, "boolean x_screen"),
    ):
        try:
            host_stimulus_output.stimulus_output(bad, "omnifin0", "main")
        except ValueError:
            continue
        raise AssertionError(f"{why} must be refused")


def main() -> int:
    test_host_stimulus_output()
    test_launcher()
    print("run_gui_arena_centering_display_tests: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
