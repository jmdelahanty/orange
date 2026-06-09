#!/usr/bin/env python3
"""Link latest dish-top-rim observations into camera/arena calibration sets."""

from __future__ import annotations

import argparse
import copy
import json
import os
from pathlib import Path
from typing import Any


FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
FNV_MASK = (1 << 64) - 1
FINGERPRINT_ALGORITHM = "fnv1a64"
IMAGE_SET_SCHEMA = "orange.calibration.image_set"
TOP_RIM_SCHEMA = "orange.calibration.dish_top_rim_observation"
MANIFEST_SCHEMA = "orange.calibration.manifest"


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise RuntimeError(f"{path} is not a JSON object")
    return value


def write_json(path: Path, value: dict[str, Any], dry_run: bool) -> None:
    if dry_run:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8") as handle:
        json.dump(value, handle, indent=2, ensure_ascii=False)
        handle.write("\n")
    tmp.replace(path)


def fnv_update(hash_value: int, data: bytes) -> int:
    for byte in data:
        hash_value ^= byte
        hash_value = (hash_value * FNV_PRIME) & FNV_MASK
    return hash_value


def fingerprint_json(value: dict[str, Any]) -> str:
    payload = copy.deepcopy(value)
    if isinstance(payload.get("calibration_ref"), dict):
        payload["calibration_ref"]["fingerprint"] = ""
    encoded = json.dumps(
        payload,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return f"{FINGERPRINT_ALGORITHM}:{fnv_update(FNV_OFFSET, encoded):x}"


def top_rim_fingerprint(observation: dict[str, Any], artifact_dir: Path) -> str:
    payload = copy.deepcopy(observation)
    if isinstance(payload.get("calibration_ref"), dict):
        payload["calibration_ref"]["fingerprint"] = ""
    compatibility_exports = payload.get("compatibility_exports")
    if isinstance(compatibility_exports, dict):
        palette = compatibility_exports.get("palette_dish_mask_v2")
        if isinstance(palette, dict):
            palette["available"] = False

    encoded = json.dumps(
        payload,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    hash_value = fnv_update(FNV_OFFSET, encoded)

    artifacts = observation.get("artifacts", {})
    paths = [
        artifacts.get("source_frame_path", "captures/source_frame.png"),
        artifacts.get("review_overlay_path", "overlays/top_rim_fit.png"),
        artifacts.get("valid_detection_overlay_path", "overlays/valid_detection_region.png"),
    ]
    for rel_path in paths:
        file_path = artifact_dir / str(rel_path)
        hash_value = fnv_update(hash_value, file_path.read_bytes())
    return f"{FINGERPRINT_ALGORITHM}:{hash_value:x}"


def latest_by_camera(rim_artifacts: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    latest: dict[str, dict[str, Any]] = {}
    for rim in rim_artifacts:
        camera_serial = rim["camera_serial"]
        current = latest.get(camera_serial)
        if current is None or rim["created_utc"] > current["created_utc"]:
            latest[camera_serial] = rim
    return latest


def relative_path(path: Path, start: Path) -> str:
    return Path(os.path.relpath(path, start)).as_posix()


def make_arena_context(
    camera_serial: str,
    aggregate_id: str,
    aggregate_image_set: dict[str, Any],
) -> dict[str, Any]:
    rig_context = aggregate_image_set.get("rig_context", {})
    if not isinstance(rig_context, dict):
        rig_context = {}
    arena_context: dict[str, Any] = {
        "camera_serial": camera_serial,
        "associated_image_set_artifact_id": aggregate_id,
    }
    for src_key, dst_key in [
        ("rig_id", "rig_id"),
        ("canvas_id", "canvas_id"),
        ("arena_id", "arena_id"),
        ("camera_id", "citrus_camera_id"),
        ("citrus_config_ref", "citrus_config_ref"),
        ("citrus_homography_ref", "citrus_homography_ref"),
    ]:
        value = rig_context.get(src_key)
        if value not in (None, "", {}):
            arena_context[dst_key] = value
    return arena_context


def make_top_rim_link(
    rim: dict[str, Any],
    arena_context: dict[str, Any],
    aggregate_dir: Path,
) -> dict[str, Any]:
    observation = rim["observation"]
    manifest_path = rim["dir"] / "manifest.json"
    observation_path = rim["dir"] / "observation.json"
    link: dict[str, Any] = {
        "artifact_id": rim["id"],
        "artifact_schema_id": TOP_RIM_SCHEMA,
        "artifact_schema_version": observation.get("schema_version", 1),
        "fingerprint": observation.get("calibration_ref", {}).get("fingerprint", ""),
        "relative_manifest_path": relative_path(manifest_path, aggregate_dir),
        "relative_observation_path": relative_path(observation_path, aggregate_dir),
        "selection_policy": "latest_saved_for_camera_arena",
        "target_plane": "dish_top_rim",
        "coordinate_space": "camera_native_pixels",
        "camera_serial": rim["camera_serial"],
        "accepted_at_utc": observation.get("created_utc", rim["created_utc"]),
        "arena_context": arena_context,
        "accepted_mask": observation.get("accepted_mask", {}),
        "observed_boundary": observation.get("observed_boundary", {}),
        "valid_detection_region": observation.get("valid_detection_region", {}),
    }
    for key in ["arena_id", "canvas_id"]:
        if arena_context.get(key):
            link[key] = arena_context[key]
    return link


def discover_artifacts(session_dir: Path) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    artifacts_dir = session_dir / "artifacts"
    aggregates: list[dict[str, Any]] = []
    rims: list[dict[str, Any]] = []
    for manifest_path in sorted(artifacts_dir.glob("*/manifest.json")):
        artifact_dir = manifest_path.parent
        manifest = load_json(manifest_path)
        schema = manifest.get("artifact_schema_id")
        artifact_id = manifest.get("artifact_id", artifact_dir.name)
        if schema == IMAGE_SET_SCHEMA:
            image_set_path = artifact_dir / "image_set.json"
            if not image_set_path.exists():
                continue
            image_set = load_json(image_set_path)
            if image_set.get("purpose") != "camera_arena_calibration_set":
                continue
            camera_serial = (
                image_set.get("camera", {}).get("serial")
                or manifest.get("summary", {}).get("camera_serial")
            )
            if camera_serial:
                aggregates.append({
                    "id": artifact_id,
                    "dir": artifact_dir,
                    "manifest": manifest,
                    "image_set": image_set,
                    "camera_serial": str(camera_serial),
                })
        elif schema == TOP_RIM_SCHEMA:
            observation_path = artifact_dir / "observation.json"
            if not observation_path.exists():
                continue
            observation = load_json(observation_path)
            camera_serial = (
                observation.get("camera", {}).get("serial")
                or manifest.get("compatibility", {}).get("camera_serial")
                or manifest.get("summary", {}).get("camera_serial")
            )
            if camera_serial:
                rims.append({
                    "id": artifact_id,
                    "dir": artifact_dir,
                    "manifest": manifest,
                    "observation": observation,
                    "camera_serial": str(camera_serial),
                    "created_utc": str(observation.get("created_utc", manifest.get("created_utc", ""))),
                })
    return aggregates, rims


def update_top_rim_artifact(
    rim: dict[str, Any],
    arena_context: dict[str, Any],
    dry_run: bool,
) -> None:
    artifact_dir: Path = rim["dir"]
    observation = rim["observation"]
    manifest = rim["manifest"]
    image_set_path = artifact_dir / "image_set.json"
    image_set = load_json(image_set_path) if image_set_path.exists() else {}

    observation["arena_context"] = arena_context
    fingerprint = top_rim_fingerprint(observation, artifact_dir)
    observation.setdefault("calibration_ref", {})["fingerprint"] = fingerprint

    manifest.setdefault("compatibility", {})["arena_context"] = arena_context
    summary = manifest.setdefault("summary", {})
    summary["camera_serial"] = rim["camera_serial"]
    summary["physical_target"] = "dish_top_rim"
    summary["coordinate_space"] = "camera_native_pixels"
    summary["arena_context"] = arena_context
    if arena_context.get("arena_id"):
        summary["arena_id"] = arena_context["arena_id"]
    if arena_context.get("canvas_id"):
        summary["canvas_id"] = arena_context["canvas_id"]
    summary["associated_image_set_artifact_id"] = arena_context["associated_image_set_artifact_id"]
    manifest.setdefault("calibration_ref", {})["fingerprint"] = fingerprint

    if image_set:
        rig_context = image_set.setdefault("rig_context", {})
        if isinstance(rig_context, dict):
            rig_context["arena_context"] = arena_context
        observations = image_set.setdefault("observations", {})
        if isinstance(observations, dict):
            observations["arena_context"] = arena_context
        for ref in image_set.get("derived_artifacts", []):
            if isinstance(ref, dict) and ref.get("artifact_id") == rim["id"]:
                ref["fingerprint"] = fingerprint

    palette_path = artifact_dir / "exports" / "palette_dish_mask_v2.json"
    if palette_path.exists():
        palette = load_json(palette_path)
        palette["orange_artifact_fingerprint"] = fingerprint
        write_json(palette_path, palette, dry_run)

    spatial_path = artifact_dir / "exports" / "spatial_dish_mask_runtime_v1.json"
    if spatial_path.exists():
        spatial = load_json(spatial_path)
        spatial.setdefault("source_observation", {})["fingerprint"] = fingerprint
        write_json(spatial_path, spatial, dry_run)

    write_json(artifact_dir / "observation.json", observation, dry_run)
    write_json(artifact_dir / "manifest.json", manifest, dry_run)
    if image_set:
        write_json(image_set_path, image_set, dry_run)

    rim["observation"] = observation
    rim["manifest"] = manifest


def link_aggregate(
    session_dir: Path,
    aggregate: dict[str, Any],
    rim: dict[str, Any],
    arena_context: dict[str, Any],
    dry_run: bool,
) -> None:
    aggregate_dir: Path = aggregate["dir"]
    image_set = aggregate["image_set"]
    manifest = aggregate["manifest"]
    link = make_top_rim_link(rim, arena_context, aggregate_dir)

    image_set.setdefault("linked_observations", {})["accepted_top_rim_observation"] = link
    image_set["updated_utc"] = rim["created_utc"]
    aggregate_fingerprint = fingerprint_json(image_set)

    manifest.setdefault("linked_observations", {})["accepted_top_rim_observation"] = link
    summary = manifest.setdefault("summary", {})
    summary["accepted_top_rim_observation_artifact_id"] = rim["id"]
    summary["accepted_top_rim_observation_fingerprint"] = (
        rim["observation"].get("calibration_ref", {}).get("fingerprint", "")
    )
    summary["accepted_top_rim_observation_created_utc"] = rim["created_utc"]
    manifest["updated_utc"] = rim["created_utc"]
    manifest.setdefault("calibration_ref", {})["fingerprint"] = aggregate_fingerprint

    write_json(aggregate_dir / "image_set.json", image_set, dry_run)
    write_json(aggregate_dir / "manifest.json", manifest, dry_run)
    aggregate["image_set"] = image_set
    aggregate["manifest"] = manifest


def rebuild_session_index(
    session_dir: Path,
    aggregates: list[dict[str, Any]],
    rim_latest: dict[str, dict[str, Any]],
    dry_run: bool,
) -> None:
    artifacts_dir = session_dir / "artifacts"
    index_path = session_dir / "session_index.json"
    old_index = load_json(index_path) if index_path.exists() else {}
    index: dict[str, Any] = {
        "schema_id": "orange.calibration.session_index",
        "schema_version": 1,
        "session_id": session_dir.name,
        "session_dir": str(session_dir),
        "artifacts_dir": str(artifacts_dir),
        "updated_utc": old_index.get("updated_utc", ""),
        "artifact_order": old_index.get("artifact_order", []),
        "artifacts_by_id": {},
    }

    for manifest_path in sorted(artifacts_dir.glob("*/manifest.json")):
        manifest = load_json(manifest_path)
        artifact_id = manifest.get("artifact_id", manifest_path.parent.name)
        rel_manifest = manifest_path.relative_to(session_dir).as_posix()
        entry = {
            "artifact_id": artifact_id,
            "artifact_schema_id": manifest.get("artifact_schema_id", ""),
            "artifact_schema_version": manifest.get("artifact_schema_version", 0),
            "created_utc": manifest.get("created_utc", ""),
            "relative_manifest_path": rel_manifest,
            "fingerprint": manifest.get("calibration_ref", {}).get("fingerprint", ""),
        }
        for key in ["summary", "producer"]:
            if key in manifest:
                entry[key] = manifest[key]
        index["artifacts_by_id"][artifact_id] = entry
        if artifact_id not in index["artifact_order"]:
            index["artifact_order"].append(artifact_id)

    by_camera: dict[str, str] = {}
    by_arena: dict[str, str] = {}
    aggregate_by_camera = {aggregate["camera_serial"]: aggregate for aggregate in aggregates}
    for camera_serial, rim in rim_latest.items():
        by_camera[camera_serial] = rim["id"]
        aggregate = aggregate_by_camera.get(camera_serial)
        if aggregate is not None:
            by_arena[aggregate["id"]] = rim["id"]
    if by_camera:
        index["latest_top_rim_observation_by_camera_serial"] = by_camera
    if by_arena:
        index["latest_top_rim_observation_by_arena_artifact_id"] = by_arena
    index["artifact_count"] = len(index["artifacts_by_id"])
    if not index["updated_utc"]:
        latest = [rim["created_utc"] for rim in rim_latest.values() if rim["created_utc"]]
        index["updated_utc"] = max(latest) if latest else ""
    write_json(index_path, index, dry_run)


def migrate_session(session_dir: Path, dry_run: bool) -> int:
    aggregates, rims = discover_artifacts(session_dir)
    aggregate_by_camera = {aggregate["camera_serial"]: aggregate for aggregate in aggregates}
    latest_rims = latest_by_camera(rims)
    migrated = 0
    for camera_serial, rim in sorted(latest_rims.items()):
        aggregate = aggregate_by_camera.get(camera_serial)
        if aggregate is None:
            print(f"[skip] no camera/arena aggregate for camera {camera_serial}")
            continue
        arena_context = make_arena_context(camera_serial, aggregate["id"], aggregate["image_set"])
        update_top_rim_artifact(rim, arena_context, dry_run)
        link_aggregate(session_dir, aggregate, rim, arena_context, dry_run)
        print(f"[link] {aggregate['id']} -> {rim['id']}")
        migrated += 1
    rebuild_session_index(session_dir, aggregates, latest_rims, dry_run)
    return migrated


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session_dir", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    session_dir = args.session_dir.resolve()
    if not (session_dir / "artifacts").is_dir():
        raise SystemExit(f"not a calibration session directory: {session_dir}")
    count = migrate_session(session_dir, args.dry_run)
    mode = "would migrate" if args.dry_run else "migrated"
    print(f"{mode} {count} camera/arena top-rim link(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
