#!/usr/bin/env python3
"""Ratchet: frame references Orange writes must follow agent-contracts frame_identity_v1.

Scans every string literal in Orange's producer sources (the names that end up in CSV headers,
JSON keys and log fields) and fails on any token that refers to a frame without one of the
contract's counter suffixes, or that is one of the contract's forbidden bare names.

A token is a frame reference if it ends in frame_id(s), frame_num(s), frame_number,
frame_index or frame_indices. It passes if it ends in a counter suffix or a position name
(singular or plural: noncontiguous_recording_frame_ids, keyframe_video_frame_indices). Anything
else that matches must be listed, with a reason, in contracts/output_names_allowlist.json.

The rule is read from contracts/frame_identity_v1.json, a pinned copy of the agent-contracts
file (see contracts/README.md). No dependencies beyond the standard library.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

# ORANGE_RATCHET_ROOT points the checks at another tree (used by their tests).
REPO = Path(os.environ.get("ORANGE_RATCHET_ROOT") or Path(__file__).resolve().parents[1])
CONTRACT = REPO / "contracts" / "frame_identity_v1.json"
ALLOWLIST = REPO / "contracts" / "output_names_allowlist.json"

# Producers: everything Orange compiles into the GUI, the headless client and the recorder.
PRODUCER_GLOBS = ("src/**/*.cpp", "src/**/*.h", "tools/external_recorder_ipc_probe.cpp")
EXCLUDE_DIRS = ("src/NvEncoder/",)

STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
FRAME_REFERENCE = re.compile(r"(^|_)frame_(id|ids|num|nums|number|index|indices)$")


def load_rule() -> tuple[tuple[str, ...], tuple[str, ...], set[str]]:
    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    suffixes = tuple(contract["counter_suffixes"])
    suffixes += tuple(s + "s" for s in suffixes)
    positions = tuple(contract.get("position_names", {}).keys()) + (
        contract.get("video_position_suffix", "video_frame_index"),
    )
    positions += tuple(p[: -len("index")] + "indices" for p in positions if p.endswith("index"))
    forbidden = set(contract["forbidden_bare_names"])
    return suffixes, positions, forbidden


def load_allowlist() -> dict[str, str]:
    if not ALLOWLIST.exists():
        return {}
    entries = json.loads(ALLOWLIST.read_text(encoding="utf-8"))["allowed"]
    for name, reason in entries.items():
        if not reason.strip():
            raise SystemExit(f"{ALLOWLIST}: allowlisted name {name!r} has no reason")
    return entries


def producer_files() -> list[Path]:
    files: set[Path] = set()
    for pattern in PRODUCER_GLOBS:
        files.update(REPO.glob(pattern))
    return sorted(
        f for f in files
        if not any(str(f.relative_to(REPO)).startswith(d) for d in EXCLUDE_DIRS)
    )


def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments while keeping line numbers and string literals."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif c == "'" and i + 2 < n:
            j = i + 1
            while j < n and text[j] != "'" and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def violations() -> list[str]:
    suffixes, positions, forbidden = load_rule()
    allowed = load_allowlist()
    used_allowlist: set[str] = set()
    found: list[str] = []
    for path in producer_files():
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for line_number, line in enumerate(text.splitlines(), start=1):
            for literal in STRING_LITERAL.findall(line):
                for token in TOKEN.findall(literal.replace("\\n", " ").replace('\\"', " ")):
                    if token in allowed:
                        used_allowlist.add(token)
                        continue
                    rel = path.relative_to(REPO)
                    if token in forbidden:
                        found.append(f"{rel}:{line_number}: forbidden name {token!r}")
                    elif FRAME_REFERENCE.search(token) and not token.endswith(suffixes + positions):
                        found.append(
                            f"{rel}:{line_number}: {token!r} refers to a frame without a "
                            f"frame_identity_v1 suffix ({', '.join(suffixes)})")
    for name in sorted(set(allowed) - used_allowlist):
        found.append(f"{ALLOWLIST.relative_to(REPO)}: {name!r} is allowlisted but no longer used")
    return found


def main() -> int:
    argparse.ArgumentParser(description=__doc__.splitlines()[0]).parse_args()
    found = violations()
    for line in found:
        print(line)
    if found:
        print(f"check_output_names: {len(found)} violation(s); rename to a frame_identity_v1 "
              f"name, or allowlist with a reason in {ALLOWLIST.relative_to(REPO)}")
        return 1
    print("check_output_names: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
