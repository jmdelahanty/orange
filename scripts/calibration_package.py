#!/usr/bin/env python3
"""Build and validate non-active calibration package candidates.

The migration command is intentionally one-way and non-activating. It reads a
version-1 Citrus commissioning release, copies checksum-bound source bytes into
an immutable candidate under a caller-selected output directory, and writes a
separate inventory/diff report. It never reads or writes an active pointer.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Iterable

from calibration_package_lib import (
    CalibrationPackageError,
    AssetSpec,
    asset,
    canonical_sha256,
    deduplicate_assets,
    package_id,
    pretty_json_bytes,
    compare_projection_geometry,
    projection_geometry_fingerprint,
    read_json,
    seal_candidate_package,
    sha256_file,
    validate_candidate_package,
)


DEFAULT_SHADOW_RELEASE = Path(
    "/home/jeremy/citrus/targets/rigs/omnifin0/shadow/calibration_artifacts/"
    "commissioning/commissioning_shadow-rig1-20260720T1638Z/commissioning.json"
)


def normalized_sha256(value: Any) -> str | None:
    if not isinstance(value, str) or not value:
        return None
    return value if value.startswith("sha256:") else f"sha256:{value}"


def json_value(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise CalibrationPackageError(f"{label} must be a JSON object")
    return value


def string_value(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise CalibrationPackageError(f"{label} must be a non-empty string")
    return value


def add_ref(
    assets: list[AssetSpec],
    source: Any,
    relative: str,
    role: str,
    *,
    expected: Any = None,
    target_plane: str = "not_applicable",
    camera_id: str | None = None,
    arena_id: str | None = None,
    required: bool = True,
    classification: str = "runtime_critical",
    provenance: dict[str, Any] | None = None,
) -> None:
    source_text = str(source) if source is not None else ""
    assets.append(
        asset(
            source_text,
            relative,
            role,
            target_plane=target_plane,
            camera_id=camera_id,
            arena_id=arena_id,
            required=required,
            expected_sha256=normalized_sha256(expected),
            classification=classification,
            provenance=provenance,
        )
    )


def resolve_release_selection(release_path: Path) -> tuple[str, Path | None]:
    """Verify the v1 release is exactly the currently selected release when possible."""

    pointer_path = release_path.parents[2] / "commissioning_active.json"
    if not pointer_path.is_file():
        return sha256_file(release_path), None
    pointer = read_json(pointer_path)
    observed = sha256_file(release_path)
    if Path(pointer.get("manifest_path", "")).resolve() != release_path.resolve():
        raise CalibrationPackageError(
            "source release is not the exact manifest named by commissioning_active.json"
        )
    if pointer.get("manifest_sha256") != observed:
        raise CalibrationPackageError(
            "commissioning_active.json checksum does not match source release bytes"
        )
    return observed, pointer_path


def camera_identity_from_canvas(
    canvas: dict[str, Any], members: Iterable[dict[str, Any]]
) -> dict[str, dict[str, Any]]:
    arenas = json_value(canvas.get("arenas"), "canvas.arenas")
    result: dict[str, dict[str, Any]] = {}
    for member in members:
        camera_id = string_value(member.get("camera_id"), "member.camera_id")
        arena_id = string_value(member.get("arena_id"), "member.arena_id")
        arena = json_value(arenas.get(arena_id), f"canvas.arenas.{arena_id}")
        matching = [
            row
            for row in arena.get("camera_calibrations", [])
            if isinstance(row, dict) and str(row.get("camera_id", "")) == camera_id
        ]
        if len(matching) != 1:
            raise CalibrationPackageError(
                f"expected one canvas calibration for {camera_id}/{arena_id}"
            )
        camera = matching[0]
        result[camera_id] = {
            "arena_id": arena_id,
            "target_plane": string_value(
                member.get("target_plane"), f"{camera_id}.target_plane"
            ),
            "native_width_px": int(camera.get("native_width_px")),
            "native_height_px": int(camera.get("native_height_px")),
            "tank_design_id": string_value(
                arena.get("tank_design_id"), f"{arena_id}.tank_design_id"
            ),
        }
    return dict(sorted(result.items()))


def commissioning_identity(
    release: dict[str, Any],
    release_sha256: str,
    canvas: dict[str, Any],
) -> dict[str, Any]:
    members = release.get("members")
    if not isinstance(members, list) or not members:
        raise CalibrationPackageError("commissioning release has no members")
    return {
        "authority": "citrus",
        "rig_id": string_value(release.get("rig_id"), "release.rig_id"),
        "rig_geometry_revision": int(release.get("rig_geometry_revision")),
        "canvas_id": string_value(release.get("canvas_name"), "release.canvas_name"),
        "source_release_id": string_value(
            release.get("release_id"), "release.release_id"
        ),
        "source_release_sha256": release_sha256,
        "requirements_profile_id": string_value(
            release.get("requirements_profile_id"),
            "release.requirements_profile_id",
        ),
        "target_plane": "projected_surface",
        "projection_geometry_identity": {
            "schema_id": "citrus.calibration.canvas_projection_geometry_identity",
            "schema_version": 1,
            "fingerprint": projection_geometry_fingerprint(canvas),
        },
        "canvas_raster": {
            "width_px": int(canvas.get("canvas_width_px")),
            "height_px": int(canvas.get("canvas_height_px")),
        },
        "coordinate_conventions": {
            "camera_native_origin": "top_left",
            "camera_native_positive_x": "right",
            "camera_native_positive_y": "down",
            "canvas_origin": "top_left",
            "canvas_positive_x": "right",
            "canvas_positive_y": "down",
            "pixel_center_convention": "integer_coordinates_are_pixel_centers",
            "boundary_inclusion": "inclusive",
        },
        "camera_arena_map": camera_identity_from_canvas(canvas, members),
    }


def add_homography_assets(
    assets: list[AssetSpec], member: dict[str, Any], include_archive_images: bool
) -> None:
    camera_id = string_value(member.get("camera_id"), "member.camera_id")
    arena_id = string_value(member.get("arena_id"), "member.arena_id")
    product = json_value(member.get("homography"), f"{camera_id}.homography")
    prefix = f"assets/cameras/Cam{camera_id}/homography"
    add_ref(
        assets,
        product.get("active_pointer_path"),
        f"{prefix}/active_selection_receipt.json",
        "active_homography_selection_receipt",
        expected=product.get("active_pointer_sha256"),
        target_plane="projected_surface",
        camera_id=camera_id,
        arena_id=arena_id,
    )
    add_ref(
        assets,
        product.get("candidate_path"),
        f"{prefix}/accepted_candidate.json",
        "accepted_homography_candidate",
        expected=product.get("candidate_sha256"),
        target_plane="projected_surface",
        camera_id=camera_id,
        arena_id=arena_id,
    )
    add_ref(
        assets,
        product.get("acceptance_receipt_path"),
        f"{prefix}/acceptance_receipt.json",
        "homography_acceptance_receipt",
        expected=product.get("acceptance_receipt_sha256"),
        target_plane="projected_surface",
        camera_id=camera_id,
        arena_id=arena_id,
    )

    active_path = Path(str(product.get("active_pointer_path", "")))
    if active_path.is_file():
        active = read_json(active_path)
        add_ref(
            assets,
            active.get("homography_yaml_path"),
            f"{prefix}/homography.yml",
            "accepted_homography_matrix",
            expected=active.get("homography_yaml_checksum"),
            target_plane="projected_surface",
            camera_id=camera_id,
            arena_id=arena_id,
        )
    candidate_path = Path(str(product.get("candidate_path", "")))
    candidate_dir = candidate_path.parent
    candidate_set_dir = candidate_path.parents[1]
    add_ref(
        assets,
        candidate_set_dir / "candidate_set.json",
        f"{prefix}/candidate_set.json",
        "homography_candidate_set_manifest",
        target_plane="projected_surface",
        camera_id=camera_id,
        arena_id=arena_id,
    )
    add_ref(
        assets,
        candidate_set_dir / "logical_canvas_layout_evidence.json",
        f"{prefix}/logical_canvas_layout_evidence.json",
        "homography_logical_canvas_layout_evidence",
        target_plane="projected_surface",
        camera_id=camera_id,
        arena_id=arena_id,
        classification="review_evidence",
    )
    add_ref(
        assets,
        candidate_dir / "reprojection_overlay.png",
        f"{prefix}/review/reprojection_overlay.png",
        "homography_review_overlay",
        target_plane="projected_surface",
        camera_id=camera_id,
        arena_id=arena_id,
        classification="review_evidence",
    )
    add_ref(
        assets,
        candidate_dir / "coordinate_frame_evidence.png",
        f"{prefix}/review/coordinate_frame_evidence.png",
        "homography_coordinate_frame_evidence",
        target_plane="projected_surface",
        camera_id=camera_id,
        arena_id=arena_id,
        classification="review_evidence",
    )
    if candidate_path.is_file():
        candidate = read_json(candidate_path)
        source = candidate.get("source", {})
        if isinstance(source, dict):
            add_ref(
                assets,
                source.get("image_set_path"),
                f"{prefix}/source/image_set.json",
                "homography_source_image_set",
                target_plane="projected_surface",
                camera_id=camera_id,
                arena_id=arena_id,
                classification="provenance",
            )
            if include_archive_images:
                add_ref(
                    assets,
                    source.get("image_path"),
                    f"{prefix}/source/source_frame.png",
                    "homography_source_capture",
                    target_plane="projected_surface",
                    camera_id=camera_id,
                    arena_id=arena_id,
                    classification="archive_evidence",
                )


def add_target_bundle_assets(
    assets: list[AssetSpec],
    observation_path: Path,
    observation: dict[str, Any],
    prefix: str,
    camera_id: str,
    arena_id: str,
) -> None:
    provenance = observation.get("target_provenance")
    if not isinstance(provenance, dict):
        return
    camera_root = observation_path.parents[2]
    references = [
        ("source_json_path", "source_json_sha256", "physical_target_definition.json"),
        ("session_json_path", None, "session_target_definition.json"),
        ("coordinate_csv_path", "coordinate_csv_sha256", "target_points.csv"),
        ("reference_png_path", "reference_png_sha256", "target_reference.png"),
        ("svg_path", "svg_sha256", "target_design.svg"),
    ]
    for path_key, checksum_key, destination in references:
        path_value = provenance.get(path_key)
        if not isinstance(path_value, str) or not path_value:
            continue
        source = Path(path_value)
        if not source.is_absolute():
            source = camera_root / source
        add_ref(
            assets,
            source,
            f"{prefix}/target/{destination}",
            f"scale_{path_key}",
            expected=provenance.get(checksum_key) if checksum_key else None,
            target_plane="projected_surface",
            camera_id=camera_id,
            arena_id=arena_id,
            classification="provenance",
        )


def add_scale_assets(
    assets: list[AssetSpec], member: dict[str, Any], include_archive_images: bool
) -> None:
    camera_id = string_value(member.get("camera_id"), "member.camera_id")
    arena_id = string_value(member.get("arena_id"), "member.arena_id")
    product = json_value(
        member.get("projected_surface_scale"), f"{camera_id}.projected_surface_scale"
    )
    prefix = f"assets/cameras/Cam{camera_id}/projected_surface_scale"
    add_ref(
        assets,
        product.get("active_pointer_path"),
        f"{prefix}/active_selection_receipt.json",
        "active_projected_surface_scale_selection_receipt",
        expected=product.get("active_pointer_sha256"),
        target_plane="projected_surface",
        camera_id=camera_id,
        arena_id=arena_id,
    )
    add_ref(
        assets,
        product.get("candidate_path"),
        f"{prefix}/accepted_candidate.json",
        "accepted_projected_surface_scale_candidate",
        expected=product.get("candidate_sha256"),
        target_plane="projected_surface",
        camera_id=camera_id,
        arena_id=arena_id,
    )
    add_ref(
        assets,
        product.get("acceptance_receipt_path"),
        f"{prefix}/acceptance_receipt.json",
        "projected_surface_scale_acceptance_receipt",
        expected=product.get("acceptance_receipt_sha256"),
        target_plane="projected_surface",
        camera_id=camera_id,
        arena_id=arena_id,
    )
    candidate_path = Path(str(product.get("candidate_path", "")))
    add_ref(
        assets,
        candidate_path.parents[1] / "manifest.json",
        f"{prefix}/candidate_set_manifest.json",
        "projected_surface_scale_candidate_set_manifest",
        target_plane="projected_surface",
        camera_id=camera_id,
        arena_id=arena_id,
    )
    source_observation = json_value(
        product.get("source_observation"), f"{camera_id}.source_observation"
    )
    observation_path = Path(str(source_observation.get("path", "")))
    add_ref(
        assets,
        observation_path,
        f"{prefix}/source/observation.json",
        "orange_projected_surface_scale_observation",
        expected=source_observation.get("sha256"),
        target_plane="projected_surface",
        camera_id=camera_id,
        arena_id=arena_id,
        classification="provenance",
    )
    add_ref(
        assets,
        observation_path.parent / "overlay.png",
        f"{prefix}/review/scale_overlay.png",
        "projected_surface_scale_review_overlay",
        target_plane="projected_surface",
        camera_id=camera_id,
        arena_id=arena_id,
        classification="review_evidence",
    )
    if observation_path.is_file():
        observation = read_json(observation_path)
        add_target_bundle_assets(
            assets, observation_path, observation, prefix, camera_id, arena_id
        )
        source_capture = observation.get("source_capture")
        if isinstance(source_capture, dict):
            add_ref(
                assets,
                source_capture.get("image_set_path"),
                f"{prefix}/source/image_set.json",
                "scale_source_image_set",
                target_plane="projected_surface",
                camera_id=camera_id,
                arena_id=arena_id,
                classification="provenance",
            )
            if include_archive_images:
                add_ref(
                    assets,
                    source_capture.get("image_path"),
                    f"{prefix}/source/source_frame.png",
                    "scale_source_capture",
                    target_plane="projected_surface",
                    camera_id=camera_id,
                    arena_id=arena_id,
                    classification="archive_evidence",
                )


def build_commissioned_setup_candidate(
    release_path: Path,
    output_parent: Path,
    *,
    fixture_manifest: Path | None = None,
    build_provenance: Path | None = None,
    include_archive_images: bool = False,
) -> tuple[Path, dict[str, Any]]:
    release_path = release_path.expanduser().resolve()
    release = read_json(release_path)
    if (
        release.get("schema_id")
        != "citrus.calibration.rig_canvas_commissioning_release"
        or release.get("schema_version") != 1
        or release.get("status") != "accepted"
    ):
        raise CalibrationPackageError("source is not an accepted v1 commissioning release")
    release_sha256, selection_pointer = resolve_release_selection(release_path)
    canvas_reference = json_value(
        release.get("canvas_configuration"), "release.canvas_configuration"
    )
    canvas_path = Path(str(canvas_reference.get("snapshot_path", "")))
    canvas = read_json(canvas_path)
    current_canvas_path = Path(str(canvas_reference.get("source_path", "")))
    if not current_canvas_path.is_file():
        raise CalibrationPackageError(
            "current canvas is unavailable; cannot establish migration compatibility"
        )
    current_canvas = read_json(current_canvas_path)
    canvas_compatibility = compare_projection_geometry(current_canvas, canvas)
    if not canvas_compatibility["compatible"]:
        raise CalibrationPackageError(
            "current canvas is not compatible with the accepted commissioning snapshot: "
            + str(canvas_compatibility["error"])
        )
    identity = commissioning_identity(release, release_sha256, canvas)

    assets: list[AssetSpec] = []
    add_ref(
        assets,
        release_path,
        "assets/source_release/commissioning.json",
        "source_v1_commissioning_release",
        expected=release_sha256,
        classification="provenance",
    )
    if selection_pointer:
        add_ref(
            assets,
            selection_pointer,
            "assets/source_release/commissioning_active_snapshot.json",
            "source_v1_active_selection_snapshot",
            classification="provenance",
        )
    rig_reference = json_value(
        release.get("rig_configuration"), "release.rig_configuration"
    )
    add_ref(
        assets,
        rig_reference.get("snapshot_path"),
        "assets/config/rig_config_snapshot.json",
        "rig_configuration_snapshot",
        expected=rig_reference.get("snapshot_sha256"),
    )
    add_ref(
        assets,
        canvas_path,
        "assets/config/canvas_config_snapshot.json",
        "authority_canvas_configuration_snapshot",
        expected=canvas_reference.get("snapshot_sha256"),
        target_plane="projected_surface",
    )

    for session in release.get("source_sessions", []):
        session = json_value(session, "release.source_sessions[]")
        session_id = string_value(session.get("session_id"), "source_session.session_id")
        prefix = f"assets/source_sessions/{session_id}"
        add_ref(
            assets,
            session.get("session_path"),
            f"{prefix}/session.json",
            "orange_calibration_session",
            expected=session.get("session_sha256"),
            classification="provenance",
        )
        add_ref(
            assets,
            session.get("session_index_path"),
            f"{prefix}/session_index.json",
            "orange_calibration_session_index",
            expected=session.get("session_index_sha256"),
            classification="provenance",
        )

    members = release.get("members", [])
    for member in members:
        member = json_value(member, "release.members[]")
        add_homography_assets(assets, member, include_archive_images)
        add_scale_assets(assets, member, include_archive_images)

    tank_ids = sorted(
        {camera["tank_design_id"] for camera in identity["camera_arena_map"].values()}
    )
    citrus_root = release_path
    while citrus_root.name != "citrus" and citrus_root != citrus_root.parent:
        citrus_root = citrus_root.parent
    for tank_id in tank_ids:
        add_ref(
            assets,
            citrus_root / "targets" / "tank_designs" / f"{tank_id}.json",
            f"assets/tank_designs/{tank_id}.json",
            "tank_design",
            classification="runtime_critical",
        )

    gaps: list[dict[str, Any]] = []
    if fixture_manifest is None:
        gaps.extend(
            [
                {
                    "role": "operational_fixture_contract",
                    "required": True,
                    "reason": "v1_release_has_no_checksum_bound_fixture_manifest",
                    "resolution": "rerun with --fixture-manifest after operator review",
                },
                {
                    "role": "holder_validation_report_and_overlays",
                    "required": True,
                    "reason": "v1_release_has_no_authoritative_holder_validation_reference",
                    "resolution": "bind reviewed holder evidence through fixture manifest",
                },
            ]
        )
    else:
        fixture_manifest = fixture_manifest.expanduser().resolve()
        add_ref(
            assets,
            fixture_manifest,
            "assets/fixture/fixture_manifest.json",
            "operational_fixture_contract",
            target_plane="projected_surface",
            classification="runtime_critical",
        )
        fixture = read_json(fixture_manifest)
        for index, member in enumerate(fixture.get("members", [])):
            member = json_value(member, f"fixture.members[{index}]")
            member_path = Path(str(member.get("path", "")))
            if not member_path.is_absolute():
                member_path = fixture_manifest.parent / member_path
            add_ref(
                assets,
                member_path,
                f"assets/fixture/members/{index:03d}_{member_path.name}",
                string_value(member.get("role"), f"fixture.members[{index}].role"),
                expected=member.get("sha256"),
                target_plane=str(member.get("target_plane", "projected_surface")),
                camera_id=member.get("camera_id"),
                arena_id=member.get("arena_id"),
                required=bool(member.get("required", True)),
                classification=str(member.get("classification", "review_evidence")),
            )

    if build_provenance is None:
        gaps.append(
            {
                "role": "software_build_provenance",
                "required": True,
                "reason": "v1_release_has_no_checksum_bound_build_provenance",
                "resolution": "rerun with --build-provenance after binding Citrus and Orange build identities",
            }
        )
    else:
        build_provenance = build_provenance.expanduser().resolve()
        json_value(read_json(build_provenance), "build provenance")
        add_ref(
            assets,
            build_provenance,
            "assets/provenance/software_builds.json",
            "software_build_provenance",
            classification="provenance",
        )

    package_path = seal_candidate_package(
        output_parent,
        "commissioned_rig_setup",
        identity,
        deduplicate_assets(assets),
        provenance={
            "writer": "orange_standalone_migration_tool",
            "source_authority": "citrus",
            "activation_performed": False,
        },
        declared_gaps=gaps,
    )
    inventory = read_json(package_path / "inventory.json")
    report = {
        "schema_id": "orange.calibration.v1_to_v2_inventory_diff",
        "schema_version": 1,
        "source_release": {
            "path": str(release_path),
            "release_id": release.get("release_id"),
            "sha256": release_sha256,
            "member_count": len(members),
        },
        "candidate": {
            "path": str(package_path),
            "package_id": read_json(package_path / "package.json").get("package_id"),
            "status": inventory.get("status"),
            "file_count": inventory.get("file_count"),
            "total_bytes": inventory.get("total_bytes"),
            "required_failure_count": inventory.get("required_failure_count"),
        },
        "comparison": {
            "all_materialized_members_are_exact_source_bytes": all(
                row.get("exact_source_bytes") is True
                and row.get("source", {}).get("sha256") == row.get("sha256")
                for row in inventory.get("files", [])
            ),
            "source_release_checksum_preserved": any(
                row.get("role") == "source_v1_commissioning_release"
                and row.get("sha256") == release_sha256
                for row in inventory.get("files", [])
            ),
            "camera_arena_scope": identity["camera_arena_map"],
            "current_canvas_compatibility": canvas_compatibility,
        },
        "gaps": inventory.get("gaps", []),
        "activation": {
            "performed": False,
            "runtime_pointers_modified": False,
            "candidate_only": True,
        },
    }
    report_path = output_parent / f"{package_path.name}.inventory_diff.json"
    report_path.write_bytes(pretty_json_bytes(report))
    return package_path, report


def build_canvas_binding_candidate(
    commissioned_path: Path, output_parent: Path
) -> Path:
    commission = read_json(commissioned_path / "package.json")
    inventory = read_json(commissioned_path / "inventory.json")
    commission_manifest_sha256 = sha256_file(commissioned_path / "package.json")
    commission_inventory_sha256 = sha256_file(commissioned_path / "inventory.json")
    commission_identity = json_value(commission.get("identity"), "commission.identity")
    canvas_asset = next(
        (
            row
            for row in inventory.get("files", [])
            if row.get("role") == "authority_canvas_configuration_snapshot"
        ),
        None,
    )
    if not isinstance(canvas_asset, dict):
        raise CalibrationPackageError("commission has no canvas configuration asset")
    identity = {
        "authority": "orange_candidate_packager",
        "rig_id": commission_identity["rig_id"],
        "canvas_id": commission_identity["canvas_id"],
        "commissioned_setup_id": commission["package_id"],
        "commissioned_setup_identity_sha256": commission["identity_sha256"],
        "commissioned_setup_manifest_sha256": commission_manifest_sha256,
        "commissioned_setup_inventory_sha256": commission_inventory_sha256,
        "canvas_config_sha256": canvas_asset["sha256"],
        "target_plane": commission_identity["target_plane"],
        "camera_arena_map": commission_identity["camera_arena_map"],
    }
    source = commissioned_path / canvas_asset["package_relative_path"]
    return seal_candidate_package(
        output_parent,
        "experiment_canvas_binding",
        identity,
        [
            asset(
                commissioned_path / "package.json",
                "assets/commissioned_setup/package.json",
                "commissioned_setup_package_manifest",
                expected_sha256=commission_manifest_sha256,
                target_plane="not_applicable",
                classification="provenance",
            ),
            asset(
                commissioned_path / "inventory.json",
                "assets/commissioned_setup/inventory.json",
                "commissioned_setup_package_inventory",
                expected_sha256=commission_inventory_sha256,
                target_plane="not_applicable",
                classification="provenance",
            ),
            asset(
                source,
                "assets/canvas/canvas_config_snapshot.json",
                "selected_experiment_canvas_configuration",
                expected_sha256=canvas_asset["sha256"],
                target_plane=commission_identity["target_plane"],
            )
        ],
        provenance={
            "writer": "orange_standalone_migration_tool",
            "source_authority": "citrus_commission_candidate",
            "activation_performed": False,
        },
    )


def build_daily_registration_candidate(
    registration_path: Path,
    commissioned_path: Path,
    binding_path: Path,
    output_parent: Path,
    *,
    include_source_images: bool = False,
) -> Path:
    registration_path = registration_path.expanduser().resolve()
    registration = read_json(registration_path)
    if (
        registration.get("schema_id") != "citrus.calibration.daily_registration"
        or registration.get("schema_version") != 1
        or registration.get("status") != "accepted"
    ):
        raise CalibrationPackageError("daily registration is not accepted v1 data")
    commission = read_json(commissioned_path / "package.json")
    binding = read_json(binding_path / "package.json")
    commission_manifest_sha256 = sha256_file(commissioned_path / "package.json")
    commission_inventory_sha256 = sha256_file(commissioned_path / "inventory.json")
    binding_manifest_sha256 = sha256_file(binding_path / "package.json")
    binding_inventory_sha256 = sha256_file(binding_path / "inventory.json")
    base = json_value(registration.get("commissioning_base"), "registration.base")
    commission_identity = json_value(commission.get("identity"), "commission.identity")
    if base.get("release_id") != commission_identity.get("source_release_id"):
        raise CalibrationPackageError("daily registration release_id mismatch")
    if base.get("manifest_sha256") != commission_identity.get("source_release_sha256"):
        raise CalibrationPackageError("daily registration commissioning checksum mismatch")

    camera_map: dict[str, dict[str, Any]] = {}
    assets: list[AssetSpec] = []
    for role, package_path, manifest_sha, inventory_sha in [
        (
            "commissioned_setup",
            commissioned_path,
            commission_manifest_sha256,
            commission_inventory_sha256,
        ),
        (
            "canvas_binding",
            binding_path,
            binding_manifest_sha256,
            binding_inventory_sha256,
        ),
    ]:
        add_ref(
            assets,
            package_path / "package.json",
            f"assets/package_refs/{role}/package.json",
            f"{role}_package_manifest",
            expected=manifest_sha,
            classification="provenance",
        )
        add_ref(
            assets,
            package_path / "inventory.json",
            f"assets/package_refs/{role}/inventory.json",
            f"{role}_package_inventory",
            expected=inventory_sha,
            classification="provenance",
        )
    add_ref(
        assets,
        registration_path,
        "assets/registration/registration.json",
        "accepted_daily_registration",
        expected=sha256_file(registration_path),
        target_plane="projected_surface",
    )
    add_ref(
        assets,
        registration.get("candidate_path"),
        "assets/registration/candidate.json",
        "accepted_daily_registration_candidate",
        expected=registration.get("candidate_sha256"),
        target_plane="projected_surface",
    )
    verification_by_scope: dict[tuple[str, str], dict[str, Any]] = {}
    verification = registration.get("verification")
    if isinstance(verification, dict):
        for row in verification.get("targets", []):
            if isinstance(row, dict):
                verification_by_scope[(str(row.get("camera_id")), str(row.get("arena_id")))] = row

    for target in registration.get("targets", []):
        target = json_value(target, "registration.targets[]")
        camera_id = string_value(target.get("camera_id"), "daily target camera_id")
        arena_id = string_value(target.get("arena_id"), "daily target arena_id")
        if camera_id not in commission_identity.get("camera_arena_map", {}):
            raise CalibrationPackageError(f"daily target camera outside commission: {camera_id}")
        if commission_identity["camera_arena_map"][camera_id]["arena_id"] != arena_id:
            raise CalibrationPackageError(f"daily target arena mismatch: {camera_id}/{arena_id}")
        camera_map[camera_id] = commission_identity["camera_arena_map"][camera_id]
        prefix = f"assets/cameras/Cam{camera_id}/daily_registration"
        rim = json_value(target.get("rim_observation"), f"{camera_id}.rim_observation")
        observation_path = Path(str(rim.get("path", "")))
        add_ref(
            assets,
            observation_path,
            f"{prefix}/observation.json",
            "accepted_dish_top_rim_observation",
            expected=rim.get("sha256"),
            target_plane="dish_top_rim_observation",
            camera_id=camera_id,
            arena_id=arena_id,
        )
        compact_members = [
            ("manifest.json", "dish_top_rim_observation_manifest"),
            ("image_set.json", "dish_top_rim_source_image_set"),
            ("exports/spatial_dish_mask_runtime_v1.json", "dish_mask_runtime"),
            ("exports/palette_dish_mask_v2.json", "palette_dish_mask"),
        ]
        for relative, role in compact_members:
            add_ref(
                assets,
                observation_path.parent / relative,
                f"{prefix}/{relative}",
                role,
                target_plane="camera_native_dish_plane",
                camera_id=camera_id,
                arena_id=arena_id,
            )
        for filename, role in [
            ("top_rim_fit.png", "accepted_inner_rim_overlay"),
            ("valid_detection_region.png", "valid_centroid_gate_overlay"),
            ("registration_hough_overlay.png", "raw_hough_proposal_overlay"),
        ]:
            add_ref(
                assets,
                observation_path.parent / "overlays" / filename,
                f"{prefix}/overlays/{filename}",
                role,
                target_plane="camera_native_dish_plane",
                camera_id=camera_id,
                arena_id=arena_id,
                classification="review_evidence",
            )
        review = verification_by_scope.get((camera_id, arena_id), {})
        review_ref = review.get("geometry_review_observation")
        if isinstance(review_ref, dict):
            add_ref(
                assets,
                review_ref.get("path"),
                f"{prefix}/geometry_review.json",
                "daily_registration_geometry_review",
                expected=review_ref.get("sha256"),
                target_plane="projected_surface",
                camera_id=camera_id,
                arena_id=arena_id,
                classification="review_evidence",
            )
        if include_source_images:
            observation = read_json(observation_path)
            source = observation.get("source_image")
            if isinstance(source, dict):
                add_ref(
                    assets,
                    source.get("path"),
                    f"{prefix}/source_frame.png",
                    "dish_top_rim_source_capture",
                    expected=source.get("sha256"),
                    target_plane="camera_native_dish_plane",
                    camera_id=camera_id,
                    arena_id=arena_id,
                    required=False,
                    classification="archive_evidence",
                )

    identity = {
        "authority": "citrus_runtime_registration_with_orange_observations",
        "rig_id": string_value(registration.get("rig_id"), "registration.rig_id"),
        "canvas_id": string_value(
            registration.get("canvas_name"), "registration.canvas_name"
        ),
        "commissioned_setup_id": commission["package_id"],
        "commissioned_setup_identity_sha256": commission["identity_sha256"],
        "commissioned_setup_manifest_sha256": commission_manifest_sha256,
        "commissioned_setup_inventory_sha256": commission_inventory_sha256,
        "canvas_binding_id": binding["package_id"],
        "canvas_binding_identity_sha256": binding["identity_sha256"],
        "canvas_binding_manifest_sha256": binding_manifest_sha256,
        "canvas_binding_inventory_sha256": binding_inventory_sha256,
        "source_registration_id": string_value(
            registration.get("registration_id"), "registration.registration_id"
        ),
        "source_registration_sha256": sha256_file(registration_path),
        "valid_until_utc": string_value(
            registration.get("valid_until_utc"), "registration.valid_until_utc"
        ),
        "transform_policy": "translation_only_move_arena_and_experimental_area_together",
        "canonical_experimental_dimensions_preserved": True,
        "target_plane": "projected_surface",
        "camera_arena_map": dict(sorted(camera_map.items())),
    }
    return seal_candidate_package(
        output_parent,
        "daily_registration_set",
        identity,
        deduplicate_assets(assets),
        provenance={
            "writer": "orange_standalone_migration_tool",
            "source_authority": "citrus_and_orange",
            "activation_performed": False,
        },
    )


def build_capsule_candidate(
    commissioned_path: Path,
    binding_path: Path,
    output_parent: Path,
    daily_path: Path | None = None,
) -> Path:
    sources = [
        ("commissioned_setup", commissioned_path),
        ("canvas_binding", binding_path),
    ]
    if daily_path:
        sources.append(("daily_registration", daily_path))
    assets: list[AssetSpec] = []
    references: dict[str, Any] = {}
    camera_map: dict[str, Any] | None = None
    for role, package_path in sources:
        manifest_path = package_path / "package.json"
        inventory_path = package_path / "inventory.json"
        manifest = read_json(manifest_path)
        identity = json_value(manifest.get("identity"), f"{role}.identity")
        if camera_map is None:
            camera_map = identity.get("camera_arena_map")
        references[role] = {
            "package_id": manifest["package_id"],
            "identity_sha256": manifest["identity_sha256"],
            "package_manifest_sha256": sha256_file(manifest_path),
            "inventory_sha256": sha256_file(inventory_path),
        }
        add_ref(
            assets,
            manifest_path,
            f"assets/package_refs/{role}/package.json",
            f"{role}_package_manifest",
            expected=sha256_file(manifest_path),
            classification="provenance",
        )
        add_ref(
            assets,
            inventory_path,
            f"assets/package_refs/{role}/inventory.json",
            f"{role}_package_inventory",
            expected=sha256_file(inventory_path),
            classification="provenance",
        )
    commission = read_json(commissioned_path / "package.json")
    commission_identity = json_value(commission.get("identity"), "commission.identity")
    identity = {
        "authority": "orange",
        "rig_id": commission_identity["rig_id"],
        "canvas_id": commission_identity["canvas_id"],
        "composition_mode": "selected_daily_registration" if daily_path else "base_only",
        "composition": references,
        "target_plane": "projected_surface",
        "camera_arena_map": camera_map or {},
    }
    return seal_candidate_package(
        output_parent,
        "recording_calibration_capsule",
        identity,
        assets,
        provenance={
            "writer": "orange_standalone_migration_tool",
            "activation_performed": False,
            "recording_arm_performed": False,
        },
    )


def write_result(value: dict[str, Any]) -> None:
    print(json.dumps(value, indent=2, sort_keys=True))


def command_package_shadow(args: argparse.Namespace) -> int:
    output_parent = args.output.expanduser().resolve()
    output_parent.mkdir(parents=True, exist_ok=True)
    commission_path, diff = build_commissioned_setup_candidate(
        args.release,
        output_parent,
        fixture_manifest=args.fixture_manifest,
        build_provenance=args.build_provenance,
        include_archive_images=args.include_archive_images,
    )
    binding_path = build_canvas_binding_candidate(commission_path, output_parent)
    daily_path = None
    if args.daily_registration:
        daily_path = build_daily_registration_candidate(
            args.daily_registration,
            commission_path,
            binding_path,
            output_parent,
            include_source_images=args.include_archive_images,
        )
    capsule_path = build_capsule_candidate(
        commission_path, binding_path, output_parent, daily_path
    )
    results: dict[str, Any] = {
        "status": "candidate_created",
        "activation_performed": False,
        "output_parent": str(output_parent),
        "commissioned_setup": str(commission_path),
        "experiment_canvas_binding": str(binding_path),
        "daily_registration_set": str(daily_path) if daily_path else None,
        "recording_calibration_capsule": str(capsule_path),
        "inventory_diff": str(
            output_parent / f"{commission_path.name}.inventory_diff.json"
        ),
        "commission_completeness": diff["candidate"]["status"],
        "commission_required_failure_count": diff["candidate"][
            "required_failure_count"
        ],
    }
    validation: dict[str, Any] = {}
    for label, path in [
        ("commissioned_setup", commission_path),
        ("experiment_canvas_binding", binding_path),
        ("daily_registration_set", daily_path),
        ("recording_calibration_capsule", capsule_path),
    ]:
        if path is None:
            continue
        try:
            validation[label] = validate_candidate_package(path)
        except CalibrationPackageError as exc:
            validation[label] = {"status": "invalid", "error": str(exc)}
    results["validation"] = validation
    write_result(results)
    return 0 if all(row.get("status") == "valid" for row in validation.values()) else 2


def command_validate(args: argparse.Namespace) -> int:
    result = validate_candidate_package(
        args.package, verify_sources=args.verify_sources
    )
    write_result(result)
    return 0


def command_identity(args: argparse.Namespace) -> int:
    identity = read_json(args.identity)
    identifier, checksum = package_id(args.kind, identity)
    write_result(
        {
            "canonical_profile": "orange.canonical_json.v1",
            "package_kind": args.kind,
            "package_id": identifier,
            "identity_sha256": checksum,
            "canonical_sha256": canonical_sha256(identity),
        }
    )
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    package_parser = subparsers.add_parser(
        "package-shadow",
        help="migrate an accepted Shadow v1 release into non-active v2 candidates",
    )
    package_parser.add_argument("--release", type=Path, default=DEFAULT_SHADOW_RELEASE)
    package_parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="new candidate parent; no active pointer is written",
    )
    package_parser.add_argument(
        "--fixture-manifest",
        type=Path,
        help="operator-reviewed checksum-bound fixture manifest; never inferred",
    )
    package_parser.add_argument(
        "--build-provenance",
        type=Path,
        help="reviewed JSON binding the exact Citrus and Orange software builds",
    )
    package_parser.add_argument("--daily-registration", type=Path)
    package_parser.add_argument("--include-archive-images", action="store_true")
    package_parser.set_defaults(handler=command_package_shadow)

    validate_parser = subparsers.add_parser("validate")
    validate_parser.add_argument("package", type=Path)
    validate_parser.add_argument("--verify-sources", action="store_true")
    validate_parser.set_defaults(handler=command_validate)

    identity_parser = subparsers.add_parser("identity")
    identity_parser.add_argument(
        "kind",
        choices=[
            "commissioned_rig_setup",
            "experiment_canvas_binding",
            "daily_registration_set",
            "recording_calibration_capsule",
        ],
    )
    identity_parser.add_argument("identity", type=Path)
    identity_parser.set_defaults(handler=command_identity)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(list(argv or sys.argv[1:]))
    try:
        return int(args.handler(args))
    except CalibrationPackageError as exc:
        print(f"calibration package error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
