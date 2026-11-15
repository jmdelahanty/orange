#!/usr/bin/env python3
"""docs/orange_local_control_events_v1.md lists exactly the events Orange writes.

Scans the local-control writers (GUI, orange_client, the shared control
host) for event names, including the ones chosen with ?: and fails when the
code and the contract disagree either way.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WRITERS = [
    ROOT / "src" / "orange.cpp",
    ROOT / "src" / "orange_headless_client.cpp",
    ROOT / "src" / "session" / "session_control_host.cpp",
]
CONTRACT = ROOT / "docs" / "orange_local_control_events_v1.md"

PLAIN = re.compile(r'\{"event",\s*"([a-z_]+)"\}')
# {"event", cond ? "a" : "b"} across one or two lines
TERNARY = re.compile(r'\{"event",\s*[^{};]*?\?\s*"([a-z_]+)"\s*:\s*"([a-z_]+)"', re.S)


def code_events() -> set[str]:
    names: set[str] = set()
    for path in WRITERS:
        text = path.read_text()
        names.update(PLAIN.findall(text))
        for a, b in TERNARY.findall(text):
            names.update((a, b))
    return names


def contract_events() -> set[str]:
    names: set[str] = set()
    for line in CONTRACT.read_text().splitlines():
        if not line.startswith("| `"):
            continue
        first_cell = line.split("|")[1]
        names.update(re.findall(r"`([a-z_]+)`", first_cell))
    return names


def main() -> int:
    code = code_events()
    contract = contract_events()
    missing = sorted(code - contract)
    stale = sorted(contract - code)
    if missing:
        print("events Orange writes that the contract does not list: " + ", ".join(missing), file=sys.stderr)
    if stale:
        print("events the contract lists that Orange no longer writes: " + ", ".join(stale), file=sys.stderr)
    if missing or stale:
        return 1
    print(f"local_control_events_contract_tests passed ({len(code)} events)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
