#!/usr/bin/env python3
"""Tests for the NIC port counters check in validate_gui_ptp_recording.py."""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import validate_gui_ptp_recording as validator  # noqa: E402


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def port(delta: dict, *, operstate: str = "up", valid: int = 3, intervals: list | None = None) -> dict:
    return {
        "pci_bdf": "0000:61:00.0",
        "operstate": operstate,
        "valid_samples": valid,
        "failed_samples": 0,
        "last_error": "",
        "first": {},
        "last": {},
        "delta": delta,
        "counters_decreased": [],
        "changed_intervals": intervals or [],
        "changed_intervals_truncated": False,
    }


def counters(ports: dict, **overrides: object) -> dict:
    value = {
        "schema_id": "orange.nic_port_counters",
        "schema_version": 1,
        "status": "completed",
        "final_sample_at_stop": True,
        "started_realtime_posix_ns": 1_000_000_000_000,
        "ports_with_loss": [],
        "ports": ports,
    }
    value.update(overrides)
    return value


def run(data: dict | None, *, require_file: bool = False) -> validator.Reporter:
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        if data is not None:
            (folder / "nic_port_counters.json").write_text(json.dumps(data), encoding="utf-8")
        reporter = validator.Reporter(verbose=False)
        validator.check_nic_port_counters(
            reporter, folder, argparse.Namespace(require_nic_port_counters=require_file))
        return reporter


def main() -> int:
    clean = {"rx_discards_phy": 0, "rx_out_of_buffer": 0, "tx_global_pause": 0, "rx_packets_phy": 18_000_000}
    reporter = run(counters({"mlnx1_p1_25g": port(clean), "mlnx2_p1_25g": port({}, operstate="down", valid=0)}))
    require(not reporter.failures and not reporter.warnings, f"clean ports pass: {reporter.failures} {reporter.warnings}")
    require(any("1 carrying camera traffic" in p for p in reporter.passes), f"pass line counts ports: {reporter.passes}")

    lossy = dict(clean, rx_discards_phy=12, tx_global_pause=3)
    interval = [{"sample_index": 40, "realtime_posix_ns": 1_000_000_000_000 + 200 * 10**9, "delta": {"rx_discards_phy": 12}}]
    reporter = run(counters({"mlnx1_p1_25g": port(lossy, intervals=interval)}))
    require(any("rx_discards_phy +12" in f and "200 s" in f for f in reporter.failures),
            f"discards fail with when they started: {reporter.failures}")
    require(any("tx_global_pause +3" in w for w in reporter.warnings), f"pause frames warn: {reporter.warnings}")
    require(not any(p.startswith("NIC port counters: no discards") for p in reporter.passes), "no pass line on loss")

    reporter = run(None)
    require(not reporter.failures and any("missing" in w for w in reporter.warnings),
            "an older recording without the file warns")
    reporter = run(None, require_file=True)
    require(any("missing" in f for f in reporter.failures), "the GUI's post-finalize run requires the file")

    reporter = run(counters({"mlnx1_p1_25g": port(clean)}, status="running", final_sample_at_stop=False))
    require(not reporter.failures and any("did not finish" in w for w in reporter.warnings),
            "a helper that never finished warns")
    reporter = run(counters({}, schema_version=2))
    require(any("unexpected schema" in f for f in reporter.failures), "an unknown version fails")

    print("validate_nic_port_counters_tests: PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL: {error}")
        sys.exit(1)
