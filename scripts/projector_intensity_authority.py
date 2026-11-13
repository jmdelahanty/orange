#!/usr/bin/env python3
"""The commissioned projector gray: one authority for every commissioning launcher.

The projector-intensity commissioning report recommends a foreground gray. The
accepted commissioning-reference homography of every camera names that report
(path, sha256, gray) in its source photometry. This resolves the gray for a set
of cameras, refusing when the cameras disagree, the report changed, or the
report does not pass the gray for every selected camera.

CLI: projector_intensity_authority.py --citrus-config <canvas json> --cameras 2010093,...
prints {"foreground_gray_u8": n, "authority": {...}}; exit 2 when unresolved.
Moved from run_holder_fixture_validation.py on 2026-11-13 (unchanged rules).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


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



def camera_arenas_from_citrus(citrus_config: Path, cameras: list[str]) -> dict[str, str]:
    citrus = json.loads(citrus_config.read_text(encoding="utf-8"))
    camera_arenas: dict[str, str] = {}
    for camera in cameras:
        matching = [
            str(arena_id)
            for arena_id, arena in citrus.get("arenas", {}).items()
            if str(arena.get("active_camera_id", "")) == camera
        ]
        if len(matching) != 1:
            raise RuntimeError(
                f"camera {camera} maps to {len(matching)} Citrus arenas; expected one"
            )
        camera_arenas[camera] = matching[0]
    return camera_arenas


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--citrus-config", type=Path, required=True)
    parser.add_argument("--cameras", required=True)
    args = parser.parse_args(argv)
    cameras = [c for c in args.cameras.split(",") if c]
    try:
        gray, authority = resolve_projector_intensity_commissioning(
            args.citrus_config, camera_arenas_from_citrus(args.citrus_config, cameras)
        )
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(f"projector_intensity_authority: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"foreground_gray_u8": gray, "authority": authority}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
