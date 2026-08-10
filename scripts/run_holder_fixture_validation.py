#!/usr/bin/env python3
"""Capture holder-installed, dish-absent validation evidence in one session."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA_ID = "orange.holder_fixture_validation.run_manifest"


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z"
    )


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def normalize_sha256(value: str) -> str:
    return value.removeprefix("sha256:").lower()


def resolve_projector_intensity_commissioning(
    citrus_config: Path,
    camera_arenas: dict[str, str],
) -> tuple[int, dict[str, Any]]:
    artifact_root = citrus_config.parent / "calibration_artifacts"
    report_path: Path | None = None
    report_sha256 = ""
    reference_gray: int | None = None
    reference_evidence: list[dict[str, Any]] = []
    for camera, arena_id in camera_arenas.items():
        pointer_path = artifact_root / (
            f"homography_reference_{arena_id}_{camera}_projected_surface.json"
        )
        if not pointer_path.is_file():
            raise RuntimeError(
                f"commissioning-reference homography is missing: {pointer_path}"
            )
        pointer = json.loads(pointer_path.read_text(encoding="utf-8"))
        photometry = pointer.get("source_photometry", {})
        provenance = photometry.get("commissioning_provenance", {})
        candidate_report_path = Path(str(provenance.get("report_path", "")))
        candidate_report_sha256 = normalize_sha256(
            str(provenance.get("report_sha256", ""))
        )
        candidate_gray = provenance.get("recommended_foreground_gray_u8")
        expected_rig = citrus_config.parent.parent.name
        expected_canvas = citrus_config.parent.name
        valid = (
            pointer.get("schema_id") == "citrus.calibration.active_homography"
            and pointer.get("schema_version") == 1
            and pointer.get("status") == "accepted"
            and pointer.get("rig_id") == expected_rig
            and pointer.get("canvas_name") == expected_canvas
            and pointer.get("arena_id") == arena_id
            and str(pointer.get("camera_id", "")) == camera
            and pointer.get("target_plane") == "projected_surface"
            and pointer.get("homography_role") == "commissioning_reference"
            and photometry.get("status") == "passed"
            and candidate_report_path.is_file()
            and len(candidate_report_sha256) == 64
            and isinstance(candidate_gray, int)
            and 0 <= candidate_gray <= 255
        )
        if not valid:
            raise RuntimeError(
                "invalid projector-intensity provenance in commissioning-reference "
                f"homography: {pointer_path}"
            )
        if report_path is None:
            report_path = candidate_report_path
            report_sha256 = candidate_report_sha256
            reference_gray = candidate_gray
        elif (
            candidate_report_path != report_path
            or candidate_report_sha256 != report_sha256
            or candidate_gray != reference_gray
        ):
            raise RuntimeError(
                "selected cameras do not share one projector-intensity authority"
            )
        reference_evidence.append({
            "camera_serial": camera,
            "arena_id": arena_id,
            "pointer_path": str(pointer_path.resolve()),
            "pointer_sha256": sha256_file(pointer_path),
        })

    if report_path is None:
        raise RuntimeError("no projector-intensity authority was resolved")
    actual_report_sha256 = normalize_sha256(sha256_file(report_path))
    if actual_report_sha256 != report_sha256:
        raise RuntimeError("projector-intensity report checksum mismatch")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    recommended = report.get("recommended_foreground_gray_u8")
    all_camera_pass = report.get("level_passes_all_cameras", {}).get(
        str(recommended), False
    )
    passing_cameras = {
        str(summary.get("camera_serial", ""))
        for summary in report.get("camera_level_summaries", [])
        if summary.get("foreground_gray_u8") == recommended
        and summary.get("passes_quality_gate") is True
    }
    valid_report = (
        report.get("schema_id")
        == "orange.projector_intensity_commissioning.report"
        and report.get("schema_version") == 1
        and report.get("status") == "pass"
        and isinstance(recommended, int)
        and 0 <= recommended <= 255
        and recommended == reference_gray
        and all_camera_pass is True
        and set(camera_arenas).issubset(passing_cameras)
    )
    if not valid_report:
        raise RuntimeError(
            "projector-intensity commissioning report does not qualify its "
            "recommended level for every selected camera"
        )
    return recommended, {
        "schema_id": "orange.projector_intensity_commissioning.reference",
        "schema_version": 1,
        "status": "validated",
        "report_path": str(report_path.resolve()),
        "report_sha256": report_sha256,
        "recommended_foreground_gray_u8": recommended,
        "validated_camera_serials": list(camera_arenas),
        "source_evidence": reference_evidence,
    }


def sudo_invoking_owner() -> tuple[int, int] | None:
    if os.geteuid() != 0:
        return None
    uid_text = os.environ.get("SUDO_UID", "")
    gid_text = os.environ.get("SUDO_GID", "")
    if not uid_text.isdigit() or not gid_text.isdigit():
        return None
    return int(uid_text), int(gid_text)


def safe_chown_created_tree(path: Path, owner: tuple[int, int] | None) -> None:
    """Hand root-created calibration outputs back without following symlinks."""
    if owner is None or not path.exists():
        return
    allowed_root = Path("/home/jeremy/orange_data/calibrations").resolve()
    resolved = path.resolve()
    try:
        resolved.relative_to(allowed_root)
    except ValueError as error:
        raise RuntimeError(
            f"refusing ownership handoff outside {allowed_root}: {resolved}"
        ) from error
    if path.is_symlink():
        raise RuntimeError(f"refusing ownership handoff through symlink: {path}")
    uid, gid = owner
    if path.is_file():
        os.chown(path, uid, gid, follow_symlinks=False)
        return
    for current, directories, files in os.walk(path, followlinks=False):
        current_path = Path(current)
        for name in directories + files:
            candidate = current_path / name
            if candidate.is_symlink():
                raise RuntimeError(
                    f"refusing ownership handoff with symlink in output: {candidate}"
                )
            os.chown(candidate, uid, gid, follow_symlinks=False)
        os.chown(current_path, uid, gid, follow_symlinks=False)


def handoff_holder_outputs(manifest_path: Path) -> None:
    owner = sudo_invoking_owner()
    if owner is None:
        return
    paths = [manifest_path.parent]
    report_path = manifest_path.parent / "validation_report.json"
    if report_path.is_file():
        report = json.loads(report_path.read_text(encoding="utf-8"))
        package_text = str(report.get("persisted_evidence", {}).get(
            "session_package_manifest_path", ""
        ))
        if package_text:
            paths.append(Path(package_text).parent)
        for camera in report.get("camera_results", []):
            observation_text = str(
                camera.get("persisted_observation", {}).get("path", "")
            )
            if observation_text:
                paths.append(Path(observation_text).parent)
    for path in paths:
        safe_chown_created_tree(path, owner)


def parse_cameras(value: str) -> list[str]:
    cameras = [item.strip() for item in value.split(",") if item.strip()]
    if not cameras or any(not camera.isdigit() for camera in cameras):
        raise argparse.ArgumentTypeError(
            "cameras must be comma-separated numeric serials"
        )
    return list(dict.fromkeys(cameras))


def analysis_command(repo_root: Path, manifest_path: Path) -> list[str]:
    analyzer = repo_root / "scripts/analyze_holder_fixture_validation.py"
    if importlib.util.find_spec("cv2") is not None:
        return [sys.executable, str(analyzer), str(manifest_path)]
    conda = shutil.which("conda")
    if conda is None:
        local_conda = Path("/home/jeremy/miniforge3/bin/conda")
        conda = str(local_conda) if local_conda.is_file() else None
    if conda:
        return [conda, "run", "-n", "juicebox", "python", str(analyzer), str(manifest_path)]
    return [sys.executable, str(analyzer), str(manifest_path)]


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description=(
            "Capture the installed operational projection plane, persist a Citrus "
            "homography candidate for review, and independently validate it without "
            "automatic promotion."
        )
    )
    parser.add_argument("--execute", action="store_true")
    parser.add_argument(
        "--confirm-holder-installed-dish-absent",
        action="store_true",
        help="confirm the holder is installed and all dishes/water are absent",
    )
    parser.add_argument(
        "--aperture-shape",
        choices=("circle", "rectangle", "rounded_rectangle", "polygon", "unknown"),
        default="circle",
    )
    parser.add_argument("--cameras", default="2010093,2010094,2010095,2010096")
    parser.add_argument(
        "--foreground-gray-u8",
        type=int,
        help=(
            "deliberate override; by default resolve the qualified value from "
            "the immutable projector-intensity commissioning report"
        ),
    )
    parser.add_argument("--frame-rate-hz", type=int, default=5)
    parser.add_argument("--exposure-us", type=int, default=100000)
    parser.add_argument(
        "--camera-iris-overrides",
        default="",
        help="optional SERIAL=VALUE[,SERIAL=VALUE...] startup iris overrides",
    )
    parser.add_argument(
        "--frame-count-per-camera-per-recipe",
        type=int,
        default=60,
        help=(
            "frames temporally averaged for every camera/recipe; the permanent "
            "holder workflow defaults to the validated 60-frame policy"
        ),
    )
    parser.add_argument("--timeout-seconds", type=int, default=300)
    parser.add_argument(
        "--citrus-config",
        type=Path,
        default=Path("/home/jeremy/citrus/targets/rigs/omnifin0/shadow/shadow.json"),
    )
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--analyze-only", type=Path, metavar="MANIFEST")
    args = parser.parse_args()

    if args.analyze_only is not None:
        returncode = subprocess.call(analysis_command(repo_root, args.analyze_only))
        run_manifest = json.loads(args.analyze_only.read_text(encoding="utf-8"))
        report_path = args.analyze_only.parent / "validation_report.json"
        run_manifest["analysis_returncode"] = returncode
        run_manifest["validation_report_path"] = str(report_path.resolve())
        if report_path.is_file():
            report = json.loads(report_path.read_text(encoding="utf-8"))
            run_manifest["status"] = (
                "complete" if report.get("status") == "pass" else "quality_failed"
            )
        else:
            run_manifest["status"] = "analysis_failed"
        run_manifest["updated_utc"] = utc_now()
        write_json_atomic(args.analyze_only, run_manifest)
        handoff_holder_outputs(args.analyze_only)
        return returncode
    try:
        cameras = parse_cameras(args.cameras)
    except argparse.ArgumentTypeError as error:
        parser.error(str(error))
    if args.foreground_gray_u8 is not None and not 1 <= args.foreground_gray_u8 <= 255:
        parser.error("--foreground-gray-u8 must be from 1 through 255")
    if (
        args.frame_rate_hz <= 0
        or args.exposure_us <= 0
        or args.frame_count_per_camera_per_recipe <= 0
        or args.timeout_seconds <= 0
    ):
        parser.error("frame count, frame rate, exposure, and timeout must be positive")
    if args.execute and not args.confirm_holder_installed_dish_absent:
        parser.error("--execute requires --confirm-holder-installed-dish-absent")
    if not args.citrus_config.is_file():
        parser.error(f"Citrus config does not exist: {args.citrus_config}")

    citrus = json.loads(args.citrus_config.read_text(encoding="utf-8"))
    active_inputs: list[dict[str, Any]] = []
    camera_arenas: dict[str, str] = {}
    for camera in cameras:
        matching_arenas = [
            str(arena_id)
            for arena_id, arena in citrus.get("arenas", {}).items()
            if str(arena.get("active_camera_id", "")) == camera
        ]
        if len(matching_arenas) != 1:
            parser.error(
                f"camera {camera} maps to {len(matching_arenas)} Citrus arenas; expected one"
            )
        arena_id = matching_arenas[0]
        camera_arenas[camera] = arena_id
        pointer = (
            args.citrus_config.parent / "calibration_artifacts" /
            f"homography_active_{arena_id}_{camera}.json"
        )
        if not pointer.is_file():
            parser.error(f"active homography pointer does not exist: {pointer}")
        active = json.loads(pointer.read_text(encoding="utf-8"))
        active_inputs.append({
            "camera_serial": camera,
            "arena_id": arena_id,
            "pointer_path": str(pointer.resolve()),
            "pointer_sha256_before_capture": sha256_file(pointer),
            "candidate_id": active.get("candidate_id"),
            "candidate_set_id": active.get("candidate_set_id"),
            "accepted_at_utc": active.get("accepted_at_utc"),
        })

    try:
        commissioned_gray, intensity_authority = (
            resolve_projector_intensity_commissioning(
                args.citrus_config, camera_arenas
            )
        )
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        parser.error(str(error))
    if args.foreground_gray_u8 is None:
        args.foreground_gray_u8 = commissioned_gray
        intensity_authority["selection_policy"] = "commissioned_default"
    else:
        intensity_authority["selection_policy"] = "operator_override"
        intensity_authority["operator_override_foreground_gray_u8"] = (
            args.foreground_gray_u8
        )

    primary_recipe = (
        "homography_grid" if args.aperture_shape == "rectangle" else "homography_rings"
    )
    recipes = [
        "black_reference",
        "uniform_gray",
        "arena_outline",
        primary_recipe,
        "verification_dots",
    ]
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output_dir = args.output_dir or Path(
        f"/home/jeremy/orange_data/calibrations/commissioning/"
        f"holder_fixture_{stamp}"
    )
    manifest_path = output_dir / "run_manifest.json"
    result_path = output_dir / "guided_capture_result.json"
    if manifest_path.exists():
        parser.error(f"output already exists: {manifest_path}")

    manifest: dict[str, Any] = {
        "schema_id": SCHEMA_ID,
        "schema_version": 1,
        "created_utc": utc_now(),
        "updated_utc": utc_now(),
        "status": "planned",
        "authority_contract": {
            "role": "operational_candidate",
            "active_homography_is_input": True,
            "commissioning_reference_is_preserved": True,
            "homography_fit_allowed": True,
            "homography_promotion_allowed": False,
            "fixture_aperture_distinct_from_experimental_area": True,
            "fixture_aperture_distinct_from_dish_inner_rim": True,
        },
        "fixture_state": {
            "state_id": "holder_installed_dish_absent",
            "holder_installed": True,
            "dish_installed": False,
            "water_present": False,
            "camera_filters_removed": True,
            "aperture_shape": args.aperture_shape,
            "operator_confirmed": bool(args.confirm_holder_installed_dish_absent),
        },
        "projection": {
            "recipe_sequence": recipes,
            "primary_support_recipe": primary_recipe,
            "foreground_gray_u8": args.foreground_gray_u8,
            "projector_intensity_commissioning": intensity_authority,
            "arena_outline_has_center_fiducial": True,
        },
        "capture": {
            "camera_serials": cameras,
            "frame_count_per_camera_per_recipe": (
                args.frame_count_per_camera_per_recipe
            ),
            "frame_rate_hz": args.frame_rate_hz,
            "exposure_us": args.exposure_us,
            "camera_iris_overrides": args.camera_iris_overrides,
            "invocation_mode": "one_orange_process_one_citrus_process",
            "session_policy": "one_calibration_session",
            "synchronization": {
                "sync_mode": "ptp_gate",
                "grouped_capture_per_recipe": True,
                "max_group_camera_timestamp_span_ns": 100000,
            },
        },
        "inputs": {
            "citrus_canvas_path": str(args.citrus_config.resolve()),
            "citrus_canvas_sha256_before_capture": sha256_file(args.citrus_config),
            "guided_capture_result_path": str(result_path.resolve()),
            "active_homographies_before_capture": active_inputs,
        },
    }

    print("Holder fixture validation plan:")
    print(f"  fixture=holder installed, dish/water absent; shape={args.aperture_shape}")
    print(f"  cameras={','.join(cameras)} PTP-grouped at {args.frame_rate_hz} fps")
    print(f"  sequence={','.join(recipes)}")
    print(
        f"  foreground_gray_u8={args.foreground_gray_u8} "
        f"({intensity_authority['selection_policy']})"
    )
    print(
        "  authority=operational candidate plus independent validation; "
        "no automatic promotion"
    )
    print(f"  output={output_dir}")
    if not args.execute:
        print(
            "Dry-run only. Add --execute --confirm-holder-installed-dish-absent "
            "when the fixture is ready."
        )
        return 0

    output_dir.mkdir(parents=True, exist_ok=False)
    manifest["status"] = "capturing"
    manifest["updated_utc"] = utc_now()
    write_json_atomic(manifest_path, manifest)

    command = [
        str(repo_root / "scripts/run_gui_guided_capture_smoke.sh"),
        "--execute",
        "--profile", "holder_installed_projected_surface",
        "--recipe", primary_recipe,
        "--purpose", "homography_grid",
        "--recipe-sequence", ",".join(recipes),
        "--fixture-aperture-shape", args.aperture_shape,
        "--cameras", ",".join(cameras),
        "--frame-count", str(args.frame_count_per_camera_per_recipe),
        "--foreground-gray-u8", str(args.foreground_gray_u8),
        "--calibration-frame-rate-hz", str(args.frame_rate_hz),
        "--calibration-exposure-us", str(args.exposure_us),
        "--save",
        "--fit-homographies",
        "--timeout-seconds", str(args.timeout_seconds),
        "--citrus-config", str(args.citrus_config),
        "--result-json", str(result_path),
    ]
    if args.camera_iris_overrides:
        command.extend([
            "--camera-iris-overrides", args.camera_iris_overrides,
        ])
    completed = subprocess.run(command, cwd=repo_root, check=False)
    if completed.returncode != 0 or not result_path.is_file():
        manifest["status"] = "capture_failed"
        manifest["capture_returncode"] = completed.returncode
        manifest["updated_utc"] = utc_now()
        write_json_atomic(manifest_path, manifest)
        handoff_holder_outputs(manifest_path)
        return completed.returncode or 1

    result = json.loads(result_path.read_text(encoding="utf-8"))
    manifest["calibration_session"] = {
        "session_id": result.get("persistence", {}).get("session_id"),
        "session_dir": result.get("persistence", {}).get("session_dir"),
    }
    manifest["capture_returncode"] = completed.returncode
    manifest["status"] = "analyzing"
    manifest["updated_utc"] = utc_now()
    write_json_atomic(manifest_path, manifest)

    analysis = subprocess.run(analysis_command(repo_root, manifest_path), check=False)
    report_path = output_dir / "validation_report.json"
    manifest["analysis_returncode"] = analysis.returncode
    manifest["validation_report_path"] = str(report_path.resolve())
    if report_path.is_file():
        report = json.loads(report_path.read_text(encoding="utf-8"))
        manifest["status"] = (
            "complete" if report.get("status") == "pass" else "quality_failed"
        )
    else:
        manifest["status"] = "analysis_failed"
    manifest["updated_utc"] = utc_now()
    write_json_atomic(manifest_path, manifest)
    handoff_holder_outputs(manifest_path)
    return analysis.returncode


if __name__ == "__main__":
    raise SystemExit(main())
