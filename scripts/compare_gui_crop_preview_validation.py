#!/usr/bin/env python3
"""Compare GUI crop-preview validation JSON summaries."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


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


def summarize_validation(label: str, payload: dict[str, Any]) -> dict[str, Any]:
    camera_summary = payload.get("summary")
    camera_summary = camera_summary if isinstance(camera_summary, dict) else {}
    crop_preview = payload.get("crop_preview")
    crop_preview = crop_preview if isinstance(crop_preview, dict) else {}
    crop_recording = payload.get("crop_recording")
    crop_recording = crop_recording if isinstance(crop_recording, dict) else {}

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

    crop_rows_total = 0
    crop_dropped_rows_total = 0
    crop_video_frames_total = 0
    external_crop_dropped_total = 0
    external_crop_queue_depth_values: set[int] = set()
    external_crop_queue_high_water_values: list[int] = []
    external_crop_enqueue_age_p95_values: list[float] = []
    for item in crop_recording.values():
        if not isinstance(item, dict):
            continue
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

    summary = {
        "label": label,
        "status": str(payload.get("status", "unknown")),
        "recording_folder": str(payload.get("recording_folder", "")),
        "camera_count": len(cameras),
        "cameras": cameras,
        "failures": len(payload.get("failures", []) if isinstance(payload.get("failures"), list) else []),
        "warnings": len(payload.get("warnings", []) if isinstance(payload.get("warnings"), list) else []),
        "detect_steady_p95_avg_ms": mean(detect_p95_values),
        "detect_steady_p95_max_ms": max_or_none(detect_p95_values),
        "queue_p95_avg_ms": mean(queue_p95_values),
        "queue_p95_max_ms": max_or_none(queue_p95_values),
        "crop_rows_total": crop_rows_total,
        "crop_dropped_rows_total": crop_dropped_rows_total,
        "crop_video_frames_total": crop_video_frames_total,
        "external_crop_dropped_total": external_crop_dropped_total,
        "external_crop_queue_depth_values": sorted(external_crop_queue_depth_values),
        "external_crop_queue_high_water_max": max_or_none(external_crop_queue_high_water_values),
        "external_crop_enqueue_age_p95_max_ms": max_or_none(external_crop_enqueue_age_p95_values),
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


def render_table(summaries: list[dict[str, Any]]) -> str:
    def fmt_ratio(accepted: Any, offered: Any) -> str:
        if accepted is None or offered is None:
            return "-"
        return f"{fmt_int(accepted)}/{fmt_int(offered)}"

    columns = [
        ("run", lambda item: str(item.get("label", ""))),
        ("status", lambda item: str(item.get("status", "unknown"))),
        ("cams", lambda item: fmt_int(item.get("camera_count"))),
        ("gui p05", lambda item: fmt_float(item.get("gui_overall_p05_fps"))),
        ("visible p05", lambda item: fmt_float(item.get("gui_visible_p05_fps"))),
        ("hidden p05", lambda item: fmt_float(item.get("gui_hidden_p05_fps"))),
        ("stream ds", lambda item: fmt_int(item.get("gui_stream_downsample"))),
        ("display fps", lambda item: fmt_int(item.get("display_preview_max_fps"))),
        ("speed graphs", lambda item: fmt_int(item.get("yolo_speed_graphs_enabled"))),
        ("frame p95", lambda item: fmt_float(item.get("gui_frame_total_p95_ms"), 2)),
        ("main upload p95", lambda item: fmt_float(item.get("gui_main_texture_upload_p95_ms"), 2)),
        ("crop upload p95", lambda item: fmt_float(item.get("gui_crop_texture_upload_p95_ms"), 2)),
        ("camera draw p95", lambda item: fmt_float(item.get("gui_camera_window_draw_p95_ms"), 2)),
        ("crop draw p95", lambda item: fmt_float(item.get("gui_crop_window_draw_p95_ms"), 2)),
        ("speed graph p95", lambda item: fmt_float(item.get("gui_speed_graph_draw_p95_ms"), 2)),
        ("present p95", lambda item: fmt_float(item.get("gui_render_present_p95_ms"), 2)),
        ("main uploads", lambda item: fmt_optional_int(item.get("gui_main_texture_upload_count"))),
        ("crop uploads", lambda item: fmt_optional_int(item.get("gui_crop_texture_upload_count"))),
        ("crop rows", lambda item: fmt_int(item.get("crop_rows_total"))),
        ("crop drops", lambda item: fmt_int(item.get("crop_dropped_rows_total"))),
        ("ext drops", lambda item: fmt_int(item.get("external_crop_dropped_total"))),
        ("ext q depth", lambda item: fmt_list(item.get("external_crop_queue_depth_values"))),
        ("ext q high", lambda item: fmt_optional_int(item.get("external_crop_queue_high_water_max"))),
        ("ext q age p95", lambda item: fmt_float(item.get("external_crop_enqueue_age_p95_max_ms"), 2)),
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


def main() -> int:
    args = parse_args()
    try:
        summaries = compare(args.validation_json)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"compare_gui_crop_preview_validation.py: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps({"schema_version": 1, "runs": summaries}, indent=2, sort_keys=True))
    else:
        print(render_table(summaries))

    failed = False
    if args.require_pass:
        failed = any(item.get("status") != "pass" for item in summaries)
    if args.require_zero_crop_drops:
        failed = failed or any(
            finite_int(item.get("crop_dropped_rows_total")) != 0 or
            finite_int(item.get("external_crop_dropped_total")) != 0
            for item in summaries
        )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
