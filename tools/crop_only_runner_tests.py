#!/usr/bin/env python3
"""Camera-free runner, adapter and wrapper regression tests."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import crop_only_validation as crop
import run_crop_only_headless_validation as headless
import validate_gui_ptp_recording as gui
sys.path.insert(0, str(ROOT / "tools"))
import run_gui_fourcam_external_ipc_validation_tests as gui_profile

CLIENT = Path("/tmp/orange-timing-build-20260906/orange_client")


def run(*args, **kwargs):
    return subprocess.run(args, cwd=ROOT, text=True, capture_output=True, timeout=30, **kwargs)


class CropRunnerTests(unittest.TestCase):
    def context(self):
        # Parsing fixture only. Not published as real registration evidence.
        return {"schema_version": 1, "enabled": True, "worker_cpu_ids": [0], "declaration": {
            "schema_id": "orange.recording.registered_scene_context.capture_declaration", "schema_version": 1,
            "registration_authority_status": "accepted_for_experiment", "subject_presence": "present",
            "dish_setup_complete": True, "nir_illumination_fixed": True, "camera_configuration_fixed": True, "rig_fixed": True}}

    def test_headless_profiles_dry_and_parse(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            context = root / "context.json"; context.write_text(json.dumps(self.context()))
            for profile in ("single", "rolling"):
                result = run("python3", str(ROOT / "scripts/run_crop_only_headless_validation.py"),
                    "--profile", profile, "--context-config", str(context), "--writer-cpu", "0", "--output-root", str(root / "no-output"))
                self.assertEqual(result.returncode, 0, result.stderr)
                plan = json.loads(result.stdout)
                self.assertFalse(plan["execute"]); self.assertIsNone(plan["spec_path"])
                spec = plan["spec"]
                self.assertNotIn("external_recorder_contract", spec["fixed"])
                self.assertNotIn("pose_worker", spec["fixed"])
                self.assertEqual(spec["fixed"]["registered_scene_context"], self.context())
                self.assertEqual(spec["fixed"]["recording_control"]["clip_seconds"], 2 if profile == "rolling" else 0)
                path = root / f"{profile}.json"; path.write_text(json.dumps(spec))
                if CLIENT.exists():
                    parsed = run(str(CLIENT), "--mode", "local", "--experiment-spec", str(path), "--validate-experiment-spec")
                    self.assertEqual(parsed.returncode, 0, parsed.stdout + parsed.stderr)
                self.assertFalse((root / "no-output").exists())
            refused = run("python3", str(ROOT / "scripts/run_crop_only_headless_validation.py"),
                "--profile", "single", "--context-config", str(context), "--writer-cpu", "0", "--execute")
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn("--confirm-scene", refused.stderr)

    def test_materialization_never_invents_context(self):
        with self.assertRaises(ValueError): headless.make_spec("single", {}, [0], "test")
        with self.assertRaises(ValueError): headless.make_spec("single", self.context(), [-1], "test")
        with self.assertRaises(ValueError): headless.make_spec("single", self.context(), [True], "test")

    def test_readonly_verifier_adapter(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = {"schema_id": "orange.recording.crop_only_validation", "schema_version": 1,
                "status": "pass", "recording_folder": tmp, "recording_session": {
                    "media_product_mode": crop.PRODUCT, "status": "completed", "cameras": ["2010093"]}}
            def fake(*args, **kwargs):
                self.assertEqual(args[0], ["/test/verifier", tmp, "--worker-cpu", "0"])
                return subprocess.CompletedProcess(args[0], 0, json.dumps(report), "")
            self.assertEqual(crop.verify_collection(tmp, "/test/verifier", 0, ["2010093"], run=fake), report)
            for field, value in (("schema_version", True), ("status", "fail"), ("recording_folder", "/different")):
                saved = report[field]; report[field] = value
                with self.assertRaises(ValueError): crop.verify_collection(tmp, "/test/verifier", 0, ["2010093"], run=fake)
                report[field] = saved
            with self.assertRaises(ValueError): crop.verify_collection(tmp, "/test/verifier", 0, ["2010094"], run=fake)
            with self.assertRaises(ValueError): crop.verify_collection(tmp, "/test/verifier", None, ["2010093"], run=fake)

    def test_gui_lifecycle_keeps_stop_checks_without_full_artifacts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            parent = {"schema_id": "orange.recording_session", "schema_version": 1, "producer": "orange_gui",
                "media_product_mode": crop.PRODUCT, "mode": "rolling_clips", "recording_control": {"record_for_seconds": 7, "clip_seconds": 2},
                "status": "completed", "cameras": ["2010093"], "camera_artifacts": {}}
            (root / "recording_session.json").write_text(json.dumps(parent))
            snapshot = {"session": {"recording_session_manifest_path": str(root / "recording_session.json"), "recording_mode": "rolling_clips"}}
            reporter = gui.Reporter(verbose=False)
            gui.check_recording_session_manifest(reporter, root, snapshot, ["2010093"], crop_only=True)
            self.assertFalse(reporter.failures, reporter.failures)
            reporter = gui.Reporter(verbose=False)
            gui.check_recording_session_manifest(reporter, root, snapshot, ["2010093"],
                expected_local_control_stop_method="stop_recording", crop_only=True)
            self.assertTrue(reporter.failures)
            reporter = gui.Reporter(verbose=False)
            gui.check_recording_session_manifest(reporter, root, snapshot, ["2010093"])
            self.assertTrue(reporter.failures)  # No implicit bypass for full-frame mode.

    def test_crop_pipeline_counters_required(self):
        final = dict.fromkeys(("camera_dropped_frames", "camera_frame_id_gaps", "get_frame_errors", "acq_starve", "pre_drops"), 0)
        for key in (None, *final):
            changed = final.copy()
            if key: changed[key] = 1
            reporter = gui.Reporter(verbose=False)
            gui.check_pipeline(reporter, {"pipeline": {"2010093": {"final": changed}}}, ["2010093"], crop_only=True)
            self.assertEqual(bool(reporter.failures), key is not None)
        reporter = gui.Reporter(verbose=False)
        gui.check_pipeline(reporter, {"pipeline": {"2010093": {"final": {}}}}, ["2010093"], crop_only=True)
        self.assertTrue(reporter.failures)

    def test_gui_main_routes_media_without_bypassing_telemetry(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            parent = {"schema_id": "orange.recording_session", "schema_version": 1, "producer": "orange_gui",
                "media_product_mode": crop.PRODUCT, "status": "completed", "mode": "single_clip", "cameras": ["2010093"]}
            (folder / "recording_session.json").write_text(json.dumps(parent))
            snapshot = {"session": {"recording_session_manifest_path": str(folder / "recording_session.json"), "recording_mode": "single_clip"}}
            (folder / "recording_snapshot.json").write_text(json.dumps(snapshot))
            output = folder / "report.json"
            with patch.object(sys, "argv", ["validator", str(folder), "--json", "--json-out", str(output),
                    "--media-products", crop.PRODUCT, "--crop-validator-cpu", "0"]), \
                    patch.object(gui.gui_summary, "summarize", return_value={}), \
                    patch.object(crop, "verify_collection", return_value={"status": "pass", "recording_session": parent}), \
                    patch.object(gui, "check_videos", side_effect=AssertionError("full-frame validation entered")), \
                    patch.object(gui, "check_crop_recording_artifacts", side_effect=AssertionError("aggregate crop validation entered")), \
                    patch("builtins.print"):
                self.assertEqual(gui.main(), 1)  # Evidence alone cannot hide missing PTP/performance telemetry.
            report = json.loads(output.read_text())
            self.assertEqual(report["crop_recording"]["status"], "pass")
            self.assertTrue(any("pipeline perf" in message for message in report["failures"]))

    def test_orchestrator_profile_preserves_lifecycle(self):
        script = str(ROOT / "scripts/run_orange_citrus_fourcam_orchestrator.sh")
        result = run("bash", script, "--crop-only", "--attach-orange", "--crop-validator-cpu", "0")
        self.assertEqual(result.returncode, 0, result.stderr)
        plan = json.loads(result.stdout)
        validations = {item["label"]: item["command"] for item in plan["validations"]}
        commands = [validations["orange_validation_1"], next(value for value in validations.values() if "validate_recording_observation_bindings.py" in value)]
        self.assertIn("--media-products registered_context_and_moving_crops", commands[0])
        self.assertIn("--expect-local-control-stop-method stop_recording", commands[0])
        self.assertNotIn("--require-external-recorder-status", commands[0])
        self.assertIn("validate_recording_observation_bindings.py", commands[1])
        self.assertNotEqual(run("bash", script, "--crop-only", "--crop-validator-cpu", "0").returncode, 0)
        self.assertNotEqual(run("bash", script, "--crop-only", "--attach-orange").returncode, 0)
        self.assertNotEqual(run("bash", script, "--crop-only", "--attach-orange", "--crop-validator-cpu", "0", "--skip-orange-validation").returncode, 0)

    def test_gui_profile_waits_for_operator(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "app.json"
            recording = {"media_products": {"schema_version": 1, "mode": crop.PRODUCT}, "registered_context_recording": {
                "enabled": True, "master_frame_journal": {"enabled": True}, "registered_scene_context": self.context()}}
            path.write_text(json.dumps({"recording": recording}))
            result = gui_profile.run_profile(["--crop-only", "--app-config", str(path), "--print-exec-env-only"])
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("ORANGE_GUI_AUTORUN_START_RECORDING=0", result.stdout)
            self.assertIn("ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START=1", result.stdout)
            self.assertEqual(json.loads(path.read_text())["recording"], recording)

    def test_wrappers_explicit_paths_dry_only(self):
        if not CLIENT.exists(): self.skipTest("isolation build absent")
        gui_bin = CLIENT.parent / "orange"
        result = run("bash", str(ROOT / "scripts/orange_gui_validation_wrapper.sh"), "--dry-run", "--orange-bin", str(gui_bin))
        self.assertEqual(result.returncode, 0, result.stderr)
        for script, flag in (("orange_gui_validation_wrapper.sh", "--orange-bin"), ("orange_local_benchmark_wrapper.sh", "--orange-client")):
            refused = run("bash", str(ROOT / "scripts" / script), "--dry-run", flag, "/bin/true")
            self.assertNotEqual(refused.returncode, 0)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "spec.json"
            path.write_text(json.dumps({"experiment_id": "test", "fixed": {"output_root": str(Path(tmp) / "must-not-exist")}}))
            result = run("bash", str(ROOT / "scripts/orange_local_benchmark_wrapper.sh"), "--dry-run", "--orange-client", str(CLIENT), str(path))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse((Path(tmp) / "must-not-exist").exists())


if __name__ == "__main__":
    unittest.main()
