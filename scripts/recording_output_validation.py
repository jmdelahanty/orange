"""Shared validation for schema-v2 recording output descriptors."""

from __future__ import annotations

from pathlib import Path
from typing import Any

SOURCE_PIXEL_CONTRACT_BY_OUTPUT_KIND = {
    "full": "orange.camera.mono8.full_frame.v1",
    "crop": "orange.crop.mono8.v1",
}

SOURCE_PIXEL_TRANSFORM_BY_OUTPUT_KIND = {
    "full": "mono8_to_nv12",
    "crop": "crop_mono8_to_nv12",
}


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


def normalized_mp4_tags(tags: Any) -> dict[str, str]:
    if not isinstance(tags, dict):
        return {}
    return {
        str(key).lower(): str(value)
        for key, value in tags.items()
        if value is not None
    }


def parse_mp4_comment_fields(comment: Any) -> dict[str, str]:
    if not isinstance(comment, str):
        return {}
    fields: dict[str, str] = {}
    for raw_part in comment.split(";"):
        part = raw_part.strip()
        if not part:
            continue
        if part.startswith("nvenc "):
            part = part[len("nvenc "):].strip()
        for token in part.split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            if key:
                fields[key] = value
    return fields


def _append_expected_field_error(
    errors: list[str],
    fields: dict[str, str],
    *,
    key: str,
    expected: str,
    label: str,
) -> None:
    actual = fields.get(key)
    if actual != expected:
        errors.append(f"{label} MP4 comment {key}={actual!r}, expected {expected!r}")


def mp4_source_pixel_tag_errors(
    tags: Any,
    *,
    output_kind: str,
    label: str,
) -> list[str]:
    errors: list[str] = []
    normalized = normalized_mp4_tags(tags)
    title = normalized.get("title")
    comment = normalized.get("comment")
    if not title:
        errors.append(f"{label} MP4 metadata missing title tag")
    if not comment:
        errors.append(f"{label} MP4 metadata missing comment tag")
        return errors

    fields = parse_mp4_comment_fields(comment)
    expected_contract = SOURCE_PIXEL_CONTRACT_BY_OUTPUT_KIND.get(output_kind)
    expected_transform = SOURCE_PIXEL_TRANSFORM_BY_OUTPUT_KIND.get(output_kind)
    if expected_contract:
        _append_expected_field_error(
            errors,
            fields,
            key="source_pixel_contract",
            expected=expected_contract,
            label=label,
        )
    if expected_transform:
        _append_expected_field_error(
            errors,
            fields,
            key="source_transform_to_encoder",
            expected=expected_transform,
            label=label,
        )
    for key, expected in (
        ("source_pixel_format", "mono8"),
        ("source_pixel_dtype", "uint8"),
        ("source_pixel_range", "0_255"),
        ("source_color_space", "linear_gray"),
        ("source_channel_order", "gray"),
        ("source_memory_layout", "HxW"),
        ("source_coordinate_origin", "top_left"),
        ("encoder_input_format", "nv12"),
        ("encoded_pix_fmt", "yuv420p"),
        ("encoded_color_range", "pc"),
    ):
        _append_expected_field_error(errors, fields, key=key, expected=expected, label=label)

    _append_expected_field_error(errors, fields, key="output_kind", expected=output_kind, label=label)
    for key in ("source_width", "source_height"):
        value = fields.get(key)
        try:
            if int(value or "0") <= 0:
                errors.append(f"{label} MP4 comment {key} must be positive, got {value!r}")
        except ValueError:
            errors.append(f"{label} MP4 comment {key} is not an integer: {value!r}")
    return errors


def video_metadata_contract_errors(
    video_metadata: Any,
    *,
    output_kind: str,
    label: str,
    camera_serial: str | None = None,
    stream_id: str | None = None,
    mp4_tags: Any | None = None,
) -> list[str]:
    errors: list[str] = []
    metadata = _as_dict(video_metadata)
    if not metadata:
        return [f"{label} missing video_metadata"]
    if metadata.get("schema_id") != "orange.video_metadata":
        errors.append(f"{label} video_metadata.schema_id={metadata.get('schema_id')!r}")
    if metadata.get("schema_version") != 1:
        errors.append(f"{label} video_metadata.schema_version={metadata.get('schema_version')!r}")
    if metadata.get("output_kind") != output_kind:
        errors.append(
            f"{label} video_metadata.output_kind={metadata.get('output_kind')!r}, "
            f"expected {output_kind!r}"
        )
    if camera_serial and metadata.get("camera_serial") != camera_serial:
        errors.append(
            f"{label} video_metadata.camera_serial={metadata.get('camera_serial')!r}, "
            f"expected {camera_serial!r}"
        )
    if stream_id and metadata.get("stream_id") != stream_id:
        errors.append(
            f"{label} video_metadata.stream_id={metadata.get('stream_id')!r}, "
            f"expected {stream_id!r}"
        )

    source = _as_dict(metadata.get("source_pixel_contract"))
    expected_contract = SOURCE_PIXEL_CONTRACT_BY_OUTPUT_KIND.get(output_kind)
    expected_transform = SOURCE_PIXEL_TRANSFORM_BY_OUTPUT_KIND.get(output_kind)
    checks = {
        "id": expected_contract,
        "pixel_format": "mono8",
        "dtype": "uint8",
        "value_range": "0_255",
        "color_space": "linear_gray",
        "channel_order": "gray",
        "memory_layout": "HxW",
        "coordinate_origin": "top_left",
        "transform_to_encoder": expected_transform,
        "encoder_input_format": "nv12",
        "encoded_pix_fmt": "yuv420p",
        "encoded_color_range": "pc",
    }
    for key, expected in checks.items():
        if expected is not None and source.get(key) != expected:
            errors.append(
                f"{label} video_metadata.source_pixel_contract.{key}="
                f"{source.get(key)!r}, expected {expected!r}"
            )
    for key in ("width", "height"):
        try:
            if int(source.get(key, 0)) <= 0:
                errors.append(f"{label} video_metadata.source_pixel_contract.{key} must be positive")
        except (TypeError, ValueError):
            errors.append(f"{label} video_metadata.source_pixel_contract.{key} is invalid")

    encoder = _as_dict(metadata.get("encoder"))
    for key in ("name", "codec", "preset", "tuning", "resolved_gop_length", "fps"):
        if encoder.get(key) in (None, ""):
            errors.append(f"{label} video_metadata.encoder.{key} missing")

    expected_tags = _as_dict(metadata.get("mp4_tags_expected"))
    errors.extend(
        mp4_source_pixel_tag_errors(
            expected_tags,
            output_kind=output_kind,
            label=f"{label} video_metadata.mp4_tags_expected",
        )
    )
    if mp4_tags is not None:
        actual_tags = normalized_mp4_tags(mp4_tags)
        for key in ("title", "comment"):
            expected = expected_tags.get(key)
            actual = actual_tags.get(key)
            if expected != actual:
                errors.append(
                    f"{label} MP4 tag {key}={actual!r}, expected video_metadata "
                    f"mp4_tags_expected.{key}={expected!r}"
                )
    embedding = _as_dict(metadata.get("mp4_metadata_embedding"))
    if embedding:
        if embedding.get("attempted") is not True:
            errors.append(f"{label} video_metadata.mp4_metadata_embedding.attempted is not true")
        if embedding.get("succeeded") is not True:
            errors.append(f"{label} video_metadata.mp4_metadata_embedding.succeeded is not true")
    return errors


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


def mp4_key_sample_flag_errors(
    packet_flags: list[str],
    *,
    label: str,
    require_all_key_samples: bool = False,
    expected_packet_count: int | None = None,
) -> list[str]:
    """Return validation errors for ffprobe packet key flags.

    ffprobe's demuxed packet key flag is populated from the MP4 sync-sample
    table for MP4 inputs. Crop videos are encoded as GOP=1, so every packet
    should be a key/sync sample.
    """

    normalized_flags = [str(flag).strip() for flag in packet_flags if str(flag).strip()]
    errors: list[str] = []
    if not normalized_flags:
        errors.append(f"{label} MP4 has no packet key flags from ffprobe")
        return errors

    if expected_packet_count is not None and len(normalized_flags) != expected_packet_count:
        errors.append(
            f"{label} MP4 packet count from ffprobe ({len(normalized_flags)}) "
            f"!= expected packet count ({expected_packet_count})"
        )

    key_flags = ["K" in flag.upper() for flag in normalized_flags]
    if not key_flags[0]:
        errors.append(
            f"{label} MP4 first packet is not a key/sync sample "
            f"(flags={normalized_flags[0]!r})"
        )

    if require_all_key_samples and not all(key_flags):
        first_non_key_indices = [
            index for index, is_key in enumerate(key_flags) if not is_key
        ][:8]
        errors.append(
            f"{label} MP4 expected every packet to be a key/sync sample "
            f"for GOP=1 crop video, but {len(key_flags) - sum(key_flags)} "
            f"of {len(key_flags)} packet(s) were non-key "
            f"(first non-key packet indices: {first_non_key_indices})"
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
