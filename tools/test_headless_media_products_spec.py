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
             ("crop_only_missing_requirements", {"schema_version": 1, "mode": "registered_context_and_moving_crops"}, crops, False),
             ("unknown_mode", {**full, "mode": "croponly"}, base, False),
             ("suppression_flag", {**full, "disable_full_frame": True}, base, False),
             ("bool_version", {**full, "schema_version": True}, base, False),
             ("fractional_version", {**full, "schema_version": 1.0}, base, False),
             ("future_version", {**full, "schema_version": 2}, base, False),
             ("null", None, base, False),
             ("stream_only", full, base, False),
             ("discard_sink", full, base, False),
             ("no_detector", both, crops, False),
             ("native_full_independent_crop", both, crops, True),
             ("crop_tool", both, crops, True),
             ("crop_tool_relative", both, crops, False),
             ("crop_tool_empty", both, crops, False),
             ("crop_tool_type", both, crops, False)]
    crop_only = {"schema_version": 1, "mode": "registered_context_and_moving_crops"}
    crop_source = copy.deepcopy(crops)
    crop_source["fixed"].pop("pose_worker", None)
    crop_source["fixed"].pop("external_recorder_contract", None)
    crop_source["fixed"]["recording_sink_mode"] = "real"
    crop_source["fixed"]["master_frame_journal"] = {"schema_version": 1, "enabled": True, "writer_cpu_ids": [0]}
    crop_source["fixed"]["registered_scene_context"] = {"schema_version": 1, "enabled": True, "worker_cpu_ids": [0],
        "declaration": {"schema_id": "orange.recording.registered_scene_context.capture_declaration", "schema_version": 1,
            "registration_authority_status": "accepted_for_experiment", "subject_presence": "present", "dish_setup_complete": True,
            "nir_illumination_fixed": True, "camera_configuration_fixed": True, "rig_fixed": True}}
    for name, accepted in [("crop_valid", True), ("crop_rolling", True), ("crop_external_sink_no_full_contract", True),
            ("crop_no_master", False), ("crop_no_context", False), ("crop_no_detector", False),
            ("crop_decimated", False), ("crop_synthetic", False), ("crop_native_encoder", False),
            ("crop_untimed", False), ("crop_full_contract", False), ("crop_daily_reuse", True)]:
        source = copy.deepcopy(crop_source)
        fixed = source["fixed"]
        if name in ("crop_rolling", "crop_external_sink_no_full_contract"): fixed["recording_control"]["clip_seconds"] = 10
        if name == "crop_external_sink_no_full_contract": fixed["recording_sink_mode"] = "external_ipc"
        if name == "crop_no_master": fixed.pop("master_frame_journal")
        if name == "crop_no_context": fixed.pop("registered_scene_context")
        if name == "crop_no_detector": fixed.pop("yolo_worker")
        if name == "crop_decimated": fixed["yolo_worker"]["decimate"] = 2
        if name == "crop_synthetic": fixed["pose_worker"] = {"mode": "noop", "roi_source": "synthetic_center_box"}
        if name == "crop_native_encoder": fixed["crop_recording"]["mode"] = "in_process"
        if name == "crop_untimed": fixed["recording_control"] = {"record_for_seconds": 0}
        if name == "crop_full_contract": fixed["recording_sink_mode"] = "external_ipc"; fixed["external_recorder_contract"] = copy.deepcopy(crops["fixed"]["external_recorder_contract"])
        if name == "crop_daily_reuse":
            fixed["registered_scene_context"]["schema_version"] = 2
            fixed["registered_scene_context"]["source"] = {"kind": "daily_registration", "descriptor_path": "/tmp/not-opened/context.json",
                "size_bytes": 1024, "sha256": "sha256:" + "a" * 64, "scene_unchanged_since_capture": True}
        cases.append((name, crop_only, source, accepted))
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
            if name == "native_full_independent_crop":
                spec["fixed"]["recording_sink_mode"] = "real"
                spec["fixed"].pop("external_recorder_contract", None)
                spec["fixed"].pop("pose_worker", None)
                spec["fixed"]["master_frame_journal"] = {
                    "schema_version": 1, "enabled": True, "writer_cpu_ids": [0]}
            if name.startswith("crop_tool"):
                spec["fixed"]["crop_recording"]["recorder_tool_path"] = {
                    "crop_tool": "/tmp/not-executed/external_recorder_ipc_probe",
                    "crop_tool_relative": "relative", "crop_tool_empty": "", "crop_tool_type": 3}[name]
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
