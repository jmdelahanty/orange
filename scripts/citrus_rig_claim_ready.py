#!/usr/bin/env python3
"""Wait until Citrus is ready for a rig's socket commands (daily registration launcher).

Citrus draft #124 makes each process claim one rig: before the claim,
status.rig_claim is null and socket commands are refused with
rig_binding_no_rig_claimed; after it, rig_claim is {rig_id, claimed_at_utc,
source}. Builds without that field (240c6cc) are ready as soon as status answers.

    citrus_rig_claim_ready.py --socket /tmp/citrus_local_control.sock --rig-id omnifin0 [--timeout-seconds 120]

Exit 0 when ready; 1 on timeout or a claim for another rig. Read only: it sends
status requests only.
"""

from __future__ import annotations

import argparse
import sys
import time
import uuid
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from orange_citrus_orchestrator import (  # noqa: E402
    CITRUS_REQUEST_SCHEMA_ID,
    build_status_request,
    send_unix_json,
)


def rig_claim_state(response: dict[str, Any], rig_id: str) -> tuple[str, str]:
    """('ready' | 'wait' | 'fail', reason) for one Citrus status response."""
    if not isinstance(response, dict):
        return "wait", "no_response"
    if not response.get("ok", False):
        error = response.get("error")
        code = error.get("code") if isinstance(error, dict) else error
        if code == "rig_binding_no_rig_claimed":
            return "wait", "rig_binding_no_rig_claimed"
        return "wait", f"status_not_ok:{code}"
    status = response.get("status")
    if not isinstance(status, dict):
        return "wait", "status_missing"
    if "rig_claim" not in status:
        return "ready", "no_rig_claim_field (Citrus before one-rig claims)"
    claim = status["rig_claim"]
    if claim is None:
        return "wait", "rig_not_claimed_yet"
    if not isinstance(claim, dict) or claim.get("rig_id") != rig_id:
        claimed = claim.get("rig_id") if isinstance(claim, dict) else claim
        return "fail", f"citrus_claimed_another_rig:{claimed}"
    return "ready", f"rig_claimed:{rig_id}:{claim.get('source')}"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--rig-id", required=True)
    parser.add_argument("--timeout-seconds", type=float, default=120.0)
    args = parser.parse_args(argv)
    deadline = time.monotonic() + args.timeout_seconds
    last = ""
    while True:
        try:
            response = send_unix_json(
                args.socket,
                build_status_request(CITRUS_REQUEST_SCHEMA_ID, f"rig_claim_ready:{uuid.uuid4()}",
                                     "orange_daily_registration_launcher"),
                5.0,
            )
        except Exception as error:  # the socket may not answer yet
            response = {"ok": False, "error": {"code": f"socket:{type(error).__name__}"}}
        state, reason = rig_claim_state(response, args.rig_id)
        if reason != last:
            print(f"[citrus_rig_claim_ready] {state}: {reason}", flush=True)
            last = reason
        if state == "ready":
            return 0
        if state == "fail" or time.monotonic() > deadline:
            print(f"[citrus_rig_claim_ready] not ready: {reason}", file=sys.stderr)
            return 1
        time.sleep(1.0)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
