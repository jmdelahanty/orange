"""Shared validation for schema-v2 recording output descriptors."""

from __future__ import annotations

from pathlib import Path
from typing import Any


def _as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def _path_key(path: Path) -> str:
    return str(path.expanduser().resolve(strict=False))


def _artifact_path(recording_folder: Path, value: Any) -> Path:
    path = Path(str(value or ""))
    return path if path.is_absolute() else recording_folder / path


def _artifact_exists(recording_folder: Path, value: Any) -> bool:
    if not value:
        return False
    return _artifact_path(recording_folder, value).exists()


def _same_artifact_path(recording_folder: Path, left: Any, right: Any) -> bool:
    if not left or not right:
        return False
    return _path_key(_artifact_path(recording_folder, left)) == _path_key(
        _artifact_path(recording_folder, right)
    )


def _append_full_output_errors(
    errors: list[str],
    *,
    recording_folder: Path,
    source_name: str,
    serial: str,
    output: dict[str, Any],
    artifact: dict[str, Any],
) -> None:
    if not output:
        errors.append(f"{source_name} missing recording_outputs.{serial}.full")
        return
    if output.get("output_kind") != "full":
        errors.append(f"{source_name} recording_outputs.{serial}.full output_kind is {output.get('output_kind')!r}")
    if output.get("role") != "ingest_authoritative":
        errors.append(f"{source_name} recording_outputs.{serial}.full role is {output.get('role')!r}")
    if not output.get("backend"):
        errors.append(f"{source_name} recording_outputs.{serial}.full missing backend")
    if output.get("status") not in {"pending", "completed", "incomplete", "disabled", "failed"}:
        errors.append(f"{source_name} recording_outputs.{serial}.full status is {output.get('status')!r}")

    for output_key, artifact_key in (
        ("video", "video"),
        ("metadata", "metadata"),
        ("keyframes", "keyframes"),
    ):
        if artifact and artifact.get(artifact_key):
            if not _same_artifact_path(recording_folder, output.get(output_key), artifact.get(artifact_key)):
                errors.append(
                    f"{source_name} recording_outputs.{serial}.full.{output_key} "
                    f"does not match camera_artifacts.{artifact_key}"
                )
        elif output.get("status") != "disabled" and not output.get(output_key):
            errors.append(f"{source_name} recording_outputs.{serial}.full missing {output_key}")

    for key in ("video", "metadata", "keyframes"):
        value = output.get(key)
        if value and output.get("status") != "disabled":
            path = _artifact_path(recording_folder, value)
            if not path.exists():
                errors.append(f"{source_name} recording_outputs.{serial}.full {key} path missing: {path}")

    for count_key in ("frame_count", "packet_count"):
        if artifact and artifact.get(count_key) is not None and output.get(count_key) is not None:
            try:
                if int(output[count_key]) != int(artifact[count_key]):
                    errors.append(
                        f"{source_name} recording_outputs.{serial}.full {count_key} "
                        f"does not match camera_artifacts"
                    )
            except (TypeError, ValueError):
                errors.append(f"{source_name} recording_outputs.{serial}.full invalid {count_key}")


def _append_crop_output_errors(
    errors: list[str],
    *,
    source_name: str,
    serial: str,
    output: dict[str, Any],
    crop_output: dict[str, Any],
) -> None:
    if output.get("output_kind") != "crop":
        errors.append(f"{source_name} recording_outputs.{serial}.crop output_kind is {output.get('output_kind')!r}")
    if output.get("role") != "sidecar":
        errors.append(f"{source_name} recording_outputs.{serial}.crop role is {output.get('role')!r}")
    if crop_output and crop_output.get("enabled") is True:
        runtime = _as_dict(crop_output.get("runtime"))
        files = _as_dict(runtime.get("files"))
        external_crop_video = output.get("backend") == "external_ipc"
        for key in ("video", "metadata", "keyframes", "perf"):
            if external_crop_video and key in {"video", "keyframes"}:
                continue
            if files.get(key) and output.get(key) != files.get(key):
                errors.append(
                    f"{source_name} recording_outputs.{serial}.crop.{key} "
                    f"does not match crop_outputs runtime files"
                )


def recording_output_contract_errors(
    recording_folder: Path,
    manifest: dict[str, Any],
    snapshot: dict[str, Any],
    cameras: list[str],
) -> list[str]:
    """Return schema-v2 output descriptor contract errors.

    The check is backward-compatible with schema-1 snapshots. When schema v2 or
    either descriptor surface is present, it validates the new contract and its
    compatibility aliases.
    """

    errors: list[str] = []
    try:
        snapshot_schema_version = int(snapshot.get("schema_version", 1))
    except (TypeError, ValueError):
        snapshot_schema_version = 1

    manifest_outputs = _as_dict(manifest.get("recording_outputs"))
    snapshot_outputs = _as_dict(snapshot.get("recording_outputs"))
    schema_v2_required = snapshot_schema_version >= 2
    descriptors_present = bool(manifest_outputs) or bool(snapshot_outputs)

    if not schema_v2_required and not descriptors_present:
        return errors
    if schema_v2_required and not snapshot_outputs:
        errors.append("recording_snapshot schema_version>=2 but recording_outputs is missing")
    if descriptors_present and not manifest_outputs:
        errors.append("recording_session recording_outputs is missing while schema-v2 descriptors are present")

    camera_artifacts = _as_dict(manifest.get("camera_artifacts"))
    crop_outputs = _as_dict(snapshot.get("crop_outputs"))
    encoders = _as_dict(snapshot.get("encoders"))

    for serial in cameras:
        artifact = _as_dict(camera_artifacts.get(serial))
        manifest_camera_outputs = _as_dict(manifest_outputs.get(serial))
        snapshot_camera_outputs = _as_dict(snapshot_outputs.get(serial))

        if manifest_outputs:
            _append_full_output_errors(
                errors,
                recording_folder=recording_folder,
                source_name="recording_session",
                serial=serial,
                output=_as_dict(manifest_camera_outputs.get("full")),
                artifact=artifact,
            )

        if schema_v2_required:
            snapshot_full = _as_dict(snapshot_camera_outputs.get("full"))
            _append_full_output_errors(
                errors,
                recording_folder=recording_folder,
                source_name="recording_snapshot",
                serial=serial,
                output=snapshot_full,
                artifact=artifact,
            )
            if manifest_camera_outputs.get("full") and snapshot_full:
                manifest_full = _as_dict(manifest_camera_outputs.get("full"))
                for key in ("video", "metadata", "keyframes"):
                    if manifest_full.get(key) and not _same_artifact_path(
                        recording_folder,
                        snapshot_full.get(key),
                        manifest_full.get(key),
                    ):
                        errors.append(
                            f"recording_snapshot recording_outputs.{serial}.full.{key} "
                            f"does not match recording_session"
                        )

            encoder_outputs = _as_dict(_as_dict(encoders.get(serial)).get("outputs"))
            if not encoder_outputs:
                errors.append(f"recording_snapshot encoders.{serial}.outputs missing")
            else:
                for output_kind, output in snapshot_camera_outputs.items():
                    if output_kind not in encoder_outputs:
                        errors.append(f"recording_snapshot encoders.{serial}.outputs missing {output_kind}")
                        continue
                    encoder_output = _as_dict(encoder_outputs.get(output_kind))
                    for key in ("role", "backend", "output_kind"):
                        if encoder_output.get(key) != _as_dict(output).get(key):
                            errors.append(
                                f"recording_snapshot encoders.{serial}.outputs.{output_kind}.{key} "
                                f"does not match recording_outputs"
                            )

        for source_name, outputs in (
            ("recording_session", manifest_camera_outputs),
            ("recording_snapshot", snapshot_camera_outputs),
        ):
            crop = _as_dict(outputs.get("crop"))
            if crop:
                _append_crop_output_errors(
                    errors,
                    source_name=source_name,
                    serial=serial,
                    output=crop,
                    crop_output=_as_dict(crop_outputs.get(serial)),
                )

    return errors


def _main_video_serial(path: Path) -> str | None:
    name = path.name
    if not name.startswith("Cam") or not name.endswith(".mp4") or name.endswith("_crop.mp4"):
        return None
    return name[len("Cam") : -len(".mp4")]


def _crop_video_serial(path: Path) -> str | None:
    name = path.name
    if not name.startswith("Cam") or not name.endswith("_crop.mp4"):
        return None
    return name[len("Cam") : -len("_crop.mp4")]


def _path_status(recording_folder: Path, value: Any) -> dict[str, Any]:
    path = _artifact_path(recording_folder, value)
    out: dict[str, Any] = {"path": str(path), "exists": path.exists()}
    if path.exists():
        try:
            out["size_bytes"] = path.stat().st_size
        except OSError:
            pass
    return out


def _summarize_output_descriptor(
    recording_folder: Path,
    descriptor: dict[str, Any],
    source: str,
) -> dict[str, Any]:
    summary: dict[str, Any] = {"source": source}
    for key in (
        "output_kind",
        "role",
        "backend",
        "status",
        "width",
        "height",
        "fps",
        "codec",
        "pixel_source_format",
        "encoded_format",
        "frame_count",
        "packet_count",
        "packet_count_source",
    ):
        if key in descriptor:
            summary[key] = descriptor.get(key)

    paths: dict[str, Any] = {}
    for key in ("video", "metadata", "keyframes", "perf", "summary", "sidecar_perf"):
        if descriptor.get(key):
            summary[key] = descriptor.get(key)
            paths[key] = _path_status(recording_folder, descriptor.get(key))
    if paths:
        summary["paths"] = paths
    return summary


def _camera_artifact_descriptor(
    artifact: dict[str, Any],
    *,
    backend: str | None,
) -> dict[str, Any]:
    descriptor: dict[str, Any] = {
        "output_kind": "full",
        "role": "ingest_authoritative",
    }
    if backend:
        descriptor["backend"] = backend
    for descriptor_key, artifact_key in (
        ("video", "video"),
        ("metadata", "metadata"),
        ("keyframes", "keyframes"),
        ("frame_count", "frame_count"),
        ("packet_count", "packet_count"),
        ("packet_count_source", "packet_count_source"),
    ):
        if artifact.get(artifact_key) is not None:
            descriptor[descriptor_key] = artifact.get(artifact_key)
    return descriptor


def _folder_main_descriptor(recording_folder: Path, serial: str) -> dict[str, Any]:
    descriptor: dict[str, Any] = {
        "output_kind": "full",
        "role": "ingest_authoritative",
        "backend": "in_process",
        "status": "completed",
        "video": f"Cam{serial}.mp4",
    }
    for key, suffix in (
        ("metadata", "_meta.csv"),
        ("keyframes", "_keyframe.json"),
        ("perf", "_pipeline_perf.csv"),
    ):
        candidate = recording_folder / f"Cam{serial}{suffix}"
        if candidate.exists():
            descriptor[key] = candidate.name
    return descriptor


def _crop_output_descriptor(crop_output: dict[str, Any]) -> dict[str, Any]:
    runtime = _as_dict(crop_output.get("runtime"))
    files = _as_dict(runtime.get("files"))
    crop_size_px = runtime.get("crop_size_px")
    descriptor: dict[str, Any] = {
        "output_kind": "crop",
        "role": "sidecar",
        "backend": "in_process",
        "status": "completed" if crop_output.get("enabled") is True else "disabled",
    }
    if isinstance(crop_size_px, int) and crop_size_px > 0:
        descriptor["width"] = crop_size_px
        descriptor["height"] = crop_size_px
    for key in ("video", "metadata", "keyframes", "perf"):
        if files.get(key):
            descriptor[key] = files.get(key)
    return descriptor


def _folder_crop_descriptor(serial: str) -> dict[str, Any]:
    return {
        "output_kind": "crop",
        "role": "sidecar",
        "backend": "in_process",
        "status": "completed",
        "video": f"Cam{serial}_crop.mp4",
        "metadata": f"Cam{serial}_crop_meta.csv",
        "keyframes": f"Cam{serial}_crop_keyframe.json",
        "perf": f"Cam{serial}_crop_perf.csv",
    }


def build_recording_output_summary(
    recording_folder: Path,
    manifest: dict[str, Any],
    snapshot: dict[str, Any],
) -> dict[str, dict[str, Any]]:
    """Return a camera -> output_kind summary with schema-v2 and legacy fallback."""

    output_summary: dict[str, dict[str, Any]] = {}
    manifest_outputs = _as_dict(manifest.get("recording_outputs"))
    snapshot_outputs = _as_dict(snapshot.get("recording_outputs"))
    camera_artifacts = _as_dict(manifest.get("camera_artifacts"))
    crop_outputs = _as_dict(snapshot.get("crop_outputs"))
    recording_backend = _as_dict(manifest.get("recording_backend"))
    backend = recording_backend.get("mode") if isinstance(recording_backend.get("mode"), str) else None

    def set_output(serial: Any, output_kind: str, descriptor: dict[str, Any], source: str) -> None:
        if not descriptor:
            return
        camera = output_summary.setdefault(str(serial), {})
        camera.setdefault(output_kind, _summarize_output_descriptor(recording_folder, descriptor, source))

    for serial, outputs in sorted(manifest_outputs.items()):
        for output_kind, descriptor in sorted(_as_dict(outputs).items()):
            set_output(serial, str(output_kind), _as_dict(descriptor), "recording_session.recording_outputs")

    for serial, outputs in sorted(snapshot_outputs.items()):
        for output_kind, descriptor in sorted(_as_dict(outputs).items()):
            set_output(serial, str(output_kind), _as_dict(descriptor), "recording_snapshot.recording_outputs")

    for serial, artifact in sorted(camera_artifacts.items()):
        set_output(
            serial,
            "full",
            _camera_artifact_descriptor(_as_dict(artifact), backend=backend),
            "recording_session.camera_artifacts",
        )

    for path in sorted(recording_folder.glob("Cam*.mp4")):
        serial = _main_video_serial(path)
        if serial is not None:
            set_output(serial, "full", _folder_main_descriptor(recording_folder, serial), "filename_fallback")

    for serial, crop_output in sorted(crop_outputs.items()):
        set_output(serial, "crop", _crop_output_descriptor(_as_dict(crop_output)), "recording_snapshot.crop_outputs")

    for path in sorted(recording_folder.glob("Cam*_crop.mp4")):
        serial = _crop_video_serial(path)
        if serial is not None:
            set_output(serial, "crop", _folder_crop_descriptor(serial), "filename_fallback")

    for outputs in output_summary.values():
        for output in outputs.values():
            if output.get("status") is None:
                video = output.get("video")
                output["status"] = "completed" if _artifact_exists(recording_folder, video) else "unknown"
    return output_summary


def recording_clip_output_contract_errors(
    recording_folder: Path,
    clip: dict[str, Any],
    cameras: list[str],
    *,
    require_outputs: bool = False,
) -> list[str]:
    errors: list[str] = []
    clip_outputs = _as_dict(clip.get("recording_outputs"))
    if not clip_outputs:
        if require_outputs:
            errors.append(f"clip {clip.get('clip_index')} missing recording_outputs")
        return errors
    camera_artifacts = _as_dict(clip.get("camera_artifacts"))
    for serial in cameras:
        _append_full_output_errors(
            errors,
            recording_folder=recording_folder,
            source_name=f"clip {clip.get('clip_index')}",
            serial=serial,
            output=_as_dict(_as_dict(clip_outputs.get(serial)).get("full")),
            artifact=_as_dict(camera_artifacts.get(serial)),
        )
    return errors
