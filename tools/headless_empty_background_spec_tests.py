#!/usr/bin/env python3
"""Camera-free admission of fixed.empty_background_capture in headless experiment specs.

The key names a capture by the absolute path of its background.json
(docs/empty_background_capture_design.md); a missing key means no background. The capture is
read and validated at spec load, so a bad path or an invalid descriptor refuses the spec before
any camera opens. Uses orange_client --validate-experiment-spec (no camera launch).

  headless_empty_background_spec_tests.py <orange_client>
"""

from __future__ import annotations

import copy
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE_SPEC = ROOT / "experiment_specs" / "2010096_headless_timed_recording_control_smoke_a16_gpu5.json"
FIXTURE = ROOT / "tools" / "fixtures" / "empty_background_v1" / "valid" / "capture_one_captured_one_failed.json"


def main() -> int:
    client = Path(sys.argv[1]).resolve()
    base = json.loads(BASE_SPEC.read_text())
    with tempfile.TemporaryDirectory(prefix="orange_empty_background_spec_") as tmp:
        directory = Path(tmp)
        descriptor = json.loads(FIXTURE.read_text())
        capture = directory / "backgrounds" / descriptor["capture_id"]
        capture.mkdir(parents=True)
        (capture / "background.json").write_text(json.dumps(descriptor, indent=2) + "\n")
        wrong_dir = directory / "backgrounds" / "other_name"
        shutil.copytree(capture, wrong_dir)
        broken = directory / "backgrounds" / "broken"
        broken.mkdir()
        (broken / "background.json").write_text("{}\n")

        cases = [
            ("no_key", None, True),
            ("capture", str(capture / "background.json"), True),
            ("relative", "backgrounds/" + descriptor["capture_id"] + "/background.json", False),
            ("directory", str(capture), False),
            ("missing", str(directory / "backgrounds" / "absent" / "background.json"), False),
            ("not_named_after_capture_id", str(wrong_dir / "background.json"), False),
            ("invalid_descriptor", str(broken / "background.json"), False),
            ("null", None, False),
        ]
        for name, value, accepted in cases:
            spec = copy.deepcopy(base)
            if name != "no_key":
                spec["fixed"]["empty_background_capture"] = value
            spec["fixed"]["output_root"] = str(directory / "must_not_exist")
            path = directory / f"{name}.json"
            path.write_text(json.dumps(spec))
            result = subprocess.run(
                [str(client), "--mode", "local", "--experiment-spec", str(path), "--validate-experiment-spec"],
                capture_output=True, text=True, timeout=20, check=False)
            if (result.returncode == 0) != accepted:
                raise AssertionError(f"{name}: exit {result.returncode}\n{result.stdout}\n{result.stderr}")
            if not accepted and "empty" not in (result.stdout + result.stderr):
                raise AssertionError(f"{name}: the refusal should name the empty background\n{result.stderr}")
            if (directory / "must_not_exist").exists():
                raise AssertionError("admission entered recording startup")
    print(f"{len(cases)} headless empty-background spec cases passed (no camera launch)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
