#!/usr/bin/env python3
"""fixed.local_control in orange_client experiment specs (--validate-experiment-spec only:
no camera, socket or recording). Usage: headless_local_control_spec_tests.py <orange_client>"""

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BASE = REPO / "experiment_specs" / "fourcam_fused_recorder_realfish_int8_192_native_ownerpush_ring_od8ff_s16_integration.json"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def validate(client: str, spec: dict) -> tuple[int, str]:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "spec.json"
        path.write_text(json.dumps(spec))
        result = subprocess.run(
            [client, "--mode", "local", "--experiment-spec", str(path), "--validate-experiment-spec"],
            capture_output=True, text=True, timeout=120,
        )
        return result.returncode, result.stdout + result.stderr


def with_local_control(base: dict, block, **fixed) -> dict:
    spec = copy.deepcopy(base)
    if block is not None:
        spec["fixed"]["local_control"] = block
    spec["fixed"].update(fixed)
    return spec


def main() -> int:
    client = sys.argv[1]
    if not Path(client).is_file():
        # orange_client is EXCLUDE_FROM_ALL: build it with --target orange_client.
        print(f"headless_local_control_spec_tests: SKIP ({client} is not built)")
        return 0
    base = json.loads(BASE.read_text())
    require(base["fixed"]["recording_control"]["record_for_seconds"] > 0, "base spec is timed")

    rc, out = validate(client, with_local_control(base, None))
    require(rc == 0 and "status=pass" in out, f"base spec passes: {out[-400:]}")

    rc, out = validate(client, with_local_control(
        base, {"enabled": True, "stop_on_citrus_completion": True, "receipt_wait_seconds": 600}))
    require(rc == 0 and "status=pass" in out, f"enabled with Citrus stop passes: {out[-400:]}")

    for block, needle in (
        ({"stop_on_citrus_completion": True}, "need enabled: true"),
        ({"enabled": True, "allow_stop_recording": "yes"}, "allow_stop_recording must be true or false"),
        ({"enabled": True, "receipt_wait_seconds": 0}, "receipt_wait_seconds must be"),
        ({"enabled": True, "receipt_wait_seconds": 1.5}, "receipt_wait_seconds must be"),
        ({"enabled": True, "socket": "/tmp/x"}, "unknown key socket"),
        ([], "must be a JSON object"),
    ):
        rc, out = validate(client, with_local_control(base, block))
        require(rc != 0 and needle in out, f"{block!r} refused with '{needle}': {out[-400:]}")

    untimed = with_local_control(base, {"enabled": True, "stop_on_citrus_completion": True})
    untimed["fixed"]["recording_control"] = {"record_for_seconds": 0}
    rc, out = validate(client, untimed)
    require(rc != 0 and "needs a recording with" in out, f"Citrus stop needs a ceiling: {out[-400:]}")

    print("headless_local_control_spec_tests: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
