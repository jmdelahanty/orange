#!/usr/bin/env python3
"""Camera-free admission checks for explicit recording products."""
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
    crops = json.loads((root / "experiment_specs/threecam_detect_latency_levers_gate_registered_crop_synthetic_interleave.json").read_text())
    full = {"schema_version": 1, "mode": "full_frame"}
    both = {"schema_version": 1, "mode": "full_frame_and_moving_crops"}
    cases = [("absent", None, base, True), ("full", full, base, True),
             ("both", both, crops, True), ("conflicting_crop", full, crops, False),
             ("missing_crop", both, base, False),
             ("crop_only_not_integrated", {"schema_version": 1, "mode": "registered_context_and_moving_crops"}, crops, False),
             ("unknown_mode", {**full, "mode": "croponly"}, base, False),
             ("suppression_flag", {**full, "disable_full_frame": True}, base, False),
             ("bool_version", {**full, "schema_version": True}, base, False),
             ("fractional_version", {**full, "schema_version": 1.0}, base, False),
             ("future_version", {**full, "schema_version": 2}, base, False),
             ("null", None, base, False),
             ("stream_only", full, base, False),
             ("discard_sink", full, base, False),
             ("no_detector", both, crops, False)]
    with tempfile.TemporaryDirectory(prefix="orange_media_spec_") as tmp:
        directory = Path(tmp)
        for name, selection, source, accepted in cases:
            spec = copy.deepcopy(source)
            spec["fixed"]["output_root"] = str(directory / "must_not_exist")
            if name == "stream_only":
                spec["fixed"]["stream_only"] = True
            if name == "discard_sink":
                spec["fixed"]["recording_sink_mode"] = "immediate_recycle"
            if name == "no_detector":
                spec["fixed"].pop("yolo_worker", None)
                spec["fixed"].pop("pose_worker", None)
            if name != "absent":
                spec["fixed"]["media_products"] = selection
            path = directory / f"{name}.json"
            path.write_text(json.dumps(spec))
            result = subprocess.run([str(args.orange_client.resolve()), "--mode", "local",
                "--experiment-spec", str(path), "--validate-experiment-spec"],
                capture_output=True, text=True, timeout=20, check=False)
            if (result.returncode == 0) != accepted:
                raise AssertionError(f"{name}: exit {result.returncode}\n{result.stdout}\n{result.stderr}")
            if (directory / "must_not_exist").exists():
                raise AssertionError("admission entered recording startup")
        print(f"{len(cases)} headless media selection cases passed (no camera launch)")


if __name__ == "__main__":
    main()
