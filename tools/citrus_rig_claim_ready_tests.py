#!/usr/bin/env python3
"""Unit tests for scripts/citrus_rig_claim_ready.py (both Citrus builds)."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from citrus_rig_claim_ready import rig_claim_state  # noqa: E402


def check(got: tuple[str, str], want: str, what: str) -> None:
    if got[0] != want:
        raise SystemExit(f"FAIL {what}: {got}")
    print(f"ok: {what} -> {got[0]} ({got[1]})")


def main() -> int:
    check(rig_claim_state({"ok": True, "status": {"experiment": {}}}, "omnifin0"), "ready",
          "no rig_claim field (240c6cc): proceed as now")
    check(rig_claim_state({"ok": True, "status": {"rig_claim": None}}, "omnifin0"), "wait",
          "rig_claim null: wait for the claim")
    check(rig_claim_state({"ok": False, "error": {"code": "rig_binding_no_rig_claimed"}}, "omnifin0"), "wait",
          "refused before the claim: wait")
    check(rig_claim_state({"ok": True, "status": {"rig_claim": {"rig_id": "omnifin0", "claimed_at_utc": "x",
                                                               "source": "autorun"}}}, "omnifin0"), "ready",
          "claimed for omnifin0: ready")
    check(rig_claim_state({"ok": True, "status": {"rig_claim": {"rig_id": "other"}}}, "omnifin0"), "fail",
          "claimed for another rig: fail")
    check(rig_claim_state({}, "omnifin0"), "wait", "empty response: wait")
    print("citrus_rig_claim_ready_tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
