#!/usr/bin/env python3
"""recording_observation_binding_cli on a synthetic bundle: the unreachable-socket
pre-arm path (optional mode: unbound decision, arming allowed, replay identical)
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
                "ORANGE_CITRUS_OBSERVATION_BINDING_MODE", "ORANGE_CITRUS_BINDING_REQUEST_VERSION"):
        env.pop(key, None)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        def make_bundle(name: str, mode: str) -> Path:
            out = root / name
            code, _ = run([str(bundle_tool), "--out", str(out), "--fixture-rig", str(root / f"rig_{name}"),
                           *(f for c in CAMERAS for f in ("--camera", c)),
                           "--intent", "stimulus_experiment", "--recording-subtype", "omit",
                           "--binding-mode", mode, "--request-version", "2"], env)
            assert code == 0
            return out

        # The sealed requests carry the binding mode; the CLI must re-verify
        # with the same mode (a different one is refused as evidence mismatch).
        out = make_bundle("bundle_optional", "optional")
        code, mismatch = run([str(cli), "prearm", "--folder", str(out), "--binding-mode", "required",
                              "--socket", str(root / "nobody.sock"), "--timeout-ms", "100",
                              "--request-version", "2"], env)
        assert code != 0 and "does not match sealed recording evidence" in mismatch.get("error", ""), mismatch

        # Pre-arm against a socket nobody listens on: optional mode records an
        # unbound decision with arming allowed; replay returns the same decision
        # and still reaches nothing.
        socket = str(root / "nobody.sock")
        code, first = run([str(cli), "prearm", "--folder", str(out), "--binding-mode", "optional",
                           "--socket", socket, "--timeout-ms", "100", "--request-version", "2"], env)
        assert code == 0, first
        assert first["lifecycle_status"] == "unbound" and first["arm_allowed"] is True, first
        assert first["transport_attempted"] is True and first["requests"]["count"] == len(CAMERAS), first
        assert first["acceptance_count"] == 0 and first["context_rejections"] == []
        decision_path = out / first["decision_relative_path"]
        assert decision_path.is_file()
        code, again = run([str(cli), "prearm", "--folder", str(out), "--binding-mode", "optional",
                           "--socket", socket, "--timeout-ms", "100", "--request-version", "2"], env)
        assert code == 0 and again["decision_sha256"] == first["decision_sha256"], again

        # Required mode against the same unreachable socket: the production gate
        # records a controlled unbound decision (handshake_not_completed) with
        # arming refused; the decision itself is written, so the step succeeds.
        out_required = make_bundle("bundle_required", "required")
        code, required = run([str(cli), "prearm", "--folder", str(out_required), "--binding-mode",
                              "required", "--socket", socket, "--timeout-ms", "100", "--request-version", "2"], env)
        assert code == 0 and required["arm_allowed"] is False, required
        assert required["lifecycle_status"] == "unbound" and required["reason"] == "handshake_not_completed", required

        # Finalize without acceptances / with malformed params is refused clearly.
        params = root / "params.json"
        params.write_text(json.dumps({"experiment_id": "citexp_none", "receipts": []}))
        code, fin = run([str(cli), "finalize", "--folder", str(out), "--receipts", str(params)], env)
        assert code != 0 and fin.get("ok") is False, fin
        params.write_text("[]")
        code, fin = run([str(cli), "finalize", "--folder", str(out), "--receipts", str(params)], env)
        assert code != 0

    print("recording_observation_binding_cli_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
