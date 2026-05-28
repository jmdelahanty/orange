#!/usr/bin/env python3
"""Compare GUI crop-preview validation JSON summaries."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


def nonnegative_float(value: str) -> float:
    try:
        number = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a number") from exc
    if number < 0.0:
        raise argparse.ArgumentTypeError("must be >= 0")
    return number


def nonnegative_int(value: str) -> int:
    try:
        number = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be an integer") from exc
    if number < 0:
        raise argparse.ArgumentTypeError("must be >= 0")
    return number


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare one or more JSON summaries produced by "
            "scripts/validate_gui_ptp_recording.py --json-out."
        )
    )
    parser.add_argument(
        "validation_json",
        nargs="+",
        metavar="LABEL=PATH",
        help=(
            "Validation JSON file. Prefix with LABEL= to choose the displayed "
            "run label; otherwise the file stem is used."
        ),
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print only machine-readable comparison JSON.",
    )
    parser.add_argument(
        "--require-pass",
        action="store_true",
        help="Exit nonzero if any compared validation summary has status != pass.",
    )
    parser.add_argument(
        "--require-zero-crop-drops",
        action="store_true",
        help="Exit nonzero if any compared run reports crop dropped rows.",
    )
    parser.add_argument(
        "--min-gui-overall-p05-fps",
        type=nonnegative_float,
        help="Exit nonzero if any compared run reports overall GUI p05 FPS below this value.",
    )
    parser.add_argument(
        "--min-gui-visible-p05-fps",
        type=nonnegative_float,
        help="Exit nonzero if any compared run reports visible crop-preview GUI p05 FPS below this value.",
    )
    parser.add_argument(
        "--min-gui-hidden-p05-fps",
        type=nonnegative_float,
        help="Exit nonzero if any compared run reports hidden crop-preview GUI p05 FPS below this value.",
    )
    parser.add_argument(
        "--require-visible-samples",
        action="store_true",
        help="Exit nonzero unless at least one compared run has visible crop-preview GUI FPS samples.",
    )
    parser.add_argument(
        "--require-hidden-samples",
        action="store_true",
        help="Exit nonzero unless at least one compared run has hidden crop-preview GUI FPS samples.",
    )
    parser.add_argument(
        "--require-matching-cameras",
        action="store_true",
        help="Exit nonzero unless all compared runs have the same camera set.",
    )
    parser.add_argument(
        "--require-matching-display-config",
        action="store_true",
        help=(
            "Exit nonzero unless all compared runs have the same stream downsample, "
            "display preview FPS cap, and YOLO speed-graph setting."
        ),
    )
    parser.add_argument(
        "--require-matching-crop-config",
        action="store_true",
        help=(
            "Exit nonzero unless all compared runs have the same crop backend, "
            "external crop queue depth, external crop GPU placement, preview max FPS, "
            "preview-disabled setting, and crop frame pool size."
        ),
    )
    parser.add_argument(
        "--require-matching-yolo-runtime-config",
        action="store_true",
        help=(
            "Exit nonzero unless all compared runs have the same per-camera "
            "YOLO requested/effective affinity mapping, recorded isolated CPU set, "
            "and recorded kernel CPU-list boot options."
        ),
    )
    parser.add_argument(
        "--require-external-crop-recorder-gpu-separate-from-analytics",
        action="store_true",
        help=(
            "Exit nonzero if any external crop stream is missing GPU placement "
            "metadata or reports recorder_gpu_id equal to analytics_gpu_id."
        ),
    )
    parser.add_argument(
        "--require-external-recorder-status",
        action="store_true",
        help=(
            "Exit nonzero if external recorder status summaries are missing or "
            "any full-frame/crop recorder stream is not completed with a valid "
            "parsed runtime heartbeat."
        ),
    )
    parser.add_argument(
        "--max-external-crop-queue-high-water",
        type=nonnegative_int,
        help="Exit nonzero if any compared run exceeds this external crop encode queue high-water.",
    )
    parser.add_argument(
        "--max-external-crop-enqueue-age-p95-ms",
        type=nonnegative_float,
        help="Exit nonzero if any compared run exceeds this external crop enqueue-age p95.",
    )
    return parser.parse_args()


def split_labeled_path(value: str) -> tuple[str, Path]:
    if "=" in value:
        label, raw_path = value.split("=", 1)
        label = label.strip()
        if label:
            return label, Path(raw_path)
    path = Path(value)
    return path.stem, path


def load_validation_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise ValueError(f"{path} did not contain a JSON object")
    return payload


def finite_float(value: Any) -> float | None:
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def finite_int(value: Any) -> int:
    if value is None:
        return 0
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def finite_optional_int(value: Any) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def parse_kernel_cpu_option_value(value: Any) -> tuple[list[int], list[str], list[str]]:
    if value is None:
        return [], [], []
    cpus: set[int] = set()
    flags: set[str] = set()
    invalid: set[str] = set()
    for raw_token in str(value).split(","):
        token = raw_token.strip()
        if not token:
            continue
        if token.isdigit():
            cpus.add(int(token))
            continue
        if "-" in token:
            first_text, last_text = token.split("-", 1)
            if first_text.isdigit() and last_text.isdigit():
                first = int(first_text)
                last = int(last_text)
                if first <= last:
                    cpus.update(range(first, last + 1))
                else:
                    invalid.add(token)
                continue
        if token[0].isdigit():
            invalid.add(token)
        else:
            flags.add(token)
    return sorted(cpus), sorted(flags), sorted(invalid)


def compact_cpu_list(cpus: list[int]) -> str:
    if not cpus:
        return ""
    ranges: list[str] = []
    start = cpus[0]
    previous = cpus[0]
    for cpu in cpus[1:]:
        if cpu == previous + 1:
            previous = cpu
            continue
        ranges.append(f"{start}-{previous}" if start != previous else str(start))
        start = previous = cpu
    ranges.append(f"{start}-{previous}" if start != previous else str(start))
    return ",".join(ranges)


def normalized_kernel_cpu_option(option: str, value: Any) -> str | None:
    if value is None or not str(value).strip():
        return None
    cpus, flags, invalid = parse_kernel_cpu_option_value(value)
    parts = [f"cpus:{compact_cpu_list(cpus) or '<none>'}"]
    if flags:
        parts.append(f"flags:{'|'.join(flags)}")
    if invalid:
        parts.append(f"invalid:{'|'.join(invalid)}")
    return f"{option}={';'.join(parts)}"


def mean(values: list[float]) -> float | None:
    if not values:
        return None
    return sum(values) / len(values)


def max_or_none(values: list[float]) -> float | None:
    return max(values) if values else None


def nested_dict(payload: dict[str, Any], *keys: str) -> dict[str, Any]:
    item: Any = payload
    for key in keys:
        if not isinstance(item, dict):
            return {}
        item = item.get(key)
    return item if isinstance(item, dict) else {}


def nested_float(payload: dict[str, Any], *keys: str) -> float | None:
    item: Any = payload
    for key in keys:
        if not isinstance(item, dict):
            return None
        item = item.get(key)
    return finite_float(item)


def iter_external_recorder_status(payload: dict[str, Any]) -> list[tuple[str, str, dict[str, Any]]]:
    status_root = payload.get("external_recorder_status")
    status_root = status_root if isinstance(status_root, dict) else {}
    entries: list[tuple[str, str, dict[str, Any]]] = []
    for group_name, streams in status_root.items():
        if not isinstance(group_name, str) or not isinstance(streams, dict):
            continue
        for stream_name, status in streams.items():
            if isinstance(status, dict):
                entries.append((group_name, str(stream_name), status))
    return entries


GUI_TIMING_BREAKDOWN_BUCKETS = [
    ("main_texture_upload_ms", "main-texture-upload"),
    ("crop_texture_upload_ms", "crop-texture-upload"),
    ("camera_window_draw_ms", "camera-window-draw"),
    ("crop_window_draw_ms", "crop-window-draw"),
    ("speed_graph_draw_ms", "speed-graph-draw"),
    ("render_present_ms", "render-present"),
]


def gui_timing_diagnosis(gui_fps: dict[str, Any]) -> dict[str, Any]:
    diagnosis = nested_dict(gui_fps, "timing_diagnosis")
    if diagnosis:
        return diagnosis

    frame_total_p95_ms = nested_float(gui_fps, "timings", "frame_total_ms", "p95_ms")
    ranked: list[dict[str, Any]] = []
    for bucket_name, label in GUI_TIMING_BREAKDOWN_BUCKETS:
        p95_ms = nested_float(gui_fps, "timings", bucket_name, "p95_ms")
        if p95_ms is None:
            continue
        ranked.append({"bucket": bucket_name, "label": label, "p95_ms": p95_ms})
    if not ranked:
        return {}

    ranked.sort(key=lambda item: item["p95_ms"], reverse=True)
    dominant = ranked[0]
    out: dict[str, Any] = {
        "dominant_timing_bucket": dominant["bucket"],
        "dominant_timing_label": dominant["label"],
        "dominant_timing_p95_ms": dominant["p95_ms"],
        "timing_p95_ranked": ranked,
    }
    if frame_total_p95_ms is not None:
        out["frame_total_p95_ms"] = frame_total_p95_ms
        if frame_total_p95_ms > 0.0:
            out["dominant_timing_fraction_of_frame_total_p95"] = (
                dominant["p95_ms"] / frame_total_p95_ms
            )
    return out


def summarize_validation(label: str, payload: dict[str, Any]) -> dict[str, Any]:
    camera_summary = payload.get("summary")
    camera_summary = camera_summary if isinstance(camera_summary, dict) else {}
    crop_preview = payload.get("crop_preview")
    crop_preview = crop_preview if isinstance(crop_preview, dict) else {}
    crop_recording = payload.get("crop_recording")
    crop_recording = crop_recording if isinstance(crop_recording, dict) else {}
    system_cpu = payload.get("system_cpu")
    system_cpu = system_cpu if isinstance(system_cpu, dict) else {}
    isolated_cpus = nested_dict(system_cpu, "isolated_cpus").get("cpus")
    isolated_cpu_values = (
        sorted(finite_int(cpu) for cpu in isolated_cpus)
        if isinstance(isolated_cpus, list)
        else []
    )
    provided_kernel_options = payload.get("system_cpu_kernel_cmdline_cpu_option_values")
    if isinstance(provided_kernel_options, list):
        kernel_cmdline_cpu_option_values = [
            str(value) for value in provided_kernel_options if str(value).strip()
        ]
    else:
        kernel_options = nested_dict(system_cpu, "kernel_cmdline", "options")
        kernel_cmdline_cpu_option_values = []
        for option in ("isolcpus", "nohz_full", "rcu_nocbs"):
            normalized = normalized_kernel_cpu_option(option, kernel_options.get(option))
            if normalized:
                kernel_cmdline_cpu_option_values.append(normalized)

    cameras = sorted(
        set(str(serial) for serial in camera_summary)
        | set(str(serial) for serial in crop_preview)
        | set(str(serial) for serial in crop_recording)
    )

    detect_p95_values = [
        value
        for item in camera_summary.values()
        for value in [finite_float(item.get("detect_steady_p95_ms")) if isinstance(item, dict) else None]
        if value is not None
    ]
    queue_p95_values = [
        value
        for item in camera_summary.values()
        for value in [finite_float(item.get("queue_p95_ms")) if isinstance(item, dict) else None]
        if value is not None
    ]
    queue_steady_p95_values = [
        value
        for item in camera_summary.values()
        for value in [finite_float(item.get("queue_steady_p95_ms")) if isinstance(item, dict) else None]
        if value is not None
    ]
    acq_worker_p95_values = [
        value
        for item in camera_summary.values()
        for value in [
            finite_float(item.get("acquisition_to_worker_start_p95_ms"))
            if isinstance(item, dict) else None
        ]
        if value is not None
    ]
    acq_worker_steady_p95_values = [
        value
        for item in camera_summary.values()
        for value in [
            finite_float(item.get("acquisition_to_worker_start_steady_p95_ms"))
            if isinstance(item, dict) else None
        ]
        if value is not None
    ]
    enqueue_dequeue_p95_values = [
        value
        for item in camera_summary.values()
        for value in [
            finite_float(item.get("yolo_enqueue_to_dequeue_p95_ms"))
            if isinstance(item, dict) else None
        ]
        if value is not None
    ]
    enqueue_dequeue_steady_p95_values = [
        value
        for item in camera_summary.values()
        for value in [
            finite_float(item.get("yolo_enqueue_to_dequeue_steady_p95_ms"))
            if isinstance(item, dict) else None
        ]
        if value is not None
    ]
    dequeue_worker_p95_values = [
        value
        for item in camera_summary.values()
        for value in [
            finite_float(item.get("yolo_dequeue_to_worker_start_p95_ms"))
            if isinstance(item, dict) else None
        ]
        if value is not None
    ]
    dequeue_worker_steady_p95_values = [
        value
        for item in camera_summary.values()
        for value in [
            finite_float(item.get("yolo_dequeue_to_worker_start_steady_p95_ms"))
            if isinstance(item, dict) else None
        ]
        if value is not None
    ]
    service_gap_p95_values = [
        value
        for item in camera_summary.values()
        for value in [
            finite_float(item.get("same_camera_service_gap_p95_ms"))
            if isinstance(item, dict) else None
        ]
        if value is not None
    ]
    service_gap_steady_p95_values = [
        value
        for item in camera_summary.values()
        for value in [
            finite_float(item.get("same_camera_service_gap_steady_p95_ms"))
            if isinstance(item, dict) else None
        ]
        if value is not None
    ]
    yolo_affinity_mapping_values: set[str] = set()
    for serial, item in camera_summary.items():
        if not isinstance(item, dict):
            continue
        affinity = item.get("yolo_affinity")
        if not isinstance(affinity, dict):
            continue
        requested = affinity.get("requested_cpus")
        effective = affinity.get("effective_cpus")
        if requested is None and effective is None:
            continue
        yolo_affinity_mapping_values.add(
            f"{serial}:{requested if requested not in {None, ''} else 'n/a'}"
            f"->{effective if effective not in {None, ''} else 'n/a'}"
        )

    crop_rows_total = 0
    crop_dropped_rows_total = 0
    crop_video_frames_total = 0
    external_crop_dropped_total = 0
    crop_backend_values: set[str] = set()
    external_crop_queue_depth_values: set[int] = set()
    external_crop_queue_high_water_values: list[int] = []
    external_crop_enqueue_age_p95_values: list[float] = []
    external_crop_gpu_mapping_values: set[str] = set()
    external_crop_same_gpu_mapping_values: set[str] = set()
    for serial, item in crop_recording.items():
        if not isinstance(item, dict):
            continue
        backend = item.get("backend")
        if isinstance(backend, str) and backend:
            crop_backend_values.add(backend)
        elif any(
            item.get(field) is not None
            for field in (
                "external_frames_received",
                "external_frames_encoded",
                "external_encode_queue_depth",
                "external_encode_queue_high_water",
            )
        ):
            crop_backend_values.add("external_ipc")
        crop_rows_total += finite_int(item.get("metadata_rows"))
        crop_dropped_rows_total += finite_int(item.get("dropped_rows"))
        crop_video_frames_total += finite_int(item.get("video_frames"))
        external_crop_dropped_total += finite_int(item.get("external_frames_dropped"))
        external_crop_dropped_total += finite_int(item.get("external_encode_dropped"))
        if item.get("external_encode_queue_depth") is not None:
            external_crop_queue_depth_values.add(finite_int(item.get("external_encode_queue_depth")))
        if item.get("external_encode_queue_high_water") is not None:
            external_crop_queue_high_water_values.append(
                finite_int(item.get("external_encode_queue_high_water"))
            )
        enqueue_age = finite_float(item.get("external_enqueue_age_p95_ms"))
        if enqueue_age is not None:
            external_crop_enqueue_age_p95_values.append(enqueue_age)
        analytics_gpu_id = item.get("external_analytics_gpu_id")
        recorder_gpu_id = item.get("external_recorder_gpu_id")
        if analytics_gpu_id is not None and recorder_gpu_id is not None:
            analytics_gpu_id_int = finite_int(analytics_gpu_id)
            recorder_gpu_id_int = finite_int(recorder_gpu_id)
            external_crop_gpu_mapping_values.add(
                f"{serial}:{analytics_gpu_id_int}->{recorder_gpu_id_int}"
            )
            if analytics_gpu_id_int == recorder_gpu_id_int:
                external_crop_same_gpu_mapping_values.add(
                    f"{serial}:{analytics_gpu_id_int}->{recorder_gpu_id_int}"
                )

    external_status_entries = iter_external_recorder_status(payload)
    external_recorder_status_values: set[str] = set()
    external_recorder_status_failed_streams: list[str] = []
    external_recorder_heartbeat_values: list[int] = []
    external_recorder_frames_received_total = 0
    external_recorder_frames_encoded_total = 0
    for group_name, stream_name, status in external_status_entries:
        status_value = status.get("status")
        status_value = status_value if isinstance(status_value, str) else ""
        if status_value:
            external_recorder_status_values.add(status_value)
        heartbeat_sequence = finite_optional_int(status.get("heartbeat_sequence"))
        if heartbeat_sequence is not None:
            external_recorder_heartbeat_values.append(heartbeat_sequence)
        external_recorder_frames_received_total += finite_int(status.get("frames_received"))
        external_recorder_frames_encoded_total += finite_int(status.get("frames_encoded"))
        if (
            status_value != "completed"
            or status.get("runtime_present") is not True
            or status.get("runtime_valid") is not True
        ):
            external_recorder_status_failed_streams.append(f"{group_name}:{stream_name}")

    preview_offered_total = 0
    preview_updated_total = 0
    preview_skipped_total = 0
    preview_clears_total = 0
    crop_metadata_detection_rows_total = 0
    producer_recording_crop_frame_offered_total = 0
    producer_recording_crop_frame_accepted_total = 0
    producer_recording_crop_frame_dropped_total = 0
    producer_preview_crop_frame_offered_total = 0
    producer_preview_crop_frame_accepted_total = 0
    producer_preview_crop_frame_dropped_total = 0
    producer_pose_crop_frame_offered_total = 0
    producer_pose_crop_frame_accepted_total = 0
    producer_pose_crop_frame_dropped_total = 0
    producer_recording_crop_frame_present = False
    producer_preview_crop_frame_present = False
    producer_pose_crop_frame_present = False
    preview_display_enabled_values: set[int] = set()
    preview_disabled_values: set[int] = set()
    preview_max_fps_values: set[int] = set()
    crop_frame_pool_size_values: set[int] = set()
    for item in crop_preview.values():
        if not isinstance(item, dict):
            continue
        preview_offered_total += finite_int(item.get("preview_frames_offered"))
        preview_updated_total += finite_int(item.get("preview_frames_updated"))
        preview_skipped_total += finite_int(item.get("preview_frames_skipped_by_cadence"))
        preview_clears_total += finite_int(item.get("preview_clears_updated"))
        crop_metadata_detection_rows_total += finite_int(item.get("crop_metadata_detection_rows"))
        if item.get("producer_recording_crop_frame_offered") is not None:
            producer_recording_crop_frame_present = True
            producer_recording_crop_frame_offered_total += finite_int(
                item.get("producer_recording_crop_frame_offered")
            )
            producer_recording_crop_frame_accepted_total += finite_int(
                item.get("producer_recording_crop_frame_accepted")
            )
            producer_recording_crop_frame_dropped_total += finite_int(
                item.get("producer_recording_crop_frame_dropped")
            )
        if item.get("producer_preview_crop_frame_offered") is not None:
            producer_preview_crop_frame_present = True
            producer_preview_crop_frame_offered_total += finite_int(
                item.get("producer_preview_crop_frame_offered")
            )
            producer_preview_crop_frame_accepted_total += finite_int(
                item.get("producer_preview_crop_frame_accepted")
            )
            producer_preview_crop_frame_dropped_total += finite_int(
                item.get("producer_preview_crop_frame_dropped")
            )
        if item.get("producer_pose_crop_frame_offered") is not None:
            producer_pose_crop_frame_present = True
            producer_pose_crop_frame_offered_total += finite_int(
                item.get("producer_pose_crop_frame_offered")
            )
            producer_pose_crop_frame_accepted_total += finite_int(
                item.get("producer_pose_crop_frame_accepted")
            )
            producer_pose_crop_frame_dropped_total += finite_int(
                item.get("producer_pose_crop_frame_dropped")
            )
        if item.get("preview_display_enabled_final") is not None:
            preview_display_enabled_values.add(finite_int(item.get("preview_display_enabled_final")))
        if item.get("preview_disabled") is not None:
            preview_disabled_values.add(finite_int(item.get("preview_disabled")))
        if item.get("preview_max_fps") is not None:
            preview_max_fps_values.add(finite_int(item.get("preview_max_fps")))
        if item.get("crop_frame_pool_size") is not None:
            crop_frame_pool_size_values.add(finite_int(item.get("crop_frame_pool_size")))

    gui_fps = payload.get("gui_display_frame_rate")
    gui_fps = gui_fps if isinstance(gui_fps, dict) else {}
    timing_diagnosis = gui_timing_diagnosis(gui_fps)
    source_version = payload.get("source_version")
    source_version = source_version if isinstance(source_version, dict) else {}

    summary = {
        "label": label,
        "status": str(payload.get("status", "unknown")),
        "recording_folder": str(payload.get("recording_folder", "")),
        "producer_version": payload.get("producer_version"),
        "source_branch": source_version.get("branch"),
        "source_commit_short": source_version.get("commit_short"),
        "source_dirty_tracked": source_version.get("dirty_tracked"),
        "isolated_cpu_values": isolated_cpu_values,
        "kernel_cmdline_cpu_option_values": kernel_cmdline_cpu_option_values,
        "yolo_affinity_mapping_values": sorted(yolo_affinity_mapping_values),
        "camera_count": len(cameras),
        "cameras": cameras,
        "failures": len(payload.get("failures", []) if isinstance(payload.get("failures"), list) else []),
        "warnings": len(payload.get("warnings", []) if isinstance(payload.get("warnings"), list) else []),
        "detect_steady_p95_avg_ms": mean(detect_p95_values),
        "detect_steady_p95_max_ms": max_or_none(detect_p95_values),
        "queue_p95_avg_ms": mean(queue_p95_values),
        "queue_p95_max_ms": max_or_none(queue_p95_values),
        "queue_steady_p95_avg_ms": mean(queue_steady_p95_values),
        "queue_steady_p95_max_ms": max_or_none(queue_steady_p95_values),
        "acq_worker_p95_avg_ms": mean(acq_worker_p95_values),
        "acq_worker_p95_max_ms": max_or_none(acq_worker_p95_values),
        "acq_worker_steady_p95_avg_ms": mean(acq_worker_steady_p95_values),
        "acq_worker_steady_p95_max_ms": max_or_none(acq_worker_steady_p95_values),
        "enqueue_dequeue_p95_avg_ms": mean(enqueue_dequeue_p95_values),
        "enqueue_dequeue_p95_max_ms": max_or_none(enqueue_dequeue_p95_values),
        "enqueue_dequeue_steady_p95_avg_ms": mean(enqueue_dequeue_steady_p95_values),
        "enqueue_dequeue_steady_p95_max_ms": max_or_none(enqueue_dequeue_steady_p95_values),
        "dequeue_worker_p95_avg_ms": mean(dequeue_worker_p95_values),
        "dequeue_worker_p95_max_ms": max_or_none(dequeue_worker_p95_values),
        "dequeue_worker_steady_p95_avg_ms": mean(dequeue_worker_steady_p95_values),
        "dequeue_worker_steady_p95_max_ms": max_or_none(dequeue_worker_steady_p95_values),
        "service_gap_p95_avg_ms": mean(service_gap_p95_values),
        "service_gap_p95_max_ms": max_or_none(service_gap_p95_values),
        "service_gap_steady_p95_avg_ms": mean(service_gap_steady_p95_values),
        "service_gap_steady_p95_max_ms": max_or_none(service_gap_steady_p95_values),
        "crop_rows_total": crop_rows_total,
        "crop_dropped_rows_total": crop_dropped_rows_total,
        "crop_video_frames_total": crop_video_frames_total,
        "external_crop_dropped_total": external_crop_dropped_total,
        "crop_backend_values": sorted(crop_backend_values),
        "external_crop_queue_depth_values": sorted(external_crop_queue_depth_values),
        "external_crop_gpu_mapping_values": sorted(external_crop_gpu_mapping_values),
        "external_crop_same_gpu_mapping_values": sorted(external_crop_same_gpu_mapping_values),
        "external_crop_queue_high_water_max": max_or_none(external_crop_queue_high_water_values),
        "external_crop_enqueue_age_p95_max_ms": max_or_none(external_crop_enqueue_age_p95_values),
        "external_recorder_status_streams_total": len(external_status_entries),
        "external_recorder_status_values": sorted(external_recorder_status_values),
        "external_recorder_status_failed_streams": external_recorder_status_failed_streams,
        "external_recorder_heartbeat_min": (
            min(external_recorder_heartbeat_values)
            if external_recorder_heartbeat_values else None
        ),
        "external_recorder_heartbeat_max": (
            max(external_recorder_heartbeat_values)
            if external_recorder_heartbeat_values else None
        ),
        "external_recorder_frames_received_total": (
            external_recorder_frames_received_total
            if external_status_entries else None
        ),
        "external_recorder_frames_encoded_total": (
            external_recorder_frames_encoded_total
            if external_status_entries else None
        ),
        "preview_offered_total": preview_offered_total,
        "preview_updated_total": preview_updated_total,
        "preview_skipped_total": preview_skipped_total,
        "preview_clears_total": preview_clears_total,
        "crop_metadata_detection_rows_total": crop_metadata_detection_rows_total,
        "producer_recording_crop_frame_offered_total": (
            producer_recording_crop_frame_offered_total
            if producer_recording_crop_frame_present else None
        ),
        "producer_recording_crop_frame_accepted_total": (
            producer_recording_crop_frame_accepted_total
            if producer_recording_crop_frame_present else None
        ),
        "producer_recording_crop_frame_dropped_total": (
            producer_recording_crop_frame_dropped_total
            if producer_recording_crop_frame_present else None
        ),
        "producer_preview_crop_frame_offered_total": (
            producer_preview_crop_frame_offered_total
            if producer_preview_crop_frame_present else None
        ),
        "producer_preview_crop_frame_accepted_total": (
            producer_preview_crop_frame_accepted_total
            if producer_preview_crop_frame_present else None
        ),
        "producer_preview_crop_frame_dropped_total": (
            producer_preview_crop_frame_dropped_total
            if producer_preview_crop_frame_present else None
        ),
        "producer_pose_crop_frame_offered_total": (
            producer_pose_crop_frame_offered_total
            if producer_pose_crop_frame_present else None
        ),
        "producer_pose_crop_frame_accepted_total": (
            producer_pose_crop_frame_accepted_total
            if producer_pose_crop_frame_present else None
        ),
        "producer_pose_crop_frame_dropped_total": (
            producer_pose_crop_frame_dropped_total
            if producer_pose_crop_frame_present else None
        ),
        "preview_update_fraction": (
            preview_updated_total / preview_offered_total if preview_offered_total > 0 else None
        ),
        "preview_display_enabled_values": sorted(preview_display_enabled_values),
        "preview_disabled_values": sorted(preview_disabled_values),
        "preview_max_fps_values": sorted(preview_max_fps_values),
        "crop_frame_pool_size_values": sorted(crop_frame_pool_size_values),
        "gui_overall_p05_fps": nested_float(gui_fps, "overall", "p05_fps"),
        "gui_visible_p05_fps": nested_float(gui_fps, "crop_preview_visible", "p05_fps"),
        "gui_hidden_p05_fps": nested_float(gui_fps, "crop_preview_hidden", "p05_fps"),
        "gui_stream_downsample": finite_int(gui_fps.get("stream_downsample")),
        "display_preview_max_fps": finite_int(gui_fps.get("display_preview_max_fps")),
        "gui_swap_interval": finite_int(gui_fps.get("swap_interval")),
        "gui_frame_max_fps": finite_int(gui_fps.get("frame_max_fps")),
        "yolo_speed_graphs_enabled": finite_int(gui_fps.get("yolo_speed_graphs_enabled")),
        "gui_frame_total_p95_ms": nested_float(gui_fps, "timings", "frame_total_ms", "p95_ms"),
        "gui_main_texture_upload_p95_ms": nested_float(
            gui_fps,
            "timings",
            "main_texture_upload_ms",
            "p95_ms",
        ),
        "gui_crop_texture_upload_p95_ms": nested_float(
            gui_fps,
            "timings",
            "crop_texture_upload_ms",
            "p95_ms",
        ),
        "gui_camera_window_draw_p95_ms": nested_float(
            gui_fps,
            "timings",
            "camera_window_draw_ms",
            "p95_ms",
        ),
        "gui_crop_window_draw_p95_ms": nested_float(
            gui_fps,
            "timings",
            "crop_window_draw_ms",
            "p95_ms",
        ),
        "gui_speed_graph_draw_p95_ms": nested_float(
            gui_fps,
            "timings",
            "speed_graph_draw_ms",
            "p95_ms",
        ),
        "gui_render_present_p95_ms": nested_float(
            gui_fps,
            "timings",
            "render_present_ms",
            "p95_ms",
        ),
        "gui_dominant_timing_bucket": timing_diagnosis.get("dominant_timing_bucket"),
        "gui_dominant_timing_label": timing_diagnosis.get("dominant_timing_label"),
        "gui_dominant_timing_p95_ms": finite_float(
            timing_diagnosis.get("dominant_timing_p95_ms")
        ),
        "gui_dominant_timing_share": finite_float(
            timing_diagnosis.get("dominant_timing_fraction_of_frame_total_p95")
        ),
        "gui_main_texture_upload_count": finite_optional_int(
            nested_dict(gui_fps, "timings").get("main_texture_upload_count")
        ),
        "gui_crop_texture_upload_count": finite_optional_int(
            nested_dict(gui_fps, "timings").get("crop_texture_upload_count")
        ),
        "gui_overall_samples": finite_int(nested_dict(gui_fps, "overall").get("sample_count")),
        "gui_visible_samples": finite_int(nested_dict(gui_fps, "crop_preview_visible").get("sample_count")),
        "gui_hidden_samples": finite_int(nested_dict(gui_fps, "crop_preview_hidden").get("sample_count")),
    }
    return summary


def add_deltas(summaries: list[dict[str, Any]]) -> None:
    if not summaries:
        return
    baseline = summaries[0]
    delta_fields = [
        "gui_overall_p05_fps",
        "gui_visible_p05_fps",
        "gui_hidden_p05_fps",
        "detect_steady_p95_avg_ms",
        "queue_p95_avg_ms",
        "queue_steady_p95_avg_ms",
        "acq_worker_p95_avg_ms",
        "acq_worker_steady_p95_avg_ms",
        "enqueue_dequeue_p95_avg_ms",
        "enqueue_dequeue_steady_p95_avg_ms",
        "dequeue_worker_p95_avg_ms",
        "dequeue_worker_steady_p95_avg_ms",
        "service_gap_p95_avg_ms",
        "service_gap_steady_p95_avg_ms",
        "gui_dominant_timing_p95_ms",
    ]
    for item in summaries:
        deltas: dict[str, float | None] = {}
        for field in delta_fields:
            current = finite_float(item.get(field))
            base = finite_float(baseline.get(field))
            deltas[field] = current - base if current is not None and base is not None else None
        item["delta_vs_first"] = deltas


def fmt_float(value: Any, digits: int = 1) -> str:
    number = finite_float(value)
    if number is None:
        return "-"
    return f"{number:.{digits}f}"


def fmt_int(value: Any) -> str:
    return str(finite_int(value))


def fmt_optional_int(value: Any) -> str:
    if value is None:
        return "-"
    return str(finite_int(value))


def fmt_list(values: Any) -> str:
    if not isinstance(values, list):
        return "-"
    if not values:
        return "-"
    return ",".join(str(value) for value in values)


def pipe_or_comma_list(values: Any) -> str:
    if not isinstance(values, list) or not values:
        return "-"
    return ",".join(str(value) for value in values)


def fmt_percent(value: Any) -> str:
    number = finite_float(value)
    if number is None:
        return "-"
    return f"{number * 100.0:.0f}%"


def render_table(summaries: list[dict[str, Any]]) -> str:
    def fmt_ratio(accepted: Any, offered: Any) -> str:
        if accepted is None or offered is None:
            return "-"
        return f"{fmt_int(accepted)}/{fmt_int(offered)}"

    columns = [
        ("run", lambda item: str(item.get("label", ""))),
        ("status", lambda item: str(item.get("status", "unknown"))),
        ("git", lambda item: str(item.get("source_commit_short") or item.get("producer_version") or "-")),
        ("dirty", lambda item: str(item.get("source_dirty_tracked")) if item.get("source_dirty_tracked") is not None else "-"),
        ("cams", lambda item: fmt_int(item.get("camera_count"))),
        ("isolated cpus", lambda item: pipe_or_comma_list(item.get("isolated_cpu_values"))),
        ("kernel opts", lambda item: fmt_list(item.get("kernel_cmdline_cpu_option_values"))),
        ("yolo affinity", lambda item: fmt_list(item.get("yolo_affinity_mapping_values"))),
        ("gui p05", lambda item: fmt_float(item.get("gui_overall_p05_fps"))),
        ("visible p05", lambda item: fmt_float(item.get("gui_visible_p05_fps"))),
        ("hidden p05", lambda item: fmt_float(item.get("gui_hidden_p05_fps"))),
        ("stream ds", lambda item: fmt_int(item.get("gui_stream_downsample"))),
        ("display fps", lambda item: fmt_int(item.get("display_preview_max_fps"))),
        ("swap", lambda item: fmt_int(item.get("gui_swap_interval"))),
        ("frame cap", lambda item: fmt_int(item.get("gui_frame_max_fps"))),
        ("speed graphs", lambda item: fmt_int(item.get("yolo_speed_graphs_enabled"))),
        ("frame p95", lambda item: fmt_float(item.get("gui_frame_total_p95_ms"), 2)),
        ("main upload p95", lambda item: fmt_float(item.get("gui_main_texture_upload_p95_ms"), 2)),
        ("crop upload p95", lambda item: fmt_float(item.get("gui_crop_texture_upload_p95_ms"), 2)),
        ("camera draw p95", lambda item: fmt_float(item.get("gui_camera_window_draw_p95_ms"), 2)),
        ("crop draw p95", lambda item: fmt_float(item.get("gui_crop_window_draw_p95_ms"), 2)),
        ("speed graph p95", lambda item: fmt_float(item.get("gui_speed_graph_draw_p95_ms"), 2)),
        ("present p95", lambda item: fmt_float(item.get("gui_render_present_p95_ms"), 2)),
        ("dominant p95", lambda item: (
            "-"
            if item.get("gui_dominant_timing_label") is None
            else (
                f"{item.get('gui_dominant_timing_label')} "
                f"{fmt_float(item.get('gui_dominant_timing_p95_ms'), 2)}"
            )
        )),
        ("dom share", lambda item: fmt_percent(item.get("gui_dominant_timing_share"))),
        ("main uploads", lambda item: fmt_optional_int(item.get("gui_main_texture_upload_count"))),
        ("crop uploads", lambda item: fmt_optional_int(item.get("gui_crop_texture_upload_count"))),
        ("crop rows", lambda item: fmt_int(item.get("crop_rows_total"))),
        ("crop drops", lambda item: fmt_int(item.get("crop_dropped_rows_total"))),
        ("crop backend", lambda item: fmt_list(item.get("crop_backend_values"))),
        ("ext drops", lambda item: fmt_int(item.get("external_crop_dropped_total"))),
        ("ext q depth", lambda item: fmt_list(item.get("external_crop_queue_depth_values"))),
        ("ext gpus", lambda item: fmt_list(item.get("external_crop_gpu_mapping_values"))),
        ("ext same-gpu", lambda item: fmt_list(item.get("external_crop_same_gpu_mapping_values"))),
        ("ext q high", lambda item: fmt_optional_int(item.get("external_crop_queue_high_water_max"))),
        ("ext q age p95", lambda item: fmt_float(item.get("external_crop_enqueue_age_p95_max_ms"), 2)),
        ("ext status", lambda item: (
            "-"
            if finite_int(item.get("external_recorder_status_streams_total")) <= 0
            else (
                "ok"
                if not item.get("external_recorder_status_failed_streams")
                else fmt_list(item.get("external_recorder_status_failed_streams"))
            )
        )),
        ("ext hb min", lambda item: fmt_optional_int(item.get("external_recorder_heartbeat_min"))),
        ("detect rows", lambda item: fmt_int(item.get("crop_metadata_detection_rows_total"))),
        ("rec fanout", lambda item: fmt_ratio(
            item.get("producer_recording_crop_frame_accepted_total"),
            item.get("producer_recording_crop_frame_offered_total"),
        )),
        ("rec fanout drops", lambda item: fmt_optional_int(
            item.get("producer_recording_crop_frame_dropped_total")
        )),
        ("preview upd/off", lambda item: (
            f"{fmt_int(item.get('preview_updated_total'))}/"
            f"{fmt_int(item.get('preview_offered_total'))}"
        )),
        ("preview fanout", lambda item: fmt_ratio(
            item.get("producer_preview_crop_frame_accepted_total"),
            item.get("producer_preview_crop_frame_offered_total"),
        )),
        ("preview fanout drops", lambda item: fmt_optional_int(
            item.get("producer_preview_crop_frame_dropped_total")
        )),
        ("pose fanout", lambda item: fmt_ratio(
            item.get("producer_pose_crop_frame_accepted_total"),
            item.get("producer_pose_crop_frame_offered_total"),
        )),
        ("pose fanout drops", lambda item: fmt_optional_int(
            item.get("producer_pose_crop_frame_dropped_total")
        )),
        ("preview skip%", lambda item: (
            "-"
            if finite_int(item.get("preview_offered_total")) <= 0
            else f"{100.0 * finite_int(item.get('preview_skipped_total')) / finite_int(item.get('preview_offered_total')):.1f}"
        )),
        ("preview fps", lambda item: fmt_list(item.get("preview_max_fps_values"))),
        ("preview disabled", lambda item: fmt_list(item.get("preview_disabled_values"))),
        ("preview shown", lambda item: fmt_list(item.get("preview_display_enabled_values"))),
        ("crop pool", lambda item: fmt_list(item.get("crop_frame_pool_size_values"))),
        ("detect p95 avg", lambda item: fmt_float(item.get("detect_steady_p95_avg_ms"), 3)),
        ("detect p95 max", lambda item: fmt_float(item.get("detect_steady_p95_max_ms"), 3)),
        ("acq worker avg", lambda item: fmt_float(item.get("acq_worker_p95_avg_ms"), 3)),
        ("acq worker steady", lambda item: fmt_float(item.get("acq_worker_steady_p95_avg_ms"), 3)),
        ("enqueue dequeue avg", lambda item: fmt_float(item.get("enqueue_dequeue_p95_avg_ms"), 3)),
        ("enqueue dequeue steady", lambda item: fmt_float(item.get("enqueue_dequeue_steady_p95_avg_ms"), 3)),
        ("dequeue worker avg", lambda item: fmt_float(item.get("dequeue_worker_p95_avg_ms"), 3)),
        ("queue avg", lambda item: fmt_float(item.get("queue_p95_avg_ms"), 3)),
        ("queue steady", lambda item: fmt_float(item.get("queue_steady_p95_avg_ms"), 3)),
        ("service gap avg", lambda item: fmt_float(item.get("service_gap_p95_avg_ms"), 3)),
        ("service gap steady", lambda item: fmt_float(item.get("service_gap_steady_p95_avg_ms"), 3)),
        ("fail/warn", lambda item: f"{fmt_int(item.get('failures'))}/{fmt_int(item.get('warnings'))}"),
    ]

    rows = [[render(item) for _, render in columns] for item in summaries]
    headers = [name for name, _ in columns]
    widths = [
        max(len(header), *(len(row[index]) for row in rows)) if rows else len(header)
        for index, header in enumerate(headers)
    ]

    def render_row(values: list[str]) -> str:
        return " | ".join(value.ljust(widths[index]) for index, value in enumerate(values))

    lines = [
        render_row(headers),
        " | ".join("-" * width for width in widths),
    ]
    lines.extend(render_row(row) for row in rows)
    return "\n".join(lines)


def compare(paths: list[str]) -> list[dict[str, Any]]:
    summaries: list[dict[str, Any]] = []
    for raw_value in paths:
        label, path = split_labeled_path(raw_value)
        summaries.append(summarize_validation(label, load_validation_json(path)))
    add_deltas(summaries)
    return summaries


def threshold_failures(args: argparse.Namespace, summaries: list[dict[str, Any]]) -> list[str]:
    failures: list[str] = []
    if args.require_pass:
        failures.extend(
            f"{item.get('label')}: status={item.get('status')}"
            for item in summaries
            if item.get("status") != "pass"
        )
    if args.require_zero_crop_drops:
        for item in summaries:
            crop_drops = finite_int(item.get("crop_dropped_rows_total"))
            external_drops = finite_int(item.get("external_crop_dropped_total"))
            if crop_drops != 0 or external_drops != 0:
                failures.append(
                    f"{item.get('label')}: crop_drops={crop_drops} external_crop_drops={external_drops}"
                )

    if args.require_visible_samples and all(
        finite_int(item.get("gui_visible_samples")) <= 0 for item in summaries
    ):
        failures.append("no compared run has visible crop-preview GUI FPS samples")
    if args.require_hidden_samples and all(
        finite_int(item.get("gui_hidden_samples")) <= 0 for item in summaries
    ):
        failures.append("no compared run has hidden crop-preview GUI FPS samples")
    if args.require_matching_cameras and summaries:
        expected_cameras = summaries[0].get("cameras")
        for item in summaries[1:]:
            if item.get("cameras") != expected_cameras:
                failures.append(
                    f"{item.get('label')}: camera set {item.get('cameras')} "
                    f"does not match {summaries[0].get('label')} {expected_cameras}"
                )
    if args.require_matching_display_config and summaries:
        display_fields = [
            "gui_stream_downsample",
            "display_preview_max_fps",
            "gui_swap_interval",
            "gui_frame_max_fps",
            "yolo_speed_graphs_enabled",
        ]
        expected_display = {field: summaries[0].get(field) for field in display_fields}
        for item in summaries[1:]:
            current_display = {field: item.get(field) for field in display_fields}
            if current_display != expected_display:
                failures.append(
                    f"{item.get('label')}: display config {current_display} "
                    f"does not match {summaries[0].get('label')} {expected_display}"
                )
    if args.require_matching_crop_config and summaries:
        crop_config_fields = [
            "crop_backend_values",
            "external_crop_queue_depth_values",
            "external_crop_gpu_mapping_values",
            "preview_max_fps_values",
            "preview_disabled_values",
            "crop_frame_pool_size_values",
        ]
        expected_crop_config = {field: summaries[0].get(field) for field in crop_config_fields}
        for item in summaries[1:]:
            current_crop_config = {field: item.get(field) for field in crop_config_fields}
            if current_crop_config != expected_crop_config:
                failures.append(
                    f"{item.get('label')}: crop config {current_crop_config} "
                    f"does not match {summaries[0].get('label')} {expected_crop_config}"
                )
    if getattr(args, "require_matching_yolo_runtime_config", False) and summaries:
        yolo_runtime_fields = [
            "yolo_affinity_mapping_values",
            "isolated_cpu_values",
            "kernel_cmdline_cpu_option_values",
        ]
        expected_yolo_runtime = {field: summaries[0].get(field) for field in yolo_runtime_fields}
        for item in summaries[1:]:
            current_yolo_runtime = {field: item.get(field) for field in yolo_runtime_fields}
            if current_yolo_runtime != expected_yolo_runtime:
                failures.append(
                    f"{item.get('label')}: YOLO runtime config {current_yolo_runtime} "
                    f"does not match {summaries[0].get('label')} {expected_yolo_runtime}"
                )

    if getattr(args, "require_external_crop_recorder_gpu_separate_from_analytics", False):
        for item in summaries:
            has_external_crop = "external_ipc" in (
                item.get("crop_backend_values")
                if isinstance(item.get("crop_backend_values"), list) else []
            )
            mappings = item.get("external_crop_gpu_mapping_values")
            mappings = mappings if isinstance(mappings, list) else []
            same_gpu_mappings = item.get("external_crop_same_gpu_mapping_values")
            same_gpu_mappings = same_gpu_mappings if isinstance(same_gpu_mappings, list) else []
            if has_external_crop and not mappings:
                failures.append(
                    f"{item.get('label')}: external crop GPU placement metadata missing"
                )
            if same_gpu_mappings:
                failures.append(
                    f"{item.get('label')}: external crop recorder uses analytics GPU "
                    f"for {same_gpu_mappings}"
                )

    if getattr(args, "require_external_recorder_status", False):
        for item in summaries:
            streams_total = finite_int(item.get("external_recorder_status_streams_total"))
            failed_streams = item.get("external_recorder_status_failed_streams")
            failed_streams = failed_streams if isinstance(failed_streams, list) else []
            if streams_total <= 0:
                failures.append(f"{item.get('label')}: external recorder status missing")
            if failed_streams:
                failures.append(
                    f"{item.get('label')}: external recorder status not healthy for {failed_streams}"
                )

    fps_thresholds = [
        ("gui_overall_p05_fps", args.min_gui_overall_p05_fps, "overall GUI p05 FPS", "gui_overall_samples"),
        ("gui_visible_p05_fps", args.min_gui_visible_p05_fps, "visible GUI p05 FPS", "gui_visible_samples"),
        ("gui_hidden_p05_fps", args.min_gui_hidden_p05_fps, "hidden GUI p05 FPS", "gui_hidden_samples"),
    ]
    for field, threshold, label, sample_field in fps_thresholds:
        if threshold is None:
            continue
        for item in summaries:
            if finite_int(item.get(sample_field)) <= 0:
                continue
            value = finite_float(item.get(field))
            if value is None or value < threshold:
                failures.append(f"{item.get('label')}: {label}={value} below {threshold:.1f}")

    if args.max_external_crop_queue_high_water is not None:
        for item in summaries:
            value = finite_float(item.get("external_crop_queue_high_water_max"))
            if value is None or value > args.max_external_crop_queue_high_water:
                failures.append(
                    f"{item.get('label')}: external crop queue high-water={value} "
                    f"> {args.max_external_crop_queue_high_water}"
                )
    if args.max_external_crop_enqueue_age_p95_ms is not None:
        for item in summaries:
            value = finite_float(item.get("external_crop_enqueue_age_p95_max_ms"))
            if value is None or value > args.max_external_crop_enqueue_age_p95_ms:
                failures.append(
                    f"{item.get('label')}: external crop enqueue-age p95={value} ms "
                    f"> {args.max_external_crop_enqueue_age_p95_ms:.3f} ms"
                )
    return failures


def main() -> int:
    args = parse_args()
    try:
        summaries = compare(args.validation_json)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"compare_gui_crop_preview_validation.py: {exc}", file=sys.stderr)
        return 2

    failures = threshold_failures(args, summaries)
    if args.json:
        print(json.dumps(
            {
                "schema_version": 1,
                "status": "fail" if failures else "pass",
                "threshold_failures": failures,
                "runs": summaries,
            },
            indent=2,
            sort_keys=True,
        ))
    else:
        print(render_table(summaries))

    for failure in failures:
        print(f"[FAIL] {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
