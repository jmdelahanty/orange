#!/usr/bin/env python3
"""Focused contract tests for the sensor-baseline runner and analyzer."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not import {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def ensure_numpy_interpreter() -> None:
    try:
        import numpy  # noqa: F401
        return
    except ImportError:
        pass
    if "--numpy-child" in sys.argv:
        raise RuntimeError("NumPy unavailable in designated analysis interpreter")
    candidate = Path("/home/jeremy/miniforge3/envs/juicebox/bin/python")
    if not candidate.is_file():
        print("sensor_baseline_characterization_tests: SKIP (NumPy unavailable)")
        raise SystemExit(0)
    completed = subprocess.run(
        [str(candidate), str(Path(__file__).resolve()), "--numpy-child"],
        check=False,
    )
    raise SystemExit(completed.returncode)


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def make_pipeline(serial: str, width: int, height: int) -> dict:
    values = {
        "Width": width,
        "Height": height,
        "FrameRate": 100,
        "Exposure": 50,
        "Gain": 256,
        "AutoGain": False,
        "PGAGain": 0,
        "Offset": 0,
        "LUTEnable": False,
        "ADC": "Bit8",
        "DualADC": False,
        "PixelFormat": "Mono8",
    }
    return {
        "schema_id": "orange.camera.sensor_pipeline_state",
        "schema_version": 1,
        "capture_stage": "post_configuration_pre_stream",
        "camera": {"serial": serial},
        "features": {
            name: {"status": "readable", "supported": True, "value": value}
            for name, value in values.items()
        },
    }


def write_capture(
    *,
    root: Path,
    phase: str,
    serial: str,
    timestamp_offset: int,
    width: int = 16,
    height: int = 16,
    frame_count: int = 4,
) -> tuple[dict, Path]:
    import numpy as np

    phase_dir = root / phase
    reference_dir = phase_dir / "sensor_reference"
    reference_dir.mkdir(parents=True, exist_ok=True)
    pitch = width
    frame_size = pitch * height * 3 // 2
    raw_path = reference_dir / f"Cam{serial}_preenc_ref.bin"
    index_path = reference_dir / f"Cam{serial}_preenc_ref_index.csv"
    metadata_path = reference_dir / f"Cam{serial}_preenc_ref.json"
    snapshot_path = phase_dir / "recording_snapshot.json"

    checker = (np.indices((height, width)).sum(axis=0) % 2).astype(np.int16)
    base = 5 if phase == "dark" else 100
    amplitude = 1 if phase == "dark" else 2
    payloads = []
    for index in range(frame_count):
        if index % 2 == 0:
            y = np.full((height, pitch), base, dtype=np.uint8)
        else:
            signed = np.where(checker == 0, -amplitude, amplitude)
            y = np.clip(base + signed, 0, 255).astype(np.uint8)
        uv = np.full((height // 2, pitch), 128, dtype=np.uint8)
        payloads.append(y.tobytes() + uv.tobytes())
    raw_path.write_bytes(b"".join(payloads))

    with index_path.open("w", encoding="utf-8") as handle:
        handle.write(
            "reference_frame_index,recording_frame_id,timestamp,timestamp_sys,byte_offset,byte_size\n"
        )
        for index in range(frame_count):
            timestamp = timestamp_offset + index * 10_000_000
            handle.write(
                f"{index},{index + 1},{timestamp},{timestamp + 1000},"
                f"{index * frame_size},{frame_size}\n"
            )
    write_json(
        metadata_path,
        {
            "camera_serial": serial,
            "width": width,
            "height": height,
            "pitch": pitch,
            "frame_size": frame_size,
            "pixel_format": "nv12",
            "path_type": "copy",
            "source_path_flavor": "mono",
            "resize_enabled": False,
            "capture": {"frames_captured": frame_count, "status": "budget_reached"},
        },
    )
    if snapshot_path.exists():
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
    else:
        snapshot = {"camera_runtime": {}}
    snapshot["camera_runtime"][serial] = {
        "sensor_pipeline": make_pipeline(serial, width, height)
    }
    write_json(snapshot_path, snapshot)
    return (
        {
            "raw_dump": str(raw_path),
            "index": str(index_path),
            "metadata": str(metadata_path),
        },
        snapshot_path,
    )


def test_runner_contract(runner) -> None:
    evidence = runner.validate_production_configs(
        runner.DEFAULT_CONFIG_FOLDER, list(runner.DEFAULT_CAMERAS)
    )
    assert list(evidence) == list(runner.DEFAULT_CAMERAS)
    assert all(evidence[serial]["source_gpu_id"] >= 0 for serial in evidence)
    with tempfile.TemporaryDirectory() as temporary:
        spec = runner.build_experiment_spec(
            session_id="sensorbaseline_test",
            phase="dark",
            session_dir=Path(temporary),
            config_folder=runner.DEFAULT_CONFIG_FOLDER,
            camera_serials=list(runner.DEFAULT_CAMERAS),
            gpu_ids=[evidence[serial]["source_gpu_id"] for serial in runner.DEFAULT_CAMERAS],
            frame_count=24,
        )
    fixed = spec["fixed"]
    assert fixed["sync_mode"] == "ptp_gate"
    assert fixed["recording_sink_mode"] == "real"
    assert fixed["pre_encoder_reference_capture"]["max_frames"] == 24
    assert fixed["pre_encoder_reference_capture"]["output_dir"] == "sensor_reference"
    assert spec["policy"]["require_valid_video_content"] is False
    try:
        runner.build_experiment_spec(
            session_id="bad",
            phase="dark",
            session_dir=Path("/tmp/bad"),
            config_folder=runner.DEFAULT_CONFIG_FOLDER,
            camera_serials=["2010093"],
            gpu_ids=[3],
            frame_count=25,
        )
    except ValueError:
        pass
    else:
        raise AssertionError("runner accepted a frame count spanning the first GOP")


def test_analysis_contract(analyzer) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        session_dir = root / "session"
        camera_serials = ["2010093", "2010094"]
        phases: dict[str, dict] = {}
        for phase in ("dark", "flat"):
            camera_artifacts = {}
            snapshot_path = None
            for camera_index, serial in enumerate(camera_serials):
                artifacts, snapshot_path = write_capture(
                    root=session_dir / "captures",
                    phase=phase,
                    serial=serial,
                    timestamp_offset=camera_index * 25,
                )
                camera_artifacts[serial] = artifacts
            phases[phase] = {
                "status": "captured",
                "physical_condition": {"condition_id": phase},
                "artifacts": {
                    "recording_snapshot": str(snapshot_path),
                    "cameras": camera_artifacts,
                },
            }
        manifest_path = session_dir / "run_manifest.json"
        write_json(
            manifest_path,
            {
                "schema_id": analyzer.MANIFEST_SCHEMA_ID,
                "schema_version": 1,
                "session_id": "sensorbaseline_test",
                "session_dir": str(session_dir),
                "status": "ready_for_analysis",
                "production_profile": {
                    "camera_serials": camera_serials,
                    "expected_camera_state": {
                        "width": 16,
                        "height": 16,
                        "frame_rate": 100,
                        "exposure": 50,
                        "gain": 256,
                        "pixel_format": "Mono8",
                        "AutoGain": False,
                        "PGAGain": 0,
                        "Offset": 0,
                        "LUTEnable": False,
                        "ADC": "Bit8",
                        "DualADC": False,
                    },
                },
                "capture_contract": {"frames_per_camera_per_phase": 4},
                "phases": phases,
                "analysis": {"status": "not_run"},
            },
        )
        report = analyzer.analyze(manifest_path)
        assert report["status"] == "valid_baseline"
        assert report["phases"]["dark"]["ptp_grouping"]["status"] == "pass"
        assert report["phases"]["flat"]["ptp_grouping"]["max_timestamp_span_ns"] == 25
        for serial in camera_serials:
            dark_sigma = report["phases"]["dark"]["cameras"][serial][
                "temporal_noise"
            ]["paired_temporal_sigma_dn"]
            flat_sigma = report["phases"]["flat"]["cameras"][serial][
                "temporal_noise"
            ]["paired_temporal_sigma_dn"]
            assert 0.70 < dark_sigma < 0.71
            assert 1.41 < flat_sigma < 1.42
            assert report["comparisons"][serial]["flat_minus_dark_mean_dn"] == 95.0
        updated = json.loads(manifest_path.read_text(encoding="utf-8"))
        assert updated["status"] == "complete"
        assert Path(updated["analysis"]["report_json"]).is_file()
        assert Path(updated["analysis"]["report_markdown"]).is_file()


def main() -> int:
    ensure_numpy_interpreter()
    runner = load_module(
        "run_sensor_baseline_characterization",
        REPO_ROOT / "scripts" / "run_sensor_baseline_characterization.py",
    )
    analyzer = load_module(
        "analyze_sensor_baseline_characterization",
        REPO_ROOT / "scripts" / "analyze_sensor_baseline_characterization.py",
    )
    test_runner_contract(runner)
    test_analysis_contract(analyzer)
    print("sensor_baseline_characterization_tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
