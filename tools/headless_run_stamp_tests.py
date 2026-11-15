#!/usr/bin/env python3
"""--run-stamp: a run gets unique names from the spec file itself, with no stamped copy.

Camera-free: orange_client --validate-experiment-spec and the benchmark
wrapper's --dry-run.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GATE_SPEC = ROOT / "experiment_specs" / "fourcam_fused_recorder_realfish_int8_192_native_ownerpush_ring_od8ff_s16.json"
WRAPPER = ROOT / "scripts" / "orange_local_benchmark_wrapper.sh"
WRAPPER_CLIENT = Path("/home/jeremy/orange/targets/release/orange_client")  # the wrapper's allowed path
STAMP = "20261114_210000"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def validate(client: Path, spec: Path, *extra: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(client), "--mode", "local", "--experiment-spec", str(spec), *extra, "--validate-experiment-spec"],
        capture_output=True, text=True, timeout=20, check=False)


def main() -> int:
    client = Path(sys.argv[1]).resolve()
    source = json.loads(GATE_SPEC.read_text())
    experiment_id = source["experiment_id"]
    artifact_root = source["fixed"]["external_recorder_contract"]["artifact_root"]
    # The wrapper only takes specs under /tmp or experiment_specs/.
    with tempfile.TemporaryDirectory(prefix="orange_run_stamp_test_", dir="/tmp") as tmp:
        spec = Path(tmp) / "spec.json"
        source["fixed"]["output_root"] = str(Path(tmp) / "must_not_exist")
        spec.write_text(json.dumps(source))
        before = spec.read_bytes()

        stamped = validate(client, spec, "--run-stamp", STAMP)
        require(stamped.returncode == 0, stamped.stdout + stamped.stderr)
        require(f"experiment_id={experiment_id}_{STAMP} " in stamped.stdout, stamped.stdout)
        require(f"external_recorder_artifact_root={artifact_root}_{STAMP}" in stamped.stdout, stamped.stdout)
        require(spec.read_bytes() == before, "the spec file is never rewritten")

        plain = validate(client, spec)
        require(plain.returncode == 0 and f"experiment_id={experiment_id} " in plain.stdout, plain.stdout)

        bad = validate(client, spec, "--run-stamp", "2026-11-14")
        require(bad.returncode != 0, "a malformed stamp is refused")

        if WRAPPER_CLIENT.exists():
            dry = subprocess.run(
                ["bash", str(WRAPPER), "--dry-run", "--orange-client", str(WRAPPER_CLIENT),
                 "--run-stamp", STAMP, str(spec)],
                capture_output=True, text=True, timeout=20, check=False)
            require(dry.returncode == 0, dry.stdout + dry.stderr)
            require(f"--run-stamp {STAMP}" in dry.stdout, f"the wrapper passes the stamp on: {dry.stdout}")
            require(f"experiment_id={experiment_id}_{STAMP}" in dry.stdout,
                    "the wrapper hands back the stamped experiment folder")
            refused = subprocess.run(
                ["bash", str(WRAPPER), "--dry-run", "--orange-client", str(WRAPPER_CLIENT),
                 "--run-stamp", "now", str(spec)],
                capture_output=True, text=True, timeout=20, check=False)
            require(refused.returncode != 0, "the wrapper refuses a malformed stamp")
        require(not (Path(tmp) / "must_not_exist").exists(), "validation entered recording startup")
    print("headless_run_stamp_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
