#!/usr/bin/env python3
"""The data-root rule (src/orange_data_root.h, scripts/orange_paths.py): cases and
C++/Python parity.

  orange_data_root_tests.py <runtime_paths_tests binary>
"""

from __future__ import annotations

import os
import pwd
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import orange_paths  # noqa: E402

ME = pwd.getpwuid(os.getuid())


def python_root(env: dict[str, str]) -> str:
    saved = {k: os.environ.get(k) for k in ("ORANGE_DATA_ROOT", "SUDO_USER", "HOME")}
    try:
        for key in saved:
            os.environ.pop(key, None)
        os.environ.update(env)
        try:
            return str(orange_paths.orange_data_root())
        except orange_paths.DataRootError as error:
            return f"ERROR: {error}"
    finally:
        for key, value in saved.items():
            os.environ.pop(key, None)
            if value is not None:
                os.environ[key] = value


def cpp_root(binary: str, env: dict[str, str]) -> str:
    base = {k: v for k, v in os.environ.items() if k not in ("ORANGE_DATA_ROOT", "SUDO_USER", "HOME")}
    base.update(env)
    out = subprocess.run([binary, "--data-root"], env=base, capture_output=True, text=True, check=True)
    return out.stdout.strip()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    if os.getuid() == 0:
        print("orange_data_root_tests: skipped as root (the cases assume a normal account)")
        return 0
    binary = sys.argv[1] if len(sys.argv) > 1 else str(REPO / "targets" / "release" / "runtime_paths_tests")
    mine = str(Path(ME.pw_dir) / "orange_data")
    cases = [
        ({"ORANGE_DATA_ROOT": "/tmp/orange_data_root_override"}, "/tmp/orange_data_root_override"),
        ({"SUDO_USER": ME.pw_name, "HOME": "/root"}, mine),
        ({"HOME": "/tmp/not_my_home"}, mine),
        ({}, mine),
        ({"SUDO_USER": "no_such_user_orange_test"}, "ERROR: "),
    ]
    for env, expected in cases:
        py = python_root(env)
        cc = cpp_root(binary, env)
        if expected.startswith("ERROR"):
            require(py.startswith("ERROR") and cc.startswith("ERROR"), f"{env}: both refuse ({py!r}, {cc!r})")
        else:
            require(py == expected, f"python {env}: {py!r} != {expected!r}")
            require(cc == expected, f"c++ {env}: {cc!r} != {expected!r}")
    print("orange_data_root_tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
