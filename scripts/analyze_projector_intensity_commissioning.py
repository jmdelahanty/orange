#!/usr/bin/env python3
"""Analyze individual projected-grid commissioning frames."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    import cv2
    import numpy as np
except ImportError as error:  # pragma: no cover - environment-specific message
    print(
        f"OpenCV and NumPy are required ({error}). Run with: conda run -n juicebox python {__file__} ...",
        file=sys.stderr,
    )
    raise SystemExit(2)


REPORT_SCHEMA_ID = "orange.projector_intensity_commissioning.report"


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def median_or_none(values: list[float]) -> float | None:
    finite = [float(value) for value in values if math.isfinite(float(value))]
    return statistics.median(finite) if finite else None


def max_or_none(values: list[float]) -> float | None:
    finite = [float(value) for value in values if math.isfinite(float(value))]
    return max(finite) if finite else None


def locate_capture(result: dict[str, Any], camera_serial: str) -> tuple[Path, dict[str, Any]]:
    persistence = result.get("persistence", {})
    session_dir = Path(str(persistence.get("session_dir", "")))
    capture_group_id = str(result.get("workflow", {}).get("capture_group_id", ""))
    for image_set_path in sorted(session_dir.glob(f"artifacts/Cam{camera_serial}_*/image_set.json")):
        image_set = json.loads(image_set_path.read_text(encoding="utf-8"))
        for image in reversed(image_set.get("images", [])):
            capture = image.get("capture", {})
            if capture.get("capture_group_id") != capture_group_id:
                continue
            image_path = image_set_path.parent / str(image.get("path", ""))
            if image_path.is_file():
                image_with_capture = dict(image)
                # The image-set top-level capture object describes the most
                # recently appended image.  Use the matched image's immutable
                # capture metadata so earlier sweep samples retain their own
                # PTP timestamp, exposure, and frame identifiers.
                image_with_capture["capture_context"] = capture
                return image_path, image_with_capture
    raise FileNotFoundError(
        f"no saved image for camera {camera_serial} group {capture_group_id} in {session_dir}"
    )


def select_result_sample(result: dict[str, Any], sample_index: int | None) -> dict[str, Any]:
    if sample_index is None:
        return result
    samples = result.get("samples")
    if not isinstance(samples, list) or sample_index < 0 or sample_index >= len(samples):
        raise IndexError(f"guided result has no sample index {sample_index}")
    sample = samples[sample_index]
    if not isinstance(sample, dict):
        raise TypeError(f"guided result sample {sample_index} is not an object")
    return {
        "workflow": sample.get("workflow", {}),
        "captures": sample.get("captures", []),
        "persistence": sample.get("persistence", result.get("persistence", {})),
    }


def target_for_camera(result: dict[str, Any], camera_serial: str) -> dict[str, Any]:
    scene = result.get("workflow", {}).get("citrus_scene_pre_capture", {})
    for target in scene.get("resolved_targets", []):
        if camera_serial in [str(value) for value in target.get("associated_camera_ids", [])]:
            return target
    raise KeyError(f"Citrus resolved target is missing for camera {camera_serial}")


def expected_grid_points(target: dict[str, Any]) -> tuple[np.ndarray, int, int]:
    pattern = target["projected_pattern"]
    arena_size = target["arena"]["size_px"]
    rows = int(pattern["grid_rows"])
    cols = int(pattern["grid_cols"])
    grid_width = float(pattern["grid_width_px"])
    grid_height = float(pattern["grid_height_px"])
    texture_width = float(arena_size["width"])
    texture_height = float(arena_size["height"])
    radius = float(pattern["dot_radius_px"])
    margin = 2.0 * max(radius, 0.0)
    offset_x = math.floor((texture_width - grid_width) / 2.0) + margin
    offset_y = math.floor((texture_height - grid_height) / 2.0) + margin
    width = grid_width - 2.0 * margin
    height = grid_height - 2.0 * margin
    points = [
        (offset_x + col * width / max(cols - 1, 1),
         offset_y + row * height / max(rows - 1, 1))
        for row in range(rows)
        for col in range(cols)
    ]
    return np.asarray(points, dtype=np.float32), rows, cols


def make_blob_detector(lenient: bool = False):
    params = cv2.SimpleBlobDetector_Params()
    params.filterByArea = True
    params.minArea = 500 if lenient else 1000
    params.maxArea = 100000 if lenient else 50000
    params.filterByCircularity = True
    params.minCircularity = 0.4 if lenient else 0.6
    params.filterByConvexity = True
    params.minConvexity = 0.5 if lenient else 0.7
    params.filterByInertia = True
    params.minInertiaRatio = 0.2 if lenient else 0.4
    params.filterByColor = True
    params.blobColor = 0
    return cv2.SimpleBlobDetector_create(params)


def detect_grid(image: np.ndarray, rows: int, cols: int) -> tuple[np.ndarray | None, str]:
    blurred = cv2.GaussianBlur(image, (3, 3), 0)
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
    processed = cv2.morphologyEx(blurred, cv2.MORPH_OPEN, kernel)
    processed = cv2.morphologyEx(processed, cv2.MORPH_CLOSE, kernel)
    inverted = cv2.bitwise_not(processed)
    pattern_size = (cols, rows)
    strategies = [
        ("findCirclesGrid_default", cv2.CALIB_CB_SYMMETRIC_GRID, None),
        ("findCirclesGrid_blob_detector", cv2.CALIB_CB_SYMMETRIC_GRID,
         make_blob_detector(False)),
        ("findCirclesGrid_lenient_blob_detector", cv2.CALIB_CB_SYMMETRIC_GRID,
         make_blob_detector(True)),
    ]
    for name, flags, detector in strategies:
        if detector is None:
            found, centers = cv2.findCirclesGrid(inverted, pattern_size, flags=flags)
        else:
            found, centers = cv2.findCirclesGrid(
                inverted, pattern_size, flags=flags, blobDetector=detector
            )
        if found and centers is not None:
            return centers.reshape(-1, 2).astype(np.float32), name
    return None, "not_found"


def local_photometry(image: np.ndarray, centers: np.ndarray) -> dict[str, float]:
    if len(centers) < 2:
        return {}
    pairwise = np.linalg.norm(centers[:, None, :] - centers[None, :, :], axis=2)
    pairwise[pairwise == 0] = np.nan
    spacing = float(np.nanmedian(np.nanmin(pairwise, axis=1)))
    core_radius = max(2.0, spacing * 0.12)
    outer_radius = max(core_radius + 2.0, spacing * 0.32)
    core_values: list[np.ndarray] = []
    background_values: list[np.ndarray] = []
    widths: list[float] = []
    height, width = image.shape
    for center_x, center_y in centers:
        x0 = max(0, int(math.floor(center_x - outer_radius)))
        x1 = min(width, int(math.ceil(center_x + outer_radius + 1)))
        y0 = max(0, int(math.floor(center_y - outer_radius)))
        y1 = min(height, int(math.ceil(center_y + outer_radius + 1)))
        patch = image[y0:y1, x0:x1]
        yy, xx = np.ogrid[y0:y1, x0:x1]
        distance = np.sqrt((xx - center_x) ** 2 + (yy - center_y) ** 2)
        core = patch[distance <= core_radius]
        background = patch[(distance >= spacing * 0.24) & (distance <= outer_radius)]
        if core.size == 0 or background.size == 0:
            continue
        core_values.append(core)
        background_values.append(background)
        local_background = float(np.median(background))
        local_peak = float(np.percentile(core, 99))
        threshold = local_background + 0.5 * max(0.0, local_peak - local_background)
        above = (patch >= threshold) & (distance <= spacing * 0.22)
        widths.append(2.0 * math.sqrt(float(np.count_nonzero(above)) / math.pi))
    if not core_values:
        return {}
    core = np.concatenate(core_values)
    background = np.concatenate(background_values)
    return {
        "nearest_neighbor_spacing_px": spacing,
        "dot_core_median_u8": float(np.median(core)),
        "dot_core_p99_u8": float(np.percentile(core, 99)),
        "background_median_u8": float(np.median(background)),
        "dot_background_contrast_u8": float(np.median(core) - np.median(background)),
        "dot_core_saturation_fraction_ge_250": float(np.mean(core >= 250)),
        "dot_fwhm_equivalent_diameter_px_median": float(np.median(widths)),
    }


def analyze_frame(result_path: Path, gray: int, repeat_index: int,
                  camera_serial: str, sample_index: int | None = None) -> dict[str, Any]:
    result = json.loads(result_path.read_text(encoding="utf-8"))
    result = select_result_sample(result, sample_index)
    image_path, image_metadata = locate_capture(result, camera_serial)
    target = target_for_camera(result, camera_serial)
    expected, rows, cols = expected_grid_points(target)
    image = cv2.imread(str(image_path), cv2.IMREAD_GRAYSCALE)
    if image is None:
        raise OSError(f"OpenCV could not read {image_path}")
    centers, strategy = detect_grid(image, rows, cols)
    metrics: dict[str, Any] = {
        "foreground_gray_u8": gray,
        "repeat_index": repeat_index,
        "camera_serial": camera_serial,
        "result_json": str(result_path),
        "result_sample_index": sample_index,
        "image_path": str(image_path),
        "image_checksum": image_metadata.get("checksum"),
        "capture_group_id": result.get("workflow", {}).get("capture_group_id"),
        "exposure_us": image_metadata.get("capture_context", {}).get("exposure_us"),
        "frame_rate_hz": image_metadata.get("capture_context", {}).get("frame_rate_hz"),
        "camera_timestamp_ns": image_metadata.get("capture_context", {}).get(
            "camera_timestamp_ns"
        ),
        "timestamp_sys_ns": image_metadata.get("capture_context", {}).get(
            "timestamp_sys_ns"
        ),
        "image_min_u8": int(image.min()),
        "image_max_u8": int(image.max()),
        "image_p999_u8": float(np.percentile(image, 99.9)),
        "image_saturation_fraction_ge_250": float(np.mean(image >= 250)),
        "expected_point_count": rows * cols,
        "grid_rows": rows,
        "grid_cols": cols,
        "detected_point_count": 0 if centers is None else int(len(centers)),
        "detection_success": centers is not None and len(centers) == rows * cols,
        "detection_strategy": strategy,
        "homography_reprojection_rms_canvas_px": None,
        "homography_reprojection_max_canvas_px": None,
        "centers_camera_px": None,
    }
    if centers is not None and len(centers) == len(expected):
        homography, _ = cv2.findHomography(centers, expected, method=0)
        if homography is not None:
            projected = cv2.perspectiveTransform(centers.reshape(-1, 1, 2), homography).reshape(-1, 2)
            residual = np.linalg.norm(projected - expected, axis=1)
            metrics["homography_reprojection_rms_canvas_px"] = float(
                math.sqrt(float(np.mean(residual ** 2)))
            )
            metrics["homography_reprojection_max_canvas_px"] = float(residual.max())
        metrics.update(local_photometry(image, centers))
        metrics["centers_camera_px"] = centers.tolist()
    return metrics


def align_centers(reference: np.ndarray, candidate: np.ndarray, rows: int, cols: int) -> np.ndarray:
    grid = candidate.reshape(rows, cols, 2)
    variants = [grid, grid[::-1, ::-1], grid[::-1, :], grid[:, ::-1]]
    if rows == cols:
        transposed = np.transpose(grid, (1, 0, 2))
        variants.extend([transposed, transposed[::-1, ::-1],
                         transposed[::-1, :], transposed[:, ::-1]])
    flattened = [variant.reshape(-1, 2) for variant in variants]
    return min(flattened, key=lambda points: float(np.mean(np.linalg.norm(points - reference, axis=1))))


def summarize_group(frames: list[dict[str, Any]], rows: int, cols: int) -> dict[str, Any]:
    successes = [frame for frame in frames if frame.get("detection_success")]
    jitter = None
    if len(successes) >= 2:
        reference = np.asarray(successes[0]["centers_camera_px"], dtype=np.float64)
        aligned = [reference]
        for frame in successes[1:]:
            aligned.append(align_centers(
                reference,
                np.asarray(frame["centers_camera_px"], dtype=np.float64),
                rows,
                cols,
            ))
        stack = np.stack(aligned)
        mean_points = np.mean(stack, axis=0)
        per_observation = np.linalg.norm(stack - mean_points[None, :, :], axis=2)
        jitter = float(math.sqrt(float(np.mean(per_observation ** 2))))
    summary = {
        "foreground_gray_u8": frames[0]["foreground_gray_u8"],
        "camera_serial": frames[0]["camera_serial"],
        "repeat_count": len(frames),
        "detection_success_count": len(successes),
        "detection_success_fraction": len(successes) / len(frames),
        "centroid_jitter_rms_camera_px": jitter,
        "homography_reprojection_rms_canvas_px_median": median_or_none([
            frame.get("homography_reprojection_rms_canvas_px", math.nan) for frame in successes
        ]),
        "homography_reprojection_rms_canvas_px_max": max_or_none([
            frame.get("homography_reprojection_rms_canvas_px", math.nan) for frame in successes
        ]),
        "dot_background_contrast_u8_median": median_or_none([
            frame.get("dot_background_contrast_u8", math.nan) for frame in successes
        ]),
        "dot_core_saturation_fraction_ge_250_max": max_or_none([
            frame.get("dot_core_saturation_fraction_ge_250", math.nan) for frame in successes
        ]),
        "dot_core_p99_u8_median": median_or_none([
            frame.get("dot_core_p99_u8", math.nan) for frame in successes
        ]),
        "dot_fwhm_equivalent_diameter_px_median": median_or_none([
            frame.get("dot_fwhm_equivalent_diameter_px_median", math.nan) for frame in successes
        ]),
    }
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--max-core-saturation-fraction", type=float, default=0.005)
    parser.add_argument("--min-contrast-u8", type=float, default=20.0)
    parser.add_argument("--max-reprojection-rms-canvas-px", type=float, default=0.5)
    parser.add_argument("--max-centroid-jitter-camera-px", type=float, default=0.5)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest.get("schema_id") != "orange.projector_intensity_commissioning.run_manifest":
        parser.error("input is not an Orange projector intensity commissioning manifest")
    cameras = [str(value) for value in manifest["capture"]["camera_serials"]]
    frame_metrics: list[dict[str, Any]] = []
    reference_images: list[dict[str, Any]] = []
    errors: list[str] = []
    grid_shapes: dict[str, tuple[int, int]] = {}
    for run in manifest.get("runs", []):
        if run.get("status") != "pass":
            errors.append(
                f"capture run failed gray={run.get('foreground_gray_u8')} repeat={run.get('repeat_index')}"
            )
            continue
        result_path = Path(str(run["result_json"]))
        for camera in cameras:
            try:
                metric = analyze_frame(
                    result_path,
                    int(run["foreground_gray_u8"]),
                    int(run["repeat_index"]),
                    camera,
                    (int(run["sample_index"]) if "sample_index" in run else None),
                )
                frame_metrics.append(metric)
                grid_shapes[camera] = (
                    int(metric["grid_rows"]), int(metric["grid_cols"])
                )
            except (OSError, KeyError, ValueError, TypeError, IndexError,
                    json.JSONDecodeError) as error:
                errors.append(f"{result_path} camera {camera}: {error}")

    for reference in manifest.get("reference_captures", []):
        if not isinstance(reference, dict) or reference.get("status") != "pass":
            continue
        result_path = Path(str(reference["result_json"]))
        try:
            result = json.loads(result_path.read_text(encoding="utf-8"))
            sample_result = select_result_sample(result, int(reference["sample_index"]))
            for camera in cameras:
                image_path, image_metadata = locate_capture(sample_result, camera)
                reference_images.append({
                    "kind": "arena_outline_with_center_fiducial",
                    "camera_serial": camera,
                    "result_json": str(result_path),
                    "result_sample_index": int(reference["sample_index"]),
                    "capture_group_id": sample_result.get("workflow", {}).get(
                        "capture_group_id"
                    ),
                    "image_path": str(image_path.resolve()),
                    "image_checksum": image_metadata.get("checksum"),
                    "camera_timestamp_ns": image_metadata.get(
                        "capture_context", {}
                    ).get("camera_timestamp_ns"),
                })
        except (OSError, KeyError, ValueError, TypeError, IndexError,
                json.JSONDecodeError) as error:
            errors.append(f"{result_path} arena outline reference: {error}")

    ptp_group_alignment: list[dict[str, Any]] = []
    synchronization = manifest.get("capture", {}).get("synchronization", {})
    max_ptp_span_ns = synchronization.get("max_group_camera_timestamp_span_ns")
    if isinstance(max_ptp_span_ns, int):
        timestamp_records = [
            {
                "kind": "homography_grid",
                "capture_group_id": metric.get("capture_group_id"),
                "camera_serial": metric.get("camera_serial"),
                "camera_timestamp_ns": metric.get("camera_timestamp_ns"),
            }
            for metric in frame_metrics
        ] + reference_images
        timestamps_by_group: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for record in timestamp_records:
            timestamps_by_group[str(record.get("capture_group_id", ""))].append(record)
        for capture_group_id, records in sorted(timestamps_by_group.items()):
            timestamps = [
                int(record["camera_timestamp_ns"])
                for record in records
                if isinstance(record.get("camera_timestamp_ns"), int)
                and int(record["camera_timestamp_ns"]) > 0
            ]
            span_ns = max(timestamps) - min(timestamps) if timestamps else None
            alignment = {
                "capture_group_id": capture_group_id,
                "kind": records[0].get("kind"),
                "camera_count": len(records),
                "timestamp_count": len(timestamps),
                "span_ns": span_ns,
                "max_allowed_span_ns": max_ptp_span_ns,
                "passes": len(timestamps) == len(cameras) and
                          span_ns is not None and span_ns <= max_ptp_span_ns,
            }
            ptp_group_alignment.append(alignment)
            if not alignment["passes"]:
                errors.append(
                    f"capture group {capture_group_id} PTP alignment failed: "
                    f"timestamps={len(timestamps)}/{len(cameras)} span_ns={span_ns} "
                    f"limit={max_ptp_span_ns}"
                )

    grouped: dict[tuple[int, str], list[dict[str, Any]]] = defaultdict(list)
    for metric in frame_metrics:
        grouped[(int(metric["foreground_gray_u8"]), str(metric["camera_serial"]))].append(metric)
    summaries: list[dict[str, Any]] = []
    for (gray, camera), frames in sorted(grouped.items()):
        rows, cols = grid_shapes.get(camera, (10, 10))
        summary = summarize_group(frames, rows, cols)
        values_present = all(summary.get(key) is not None for key in (
            "centroid_jitter_rms_camera_px",
            "homography_reprojection_rms_canvas_px_max",
            "dot_background_contrast_u8_median",
            "dot_core_saturation_fraction_ge_250_max",
        ))
        summary["passes_quality_gate"] = bool(
            values_present
            and summary["detection_success_fraction"] == 1.0
            and summary["centroid_jitter_rms_camera_px"] <= args.max_centroid_jitter_camera_px
            and summary["homography_reprojection_rms_canvas_px_max"] <= args.max_reprojection_rms_canvas_px
            and summary["dot_background_contrast_u8_median"] >= args.min_contrast_u8
            and summary["dot_core_saturation_fraction_ge_250_max"] <= args.max_core_saturation_fraction
        )
        summaries.append(summary)

    levels = [int(value) for value in manifest["projection"]["levels_gray_u8"]]
    level_gate = {
        gray: all(
            any(summary["camera_serial"] == camera and
                summary["foreground_gray_u8"] == gray and
                summary["passes_quality_gate"] for summary in summaries)
            for camera in cameras
        )
        for gray in levels
    }
    passing_levels = [gray for gray in levels if level_gate[gray]]
    recommendation = min(passing_levels) if passing_levels else None
    output_dir = args.manifest.parent
    images_root = output_dir / "images"
    image_index_rows: list[dict[str, Any]] = []
    for metric in frame_metrics:
        browse_path = (
            images_root
            / f"gray_{int(metric['foreground_gray_u8']):03d}"
            / f"repeat_{int(metric['repeat_index']):02d}"
            / f"Cam{metric['camera_serial']}.png"
        )
        browse_path.parent.mkdir(parents=True, exist_ok=True)
        source_path = Path(str(metric["image_path"])).resolve()
        if browse_path.is_symlink():
            if browse_path.resolve() != source_path:
                browse_path.unlink()
        elif browse_path.exists():
            errors.append(f"refusing to replace non-symlink browse path: {browse_path}")
        if not browse_path.exists():
            browse_path.symlink_to(source_path)
        metric["browse_path"] = str(browse_path.absolute())
        image_index_rows.append({
            "kind": "homography_grid",
            "foreground_gray_u8": metric["foreground_gray_u8"],
            "repeat_index": metric["repeat_index"],
            "camera_serial": metric["camera_serial"],
            "capture_group_id": metric["capture_group_id"],
            "browse_path": str(browse_path.absolute()),
            "authoritative_image_path": str(source_path),
            "image_checksum": metric.get("image_checksum"),
        })
    for reference in reference_images:
        browse_path = (
            images_root
            / "arena_outline_with_center_fiducial"
            / f"Cam{reference['camera_serial']}.png"
        )
        browse_path.parent.mkdir(parents=True, exist_ok=True)
        source_path = Path(str(reference["image_path"])).resolve()
        if browse_path.is_symlink():
            if browse_path.resolve() != source_path:
                browse_path.unlink()
        elif browse_path.exists():
            errors.append(f"refusing to replace non-symlink browse path: {browse_path}")
        if not browse_path.exists():
            browse_path.symlink_to(source_path)
        reference["browse_path"] = str(browse_path.absolute())
        image_index_rows.append({
            "kind": reference["kind"],
            "foreground_gray_u8": None,
            "repeat_index": 1,
            "camera_serial": reference["camera_serial"],
            "capture_group_id": reference["capture_group_id"],
            "browse_path": str(browse_path.absolute()),
            "authoritative_image_path": str(source_path),
            "image_checksum": reference.get("image_checksum"),
        })
    report = {
        "schema_id": REPORT_SCHEMA_ID,
        "schema_version": 1,
        "created_utc": utc_now(),
        "manifest_path": str(args.manifest.resolve()),
        "images_root": str(images_root.resolve(strict=False)),
        "status": "pass" if recommendation is not None and not errors else "no_passing_level",
        "method": {
            "individual_frames_only": True,
            "temporal_averaging": False,
            "projection_alpha_semantics": "opaque",
            "saturation_pixel_threshold_u8": 250,
            "quality_gates": {
                "detection_success_fraction": 1.0,
                "max_core_saturation_fraction": args.max_core_saturation_fraction,
                "min_dot_background_contrast_u8": args.min_contrast_u8,
                "max_homography_reprojection_rms_canvas_px": args.max_reprojection_rms_canvas_px,
                "max_centroid_jitter_rms_camera_px": args.max_centroid_jitter_camera_px,
            },
        },
        "recommended_foreground_gray_u8": recommendation,
        "level_passes_all_cameras": {str(key): value for key, value in level_gate.items()},
        "camera_level_summaries": summaries,
        "reference_images": reference_images,
        "ptp_group_alignment": ptp_group_alignment,
        "frame_metrics": frame_metrics,
        "errors": errors,
    }
    report_path = output_dir / "commissioning_report.json"
    write_json_atomic(report_path, report)

    csv_path = output_dir / "frame_metrics.csv"
    scalar_keys = sorted({
        key for row in frame_metrics for key, value in row.items()
        if key != "centers_camera_px" and not isinstance(value, (dict, list))
    })
    with csv_path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=scalar_keys)
        writer.writeheader()
        for row in frame_metrics:
            writer.writerow({key: row.get(key) for key in scalar_keys})

    image_index_path = output_dir / "image_index.csv"
    with image_index_path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=[
            "kind",
            "foreground_gray_u8",
            "repeat_index",
            "camera_serial",
            "capture_group_id",
            "browse_path",
            "authoritative_image_path",
            "image_checksum",
        ])
        writer.writeheader()
        writer.writerows(image_index_rows)

    markdown_path = output_dir / "commissioning_report.md"
    lines = [
        "# Projector intensity commissioning report",
        "",
        f"- Status: `{report['status']}`",
        f"- Recommended opaque foreground gray: `{recommendation}`" if recommendation is not None
        else "- Recommended opaque foreground gray: none passed all quality gates",
        "- Capture: individual full-resolution frames; no temporal averaging",
        f"- Browse images: `{images_root.resolve(strict=False)}`",
        f"- Authoritative calibration session: `{manifest.get('calibration_session', {}).get('session_dir', '')}`",
        f"- Arena outline + center fiducial references: `{len(reference_images)}` images",
        "",
        "| Gray | Camera | Detect | Saturated core max | Contrast median | Reprojection max (canvas px) | Jitter (camera px) | Pass |",
        "|---:|---:|---:|---:|---:|---:|---:|:---:|",
    ]
    for summary in summaries:
        lines.append(
            f"| {summary['foreground_gray_u8']} | {summary['camera_serial']} | "
            f"{summary['detection_success_count']}/{summary['repeat_count']} | "
            f"{summary['dot_core_saturation_fraction_ge_250_max']} | "
            f"{summary['dot_background_contrast_u8_median']} | "
            f"{summary['homography_reprojection_rms_canvas_px_max']} | "
            f"{summary['centroid_jitter_rms_camera_px']} | "
            f"{'yes' if summary['passes_quality_gate'] else 'no'} |"
        )
    if errors:
        lines.extend(["", "## Errors", ""] + [f"- {error}" for error in errors])
    markdown_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    manifest["status"] = "analyzed" if recommendation is not None and not errors else "analysis_failed_gate"
    manifest["updated_utc"] = utc_now()
    manifest["report_json"] = str(report_path.resolve())
    manifest["recommended_foreground_gray_u8"] = recommendation
    write_json_atomic(args.manifest, manifest)
    print(f"Report: {report_path}")
    print(f"Recommended opaque foreground gray: {recommendation}")
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
    return 0 if recommendation is not None and not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
