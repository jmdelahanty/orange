#!/usr/bin/env python3
"""recording_observation_binding_cli on a synthetic bundle: the unreachable-socket
pre-arm path (mode from the intent: stimulus_experiment gets an unbound decision
with arming refused and an identical replay; recording_only binds nothing)
and the finalize refusal paths (no acceptances; malformed params).

Usage: recording_observation_binding_cli_tests.py <cli> <synthetic_recording_bundle>
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

CAMERAS = ("2010095", "2010096")


def run(cmd: list[str], env: dict | None = None) -> tuple[int, dict]:
    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    try:
        payload = json.loads(result.stdout[result.stdout.index("{"):]) if "{" in result.stdout else {}
    except json.JSONDecodeError:
        payload = {}
    return result.returncode, payload | {"_stderr": result.stderr}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    cli, bundle_tool = Path(sys.argv[1]), Path(sys.argv[2])
    assert cli.is_file() and bundle_tool.is_file()
    env = dict(os.environ)
    for key in ("ORANGE_CITRUS_OBSERVATION_BINDING_SOCKET", "ORANGE_CITRUS_PROJECTION_SNAPSHOT_SOCKET",
                "ORANGE_CITRUS_BINDING_REQUEST_VERSION"):
        env.pop(key, None)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        def make_bundle(name: str, intent: str) -> Path:
            out = root / name
            code, _ = run([str(bundle_tool), "--out", str(out), "--fixture-rig", str(root / f"rig_{name}"),
                           *(f for c in CAMERAS for f in ("--camera", c)),
                           "--intent", intent, "--recording-subtype", "omit",
                           "--request-version", "2"], env)
            assert code == 0
            return out

        # The binding mode follows the frozen intent; there is no --binding-mode
        # and no optional mode (2026-11-02).
        socket = str(root / "nobody.sock")
        out_required = make_bundle("bundle_required", "stimulus_experiment")
        code, removed = run([str(cli), "prearm", "--folder", str(out_required), "--binding-mode", "optional"], env)
        assert code != 0, removed

        # stimulus_experiment against a socket nobody listens on: the production
        # gate records a controlled unbound decision (handshake_not_completed)
        # with arming refused; a decision exists (ok) but nothing is bound: exit 3.
        # A replay returns the same decision and reaches nothing.
        code, required = run([str(cli), "prearm", "--folder", str(out_required),
                              "--socket", socket, "--timeout-ms", "100", "--request-version", "2"], env)
        assert code == 3 and required["ok"] is True and required["bound"] is False, required
        assert required["binding_mode"] == "required" and required["arm_allowed"] is False, required
        assert required["lifecycle_status"] == "unbound" and required["reason"] == "handshake_not_completed", required
        assert required["transport_attempted"] is True and required["requests"]["count"] == len(CAMERAS), required
        assert required["acceptance_count"] == 0 and required["context_rejections"] == []
        assert (out_required / required["decision_relative_path"]).is_file()
        code, again = run([str(cli), "prearm", "--folder", str(out_required),
                           "--socket", socket, "--timeout-ms", "100", "--request-version", "2"], env)
        assert code == 3 and again["decision_sha256"] == required["decision_sha256"], again

        # A recording-only bundle has nothing to bind: exit 0 with not_applicable.
        out_na = root / "bundle_not_applicable"
        code, _ = run([str(bundle_tool), "--out", str(out_na), "--fixture-rig", str(root / "rig_na"),
                       *(f for c in CAMERAS for f in ("--camera", c)), "--intent", "recording_only",
                       "--recording-subtype", "dish_freeswim"], env)
        assert code == 0
        code, na = run([str(cli), "prearm", "--folder", str(out_na),
                        "--socket", socket, "--timeout-ms", "100"], env)
        assert code == 0 and na["bound"] is True and na["lifecycle_status"] == "not_applicable", na

        # Finalize without acceptances / with malformed params is refused clearly.
        params = root / "params.json"
        params.write_text(json.dumps({"experiment_id": "citexp_none", "receipts": []}))
        code, fin = run([str(cli), "finalize", "--folder", str(out_required), "--receipts", str(params)], env)
        assert code == 1 and fin.get("ok") is False and fin.get("bound") is False, fin
        params.write_text("[]")
        code, fin = run([str(cli), "finalize", "--folder", str(out_required), "--receipts", str(params)], env)
        assert code == 1

    print("recording_observation_binding_cli_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
