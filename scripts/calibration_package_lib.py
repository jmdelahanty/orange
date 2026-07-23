#!/usr/bin/env python3
"""Immutable, non-active calibration package primitives.

This module deliberately has no Orange or Citrus runtime dependency.  It is
used by the migration CLI and its focused tests, and is intentionally not
wired into experiment start or any active calibration pointer.
"""

from __future__ import annotations

import copy
import hashlib
import json
import math
import os
import shutil
import tempfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any, Iterable, Mapping, Sequence


CANONICAL_JSON_PROFILE = "orange.canonical_json.v1"
INVENTORY_SCHEMA_ID = "orange.calibration.package_inventory"
INVENTORY_SCHEMA_VERSION = 1
PACKAGE_SCHEMA_VERSION = 2
PACKAGE_SCHEMA_IDS = {
    "commissioned_rig_setup": "orange.calibration.commissioned_rig_setup",
    "experiment_canvas_binding": "orange.calibration.experiment_canvas_binding",
    "daily_registration_set": "orange.calibration.daily_registration_set",
    "recording_calibration_capsule": "orange.recording.calibration_capsule",
}
PACKAGE_ID_PREFIXES = {
    "commissioned_rig_setup": "commission",
    "experiment_canvas_binding": "binding",
    "daily_registration_set": "dailyreg",
    "recording_calibration_capsule": "capsule",
}
SHA256_PREFIX = "sha256:"


class CalibrationPackageError(RuntimeError):
    """Raised when a package cannot be materialized or validated safely."""


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z"
    )


def sha256_bytes(data: bytes) -> str:
    return SHA256_PREFIX + hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return SHA256_PREFIX + digest.hexdigest()


def _validate_canonical_value(value: Any, location: str = "$") -> None:
    """Restrict identity inputs to the cross-language canonical v1 subset.

    Floating point serialization differs subtly across standard libraries.
    Package identities therefore use strings/integers/booleans/null and
    containers only. Numeric calibration results remain exact-byte assets.
    """

    if value is None or isinstance(value, (str, bool)):
        return
    if isinstance(value, int) and not isinstance(value, bool):
        if value < -(2**63) or value > 2**63 - 1:
            raise CalibrationPackageError(
                f"{location}: integer is outside the signed 64-bit canonical range"
            )
        return
    if isinstance(value, float):
        raise CalibrationPackageError(
            f"{location}: floating-point values are not permitted in "
            f"{CANONICAL_JSON_PROFILE} identities"
        )
    if isinstance(value, list):
        for index, item in enumerate(value):
            _validate_canonical_value(item, f"{location}[{index}]")
        return
    if isinstance(value, dict):
        for key, item in value.items():
            if not isinstance(key, str):
                raise CalibrationPackageError(
                    f"{location}: canonical JSON object keys must be strings"
                )
            _validate_canonical_value(item, f"{location}.{key}")
        return
    raise CalibrationPackageError(
        f"{location}: unsupported canonical JSON value {type(value).__name__}"
    )


def canonical_json_bytes(value: Any) -> bytes:
    """Return the normative UTF-8 encoding for orange.canonical_json.v1."""

    _validate_canonical_value(value)
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")


def canonical_sha256(value: Any) -> str:
    return sha256_bytes(canonical_json_bytes(value))


def _positive_number(value: Any) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
        and value > 0
    )


def projection_geometry_identity(canvas: Mapping[str, Any]) -> dict[str, Any]:
    """Build Citrus/Orange canvas projection-geometry identity v1.

    Keep this rule structurally identical to
    ``canvas_projection_geometry_identity.h``. It excludes only calibration
    presentation controls, accepted projected-surface scale caches, and pixel
    dimensions that are derived from explicit positive physical dimensions.
    """

    if not isinstance(canvas, dict) or not all(
        key in canvas
        for key in ("canvas_name", "canvas_width_px", "canvas_height_px", "arenas")
    ) or not isinstance(canvas.get("arenas"), dict):
        raise CalibrationPackageError("canvas_geometry_identity_invalid")
    geometry = copy.deepcopy(dict(canvas))
    for arena in geometry["arenas"].values():
        if not isinstance(arena, dict):
            raise CalibrationPackageError("canvas_arena_geometry_invalid")
        for key in list(arena):
            if (
                key in {
                    "calibration_pattern_mode",
                    "calibration_pattern_mask_policy",
                    "dot_radius_px",
                    "grid_cols",
                    "grid_rows",
                }
                or key.startswith("calibration_ring_")
                or key.startswith("calibration_verification_")
            ):
                del arena[key]
        for physical_key, pixel_key in (
            ("experimental_area_width_mm", "experimental_area_width_px"),
            ("experimental_area_height_mm", "experimental_area_height_px"),
            ("experimental_area_radius_mm", "experimental_area_radius_px"),
            ("experimental_area_corner_radius_mm", "experimental_area_corner_radius_px"),
        ):
            if _positive_number(arena.get(physical_key)):
                arena.pop(pixel_key, None)
        cameras = arena.get("camera_calibrations")
        if cameras is None:
            continue
        if not isinstance(cameras, list):
            raise CalibrationPackageError("canvas_camera_calibrations_invalid")
        for camera in cameras:
            if not isinstance(camera, dict):
                raise CalibrationPackageError("canvas_camera_calibration_invalid")
            for key in (
                "scale_image_path",
                "real_world_ref_mm",
                "pixels_per_mm_camera",
                "pixels_per_mm_projector",
            ):
                camera.pop(key, None)
            models = camera.get("scale_models")
            if isinstance(models, list):
                camera["scale_models"] = [
                    model
                    for model in models
                    if not (
                        isinstance(model, dict)
                        and model.get("target_plane") == "projected_surface"
                    )
                ]
    return {
        "schema_id": "citrus.calibration.canvas_projection_geometry_identity",
        "schema_version": 1,
        "canvas": geometry,
    }


def projection_geometry_fingerprint(canvas: Mapping[str, Any]) -> str:
    identity = projection_geometry_identity(canvas)
    # Unlike package-ID identities, canvas geometry contains calibrated
    # floating-point values. Both nlohmann::json and Python emit compact,
    # key-sorted JSON for this established v1 contract.
    encoded = json.dumps(
        identity,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")
    return sha256_bytes(encoded)


def compare_projection_geometry(
    current: Mapping[str, Any], accepted: Mapping[str, Any]
) -> dict[str, Any]:
    current_fingerprint = projection_geometry_fingerprint(current)
    accepted_fingerprint = projection_geometry_fingerprint(accepted)
    compatible = current_fingerprint == accepted_fingerprint
    return {
        "compatible": compatible,
        "basis": "projection_geometry_identity_v1",
        "warning": "canvas_non_geometry_calibration_state_only_change"
        if compatible and current != accepted
        else None,
        "error": None if compatible else "canvas_projection_geometry_changed",
        "current_geometry_fingerprint": current_fingerprint,
        "accepted_geometry_fingerprint": accepted_fingerprint,
    }


def pretty_json_bytes(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CalibrationPackageError(f"cannot read JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise CalibrationPackageError(f"expected a JSON object in {path}")
    return value


def require_sha256(value: str, label: str) -> None:
    if (
        not isinstance(value, str)
        or len(value) != len(SHA256_PREFIX) + 64
        or not value.startswith(SHA256_PREFIX)
        or any(character not in "0123456789abcdef" for character in value[7:])
    ):
        raise CalibrationPackageError(f"{label} is not a lowercase SHA-256 identity")


def safe_component(value: str) -> str:
    safe = "".join(
        character if character.isalnum() or character in "-_." else "_"
        for character in str(value)
    )[:120]
    while safe.startswith("."):
        safe = "_" + safe[1:]
    return safe or "unnamed"


def validate_relative_path(value: str, label: str = "package_relative_path") -> None:
    if not isinstance(value, str) or not value:
        raise CalibrationPackageError(f"{label} must be a non-empty string")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or "." in path.parts:
        raise CalibrationPackageError(f"{label} escapes or is not normalized: {value}")
    if "\\" in value:
        raise CalibrationPackageError(f"{label} must use POSIX separators: {value}")


@dataclass(frozen=True)
class AssetSpec:
    source_path: Path
    package_relative_path: str
    role: str
    target_plane: str
    camera_id: str | None
    arena_id: str | None
    required: bool = True
    expected_sha256: str | None = None
    classification: str = "runtime_critical"
    provenance: Mapping[str, Any] = field(default_factory=dict)

    def validate(self) -> None:
        validate_relative_path(self.package_relative_path)
        if not self.role:
            raise CalibrationPackageError("asset role must be non-empty")
        if not self.target_plane:
            raise CalibrationPackageError(f"{self.role}: target_plane must be explicit")
        if (self.camera_id is None) != (self.arena_id is None):
            raise CalibrationPackageError(
                f"{self.role}: camera_id and arena_id must both be set or both be null"
            )
        if self.expected_sha256:
            require_sha256(self.expected_sha256, f"{self.role}.expected_sha256")
        if self.classification not in {
            "runtime_critical",
            "review_evidence",
            "archive_evidence",
            "provenance",
        }:
            raise CalibrationPackageError(
                f"{self.role}: unsupported classification {self.classification}"
            )


def package_id(package_kind: str, identity: Mapping[str, Any]) -> tuple[str, str]:
    if package_kind not in PACKAGE_SCHEMA_IDS:
        raise CalibrationPackageError(f"unsupported package kind: {package_kind}")
    identity_sha256 = canonical_sha256(dict(identity))
    return (
        f"{PACKAGE_ID_PREFIXES[package_kind]}_{identity_sha256[7:]}",
        identity_sha256,
    )


def _fsync_file(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _fsync_tree_directories(root: Path) -> None:
    directories = [path for path in root.rglob("*") if path.is_dir()]
    directories.sort(key=lambda path: len(path.parts), reverse=True)
    for directory in directories:
        _fsync_directory(directory)
    _fsync_directory(root)


def _write_exact(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as destination:
        destination.write(data)
        destination.flush()
        os.fsync(destination.fileno())


def _asset_scope_row(spec: AssetSpec) -> dict[str, Any]:
    return {
        "camera_id": spec.camera_id,
        "arena_id": spec.arena_id,
        "target_plane": spec.target_plane,
    }


def seal_candidate_package(
    output_parent: Path,
    package_kind: str,
    identity: Mapping[str, Any],
    assets: Sequence[AssetSpec],
    *,
    provenance: Mapping[str, Any] | None = None,
    declared_gaps: Sequence[Mapping[str, Any]] = (),
) -> Path:
    """Materialize a new immutable candidate beneath ``output_parent``.

    No active pointer is written. Existing destinations are never overwritten;
    an identical existing candidate must be validated by the caller instead.
    """

    output_parent = output_parent.resolve()
    output_parent.mkdir(parents=True, exist_ok=True)
    identifier, identity_sha256 = package_id(package_kind, identity)
    final_path = output_parent / identifier
    if final_path.exists():
        raise CalibrationPackageError(
            f"immutable candidate already exists; refusing overwrite: {final_path}"
        )

    seen_destinations: set[str] = set()
    for spec in assets:
        spec.validate()
        if spec.package_relative_path in seen_destinations:
            raise CalibrationPackageError(
                f"duplicate package path: {spec.package_relative_path}"
            )
        seen_destinations.add(spec.package_relative_path)

    staging = Path(
        tempfile.mkdtemp(prefix=f".{identifier}.staging.", dir=output_parent)
    )
    published = False
    try:
        file_rows: list[dict[str, Any]] = []
        missing_rows: list[dict[str, Any]] = []
        total_bytes = 0
        required_failures = 0
        sorted_assets = sorted(assets, key=lambda asset: asset.package_relative_path)
        for spec in sorted_assets:
            source = spec.source_path.expanduser().resolve()
            if not source.is_file():
                row = {
                    "role": spec.role,
                    "source_path": str(source),
                    "package_relative_path": spec.package_relative_path,
                    "required": spec.required,
                    "classification": spec.classification,
                    "scope": _asset_scope_row(spec),
                    "reason": "source_not_regular_file",
                }
                missing_rows.append(row)
                required_failures += int(spec.required)
                continue
            data = source.read_bytes()
            checksum = sha256_bytes(data)
            if spec.expected_sha256 and checksum != spec.expected_sha256:
                row = {
                    "role": spec.role,
                    "source_path": str(source),
                    "package_relative_path": spec.package_relative_path,
                    "required": spec.required,
                    "classification": spec.classification,
                    "scope": _asset_scope_row(spec),
                    "reason": "declared_source_sha256_mismatch",
                    "expected_sha256": spec.expected_sha256,
                    "observed_sha256": checksum,
                }
                missing_rows.append(row)
                required_failures += int(spec.required)
                continue
            destination = staging / spec.package_relative_path
            _write_exact(destination, data)
            row = {
                "role": spec.role,
                "package_relative_path": spec.package_relative_path,
                "sha256": checksum,
                "size_bytes": len(data),
                "required": spec.required,
                "classification": spec.classification,
                "exact_source_bytes": True,
                "scope": _asset_scope_row(spec),
                "source": {
                    "path": str(source),
                    "sha256": checksum,
                    "size_bytes": len(data),
                },
            }
            if spec.expected_sha256:
                row["source"]["declared_sha256"] = spec.expected_sha256
                row["source"]["declared_sha256_verified"] = True
            if spec.provenance:
                row["provenance"] = dict(spec.provenance)
            file_rows.append(row)
            total_bytes += len(data)

        for gap in declared_gaps:
            gap_row = dict(gap)
            gap_row.setdefault("required", False)
            missing_rows.append(gap_row)
            required_failures += int(bool(gap_row.get("required")))

        completeness = "complete" if required_failures == 0 else "incomplete"
        inventory = {
            "schema_id": INVENTORY_SCHEMA_ID,
            "schema_version": INVENTORY_SCHEMA_VERSION,
            "package_id": identifier,
            "package_kind": package_kind,
            "status": completeness,
            "policy": {
                "exact_source_bytes": True,
                "package_relative_paths_authoritative": True,
                "absolute_source_paths_provenance_only": True,
                "cross_camera_fallback_allowed": False,
                "cross_arena_fallback_allowed": False,
            },
            "file_count": len(file_rows),
            "total_bytes": total_bytes,
            "required_failure_count": required_failures,
            "files": file_rows,
            "gaps": missing_rows,
        }
        inventory_bytes = pretty_json_bytes(inventory)
        _write_exact(staging / "inventory.json", inventory_bytes)
        inventory_sha256 = sha256_bytes(inventory_bytes)

        manifest = {
            "schema_id": PACKAGE_SCHEMA_IDS[package_kind],
            "schema_version": PACKAGE_SCHEMA_VERSION,
            "package_kind": package_kind,
            "package_id": identifier,
            "identity_sha256": identity_sha256,
            "identity": dict(identity),
            "canonicalization": {
                "profile": CANONICAL_JSON_PROFILE,
                "identity_input_path": "package.json#/identity",
                "digest_algorithm": "sha256",
                "floating_point_values_allowed": False,
            },
            "lifecycle": {
                "state": "candidate",
                "activation_allowed": False,
                "active_pointer_written": False,
                "immutable": True,
            },
            "created_at_utc": utc_now(),
            "authority": dict(provenance or {}),
            "inventory": {
                "package_relative_path": "inventory.json",
                "sha256": inventory_sha256,
                "file_count": len(file_rows),
                "total_bytes": total_bytes,
            },
            "completeness": {
                "status": completeness,
                "required_failure_count": required_failures,
                "gap_count": len(missing_rows),
            },
        }
        _write_exact(staging / "package.json", pretty_json_bytes(manifest))
        _fsync_tree_directories(staging)
        staging.rename(final_path)
        published = True
        _fsync_directory(output_parent)
        return final_path
    finally:
        if not published:
            shutil.rmtree(staging, ignore_errors=True)


def _required_object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise CalibrationPackageError(f"{label} must be an object")
    return value


def _validate_identity_scope(
    identity: Mapping[str, Any], file_row: Mapping[str, Any]
) -> None:
    scope = _required_object(file_row.get("scope"), "inventory file scope")
    for key in ("camera_id", "arena_id", "target_plane"):
        if key not in scope:
            raise CalibrationPackageError(f"inventory file scope lacks {key}")
    camera_id = scope["camera_id"]
    arena_id = scope["arena_id"]
    if (camera_id is None) != (arena_id is None):
        raise CalibrationPackageError(
            "inventory file camera_id and arena_id must both be null or both be set"
        )
    if camera_id is None:
        return
    camera_map = identity.get("camera_arena_map")
    if not isinstance(camera_map, dict) or camera_id not in camera_map:
        raise CalibrationPackageError(
            f"inventory camera {camera_id} is outside package identity scope"
        )
    camera_identity = _required_object(
        camera_map[camera_id], f"identity camera {camera_id}"
    )
    if camera_identity.get("arena_id") != arena_id:
        raise CalibrationPackageError(
            f"inventory arena {arena_id} does not match identity for camera {camera_id}"
        )


def _validate_embedded_package(
    package_path: Path,
    files: Sequence[Mapping[str, Any]],
    identity: Mapping[str, Any],
    role: str,
    identity_prefix: str,
) -> None:
    by_role = {row.get("role"): row for row in files}
    manifest_row = by_role.get(f"{role}_package_manifest")
    inventory_row = by_role.get(f"{role}_package_inventory")
    if not isinstance(manifest_row, dict) or not isinstance(inventory_row, dict):
        raise CalibrationPackageError(f"package lacks embedded {role} metadata")
    embedded_manifest = read_json(
        package_path / manifest_row["package_relative_path"]
    )
    embedded_inventory = read_json(
        package_path / inventory_row["package_relative_path"]
    )
    if (
        embedded_manifest.get("completeness", {}).get("status") != "complete"
        or embedded_manifest.get("package_id")
        != identity.get(f"{identity_prefix}_id")
        or embedded_manifest.get("identity_sha256")
        != identity.get(f"{identity_prefix}_identity_sha256")
        or manifest_row.get("sha256")
        != identity.get(f"{identity_prefix}_manifest_sha256")
        or inventory_row.get("sha256")
        != identity.get(f"{identity_prefix}_inventory_sha256")
        or embedded_manifest.get("inventory", {}).get("sha256")
        != inventory_row.get("sha256")
        or embedded_inventory.get("package_id") != embedded_manifest.get("package_id")
    ):
        raise CalibrationPackageError(
            f"embedded {role} package is incomplete or does not match identity"
        )


def validate_candidate_package(
    package_path: Path, *, verify_sources: bool = False
) -> dict[str, Any]:
    """Validate package identities and every materialized byte."""

    package_path = package_path.resolve()
    manifest_path = package_path / "package.json"
    inventory_path = package_path / "inventory.json"
    manifest = read_json(manifest_path)
    inventory = read_json(inventory_path)

    package_kind = manifest.get("package_kind")
    if package_kind not in PACKAGE_SCHEMA_IDS:
        raise CalibrationPackageError(f"unknown package_kind: {package_kind}")
    if manifest.get("schema_id") != PACKAGE_SCHEMA_IDS[package_kind]:
        raise CalibrationPackageError("package schema_id does not match package_kind")
    if manifest.get("schema_version") != PACKAGE_SCHEMA_VERSION:
        raise CalibrationPackageError("unsupported package schema_version")
    lifecycle = _required_object(manifest.get("lifecycle"), "package lifecycle")
    if (
        lifecycle.get("state") != "candidate"
        or lifecycle.get("activation_allowed") is not False
        or lifecycle.get("active_pointer_written") is not False
        or lifecycle.get("immutable") is not True
    ):
        raise CalibrationPackageError("package is not a non-active immutable candidate")
    canonicalization = _required_object(
        manifest.get("canonicalization"), "package canonicalization"
    )
    if canonicalization != {
        "profile": CANONICAL_JSON_PROFILE,
        "identity_input_path": "package.json#/identity",
        "digest_algorithm": "sha256",
        "floating_point_values_allowed": False,
    }:
        raise CalibrationPackageError("package canonicalization contract is invalid")

    identity = _required_object(manifest.get("identity"), "package identity")
    expected_id, expected_identity_sha256 = package_id(package_kind, identity)
    if manifest.get("package_id") != expected_id:
        raise CalibrationPackageError("package_id does not match canonical identity")
    if manifest.get("identity_sha256") != expected_identity_sha256:
        raise CalibrationPackageError("identity_sha256 does not match canonical identity")
    if package_path.name != expected_id:
        raise CalibrationPackageError("package directory name does not match package_id")

    inventory_reference = _required_object(
        manifest.get("inventory"), "package inventory reference"
    )
    if inventory_reference.get("package_relative_path") != "inventory.json":
        raise CalibrationPackageError("inventory path must be inventory.json")
    observed_inventory_sha256 = sha256_file(inventory_path)
    if inventory_reference.get("sha256") != observed_inventory_sha256:
        raise CalibrationPackageError("inventory checksum mismatch")
    if (
        inventory.get("schema_id") != INVENTORY_SCHEMA_ID
        or inventory.get("schema_version") != INVENTORY_SCHEMA_VERSION
        or inventory.get("package_id") != expected_id
        or inventory.get("package_kind") != package_kind
    ):
        raise CalibrationPackageError("inventory identity does not match package")
    completeness = _required_object(
        manifest.get("completeness"), "package completeness"
    )
    gaps = inventory.get("gaps")
    if not isinstance(gaps, list):
        raise CalibrationPackageError("inventory gaps must be an array")
    observed_required_failures = 0
    for index, gap_value in enumerate(gaps):
        gap = _required_object(gap_value, f"inventory.gaps[{index}]")
        if not isinstance(gap.get("role"), str) or not gap["role"]:
            raise CalibrationPackageError(f"inventory.gaps[{index}] lacks a role")
        if not isinstance(gap.get("required"), bool):
            raise CalibrationPackageError(
                f"inventory.gaps[{index}].required must be boolean"
            )
        observed_required_failures += int(gap["required"])
    expected_inventory_status = (
        "complete" if observed_required_failures == 0 else "incomplete"
    )
    if (
        inventory.get("status") != expected_inventory_status
        or inventory.get("required_failure_count") != observed_required_failures
    ):
        raise CalibrationPackageError("inventory completeness metadata is inconsistent")
    if (
        completeness.get("status") != inventory.get("status")
        or completeness.get("required_failure_count")
        != inventory.get("required_failure_count")
        or completeness.get("gap_count") != len(gaps)
    ):
        raise CalibrationPackageError("package completeness disagrees with inventory")
    policy = _required_object(inventory.get("policy"), "inventory policy")
    for policy_key in (
        "exact_source_bytes",
        "package_relative_paths_authoritative",
        "absolute_source_paths_provenance_only",
    ):
        if policy.get(policy_key) is not True:
            raise CalibrationPackageError(f"inventory policy {policy_key} must be true")
    for policy_key in (
        "cross_camera_fallback_allowed",
        "cross_arena_fallback_allowed",
    ):
        if policy.get(policy_key) is not False:
            raise CalibrationPackageError(f"inventory policy {policy_key} must be false")

    files = inventory.get("files")
    if not isinstance(files, list):
        raise CalibrationPackageError("inventory files must be an array")
    seen_paths: set[str] = set()
    observed_total = 0
    for index, file_row_value in enumerate(files):
        file_row = _required_object(file_row_value, f"inventory.files[{index}]")
        for field_name in (
            "role",
            "package_relative_path",
            "sha256",
            "size_bytes",
            "required",
            "classification",
            "exact_source_bytes",
            "scope",
            "source",
        ):
            if field_name not in file_row:
                raise CalibrationPackageError(
                    f"inventory.files[{index}] lacks {field_name}"
                )
        relative = file_row["package_relative_path"]
        validate_relative_path(relative, f"inventory.files[{index}].package_relative_path")
        if relative in seen_paths:
            raise CalibrationPackageError(f"duplicate inventory path: {relative}")
        seen_paths.add(relative)
        if file_row["exact_source_bytes"] is not True:
            raise CalibrationPackageError(f"{relative}: exact_source_bytes must be true")
        require_sha256(file_row["sha256"], f"{relative}.sha256")
        member_path = package_path / PurePosixPath(relative)
        current_path = package_path
        for part in PurePosixPath(relative).parts:
            current_path = current_path / part
            if current_path.is_symlink():
                raise CalibrationPackageError(
                    f"materialized member path contains a symlink: {relative}"
                )
        if not member_path.is_file():
            raise CalibrationPackageError(f"materialized member is missing: {relative}")
        size = member_path.stat().st_size
        if not isinstance(file_row["size_bytes"], int) or file_row["size_bytes"] != size:
            raise CalibrationPackageError(f"materialized member size mismatch: {relative}")
        if sha256_file(member_path) != file_row["sha256"]:
            raise CalibrationPackageError(f"materialized member checksum mismatch: {relative}")
        source = _required_object(file_row["source"], f"{relative}.source")
        for source_field in ("path", "sha256", "size_bytes"):
            if source_field not in source:
                raise CalibrationPackageError(f"{relative}.source lacks {source_field}")
        if source["sha256"] != file_row["sha256"] or source["size_bytes"] != size:
            raise CalibrationPackageError(f"{relative}: source provenance disagrees")
        if verify_sources:
            source_path = Path(source["path"])
            if not source_path.is_file():
                raise CalibrationPackageError(f"source is unavailable: {source_path}")
            if source_path.stat().st_size != size or sha256_file(source_path) != file_row["sha256"]:
                raise CalibrationPackageError(f"source changed after packaging: {source_path}")
        _validate_identity_scope(identity, file_row)
        observed_total += size

    if inventory.get("file_count") != len(files):
        raise CalibrationPackageError("inventory file_count mismatch")
    if inventory.get("total_bytes") != observed_total:
        raise CalibrationPackageError("inventory total_bytes mismatch")
    if inventory_reference.get("file_count") != len(files):
        raise CalibrationPackageError("package inventory file_count mismatch")
    if inventory_reference.get("total_bytes") != observed_total:
        raise CalibrationPackageError("package inventory total_bytes mismatch")
    if observed_required_failures != 0:
        raise CalibrationPackageError(
            "candidate has missing required package members after byte/source audit"
        )
    if package_kind == "commissioned_rig_setup":
        canvas_rows = [
            row
            for row in files
            if row.get("role") == "authority_canvas_configuration_snapshot"
        ]
        if len(canvas_rows) != 1:
            raise CalibrationPackageError(
                "commissioned setup must contain exactly one authority canvas snapshot"
            )
        canvas = read_json(package_path / canvas_rows[0]["package_relative_path"])
        observed_fingerprint = projection_geometry_fingerprint(canvas)
        projection_identity = identity.get("projection_geometry_identity")
        if (
            not isinstance(projection_identity, dict)
            or projection_identity.get("schema_id")
            != "citrus.calibration.canvas_projection_geometry_identity"
            or projection_identity.get("schema_version") != 1
            or projection_identity.get("fingerprint") != observed_fingerprint
        ):
            raise CalibrationPackageError(
                "commissioned setup projection geometry fingerprint mismatch"
            )
    elif package_kind == "experiment_canvas_binding":
        canvas_rows = [
            row
            for row in files
            if row.get("role") == "selected_experiment_canvas_configuration"
        ]
        if len(canvas_rows) != 1 or identity.get("canvas_config_sha256") != canvas_rows[0].get(
            "sha256"
        ):
            raise CalibrationPackageError(
                "canvas binding does not checksum-bind exactly one selected canvas"
            )
        _validate_embedded_package(
            package_path,
            files,
            identity,
            "commissioned_setup",
            "commissioned_setup",
        )
    elif package_kind == "daily_registration_set":
        if identity.get("canonical_experimental_dimensions_preserved") is not True:
            raise CalibrationPackageError("daily registration may not resize canonical geometry")
        if identity.get("transform_policy") != (
            "translation_only_move_arena_and_experimental_area_together"
        ):
            raise CalibrationPackageError("daily registration transform policy is unsupported")
        _validate_embedded_package(
            package_path,
            files,
            identity,
            "commissioned_setup",
            "commissioned_setup",
        )
        _validate_embedded_package(
            package_path,
            files,
            identity,
            "canvas_binding",
            "canvas_binding",
        )
        roles_by_camera: dict[str, dict[str, dict[str, Any]]] = {}
        for row in files:
            scope = row.get("scope", {})
            camera_id = scope.get("camera_id") if isinstance(scope, dict) else None
            if camera_id:
                roles_by_camera.setdefault(camera_id, {})[row.get("role", "")] = row
        required_roles = {
            "accepted_dish_top_rim_observation",
            "dish_top_rim_observation_manifest",
            "dish_top_rim_source_image_set",
            "dish_mask_runtime",
            "palette_dish_mask",
            "accepted_inner_rim_overlay",
            "valid_centroid_gate_overlay",
            "raw_hough_proposal_overlay",
            "daily_registration_geometry_review",
        }
        for camera_id, camera_identity in identity.get("camera_arena_map", {}).items():
            roles = roles_by_camera.get(camera_id, {})
            missing_roles = sorted(required_roles - set(roles))
            if missing_roles:
                raise CalibrationPackageError(
                    f"daily registration camera {camera_id} lacks roles: "
                    + ", ".join(missing_roles)
                )
            observation = read_json(
                package_path
                / roles["accepted_dish_top_rim_observation"]["package_relative_path"]
            )
            arena_context = observation.get("arena_context", {})
            if (
                not isinstance(arena_context, dict)
                or str(arena_context.get("camera_serial", "")) != camera_id
                or arena_context.get("arena_id") != camera_identity.get("arena_id")
            ):
                raise CalibrationPackageError(
                    f"daily rim observation identity mismatch for camera {camera_id}"
                )
            mask = read_json(
                package_path / roles["dish_mask_runtime"]["package_relative_path"]
            )
            geometry = mask.get("geometry", {})
            outer = geometry.get("outer_geometry", {}) if isinstance(geometry, dict) else {}
            valid = geometry.get("valid_geometry", {}) if isinstance(geometry, dict) else {}
            if (
                geometry.get("coordinate_space") != "camera_native_pixels"
                or outer.get("type") != "circle"
                or valid.get("type") != "circle"
                or not isinstance(outer.get("r"), (int, float))
                or not isinstance(valid.get("r"), (int, float))
                or valid["r"] < outer["r"]
            ):
                raise CalibrationPackageError(
                    f"daily dish mask geometry is invalid for camera {camera_id}"
                )
            source_observation = mask.get("source_observation", {})
            if (
                not isinstance(source_observation, dict)
                or source_observation.get("artifact_id") != observation.get("artifact_id")
            ):
                raise CalibrationPackageError(
                    f"daily dish mask does not bind its rim observation for {camera_id}"
                )
    elif package_kind == "recording_calibration_capsule":
        mode = identity.get("composition_mode")
        composition = identity.get("composition")
        if mode not in {"base_only", "selected_daily_registration"} or not isinstance(
            composition, dict
        ):
            raise CalibrationPackageError("recording capsule composition is invalid")
        expected_roles = {"commissioned_setup", "canvas_binding"}
        if mode == "selected_daily_registration":
            expected_roles.add("daily_registration")
        elif "daily_registration" in composition:
            raise CalibrationPackageError("base_only capsule contains daily registration")
        if set(composition) != expected_roles:
            raise CalibrationPackageError("recording capsule package reference set mismatch")
        by_role = {row.get("role"): row for row in files}
        for role in expected_roles:
            reference = _required_object(composition.get(role), f"composition.{role}")
            manifest_row = by_role.get(f"{role}_package_manifest")
            inventory_row = by_role.get(f"{role}_package_inventory")
            if not isinstance(manifest_row, dict) or not isinstance(inventory_row, dict):
                raise CalibrationPackageError(f"capsule lacks copied {role} package metadata")
            source_manifest = read_json(
                package_path / manifest_row["package_relative_path"]
            )
            if (
                source_manifest.get("completeness", {}).get("status") != "complete"
                or
                source_manifest.get("package_id") != reference.get("package_id")
                or source_manifest.get("identity_sha256")
                != reference.get("identity_sha256")
                or manifest_row.get("sha256")
                != reference.get("package_manifest_sha256")
                or inventory_row.get("sha256") != reference.get("inventory_sha256")
            ):
                raise CalibrationPackageError(
                    f"capsule {role} reference does not match copied exact bytes"
                )
    return {
        "status": "valid",
        "package_id": expected_id,
        "package_kind": package_kind,
        "identity_sha256": expected_identity_sha256,
        "inventory_sha256": observed_inventory_sha256,
        "file_count": len(files),
        "total_bytes": observed_total,
        "sources_verified": verify_sources,
    }


def asset(
    source: str | Path,
    relative: str,
    role: str,
    *,
    target_plane: str = "not_applicable",
    camera_id: str | None = None,
    arena_id: str | None = None,
    required: bool = True,
    expected_sha256: str | None = None,
    classification: str = "runtime_critical",
    provenance: Mapping[str, Any] | None = None,
) -> AssetSpec:
    return AssetSpec(
        source_path=Path(source),
        package_relative_path=relative,
        role=role,
        target_plane=target_plane,
        camera_id=camera_id,
        arena_id=arena_id,
        required=required,
        expected_sha256=expected_sha256 or None,
        classification=classification,
        provenance=dict(provenance or {}),
    )


def deduplicate_assets(assets: Iterable[AssetSpec]) -> list[AssetSpec]:
    """Reject conflicting destinations and retain identical repeated specs."""

    by_destination: dict[str, AssetSpec] = {}
    for spec in assets:
        existing = by_destination.get(spec.package_relative_path)
        if existing is None:
            by_destination[spec.package_relative_path] = spec
        elif existing != spec:
            raise CalibrationPackageError(
                f"conflicting assets for {spec.package_relative_path}"
            )
    return list(by_destination.values())
