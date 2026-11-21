#!/usr/bin/env python3
"""Orange's C++ camera preset digest equals scripts/camera_config_dir_digest.py (rule
orange_camera_config_dir_sha256_v1) on the same folders, including the error cases.

  launch_configuration_digest_tests.py <launch_configuration_tests binary>
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DIGEST = REPO / "scripts" / "camera_config_dir_digest.py"


def python_digest(folder: Path) -> str | None:
    result = subprocess.run([sys.executable, str(DIGEST), str(folder)], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def cpp_digest(binary: str, folder: Path) -> str | None:
    result = subprocess.run([binary, "--digest", str(folder)], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def main() -> int:
    binary = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory(prefix="launch_configuration_digest_") as tmp:
        root = Path(tmp)
        cases: list[tuple[str, Path, bool]] = []

        plain = root / "100_cam4_ptp_fourcam"
        plain.mkdir()
        for serial in ("2010093", "2010094", "2010095", "2010096"):
            (plain / f"Cam{serial}.json").write_bytes(b'{"exposure": 100, "serial": "%s"}\n' % serial.encode())
        (plain / "README.md").write_text("ignored")
        (plain / "sub").mkdir()
        (plain / "sub" / "Cam1.json").write_text("{}")
        cases.append(("plain preset", plain, True))

        odd = root / "odd_names"
        odd.mkdir()
        (odd / ".hidden.json").write_bytes(b"{}")
        (odd / "Cam é中.json").write_bytes(b"\xff\x00 raw bytes, not JSON")
        (odd / 'quote"back\\slash.json').write_bytes(b"[]")
        (odd / "UPPER.JSON").write_bytes(b"not included: the suffix is case-sensitive")
        (odd / "a.json").write_bytes(b"")
        cases.append(("hidden, unicode, quote/backslash names, empty and non-JSON bytes", odd, True))

        link = root / "with_symlink"
        link.mkdir()
        (link / "Cam1.json").write_text("{}")
        os.symlink(link / "Cam1.json", link / "Cam2.json")
        cases.append(("symlinked *.json", link, False))

        nested = root / "json_folder"
        nested.mkdir()
        (nested / "Cam1.json").write_text("{}")
        (nested / "x.json").mkdir()
        cases.append(("*.json that is a folder", nested, False))

        empty = root / "no_json"
        empty.mkdir()
        (empty / "notes.txt").write_text("x")
        cases.append(("no *.json", empty, False))
        cases.append(("missing folder", root / "missing", False))

        for label, folder, expect_digest in cases:
            py = python_digest(folder)
            cpp = cpp_digest(binary, folder)
            ok = py == cpp and (py is not None) == expect_digest
            print(f"{'ok  ' if ok else 'FAIL'} {label}: python={py} cpp={cpp}")
            failures += 0 if ok else 1
    print("launch_configuration_digest_tests: " + ("PASS" if failures == 0 else f"{failures} failure(s)"))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
