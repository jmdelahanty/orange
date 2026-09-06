#!/usr/bin/env python3
"""Exercise only orange_client's camera-free experiment-spec validation path."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--orange-client", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    base = json.loads((root / "experiment_specs/2010096_headless_timed_recording_control_smoke_a16_gpu5.json").read_text())
    valid = {"schema_version": 1, "enabled": True, "writer_cpu_ids": [0], "queue_capacity": 4096}
    cases = [("absent", None, True), ("enabled", valid, True),
             ("disabled", {"schema_version": 1, "enabled": False}, True)]
    for name, update in [
        ("missing_cpu", {"writer_cpu_ids": []}),
        ("version", {"schema_version": 2}),
        ("bool_version", {"schema_version": True}),
        ("fractional_capacity", {"queue_capacity": 3.5}),
        ("bool_enabled", {"enabled": "true"}),
        ("unknown", {"misspelled": 1}),
        ("duplicate_cpu", {"writer_cpu_ids": [0, 0]}),
    ]:
        cases.append((name, {**valid, **update}, False))
    cases.extend([("untimed", valid, False), ("stream_only", valid, False)])
    with tempfile.TemporaryDirectory(prefix="orange_master_spec_") as tmp:
        directory = Path(tmp)
        for name, config, accepted in cases:
            spec = copy.deepcopy(base)
            spec["fixed"]["output_root"] = str(directory / "must_not_be_created")
            if config is not None:
                spec["fixed"]["master_frame_journal"] = config
            if name == "untimed":
                del spec["fixed"]["recording_control"]
            if name == "stream_only":
                spec["fixed"]["stream_only"] = True
            path = directory / f"{name}.json"
            path.write_text(json.dumps(spec))
            result = subprocess.run(
                [str(args.orange_client.resolve()), "--mode", "local", "--experiment-spec", str(path),
                 "--validate-experiment-spec"],
                text=True, capture_output=True, timeout=20, check=False,
            )
            if (result.returncode == 0) != accepted:
                raise AssertionError(f"{name}: unexpected exit {result.returncode}\n{result.stdout}\n{result.stderr}")
            if not accepted and "master" not in result.stderr.lower():
                raise AssertionError(f"{name}: refused for an unrelated reason: {result.stderr}")
            if (directory / "must_not_be_created").exists():
                raise AssertionError("validation entered the recording/output workflow")
        print(f"{len(cases)} headless journal spec admission cases passed (no camera launch)")


if __name__ == "__main__":
    main()
