#!/usr/bin/env python3
"""Check a staged Orange release's shared libraries and write their record.

    scripts/check_release_libraries.py <stage dir> [--record <out.json>]

For every ELF file in <stage>/bin and <stage>/lib:
  - refuses (exit 1) when a library is not found, when the RUNPATH/RPATH names
    a folder under /home, or when a library resolves under /home: an installed
    release must not depend on anything a user can change;
  - records each resolved library: inside the release as its relative path,
    outside as its absolute path with the sha256 of the file it resolves to
    (the system and /opt/orange/lib libraries the release uses but does not
    ship, so a later change to one of them is visible).
The record (--record) is the `runtime_libraries` block of INSTALLED.json.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

FORBIDDEN_PREFIXES = ("/home/", "/root/", "/tmp/")


def is_elf(path: Path) -> bool:
    try:
        with path.open("rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def runpath(path: Path) -> list[str]:
    out = subprocess.run(["readelf", "-d", str(path)], capture_output=True, text=True, check=True).stdout
    folders: list[str] = []
    for m in re.finditer(r"\((?:RUNPATH|RPATH)\)\s+Library r(?:un)?path: \[(.*)\]", out):
        folders.extend(x for x in m.group(1).split(":") if x)
    return folders


def resolved(path: Path) -> list[tuple[str, str | None]]:
    out = subprocess.run(["ldd", str(path)], capture_output=True, text=True).stdout
    libs = []
    for line in out.splitlines():
        line = line.strip()
        m = re.match(r"(\S+) => (not found|(\S+) \(0x[0-9a-f]+\))", line)
        if m:
            libs.append((m.group(1), None if m.group(2) == "not found" else m.group(3)))
    return libs


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return "sha256:" + h.hexdigest()


def check(stage: Path) -> tuple[dict, list[str]]:
    stage = stage.resolve()
    problems: list[str] = []
    record: dict = {"rule": "ldd_resolved_at_build_v1", "binaries": {}}
    files = sorted(p for d in ("bin", "lib") if (stage / d).is_dir()
                   for p in (stage / d).iterdir() if p.is_file() and not p.is_symlink() and is_elf(p))
    for path in files:
        rel = path.relative_to(stage).as_posix()
        for folder in runpath(path):
            if folder.startswith(FORBIDDEN_PREFIXES):
                problems.append(f"{rel}: runpath names {folder}")
        entries = []
        for soname, target in resolved(path):
            if target is None:
                problems.append(f"{rel}: {soname} not found")
                continue
            real = os.path.realpath(target)
            if real.startswith(FORBIDDEN_PREFIXES) and not real.startswith(str(stage) + "/"):
                problems.append(f"{rel}: {soname} resolves to {real}")
            if real.startswith(str(stage) + "/"):
                entries.append({"soname": soname, "release_path": Path(real).relative_to(stage).as_posix()})
            else:
                entries.append({"soname": soname, "path": real, "sha256": sha256(real)})
        record["binaries"][rel] = entries
    return record, problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("stage", type=Path)
    ap.add_argument("--record", type=Path)
    args = ap.parse_args()
    record, problems = check(args.stage)
    for p in problems:
        print(f"[release] library check: {p}", file=sys.stderr)
    if problems:
        return 1
    if args.record:
        args.record.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
