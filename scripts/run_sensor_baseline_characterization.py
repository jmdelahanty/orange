#!/usr/bin/env python3
"""Plan and run a resumable production-baseline sensor characterization.

The first contract intentionally captures only the established production
camera mode.  It does not sweep or promote camera settings.  Dark and uniform
field conditions are separate, explicitly confirmed invocations so the runner
never advances across a physical fixture transition without the operator.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
SCHEMA_ID = "orange.sensor_baseline_characterization.run_manifest"
DEFAULT_CONFIG_FOLDER = (
    REPO_ROOT / "config" / "validated_split_gop_hevc_100fps_gop25_fourcam_a16"
)
DEFAULT_CAMERAS = ("2010093", "2010094", "2010095", "2010096")
DEFAULT_BENCHMARK = Path("/usr/local/bin/orange-local-benchmark")
DEFAULT_ORANGE_CLIENT = REPO_ROOT / "targets" / "release" / "orange_client"
DEFAULT_OUTPUT_PARENT = Path(
    "/home/jeremy/orange_data/calibrations/rig_characterization"
)
PRODUCTION_EXPECTED = {
    "width": 4512,
    "height": 4512,
    "frame_rate": 100,
    "exposure": 50,
    "gain": 256,
    "pixel_format": "Mono8",
}
MAX_BASELINE_FRAMES = 24


def utc_now() -> str:
    return (
        datetime.now(timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z")
    )


def utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise ValueError(f"expected object JSON in {path}")
    return payload


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def parse_csv(value: str) -> list[str]:
    items = [item.strip() for item in value.split(",") if item.strip()]
    if not items or any(not item.isdigit() for item in items):
        raise argparse.ArgumentTypeError(
            "camera serials must be a comma-separated list of digits"
        )
    if len(items) != len(set(items)):
        raise argparse.ArgumentTypeError("camera serials must be unique")
    return items


def validate_production_configs(
    config_folder: Path, camera_serials: list[str]
) -> dict[str, dict[str, Any]]:
    resolved = config_folder.resolve(strict=True)
    if not resolved.is_dir():
        raise ValueError(f"camera config folder is not a directory: {resolved}")

    evidence: dict[str, dict[str, Any]] = {}
    for serial in camera_serials:
        path = resolved / f"{serial}.json"
        if not path.is_file():
            raise ValueError(f"missing production camera config: {path}")
        config = read_json(path)
        if str(config.get("device_serial_number", "")) != serial:
            raise ValueError(f"{path}: device_serial_number does not match {serial}")
        mismatches = []
        for key, expected in PRODUCTION_EXPECTED.items():
            if config.get(key) != expected:
                mismatches.append(
                    f"{key}={config.get(key)!r} (expected {expected!r})"
                )
        if config.get("color") is not False:
            mismatches.append(f"color={config.get('color')!r} (expected false)")
        if config.get("gpu_direct") is not True:
            mismatches.append(
                f"gpu_direct={config.get('gpu_direct')!r} (expected true)"
            )
        recording = config.get("recording", {})
        encode = recording.get("encode", {}) if isinstance(recording, dict) else {}
        if recording.get("mode") != "split_gop":
            mismatches.append(
                f"recording.mode={recording.get('mode')!r} (expected 'split_gop')"
            )
        if encode.get("gop_length") != 25:
            mismatches.append(
                f"recording.encode.gop_length={encode.get('gop_length')!r} "
                "(expected 25)"
            )
        source_gpu = config.get("source_gpu_id")
        if not isinstance(source_gpu, int) or source_gpu < 0:
            mismatches.append(f"source_gpu_id={source_gpu!r} is invalid")
        if mismatches:
            raise ValueError(f"{path}: production profile mismatch: " + "; ".join(mismatches))

        evidence[serial] = {
            "path": str(path.resolve()),
            "sha256": sha256_file(path),
            "source_gpu_id": source_gpu,
            "recording_profile_name": recording.get("profile_name", ""),
        }
    return evidence


def build_experiment_spec(
    *,
    session_id: str,
    phase: str,
    session_dir: Path,
    config_folder: Path,
    camera_serials: list[str],
    gpu_ids: list[int],
    frame_count: int,
) -> dict[str, Any]:
    if phase not in ("dark", "flat"):
        raise ValueError(f"unsupported phase: {phase}")
    if frame_count < 4 or frame_count > MAX_BASELINE_FRAMES or frame_count % 2:
        raise ValueError(
            f"frame_count must be an even value in [4,{MAX_BASELINE_FRAMES}]"
        )
    if len(camera_serials) != len(gpu_ids):
        raise ValueError("one source GPU id is required per camera")

    experiment_id = f"{session_id}_{phase}"
    output_root = session_dir / "captures" / phase
    return {
        "experiment_id": experiment_id,
        "notes": (
            "Bounded production-mode sensor-baseline capture. The raw evidence is "
            "pre-compression NV12; for these Mono8 cameras the active Y bytes are "
            "the measurement plane. This is not a codec or throughput benchmark."
        ),
        "selection": {
            "camera_serials": camera_serials,
            "gpu_ids": gpu_ids,
        },
        "fixed": {
            "duration_s": 2,
            "warmup_s": 1,
            "display": False,
            "yolo": False,
            "stream_only": False,
            "sync_mode": "ptp_gate",
            "recording_sink_mode": "real",
            "config_folder": str(config_folder.resolve()),
            "output_root": str(output_root.resolve()),
            "ptp_register_read_decimate": 100,
            "nvenc_direct_input": False,
            "pre_encoder_reference_capture": {
                "enabled": True,
                "max_frames": frame_count,
                "max_seconds": 0,
                "output_dir": "sensor_reference",
            },
        },
        "matrix": {
            "codec": ["hevc"],
            "preset": ["p1"],
            "tuning": ["ll"],
            "rate_control_mode": ["vbr"],
            "quality_value": [20],
            "gop_length": [25],
            "aq": ["off"],
            "temporal_aq": ["off"],
            "lookahead": ["off"],
            "target_bitrate_bps": [150000000],
            "max_bitrate_bps": [150000000],
            "vbv_buffer_size": [150000000],
        },
        "policy": {
            "target_fps_tolerance_pct": 2.0,
            "require_zero_acq_starve": True,
            "require_zero_pre_drops": True,
            "require_zero_enc_fail": True,
            "require_zero_camera_drops": True,
            "require_valid_video_content": False,
        },
    }


def active_rig_processes() -> list[str]:
    matches: list[str] = []
    own_pid = os.getpid()
    markers = (
        "/targets/release/orange",
        "/targets/release/orange_client",
        "orange-gui-validation",
        "/citrus/targets/citrus",
    )
    proc = Path("/proc")
    if not proc.is_dir():
        return matches
    for child in proc.iterdir():
        if not child.name.isdigit() or int(child.name) == own_pid:
            continue
        try:
            command = (child / "cmdline").read_bytes().replace(b"\0", b" ").decode(
                "utf-8", errors="replace"
            )
        except (OSError, PermissionError):
            continue
        if command and any(marker in command for marker in markers):
            matches.append(f"pid={child.name} {command.strip()}")
    return sorted(matches)


def make_initial_manifest(
    *,
    session_id: str,
    session_dir: Path,
    config_folder: Path,
    camera_serials: list[str],
    config_evidence: dict[str, dict[str, Any]],
    frame_count: int,
) -> dict[str, Any]:
    return {
        "schema_id": SCHEMA_ID,
        "schema_version": 1,
        "session_id": session_id,
        "created_utc": utc_now(),
        "updated_utc": utc_now(),
        "status": "awaiting_phases",
        "scope": "production_baseline_only",
        "policy": {
            "settings_sweep_permitted": False,
            "automatic_setting_promotion_permitted": False,
            "operator_confirmation_required_for_each_physical_condition": True,
        },
        "production_profile": {
            "config_folder": str(config_folder.resolve()),
            "camera_serials": camera_serials,
            "expected_camera_state": {
                **PRODUCTION_EXPECTED,
                "AutoGain": False,
                "PGAGain": 0,
                "Offset": 0,
                "LUTEnable": False,
                "ADC": "Bit8",
                "DualADC": False,
            },
            "camera_configs": config_evidence,
        },
        "capture_contract": {
            "frames_per_camera_per_phase": frame_count,
            "frame_count_reason": (
                "bounded below the 25-frame GOP so primary-path evidence remains "
                "one consecutive 100 Hz sequence"
            ),
            "representation": "pre_encoder_nv12",
            "measurement_plane": "active_luma_y",
            "mono_contract": "Mono8 camera samples copied to Y; UV must remain 128",
            "temporal_averaging_during_capture": False,
            "sync_mode": "ptp_gate",
            "conditions": ["dark", "flat"],
        },
        "phases": {
            "dark": {"status": "not_captured"},
            "flat": {"status": "not_captured"},
        },
        "analysis": {"status": "not_run"},
        "session_dir": str(session_dir.resolve()),
    }


def phase_confirmation(args: argparse.Namespace) -> dict[str, Any]:
    common = {
        "operator_confirmed_production_optical_path": bool(
            args.confirm_production_optical_path
        ),
        "operator_notes": args.condition_notes,
    }
    if args.phase == "dark":
        common.update(
            {
                "condition_id": "lens_capped_dark",
                "all_lens_fronts_opaque_capped": bool(args.confirm_lens_capped),
                "projector_required": False,
                "interpretation": "dark/readout baseline at the production camera mode",
            }
        )
    else:
        common.update(
            {
                "condition_id": "uniform_stationary_field",
                "uniform_stationary_field_confirmed": bool(
                    args.confirm_uniform_stationary_field
                ),
                "production_nir_illumination_confirmed": bool(
                    args.confirm_production_illumination
                ),
                "flat_target_id": args.flat_target_id,
                "flat_reference_plane": args.flat_reference_plane,
                "interpretation": (
                    "one-level uniform-field baseline; not a photon-transfer curve"
                ),
            }
        )
    return common


def validate_confirmation(args: argparse.Namespace) -> None:
    if not args.confirm_production_optical_path:
        raise ValueError("--execute requires --confirm-production-optical-path")
    if args.phase == "dark":
        if not args.confirm_lens_capped:
            raise ValueError("dark capture requires --confirm-lens-capped")
    else:
        if not args.confirm_uniform_stationary_field:
            raise ValueError(
                "flat capture requires --confirm-uniform-stationary-field"
            )
        if not args.confirm_production_illumination:
            raise ValueError("flat capture requires --confirm-production-illumination")
        if not args.flat_target_id.strip() or not args.flat_reference_plane.strip():
            raise ValueError(
                "flat capture requires --flat-target-id and --flat-reference-plane"
            )


def discover_run_folder(experiment_root: Path) -> Path:
    candidates = sorted(
        path for path in experiment_root.glob("run_*") if path.is_dir()
    )
    if len(candidates) != 1:
        raise ValueError(
            f"expected exactly one run folder under {experiment_root}, found {len(candidates)}"
        )
    return candidates[0].resolve()


def validate_phase_artifacts(
    run_folder: Path, camera_serials: list[str]
) -> dict[str, Any]:
    reference_dir = run_folder / "sensor_reference"
    missing: list[str] = []
    cameras: dict[str, Any] = {}
    for serial in camera_serials:
        paths = {
            "raw_dump": reference_dir / f"Cam{serial}_preenc_ref.bin",
            "index": reference_dir / f"Cam{serial}_preenc_ref_index.csv",
            "metadata": reference_dir / f"Cam{serial}_preenc_ref.json",
        }
        for label, path in paths.items():
            if not path.is_file() or path.stat().st_size == 0:
                missing.append(f"Cam{serial} {label}: {path}")
        cameras[serial] = {key: str(path.resolve()) for key, path in paths.items()}
    snapshot = run_folder / "recording_snapshot.json"
    if not snapshot.is_file():
        missing.append(f"recording snapshot: {snapshot}")
    if missing:
        raise ValueError("missing capture artifacts: " + "; ".join(missing))
    return {
        "run_folder": str(run_folder.resolve()),
        "reference_dir": str(reference_dir.resolve()),
        "recording_snapshot": str(snapshot.resolve()),
        "cameras": cameras,
    }


def run_analysis(manifest_path: Path) -> int:
    analyzer = REPO_ROOT / "scripts" / "analyze_sensor_baseline_characterization.py"
    conda = shutil.which("conda") or "/home/jeremy/miniforge3/bin/conda"
    if Path(conda).is_file() or shutil.which(conda):
        command = [conda, "run", "-n", "juicebox", "python", str(analyzer), str(manifest_path)]
    else:
        command = [sys.executable, str(analyzer), str(manifest_path)]
    print("ANALYZE:", " ".join(command), flush=True)
    return subprocess.call(command, cwd=REPO_ROOT)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Capture a bounded dark/flat sensor baseline at the established "
            "four-camera production settings."
        )
    )
    parser.add_argument("--phase", choices=("dark", "flat"))
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--session-dir", type=Path)
    parser.add_argument("--analyze-only", type=Path, metavar="SESSION_OR_MANIFEST")
    parser.add_argument("--config-folder", type=Path, default=DEFAULT_CONFIG_FOLDER)
    parser.add_argument("--cameras", default=",".join(DEFAULT_CAMERAS))
    parser.add_argument("--frames", type=int, default=MAX_BASELINE_FRAMES)
    parser.add_argument("--benchmark", type=Path, default=DEFAULT_BENCHMARK)
    parser.add_argument("--orange-client", type=Path, default=DEFAULT_ORANGE_CLIENT)
    parser.add_argument("--no-sudo", action="store_true")
    parser.add_argument("--no-auto-analyze", action="store_true")
    parser.add_argument("--confirm-production-optical-path", action="store_true")
    parser.add_argument("--confirm-lens-capped", action="store_true")
    parser.add_argument("--confirm-uniform-stationary-field", action="store_true")
    parser.add_argument("--confirm-production-illumination", action="store_true")
    parser.add_argument("--flat-target-id", default="")
    parser.add_argument("--flat-reference-plane", default="")
    parser.add_argument("--condition-notes", default="")
    args = parser.parse_args()

    if args.analyze_only is not None:
        target = args.analyze_only.resolve()
        manifest_path = target if target.is_file() else target / "run_manifest.json"
        if not manifest_path.is_file():
            parser.error(f"manifest not found: {manifest_path}")
        return run_analysis(manifest_path)

    if args.phase is None:
        parser.error("--phase dark|flat is required unless --analyze-only is used")
    try:
        camera_serials = parse_csv(args.cameras)
        config_evidence = validate_production_configs(
            args.config_folder, camera_serials
        )
        if args.frames < 4 or args.frames > MAX_BASELINE_FRAMES or args.frames % 2:
            raise ValueError(
                f"--frames must be an even value in [4,{MAX_BASELINE_FRAMES}]"
            )
        if args.execute:
            validate_confirmation(args)
    except (ValueError, argparse.ArgumentTypeError, OSError) as error:
        parser.error(str(error))

    session_id = "sensorbaseline_" + utc_stamp()
    session_dir = args.session_dir or (DEFAULT_OUTPUT_PARENT / session_id)
    manifest_path = session_dir / "run_manifest.json"
    manifest: dict[str, Any]
    if manifest_path.is_file():
        manifest = read_json(manifest_path)
        if manifest.get("schema_id") != SCHEMA_ID:
            parser.error(f"not a sensor-baseline session: {manifest_path}")
        session_id = str(manifest.get("session_id", ""))
        contract = manifest.get("capture_contract", {})
        configured_frames = contract.get("frames_per_camera_per_phase")
        configured_cameras = manifest.get("production_profile", {}).get(
            "camera_serials", []
        )
        configured_folder = manifest.get("production_profile", {}).get(
            "config_folder", ""
        )
        if configured_frames != args.frames:
            parser.error(
                f"existing session uses {configured_frames} frames, not {args.frames}"
            )
        if configured_cameras != camera_serials:
            parser.error(
                f"existing session camera scope {configured_cameras} does not match {camera_serials}"
            )
        if Path(str(configured_folder)).resolve() != args.config_folder.resolve():
            parser.error("existing session uses a different production config folder")
        recorded_configs = manifest.get("production_profile", {}).get(
            "camera_configs", {}
        )
        changed_configs = [
            serial
            for serial in camera_serials
            if recorded_configs.get(serial, {}).get("sha256")
            != config_evidence[serial]["sha256"]
        ]
        if changed_configs:
            parser.error(
                "production camera config changed after session creation for: "
                + ",".join(changed_configs)
            )
    else:
        manifest = make_initial_manifest(
            session_id=session_id,
            session_dir=session_dir,
            config_folder=args.config_folder,
            camera_serials=camera_serials,
            config_evidence=config_evidence,
            frame_count=args.frames,
        )

    gpu_ids = [int(config_evidence[serial]["source_gpu_id"]) for serial in camera_serials]
    spec = build_experiment_spec(
        session_id=session_id,
        phase=args.phase,
        session_dir=session_dir,
        config_folder=args.config_folder,
        camera_serials=camera_serials,
        gpu_ids=gpu_ids,
        frame_count=args.frames,
    )
    experiment_root = Path(spec["fixed"]["output_root"]) / spec["experiment_id"]

    print("Sensor production-baseline characterization:")
    print(f"  phase={args.phase} cameras={','.join(camera_serials)}")
    print(
        "  mode=4512x4512 Mono8, 100 fps, 50 us, Gain 256, "
        "PGAGain 0, Offset 0, LUT/AutoGain/DualADC off"
    )
    print(f"  frames={args.frames} unaveraged pre-compression frames per camera")
    print("  sync=ptp_gate; analyzer uses active NV12 Y bytes and verifies UV=128")
    print(f"  session={session_dir}")
    print(f"  experiment_root={experiment_root}")
    if not args.execute:
        print("Dry-run only. Add --execute and the phase-specific confirmation flags.")
        return 0

    active = active_rig_processes()
    if active:
        print("Refusing to open cameras while rig processes are active:", file=sys.stderr)
        for process in active:
            print(f"  {process}", file=sys.stderr)
        return 1

    phase_record = manifest.setdefault("phases", {}).setdefault(args.phase, {})
    if phase_record.get("status", "not_captured") != "not_captured":
        print(
            f"Phase {args.phase} already has status {phase_record.get('status')!r}; "
            "create a new session rather than overwriting evidence.",
            file=sys.stderr,
        )
        return 1

    session_dir.mkdir(parents=True, exist_ok=True)
    specs_dir = session_dir / "specs"
    logs_dir = session_dir / "logs"
    specs_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)
    evidence_spec = specs_dir / f"{args.phase}_experiment_spec.json"
    write_json_atomic(evidence_spec, spec)

    temp_spec = Path("/tmp") / f"orange_{session_id}_{args.phase}_experiment_spec.json"
    write_json_atomic(temp_spec, spec)
    confirmation = phase_confirmation(args)
    phase_record.clear()
    phase_record.update(
        {
            "status": "capturing",
            "started_utc": utc_now(),
            "physical_condition": confirmation,
            "experiment_id": spec["experiment_id"],
            "experiment_root": str(experiment_root.resolve()),
            "experiment_spec": str(evidence_spec.resolve()),
            "experiment_spec_sha256": sha256_file(evidence_spec),
        }
    )
    manifest["status"] = "capturing"
    manifest["updated_utc"] = utc_now()
    write_json_atomic(manifest_path, manifest)

    command = [str(args.benchmark), "--orange-client", str(args.orange_client), str(temp_spec)]
    if not args.no_sudo:
        command = ["sudo", "-n", *command]
    phase_record["command"] = command
    stdout_path = logs_dir / f"{args.phase}_orange.stdout.log"
    stderr_path = logs_dir / f"{args.phase}_orange.stderr.log"
    print("CAPTURE:", " ".join(command), flush=True)
    completed: subprocess.CompletedProcess[str] | None = None
    invocation_error = ""
    try:
        with stdout_path.open("w", encoding="utf-8") as stdout, stderr_path.open(
            "w", encoding="utf-8"
        ) as stderr:
            try:
                completed = subprocess.run(
                    command,
                    cwd=REPO_ROOT,
                    stdout=stdout,
                    stderr=stderr,
                    text=True,
                    check=False,
                )
            except OSError as error:
                invocation_error = str(error)
    finally:
        try:
            temp_spec.unlink()
        except FileNotFoundError:
            pass

    phase_record["completed_utc"] = utc_now()
    phase_record["returncode"] = completed.returncode if completed is not None else None
    phase_record["stdout_log"] = str(stdout_path.resolve())
    phase_record["stderr_log"] = str(stderr_path.resolve())
    try:
        if invocation_error:
            raise ValueError(f"camera capture could not start: {invocation_error}")
        if completed is None:
            raise ValueError("camera capture did not return a process result")
        if completed.returncode != 0:
            raise ValueError(f"camera capture exited {completed.returncode}")
        run_folder = discover_run_folder(experiment_root)
        phase_record["artifacts"] = validate_phase_artifacts(
            run_folder, camera_serials
        )
        phase_record["status"] = "captured"
    except (ValueError, OSError) as error:
        phase_record["status"] = "capture_failed"
        phase_record["error"] = str(error)
        manifest["status"] = "capture_failed"
        manifest["updated_utc"] = utc_now()
        write_json_atomic(manifest_path, manifest)
        print(f"Capture failed: {error}", file=sys.stderr)
        print(f"See {stdout_path} and {stderr_path}", file=sys.stderr)
        return (completed.returncode if completed is not None else 1) or 1

    captured = all(
        manifest["phases"].get(name, {}).get("status") == "captured"
        for name in ("dark", "flat")
    )
    manifest["status"] = "ready_for_analysis" if captured else "awaiting_phases"
    manifest["updated_utc"] = utc_now()
    write_json_atomic(manifest_path, manifest)
    print(f"Captured {args.phase}; manifest={manifest_path}")
    if captured and not args.no_auto_analyze:
        return run_analysis(manifest_path)
    if not captured:
        other = "flat" if args.phase == "dark" else "dark"
        print(f"Next physical checkpoint: capture phase '{other}' using this session directory.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
