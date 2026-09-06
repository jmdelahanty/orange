#!/usr/bin/env python3
"""Camera-free admission checks for opt-in native registered context capture."""
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
    base = json.loads((root / "experiment_specs/threecam_detect_latency_levers_gate_registered_crop_synthetic_interleave.json").read_text())
    base["fixed"].pop("pose_worker", None)
    base["fixed"]["master_frame_journal"] = {
        "schema_version": 1, "enabled": True, "writer_cpu_ids": [0], "queue_capacity": 4096,
    }
    declaration = {
        "schema_id": "orange.recording.registered_scene_context.capture_declaration",
        "schema_version": 1, "registration_authority_status": "accepted_for_experiment",
        "subject_presence": "present", "dish_setup_complete": True,
        "nir_illumination_fixed": True, "camera_configuration_fixed": True, "rig_fixed": True,
    }
    valid = {"schema_version": 1, "enabled": True, "timeout_ms": 10000,
             "worker_cpu_ids": [0], "declaration": declaration}
    cases = [("present", {}, True), ("rolling", {}, True), ("absent", {}, True),
             ("unknown_subject", {}, True), ("disabled", {"enabled": False}, True),
             ("no_master", {}, False), ("no_yolo", {}, False), ("decimated", {}, False),
             ("synthetic", {}, False), ("no_declaration", {}, False), ("diagnostic", {}, False),
             ("moving_rig", {}, False), ("declaration_extension", {}, False)]
    cases.extend((name, update, False) for name, update in [
        ("no_cpu", {"worker_cpu_ids": []}), ("duplicate_cpu", {"worker_cpu_ids": [0, 0]}),
        ("bool_cpu", {"worker_cpu_ids": [True]}), ("bool_version", {"schema_version": True}),
        ("overflow_version", {"schema_version": 4294967297}), ("wrong_version", {"schema_version": 2}),
        ("unknown_field", {"typo": True}), ("timeout_low", {"timeout_ms": 99}),
        ("timeout_high", {"timeout_ms": 60001}), ("timeout_fractional", {"timeout_ms": 100.5}),
    ])
    with tempfile.TemporaryDirectory(prefix="orange_context_spec_") as tmp:
        directory = Path(tmp)
        for name, update, accepted in cases:
            spec = copy.deepcopy(base)
            fixed = spec["fixed"]
            context = copy.deepcopy(valid)
            context.update(update)
            fixed["registered_scene_context"] = context
            fixed["output_root"] = str(directory / "must_not_be_created")
            if name == "rolling":
                fixed["recording_control"]["clip_seconds"] = 10
            if name == "absent":
                context["declaration"]["subject_presence"] = "absent"
            if name == "unknown_subject":
                context["declaration"]["subject_presence"] = "unknown"
            if name == "no_master":
                fixed.pop("master_frame_journal")
            if name == "no_yolo":
                fixed.pop("yolo_worker")
            if name == "decimated":
                fixed["yolo_worker"]["decimate"] = 2
            if name == "synthetic":
                fixed["pose_worker"] = {"mode": "noop", "roi_source": "synthetic_center_box"}
            if name == "no_declaration":
                context.pop("declaration")
            if name == "diagnostic":
                context["declaration"]["registration_authority_status"] = "diagnostic_not_physical_acceptance"
            if name == "moving_rig":
                context["declaration"]["rig_fixed"] = False
            if name == "declaration_extension":
                context["declaration"]["typo"] = True
            path = directory / f"{name}.json"
            path.write_text(json.dumps(spec))
            result = subprocess.run([str(args.orange_client.resolve()), "--mode", "local", "--experiment-spec",
                                     str(path), "--validate-experiment-spec"],
                                    capture_output=True, text=True, timeout=20, check=False)
            if (result.returncode == 0) != accepted:
                raise AssertionError(f"{name}: unexpected exit {result.returncode}\n{result.stdout}\n{result.stderr}")
            if not accepted and "registered_scene_context" not in result.stderr:
                raise AssertionError(f"{name}: refused for an unrelated reason: {result.stderr}")
            if (directory / "must_not_be_created").exists():
                raise AssertionError("validation entered recording/output workflow")
    print(f"{len(cases)} registered context admission cases passed (no camera launch)")


if __name__ == "__main__":
    main()
