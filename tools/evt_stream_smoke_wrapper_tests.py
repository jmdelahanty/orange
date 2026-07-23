#!/usr/bin/env python3
"""No-hardware tests for the EVT stream-smoke wrappers."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SMOKE_WRAPPER = REPO_ROOT / "scripts" / "orange_evt_stream_smoke_wrapper.sh"
LINK_HEALTH = REPO_ROOT / "scripts" / "orange_evt_stream_link_health.sh"
INSTALLER = REPO_ROOT / "scripts" / "install_orange_evt_stream_smoke_wrapper.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(
    command: list[str],
    *,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=REPO_ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def test_shell_scripts_parse() -> None:
    for script in (SMOKE_WRAPPER, LINK_HEALTH, INSTALLER):
        result = run(["bash", "-n", str(script)])
        require(result.returncode == 0, f"{script.name} failed bash -n: {result.stderr}")

    wrapper_source = SMOKE_WRAPPER.read_text(encoding="utf-8")
    require(
        "sudo -n -v" not in "\n".join(
            line for line in wrapper_source.splitlines() if not line.lstrip().startswith("#")
        ),
        "wrapper must execute its specifically authorized command without a generic sudo credential check",
    )


def test_wrapper_dry_run_builds_expected_command() -> None:
    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        config_dir = Path(tmp)
        result = run(
            [
                str(SMOKE_WRAPPER),
                "--dry-run",
                "--config-dir",
                str(config_dir),
                "--serial",
                "2012632",
                "--frame-rate",
                "250",
                "--gpu-direct",
                "1",
                "--frame-stats",
                "--measure-seconds",
                "1",
                "--buffer-count",
                "8",
            ]
        )

    require(result.returncode == 0, f"wrapper dry-run failed: {result.stderr}")
    require("targets/release/evt_stream_smoke" in result.stdout, "dry-run should name target binary")
    require("--config-dir" in result.stdout and "--serial" in result.stdout, "dry-run should include selection")
    require("2012632" in result.stdout, "dry-run should include serial")
    require("--frame-rate 250" in result.stdout, "dry-run should include frame-rate override")
    require("--gpu-direct 1" in result.stdout, "dry-run should include GPUDirect override")
    require("--frame-stats" in result.stdout, "dry-run should include frame statistics")


def test_wrapper_dry_run_inherits_env_defaults() -> None:
    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        env = os.environ.copy()
        env.update(
            {
                "ORANGE_GUI_CONFIG_DIR": tmp,
                "ORANGE_GUI_EXPECT_CAMERAS": "2012632,2010096",
            }
        )
        result = run(
            [str(SMOKE_WRAPPER), "--dry-run", "--measure-seconds", "0"],
            env=env,
        )

    require(result.returncode == 0, f"wrapper env dry-run failed: {result.stderr}")
    require("--config-dir" in result.stdout, "dry-run should inherit config dir")
    require("--serials 2012632\\,2010096" in result.stdout, "dry-run should inherit expected serials")


def test_wrapper_rejects_config_outside_allowed_roots() -> None:
    result = run(
        [
            str(SMOKE_WRAPPER),
            "--dry-run",
            "--config-dir",
            "/etc",
            "--serial",
            "2012632",
        ]
    )

    require(result.returncode == 2, "wrapper should reject config roots outside the allowlist")
    require("outside allowed roots" in result.stderr, "rejection should explain allowlist failure")


def test_link_health_dry_run_builds_smoke_command() -> None:
    if shutil.which("ip") is None:
        print("SKIP: ip command unavailable")
        return

    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        result = run(
            [
                str(LINK_HEALTH),
                "--dry-run",
                "--camera-ip",
                "127.0.0.1",
                "--config-dir",
                tmp,
                "--serial",
                "2012632",
                "--frame-rate",
                "250",
                "--gpu-direct",
                "0",
                "--measure-seconds",
                "1",
                "--buffer-count",
                "8",
            ]
        )

    if result.returncode != 0 and (
        "Cannot open netlink socket" in result.stderr
        or "Could not resolve interface" in result.stderr
    ):
        print("SKIP: link-health dry-run needs local netlink route access")
        return

    require(result.returncode == 0, f"link-health dry-run failed: {result.stderr}")
    require("interface=" in result.stdout, "link-health dry-run should resolve interface")
    require("/usr/local/bin/orange-evt-stream-smoke" in result.stdout, "dry-run should call installed wrapper")
    require("--frame-rate 250" in result.stdout, "dry-run should include frame-rate override")
    require("--gpu-direct 0" in result.stdout, "dry-run should include GPUDirect override")


def main() -> None:
    test_shell_scripts_parse()
    test_wrapper_dry_run_builds_expected_command()
    test_wrapper_dry_run_inherits_env_defaults()
    test_wrapper_rejects_config_outside_allowed_roots()
    test_link_health_dry_run_builds_smoke_command()
    print("evt_stream_smoke_wrapper_tests passed")


if __name__ == "__main__":
    main()
