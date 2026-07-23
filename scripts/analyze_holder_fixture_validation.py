#!/usr/bin/env python3
"""Characterize a fixture aperture and revalidate an active Citrus homography."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    import cv2
    import numpy as np
except ImportError as error:  # pragma: no cover - environment-specific message
    print(
        f"OpenCV and NumPy are required ({error}). Run with: "
        f"conda run -n juicebox python {__file__} ...",
        file=sys.stderr,
    )
    raise SystemExit(2)


REPORT_SCHEMA_ID = "orange.holder_fixture_validation.report"
OBSERVATION_SCHEMA_ID = "orange.calibration.holder_fixture_observation"
EVIDENCE_PACKAGE_SCHEMA_ID = "orange.calibration.holder_fixture_evidence_package"


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z"
    )


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def write_text_atomic(path: Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(contents, encoding="utf-8")
    temporary.replace(path)


def copy_file_atomic(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    shutil.copyfile(source, temporary)
    temporary.replace(destination)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def relative_to_checked(path: Path, parent: Path, label: str) -> Path:
    try:
        return path.resolve().relative_to(parent.resolve())
    except ValueError as error:
        raise ValueError(f"{label} is outside {parent}: {path}") from error


def safe_artifact_component(value: str, label: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", value):
        raise ValueError(f"{label} is not a safe artifact path component: {value!r}")
    return value


def sample_result(result: dict[str, Any], recipe: str) -> dict[str, Any]:
    samples = result.get("samples", [])
    matches = [
        sample for sample in samples
        if isinstance(sample, dict) and sample.get("recipe") == recipe
    ]
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {recipe!r} sample, found {len(matches)}")
    sample = matches[0]
    return {
        "workflow": sample.get("workflow", {}),
        "captures": sample.get("captures", []),
        "persistence": sample.get("persistence", result.get("persistence", {})),
        "sample_index": sample.get("sample_index"),
    }


def locate_capture(sample: dict[str, Any], camera_serial: str) -> tuple[Path, dict[str, Any]]:
    persistence = sample.get("persistence", {})
    session_dir = Path(str(persistence.get("session_dir", "")))
    group_id = str(sample.get("workflow", {}).get("capture_group_id", ""))
    for image_set_path in sorted(
        session_dir.glob(f"artifacts/Cam{camera_serial}_*/image_set.json")
    ):
        image_set = json.loads(image_set_path.read_text(encoding="utf-8"))
        for image in reversed(image_set.get("images", [])):
            capture = image.get("capture", {})
            if capture.get("capture_group_id") != group_id:
                continue
            image_path = image_set_path.parent / str(image.get("path", ""))
            if image_path.is_file():
                return image_path, image
    raise FileNotFoundError(
        f"no saved image for camera {camera_serial}, capture group {group_id}"
    )


def target_for_camera(sample: dict[str, Any], camera_serial: str) -> dict[str, Any]:
    scene = sample.get("workflow", {}).get("citrus_scene_pre_capture", {})
    for target in scene.get("resolved_targets", []):
        associated = [str(value) for value in target.get("associated_camera_ids", [])]
        if camera_serial in associated:
            return target
    raise KeyError(f"Citrus resolved target is missing for camera {camera_serial}")


def transform_points(points: np.ndarray, matrix: np.ndarray) -> np.ndarray:
    return cv2.perspectiveTransform(
        points.astype(np.float64).reshape(-1, 1, 2), matrix
    ).reshape(-1, 2)


def expected_grid_points(target: dict[str, Any]) -> np.ndarray:
    pattern = target["projected_pattern"]
    arena = target["arena"]
    size = arena["size_px"]
    rows = int(pattern["grid_rows"])
    cols = int(pattern["grid_cols"])
    grid_width = float(pattern["grid_width_px"])
    grid_height = float(pattern["grid_height_px"])
    dot_radius = float(pattern["dot_radius_px"])
    margin = 2.0 * max(dot_radius, 0.0)
    offset_x = math.floor((float(size["width"]) - grid_width) / 2.0) + margin
    offset_y = math.floor((float(size["height"]) - grid_height) / 2.0) + margin
    usable_width = grid_width - 2.0 * margin
    usable_height = grid_height - 2.0 * margin
    origin = arena["origin_final_display_canvas_px"]
    return np.asarray([
        (
            float(origin["x"]) + offset_x + col * usable_width / max(cols - 1, 1),
            float(origin["y"]) + offset_y + row * usable_height / max(rows - 1, 1),
        )
        for row in range(rows)
        for col in range(cols)
    ], dtype=np.float64)


def expected_ring_points(target: dict[str, Any], arena_config: dict[str, Any]) -> np.ndarray:
    pattern = target["projected_pattern"]
    origin = target["arena"]["origin_final_display_canvas_px"]
    center_x = float(origin["x"]) + float(pattern["center_x_px"])
    center_y = float(origin["y"]) + float(pattern["center_y_px"])
    ring_count = max(1, int(pattern["ring_count"]))
    inner_count = max(1, int(pattern["ring_dots_inner"]))
    outer_count = max(1, int(pattern["ring_dots_outer"]))
    inner_radius = float(pattern["ring_inner_radius_px"])
    outer_radius = max(inner_radius, float(pattern["ring_outer_radius_px"]))
    start = math.radians(float(
        arena_config.get("calibration_ring_orientation_marker_angle_deg", 0.0)
    ))
    points: list[tuple[float, float]] = []
    if bool(pattern.get("ring_include_center_dot", True)):
        points.append((center_x, center_y))
    for ring_index in range(1, ring_count + 1):
        interpolation = 1.0 if ring_count == 1 else (ring_index - 1) / (ring_count - 1)
        radius = inner_radius + interpolation * (outer_radius - inner_radius)
        point_count = max(1, int(math.floor(
            inner_count + interpolation * (outer_count - inner_count) + 0.5
        )))
        for point_index in range(point_count):
            angle = start + 2.0 * math.pi * point_index / point_count
            points.append((center_x + math.cos(angle) * radius,
                           center_y + math.sin(angle) * radius))
    return np.asarray(points, dtype=np.float64)


def expected_verification_points(
    target: dict[str, Any], arena_config: dict[str, Any]
) -> np.ndarray:
    pattern = target["projected_pattern"]
    origin = target["arena"]["origin_final_display_canvas_px"]
    center_x = float(origin["x"]) + float(pattern["center_x_px"])
    center_y = float(origin["y"]) + float(pattern["center_y_px"])
    area_radius = float(pattern["verification_area_radius_px"])
    specifications = [
        (
            float(pattern["verification_inner_radius_fraction"]),
            int(pattern["verification_inner_point_count"]),
            float(arena_config.get(
                "calibration_verification_inner_angle_offset_deg", 45.0
            )),
        ),
        (
            float(pattern["verification_outer_radius_fraction"]),
            int(pattern["verification_outer_point_count"]),
            float(arena_config.get(
                "calibration_verification_outer_angle_offset_deg", 22.5
            )),
        ),
    ]
    points: list[tuple[float, float]] = []
    if bool(pattern.get("verification_include_center_dot", True)):
        points.append((center_x, center_y))
    for fraction, count, angle_degrees in specifications:
        radius = area_radius * min(1.0, max(0.0, fraction))
        start = math.radians(angle_degrees)
        for point_index in range(max(1, count)):
            angle = start + 2.0 * math.pi * point_index / max(1, count)
            points.append((center_x + math.cos(angle) * radius,
                           center_y + math.sin(angle) * radius))
    return np.asarray(points, dtype=np.float64)


def segment_aperture(
    uniform: np.ndarray, black: np.ndarray
) -> tuple[np.ndarray, np.ndarray, dict[str, Any]]:
    delta = cv2.subtract(uniform, black)
    blurred = cv2.GaussianBlur(delta, (11, 11), 0)
    low = float(np.percentile(blurred, 5))
    high = float(np.percentile(blurred, 95))
    if high - low < 3.0:
        raise ValueError(f"uniform-minus-black contrast is only {high - low:.2f} u8")
    # The illuminated arena can occupy more than 80% of a camera image. A
    # foreground-percentile threshold therefore lands inside a smooth
    # projector/camera brightness gradient and invents a diagonal "aperture"
    # edge. Otsu separates the near-black exterior from the illuminated
    # support without assuming how much of the sensor either class occupies.
    threshold, mask = cv2.threshold(
        blurred, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU
    )
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (21, 21))
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel)
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, kernel)
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    if not contours:
        raise ValueError("uniform-minus-black image has no aperture contour")
    contour = max(contours, key=cv2.contourArea)
    area = float(cv2.contourArea(contour))
    image_area = float(mask.shape[0] * mask.shape[1])
    if area <= 0.0:
        raise ValueError("aperture contour has zero area")
    clean_mask = np.zeros_like(mask)
    cv2.drawContours(clean_mask, [contour], -1, 255, cv2.FILLED)
    return contour, clean_mask, {
        "difference_p05_u8": low,
        "difference_p95_u8": high,
        "threshold_method": "global_otsu_on_uniform_minus_black",
        "threshold_u8": float(threshold),
        "area_camera_px2": area,
        "area_fraction_of_sensor": area / image_area,
    }


def resample_contour(contour: np.ndarray, maximum_points: int = 256) -> np.ndarray:
    points = contour.reshape(-1, 2).astype(np.float64)
    if len(points) <= maximum_points:
        return points
    indices = np.linspace(0, len(points) - 1, maximum_points, dtype=int)
    return points[indices]


def classify_visibility_boundary(
    boundary_camera: np.ndarray,
    homography: np.ndarray,
    target: dict[str, Any],
    image_shape: tuple[int, int],
    arena_edge_margin_canvas_px: float,
    sensor_edge_margin_camera_px: float,
) -> dict[str, Any]:
    """Separate configured-arena clipping from observed physical aperture arcs.

    The uniform-gray image illuminates the configured arena rectangle, not an
    infinite plane. Its outer contour is therefore the intersection of that
    rectangle, the holder aperture, and the camera sensor. Only contour points
    comfortably away from known arena and sensor edges are physical-aperture
    evidence.
    """
    boundary_canvas = transform_points(boundary_camera, homography)
    arena = target["arena"]
    origin = arena["origin_final_display_canvas_px"]
    size = arena["size_px"]
    x0 = float(origin["x"])
    y0 = float(origin["y"])
    x1 = x0 + float(size["width"])
    y1 = y0 + float(size["height"])
    canvas_edge_distance = np.min(np.column_stack((
        np.abs(boundary_canvas[:, 0] - x0),
        np.abs(boundary_canvas[:, 0] - x1),
        np.abs(boundary_canvas[:, 1] - y0),
        np.abs(boundary_canvas[:, 1] - y1),
    )), axis=1)
    arena_clipped = canvas_edge_distance <= arena_edge_margin_canvas_px

    height, width = image_shape
    sensor_edge_distance = np.min(np.column_stack((
        boundary_camera[:, 0],
        float(width - 1) - boundary_camera[:, 0],
        boundary_camera[:, 1],
        float(height - 1) - boundary_camera[:, 1],
    )), axis=1)
    sensor_clipped = sensor_edge_distance <= sensor_edge_margin_camera_px
    aperture_observed = ~(arena_clipped | sensor_clipped)
    return {
        "boundary_canvas": boundary_canvas,
        "arena_clipped_mask": arena_clipped,
        "sensor_clipped_mask": sensor_clipped,
        "aperture_observed_mask": aperture_observed,
        "arena_edge_margin_canvas_px": arena_edge_margin_canvas_px,
        "sensor_edge_margin_camera_px": sensor_edge_margin_camera_px,
        "configured_arena_bounds_canvas_px": {
            "x_min": x0,
            "y_min": y0,
            "x_max": x1,
            "y_max": y1,
        },
    }


def fit_circle(points: np.ndarray) -> dict[str, Any]:
    system = np.column_stack((2.0 * points[:, 0], 2.0 * points[:, 1], np.ones(len(points))))
    rhs = points[:, 0] ** 2 + points[:, 1] ** 2
    center_x, center_y, constant = np.linalg.lstsq(system, rhs, rcond=None)[0]
    radius = math.sqrt(max(0.0, constant + center_x ** 2 + center_y ** 2))
    residual = np.abs(np.linalg.norm(points - [center_x, center_y], axis=1) - radius)
    return {
        "type": "circle",
        "center": {"x": float(center_x), "y": float(center_y)},
        "radius": float(radius),
        "boundary_fit_rms": float(math.sqrt(float(np.mean(residual ** 2)))),
        "boundary_fit_max": float(residual.max()),
    }


def geometry_for_shape(points: np.ndarray, shape: str) -> dict[str, Any]:
    if shape == "circle":
        return fit_circle(points)
    if shape in ("rectangle", "rounded_rectangle"):
        rectangle = cv2.minAreaRect(points.astype(np.float32).reshape(-1, 1, 2))
        box = cv2.boxPoints(rectangle)
        return {
            "type": shape,
            "center": {"x": float(rectangle[0][0]), "y": float(rectangle[0][1])},
            "size": {"width": float(rectangle[1][0]), "height": float(rectangle[1][1])},
            "rotation_degrees": float(rectangle[2]),
            "corners": box.astype(float).tolist(),
            "rounded_corner_radius_status": (
                "not_fitted" if shape == "rounded_rectangle" else "not_applicable"
            ),
        }
    return {
        "type": "polygon" if shape == "polygon" else "unknown",
        "boundary_points": points.astype(float).tolist(),
    }


def detect_expected_dots(
    pattern: np.ndarray,
    black: np.ndarray,
    expected_canvas: np.ndarray,
    homography: np.ndarray,
    aperture_contour: np.ndarray,
    edge_margin_camera_px: float,
) -> dict[str, Any]:
    inverse = np.linalg.inv(homography)
    predicted_camera = transform_points(expected_canvas, inverse)
    if len(predicted_camera) > 1:
        distances = np.linalg.norm(
            predicted_camera[:, None, :] - predicted_camera[None, :, :], axis=2
        )
        distances[distances == 0] = np.nan
        spacing = float(np.nanmedian(np.nanmin(distances, axis=1)))
    else:
        spacing = 200.0
    half_window = int(max(45.0, min(180.0, spacing * 0.32)))
    delta = cv2.subtract(pattern, black)
    detected_camera: list[list[float]] = []
    matched_expected: list[list[float]] = []
    predicted_visible: list[list[float]] = []
    missed: list[list[float]] = []
    height, width = delta.shape
    for expected, predicted in zip(expected_canvas, predicted_camera):
        signed_distance = cv2.pointPolygonTest(
            aperture_contour,
            (float(predicted[0]), float(predicted[1])),
            True,
        )
        if signed_distance < edge_margin_camera_px:
            continue
        predicted_visible.append(predicted.astype(float).tolist())
        x0 = max(0, int(round(predicted[0])) - half_window)
        x1 = min(width, int(round(predicted[0])) + half_window + 1)
        y0 = max(0, int(round(predicted[1])) - half_window)
        y1 = min(height, int(round(predicted[1])) + half_window + 1)
        patch = delta[y0:y1, x0:x1]
        if patch.size == 0:
            missed.append(predicted.astype(float).tolist())
            continue
        baseline = float(np.percentile(patch, 30))
        peak = float(np.percentile(patch, 99.5))
        if peak - baseline < 4.0:
            missed.append(predicted.astype(float).tolist())
            continue
        threshold = baseline + 0.45 * (peak - baseline)
        binary = np.where(patch >= threshold, 255, 0).astype(np.uint8)
        binary = cv2.morphologyEx(
            binary,
            cv2.MORPH_OPEN,
            cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3)),
        )
        count, _, stats, centroids = cv2.connectedComponentsWithStats(binary)
        candidates: list[tuple[float, np.ndarray]] = []
        patch_area = float(patch.shape[0] * patch.shape[1])
        for component in range(1, count):
            area = float(stats[component, cv2.CC_STAT_AREA])
            if area < 20.0 or area > patch_area * 0.35:
                continue
            center = centroids[component] + np.asarray([x0, y0], dtype=float)
            distance = float(np.linalg.norm(center - predicted))
            if distance <= half_window * 0.75:
                candidates.append((distance, center))
        if not candidates:
            missed.append(predicted.astype(float).tolist())
            continue
        center = min(candidates, key=lambda value: value[0])[1]
        detected_camera.append(center.astype(float).tolist())
        matched_expected.append(expected.astype(float).tolist())

    detected = np.asarray(detected_camera, dtype=np.float64).reshape(-1, 2)
    expected = np.asarray(matched_expected, dtype=np.float64).reshape(-1, 2)
    residuals: list[float] = []
    detected_canvas: list[list[float]] = []
    if len(detected):
        projected = transform_points(detected, homography)
        detected_canvas = projected.astype(float).tolist()
        residuals = np.linalg.norm(projected - expected, axis=1).astype(float).tolist()
    return {
        "expected_total_count": int(len(expected_canvas)),
        "expected_visible_count": int(len(predicted_visible)),
        "detected_visible_count": int(len(detected_camera)),
        "visible_detection_fraction": (
            len(detected_camera) / len(predicted_visible) if predicted_visible else 0.0
        ),
        "nearest_neighbor_spacing_camera_px": spacing,
        "local_search_half_window_camera_px": half_window,
        "predicted_visible_camera_px": predicted_visible,
        "missed_predicted_camera_px": missed,
        "detected_camera_px": detected_camera,
        "detected_canvas_px": detected_canvas,
        "matched_expected_canvas_px": matched_expected,
        "active_homography_residuals_canvas_px": residuals,
        "active_homography_rms_canvas_px": (
            float(math.sqrt(float(np.mean(np.square(residuals))))) if residuals else None
        ),
        "active_homography_max_canvas_px": max(residuals) if residuals else None,
    }


def diagnostic_refit(
    primary: dict[str, Any], verification: dict[str, Any]
) -> dict[str, Any]:
    """Fit current primary observations only for read-only drift diagnosis."""
    detected = np.asarray(primary["detected_camera_px"], dtype=np.float64)
    expected = np.asarray(primary["matched_expected_canvas_px"], dtype=np.float64)
    unavailable = {
        "status": "unavailable",
        "authority_role": "diagnostic_only_not_a_candidate",
        "candidate_created": False,
        "promotion_allowed": False,
    }
    if len(detected) < 4 or len(expected) != len(detected):
        return {**unavailable, "error": "fewer_than_four_matched_primary_points"}
    matrix, _ = cv2.findHomography(detected, expected, 0)
    if matrix is None:
        return {**unavailable, "error": "homography_fit_failed"}

    def evaluate(metrics: dict[str, Any]) -> dict[str, Any]:
        camera = np.asarray(metrics["detected_camera_px"], dtype=np.float64)
        canvas = np.asarray(metrics["matched_expected_canvas_px"], dtype=np.float64)
        if not len(camera):
            return {"point_count": 0, "rms_canvas_px": None, "max_canvas_px": None}
        residuals = np.linalg.norm(transform_points(camera, matrix) - canvas, axis=1)
        return {
            "point_count": int(len(camera)),
            "rms_canvas_px": float(math.sqrt(float(np.mean(residuals ** 2)))),
            "max_canvas_px": float(residuals.max()),
            "residuals_canvas_px": residuals.astype(float).tolist(),
        }

    return {
        "status": "available",
        "authority_role": "diagnostic_only_not_a_candidate",
        "purpose": "distinguish_active_transform_drift_from_detection_failure",
        "candidate_created": False,
        "promotion_allowed": False,
        "must_not_be_loaded_by_runtime": True,
        "matrix_camera_native_px_to_final_display_canvas_px": (
            matrix.astype(float).tolist()
        ),
        "primary_support": evaluate(primary),
        "held_out_verification": evaluate(verification),
    }


def assess_operational_candidate(
    refit: dict[str, Any],
    maximum_rms_canvas_px: float,
    maximum_point_error_canvas_px: float,
) -> dict[str, Any]:
    """Assess the holder-plane fit without granting it candidate authority."""
    errors: list[str] = []
    if refit.get("status") != "available":
        errors.append(str(refit.get("error", "diagnostic_refit_unavailable")))
    for key, label in (
        ("primary_support", "primary support"),
        ("held_out_verification", "independent verification dots"),
    ):
        metrics = refit.get(key, {})
        count = int(metrics.get("point_count", 0))
        rms = metrics.get("rms_canvas_px")
        maximum = metrics.get("max_canvas_px")
        if count < 4:
            errors.append(f"{label} has fewer than four points")
        if rms is None or float(rms) > maximum_rms_canvas_px:
            errors.append(
                f"{label} RMS {rms!r} exceeds {maximum_rms_canvas_px} canvas px"
            )
        if maximum is None or float(maximum) > maximum_point_error_canvas_px:
            errors.append(
                f"{label} maximum {maximum!r} exceeds "
                f"{maximum_point_error_canvas_px} canvas px"
            )
    return {
        "status": "passed" if not errors else "failed",
        "errors": errors,
        "authority_role": "quality_evidence_for_future_operational_candidate",
        "candidate_created": False,
        "promotion_performed": False,
        "runtime_authority_changed": False,
        "quality_thresholds": {
            "maximum_rms_canvas_px": maximum_rms_canvas_px,
            "maximum_point_error_canvas_px": maximum_point_error_canvas_px,
        },
        "primary_support": refit.get("primary_support", {}),
        "held_out_verification": refit.get("held_out_verification", {}),
    }


def draw_reprojection_debug(
    image: np.ndarray,
    metrics: dict[str, Any],
    active_homography: np.ndarray,
    refit: dict[str, Any],
    label: str,
    output: Path,
) -> None:
    overlay = cv2.cvtColor(image, cv2.COLOR_GRAY2BGR)
    detected = np.asarray(metrics["detected_camera_px"], dtype=np.float64)
    expected = np.asarray(metrics["matched_expected_canvas_px"], dtype=np.float64)
    active_predicted = transform_points(expected, np.linalg.inv(active_homography))
    refit_predicted = np.empty((0, 2), dtype=np.float64)
    if refit.get("status") == "available":
        matrix = np.asarray(
            refit["matrix_camera_native_px_to_final_display_canvas_px"],
            dtype=np.float64,
        )
        refit_predicted = transform_points(expected, np.linalg.inv(matrix))

    for index, observed in enumerate(detected):
        observed_px = tuple(np.rint(observed).astype(int))
        active_px = tuple(np.rint(active_predicted[index]).astype(int))
        cv2.arrowedLine(
            overlay, active_px, observed_px, (0, 0, 255), 4, cv2.LINE_AA,
            tipLength=0.25,
        )
        cv2.drawMarker(
            overlay, active_px, (0, 255, 255), cv2.MARKER_CROSS, 28, 4
        )
        cv2.circle(overlay, observed_px, 11, (255, 255, 0), 4)
        if len(refit_predicted):
            refit_px = tuple(np.rint(refit_predicted[index]).astype(int))
            cv2.drawMarker(
                overlay, refit_px, (0, 255, 0), cv2.MARKER_TILTED_CROSS, 22, 3
            )

    active_rms = metrics.get("active_homography_rms_canvas_px")
    refit_key = (
        "primary_support" if label == "primary_support"
        else "held_out_verification"
    )
    refit_rms = refit.get(refit_key, {}).get("rms_canvas_px")
    lines = [
        f"{label}: active RMS={active_rms} canvas px; diagnostic refit RMS={refit_rms}",
        "cyan circle=observed; yellow +=active prediction; red arrow=active residual; green x=diagnostic refit",
        "DIAGNOSTIC REFIT ONLY - NOT A CANDIDATE - NOT RUNTIME AUTHORITY",
    ]
    for index, text in enumerate(lines):
        cv2.putText(
            overlay, text, (40, 70 + 58 * index), cv2.FONT_HERSHEY_SIMPLEX,
            1.05, (255, 255, 255), 3, cv2.LINE_AA,
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    if not cv2.imwrite(str(output), overlay):
        raise OSError(f"could not write homography QC image {output}")


def commissioning_debug_evidence(active: dict[str, Any]) -> dict[str, Any]:
    candidate_json = Path(str(active.get("candidate_json_path", "")))
    candidate_dir = candidate_json.parent
    evidence: dict[str, Any] = {}
    for name in (
        "detection_overlay.png",
        "reprojection_overlay.png",
        "coordinate_frame_evidence.png",
    ):
        path = candidate_dir / name
        evidence[name.removesuffix(".png")] = {
            "path": str(path.resolve()),
            "sha256": sha256_file(path) if path.is_file() else None,
            "available": path.is_file(),
        }
    return evidence


def persist_holder_fixture_evidence(
    report: dict[str, Any],
    manifest: dict[str, Any],
    result: dict[str, Any],
    report_path: Path,
    markdown_path: Path,
    markdown_contents: str,
) -> dict[str, Any]:
    """Archive holder overlays and QC beside their immutable source captures.

    The capture image set and its manifest are deliberately not rewritten: a
    Citrus candidate may already refer to their fingerprints. Holder analysis
    is instead a derived, checksummed observation below each camera artifact,
    plus a session-level manifest joining the per-camera products.
    """
    persistence = result.get("persistence", {})
    session_dir_text = str(
        manifest.get("calibration_session", {}).get("session_dir")
        or persistence.get("session_dir", "")
    )
    session_id = str(
        manifest.get("calibration_session", {}).get("session_id")
        or persistence.get("session_id", "")
    )
    if not session_dir_text or not session_id:
        raise ValueError("holder evidence persistence requires a calibration session")
    session_dir = Path(session_dir_text).resolve()
    if not (session_dir / "artifacts").is_dir():
        raise ValueError(f"calibration session artifact directory is missing: {session_dir}")
    safe_artifact_component(session_id, "session_id")
    run_id = safe_artifact_component(report_path.parent.name, "holder run id")
    session_package_dir = (
        session_dir / "derived" / "holder_fixture" / run_id
    )
    session_manifest_path = session_package_dir / "manifest.json"
    report["persisted_evidence"] = {
        "status": "persisted_with_source_session",
        "storage_policy": "derived_artifacts_do_not_mutate_source_image_set",
        "session_id": session_id,
        "session_dir": str(session_dir),
        "session_package_manifest_path": str(session_manifest_path),
        "per_camera_observation_schema_id": OBSERVATION_SCHEMA_ID,
    }

    observation_entries: list[dict[str, Any]] = []
    for camera in report.get("camera_results", []):
        camera_serial = safe_artifact_component(
            str(camera.get("camera_serial", "")), "camera serial"
        )
        arena_id = safe_artifact_component(
            str(camera.get("arena_id", "")), "arena id"
        )
        source_paths = [
            Path(str(source.get("path", ""))).resolve()
            for source in camera.get("source_images", {}).values()
        ]
        if not source_paths:
            raise ValueError(f"Cam{camera_serial} has no source captures")
        artifact_dirs = {path.parent.parent for path in source_paths}
        if len(artifact_dirs) != 1:
            raise ValueError(
                f"Cam{camera_serial} source captures span multiple artifacts"
            )
        artifact_dir = next(iter(artifact_dirs))
        relative_to_checked(
            artifact_dir, session_dir / "artifacts", "camera artifact directory"
        )
        if not (artifact_dir / "image_set.json").is_file():
            raise ValueError(f"source image_set.json is missing: {artifact_dir}")
        observation_dir = (
            artifact_dir / "derived" / "holder_fixture_observations" / run_id
        )
        observation_path = observation_dir / "observation.json"
        evidence_sources = {
            "holder_aperture_overlay": Path(str(camera["overlay_path"])),
            "active_primary_reprojection": Path(str(
                camera["homography_qc_images"]["active_primary_reprojection"]
            )),
            "active_heldout_reprojection": Path(str(
                camera["homography_qc_images"]["active_heldout_reprojection"]
            )),
        }
        evidence_names = {
            "holder_aperture_overlay": "holder_aperture_overlay.png",
            "active_primary_reprojection": "active_primary_reprojection.png",
            "active_heldout_reprojection": "active_heldout_reprojection.png",
        }
        evidence: dict[str, Any] = {}
        for key, source in evidence_sources.items():
            if not source.is_file():
                raise FileNotFoundError(f"holder evidence image is missing: {source}")
            destination = observation_dir / evidence_names[key]
            copy_file_atomic(source, destination)
            evidence[key] = {
                "path": destination.name,
                "sha256": sha256_file(destination),
                "media_type": "image/png",
            }

        source_captures: dict[str, Any] = {}
        for recipe, source in camera.get("source_images", {}).items():
            source_path = Path(str(source.get("path", ""))).resolve()
            relative_source = relative_to_checked(
                source_path, artifact_dir, f"{recipe} source capture"
            )
            source_captures[recipe] = {
                "path_relative_to_camera_artifact": relative_source.as_posix(),
                "declared_checksum": source.get("checksum"),
                "sha256": sha256_file(source_path),
                "capture_group_id": source.get("capture_group_id"),
                "camera_frame_id": source.get("camera_frame_id"),
                "local_frame_id": source.get("local_frame_id"),
                "camera_timestamp_ns": source.get("camera_timestamp_ns"),
                "camera_timestamp_clock_domain": source.get(
                    "camera_timestamp_clock_domain"
                ),
            }

        observation = {
            "schema_id": OBSERVATION_SCHEMA_ID,
            "schema_version": 1,
            "observation_id": f"{run_id}_Cam{camera_serial}_{arena_id}",
            "created_utc": report.get("created_utc"),
            "producer": "analyze_holder_fixture_validation.py",
            "status": camera.get("status"),
            "errors": camera.get("errors", []),
            "session": {
                "session_id": session_id,
                "session_dir": str(session_dir),
                "camera_artifact_id": artifact_dir.name,
                "camera_artifact_dir": str(artifact_dir),
            },
            "physical_state": manifest.get("fixture_state", {}),
            "authority_contract": report.get("authority_contract", {}),
            "coordinate_contract": report.get("coordinate_contract", {}),
            "source_captures": source_captures,
            "holder_fixture_geometry": camera.get("fixture_aperture", {}),
            "operational_candidate_assessment": camera.get(
                "operational_candidate_assessment", {}
            ),
            "commissioning_reference_comparison": camera.get(
                "commissioning_reference_comparison", {}
            ),
            "homography_evaluation": {
                "evaluation_input": camera.get("active_homography", {}),
                "primary_support": camera.get("primary_support", {}),
                "held_out_verification": camera.get("verification", {}),
                "diagnostic_refit": camera.get("diagnostic_refit", {}),
                "operational_candidate_assessment": camera.get(
                    "operational_candidate_assessment", {}
                ),
                "commissioning_reference_role": "evaluated_read_only_input",
                "operational_candidate_role": "not_created_by_this_analysis",
                "runtime_authority_changed": False,
                "persisted_citrus_candidate_set": report.get(
                    "citrus_homography_candidate", {}
                ),
            },
            "evidence": evidence,
            "source_validation_report": {
                "path": str(report_path.resolve()),
                "checksum_omitted_to_avoid_self_referential_artifact_graph": True,
            },
        }
        write_json_atomic(observation_path, observation)
        observation_sha256 = sha256_file(observation_path)
        relative_observation = relative_to_checked(
            observation_path, session_dir, "holder observation"
        )
        camera["persisted_observation"] = {
            "schema_id": OBSERVATION_SCHEMA_ID,
            "path": str(observation_path),
            "path_relative_to_session": relative_observation.as_posix(),
            "sha256": observation_sha256,
        }
        observation_entries.append({
            "camera_serial": camera_serial,
            "arena_id": arena_id,
            "status": camera.get("status"),
            "camera_artifact_id": artifact_dir.name,
            "observation_path": relative_observation.as_posix(),
            "observation_sha256": observation_sha256,
            "evidence": evidence,
        })

    # Write the final report only after its per-camera observation pointers and
    # checksums are stable. Copies below then remain byte-identical.
    write_json_atomic(report_path, report)
    write_text_atomic(markdown_path, markdown_contents)
    package_files = {
        "validation_report_json": (report_path, "validation_report.json"),
        "validation_report_markdown": (markdown_path, "validation_report.md"),
        "guided_capture_result": (
            Path(str(report["guided_capture_result_path"])),
            "guided_capture_result.json",
        ),
    }
    packaged_files: dict[str, Any] = {}
    for key, (source, filename) in package_files.items():
        destination = session_package_dir / filename
        copy_file_atomic(source, destination)
        packaged_files[key] = {
            "path": filename,
            "sha256": sha256_file(destination),
        }
    package = {
        "schema_id": EVIDENCE_PACKAGE_SCHEMA_ID,
        "schema_version": 1,
        "created_utc": utc_now(),
        "package_id": run_id,
        "producer": "analyze_holder_fixture_validation.py",
        "status": report.get("status"),
        "session_id": session_id,
        "session_dir": str(session_dir),
        "physical_state": manifest.get("fixture_state", {}),
        "authority_contract": report.get("authority_contract", {}),
        "operational_candidate_assessment": report.get(
            "operational_candidate_assessment", {}
        ),
        "commissioning_reference_comparison": report.get(
            "commissioning_reference_comparison", {}
        ),
        "citrus_homography_candidate": report.get(
            "citrus_homography_candidate", {}
        ),
        "source_image_sets_are_immutable": True,
        "source_image_sets_modified": False,
        "files": packaged_files,
        "camera_observations": observation_entries,
    }
    write_json_atomic(session_manifest_path, package)
    return {
        "session_manifest_path": str(session_manifest_path),
        "session_manifest_sha256": sha256_file(session_manifest_path),
        "camera_observation_count": len(observation_entries),
    }


def draw_overlay(
    image: np.ndarray,
    contour: np.ndarray,
    boundary_camera: np.ndarray,
    boundary_classification: dict[str, Any],
    analyses: list[tuple[str, dict[str, Any]]],
    output: Path,
    status: str,
) -> None:
    overlay = cv2.cvtColor(image, cv2.COLOR_GRAY2BGR)
    # The entire contour is observed illuminated support, not necessarily the
    # holder itself. Keep it neutral; reserve green for holder arcs that are
    # not explained by configured-arena or sensor clipping.
    cv2.drawContours(overlay, [contour], -1, (160, 160, 160), 4)
    arena_clipped = boundary_classification["arena_clipped_mask"]
    sensor_clipped = boundary_classification["sensor_clipped_mask"]
    aperture_observed = boundary_classification["aperture_observed_mask"]
    for index, point in enumerate(boundary_camera):
        center = (int(round(point[0])), int(round(point[1])))
        if aperture_observed[index]:
            color = (0, 255, 0)
        elif arena_clipped[index]:
            color = (0, 190, 255)
        elif sensor_clipped[index]:
            color = (255, 160, 0)
        else:
            continue
        cv2.circle(overlay, center, 5, color, cv2.FILLED)
    colors = ((255, 255, 0), (255, 0, 255))
    for (label, metrics), color in zip(analyses, colors):
        predicted = metrics.get("predicted_visible_camera_px", [])
        detected = metrics.get("detected_camera_px", [])
        for point in predicted:
            cv2.drawMarker(
                overlay,
                (int(round(point[0])), int(round(point[1]))),
                (255, 255, 0),
                cv2.MARKER_CROSS,
                24,
                3,
            )
        for point in detected:
            cv2.circle(
                overlay,
                (int(round(point[0])), int(round(point[1]))),
                10,
                color,
                3,
            )
        text = (
            f"{label}: {metrics.get('detected_visible_count')}/"
            f"{metrics.get('expected_visible_count')} visible, "
            f"RMS={metrics.get('active_homography_rms_canvas_px')} canvas px"
        )
        cv2.putText(
            overlay, text, (40, 90 + 70 * analyses.index((label, metrics))),
            cv2.FONT_HERSHEY_SIMPLEX, 1.25, color, 3, cv2.LINE_AA
        )
    cv2.putText(
        overlay,
        "boundary: green=observed holder arc, gold=arena clip, gray=illuminated support",
        (40, image.shape[0] - 110),
        cv2.FONT_HERSHEY_SIMPLEX, 1.05, (220, 220, 220), 3, cv2.LINE_AA
    )
    cv2.putText(
        overlay, f"holder fixture validation: {status}", (40, image.shape[0] - 50),
        cv2.FONT_HERSHEY_SIMPLEX, 1.5,
        (0, 255, 0) if status == "pass" else (0, 0, 255), 4, cv2.LINE_AA
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    if not cv2.imwrite(str(output), overlay):
        raise OSError(f"could not write overlay {output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--max-rms-canvas-px", type=float, default=0.75)
    parser.add_argument("--max-point-error-canvas-px", type=float, default=1.5)
    parser.add_argument("--min-visible-detection-fraction", type=float, default=0.95)
    parser.add_argument("--edge-margin-camera-px", type=float, default=12.0)
    parser.add_argument("--arena-edge-margin-canvas-px", type=float, default=3.0)
    parser.add_argument("--sensor-edge-margin-camera-px", type=float, default=12.0)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest.get("schema_id") != "orange.holder_fixture_validation.run_manifest":
        parser.error("input is not a holder fixture validation manifest")
    result_path = Path(manifest["inputs"]["guided_capture_result_path"])
    citrus_path = Path(manifest["inputs"]["citrus_canvas_path"])
    result = json.loads(result_path.read_text(encoding="utf-8"))
    citrus = json.loads(citrus_path.read_text(encoding="utf-8"))
    cameras = [str(value) for value in manifest["capture"]["camera_serials"]]
    aperture_shape = str(manifest["fixture_state"]["aperture_shape"])
    primary_recipe = str(manifest["projection"]["primary_support_recipe"])
    expected_sequence = manifest["projection"]["recipe_sequence"]
    if result.get("status") != "pass":
        parser.error("guided capture result did not pass")
    if result.get("config", {}).get("workflow_profile_id") != \
            "holder_installed_projected_surface":
        parser.error("guided result is not the holder-installed workflow profile")
    if result.get("config", {}).get("recipe_sequence") != expected_sequence:
        parser.error("guided result recipe sequence does not match the run manifest")
    if result.get("config", {}).get("fixture_aperture_shape") != aperture_shape:
        parser.error("guided result aperture shape does not match the run manifest")
    if result.get("session_policy") != "one_session_per_recipe_sequence":
        parser.error("guided result does not declare one session per recipe sequence")
    captured_active_inputs = {
        str(item.get("camera_serial")): item
        for item in manifest.get("inputs", {}).get(
            "active_homographies_before_capture", []
        )
        if isinstance(item, dict)
    }

    samples = {
        recipe: sample_result(result, recipe)
        for recipe in ("black_reference", "uniform_gray", primary_recipe, "verification_dots")
    }
    camera_reports: list[dict[str, Any]] = []
    overall_errors: list[str] = []
    reference_comparison_errors: list[str] = []
    citrus_sha256 = sha256_file(citrus_path)
    if manifest["inputs"].get("citrus_canvas_sha256_before_capture") != citrus_sha256:
        overall_errors.append(
            "Citrus canvas changed between the pre-capture snapshot and analysis"
        )
    for camera in cameras:
        camera_errors: list[str] = []
        camera_reference_errors: list[str] = []
        image_paths: dict[str, Path] = {}
        image_metadata: dict[str, dict[str, Any]] = {}
        images: dict[str, np.ndarray] = {}
        for recipe, sample in samples.items():
            path, metadata = locate_capture(sample, camera)
            image = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
            if image is None:
                raise OSError(f"OpenCV could not read {path}")
            image_paths[recipe] = path
            image_metadata[recipe] = metadata
            images[recipe] = image
        target = target_for_camera(samples[primary_recipe], camera)
        arena_id = str(target["arena_id"])
        arena_config = citrus.get("arenas", {}).get(arena_id, {})
        active_pointer = (
            citrus_path.parent / "calibration_artifacts" /
            f"homography_active_{arena_id}_{camera}.json"
        )
        if not active_pointer.is_file():
            raise FileNotFoundError(f"active homography pointer is missing: {active_pointer}")
        active = json.loads(active_pointer.read_text(encoding="utf-8"))
        if str(active.get("camera_id")) != camera or str(active.get("arena_id")) != arena_id:
            raise ValueError(f"active pointer identity mismatch for camera {camera}")
        captured_active = captured_active_inputs.get(camera, {})
        pointer_sha256 = sha256_file(active_pointer)
        if captured_active.get("pointer_sha256_before_capture") != pointer_sha256:
            camera_errors.append(
                "active homography pointer changed between pre-capture snapshot and analysis"
            )
        homography = np.asarray(active["homography_matrix"], dtype=np.float64)

        contour, _, segmentation = segment_aperture(
            images["uniform_gray"], images["black_reference"]
        )
        boundary_camera = resample_contour(contour, maximum_points=512)
        boundary_classification = classify_visibility_boundary(
            boundary_camera,
            homography,
            target,
            images["uniform_gray"].shape,
            args.arena_edge_margin_canvas_px,
            args.sensor_edge_margin_camera_px,
        )
        boundary_canvas = boundary_classification["boundary_canvas"]
        aperture_mask = boundary_classification["aperture_observed_mask"]
        arena_clip_mask = boundary_classification["arena_clipped_mask"]
        sensor_clip_mask = boundary_classification["sensor_clipped_mask"]
        aperture_boundary_camera = boundary_camera[aperture_mask]
        aperture_boundary_canvas = boundary_canvas[aperture_mask]
        segmentation_pass = 0.05 <= segmentation["area_fraction_of_sensor"] <= 0.98
        if not segmentation_pass:
            camera_errors.append(
                "aperture area fraction is outside the plausible 0.05..0.98 range"
            )

        if primary_recipe == "homography_grid":
            primary_expected = expected_grid_points(target)
        else:
            primary_expected = expected_ring_points(target, arena_config)
        verification_target = target_for_camera(samples["verification_dots"], camera)
        verification_expected = expected_verification_points(
            verification_target, arena_config
        )
        primary = detect_expected_dots(
            images[primary_recipe], images["black_reference"], primary_expected,
            homography, contour, args.edge_margin_camera_px,
        )
        verification = detect_expected_dots(
            images["verification_dots"], images["black_reference"],
            verification_expected, homography, contour, args.edge_margin_camera_px,
        )
        refit = diagnostic_refit(primary, verification)
        operational_candidate_assessment = assess_operational_candidate(
            refit,
            args.max_rms_canvas_px,
            args.max_point_error_canvas_px,
        )
        camera_errors.extend(
            str(error)
            for error in operational_candidate_assessment.get("errors", [])
        )
        for label, metrics in (("primary_support", primary), ("verification", verification)):
            if metrics["expected_visible_count"] < 4:
                camera_errors.append(f"{label} has fewer than four visible expected points")
            if metrics["visible_detection_fraction"] < args.min_visible_detection_fraction:
                camera_errors.append(
                    f"{label} visible detection fraction "
                    f"{metrics['visible_detection_fraction']:.3f} is below "
                    f"{args.min_visible_detection_fraction:.3f}"
                )
            rms = metrics["active_homography_rms_canvas_px"]
            maximum = metrics["active_homography_max_canvas_px"]
            if rms is None or rms > args.max_rms_canvas_px:
                camera_reference_errors.append(
                    f"{label} active-homography RMS {rms!r} exceeds "
                    f"{args.max_rms_canvas_px} canvas px"
                )
            if maximum is None or maximum > args.max_point_error_canvas_px:
                camera_reference_errors.append(
                    f"{label} maximum error {maximum!r} exceeds "
                    f"{args.max_point_error_canvas_px} canvas px"
                )

        status = "pass" if not camera_errors else "fail"
        reference_status = (
            "within_tolerance"
            if not camera_reference_errors
            else "mismatch_detected"
        )
        overlay_path = args.manifest.parent / "overlays" / f"Cam{camera}.png"
        draw_overlay(
            images["uniform_gray"], contour, boundary_camera,
            boundary_classification,
            [(primary_recipe, primary), ("verification_dots", verification)],
            overlay_path, status,
        )
        primary_qc_path = (
            args.manifest.parent / "homography_qc" /
            f"Cam{camera}_active_primary_reprojection.png"
        )
        verification_qc_path = (
            args.manifest.parent / "homography_qc" /
            f"Cam{camera}_active_heldout_reprojection.png"
        )
        draw_reprojection_debug(
            images[primary_recipe], primary, homography, refit,
            "primary_support", primary_qc_path,
        )
        draw_reprojection_debug(
            images["verification_dots"], verification, homography, refit,
            "held_out_verification", verification_qc_path,
        )
        camera_report = {
            "camera_serial": camera,
            "arena_id": arena_id,
            "status": status,
            "errors": camera_errors,
            "commissioning_reference_comparison": {
                "status": reference_status,
                "within_operational_tolerance": not camera_reference_errors,
                "findings": camera_reference_errors,
                "authority_effect": "diagnostic_only",
                "commissioning_reference_modified": False,
            },
            "active_homography": {
                "pointer_path": str(active_pointer.resolve()),
                "pointer_sha256": pointer_sha256,
                "pointer_sha256_before_capture": captured_active.get(
                    "pointer_sha256_before_capture"
                ),
                "candidate_id": active.get("candidate_id"),
                "candidate_set_id": active.get("candidate_set_id"),
                "accepted_at_utc": active.get("accepted_at_utc"),
                "direction": active.get("homography_direction"),
                "matrix": active.get("homography_matrix"),
                "used_read_only": True,
                "commissioning_candidate_debug_evidence": (
                    commissioning_debug_evidence(active)
                ),
            },
            "source_images": {
                recipe: {
                    "path": str(image_paths[recipe].resolve()),
                    "checksum": image_metadata[recipe].get("checksum"),
                    "capture_group_id": image_metadata[recipe].get("capture", {}).get(
                        "capture_group_id"
                    ),
                    "camera_frame_id": image_metadata[recipe].get("capture", {}).get(
                        "last_camera_frame_id"
                    ),
                    "local_frame_id": image_metadata[recipe].get("capture", {}).get(
                        "last_local_frame_id"
                    ),
                    "camera_timestamp_ns": image_metadata[recipe].get(
                        "capture", {}
                    ).get("camera_timestamp_ns"),
                    "camera_timestamp_clock_domain": image_metadata[recipe].get(
                        "capture", {}
                    ).get("camera_timestamp_clock_domain"),
                }
                for recipe in image_paths
            },
            "fixture_aperture": {
                "semantic_role": "fixture_visibility_aperture",
                "shape": aperture_shape,
                "evidence_scope": "observed_boundary_arcs_only",
                "visibility_model": "configured_arena_intersect_fixture_aperture_intersect_camera_sensor",
                "coordinate_contract": {
                    "camera_boundary_space": "camera_native_px",
                    "canvas_boundary_space": "final_display_canvas_px",
                    "canvas_transform_source": "active_dry_commissioning_homography",
                },
                "segmentation": segmentation,
                "observed_illuminated_support": {
                    "semantic_role": "configured_arena_intersect_fixture_aperture_intersect_camera_sensor",
                    "boundary_camera_px": boundary_camera.astype(float).tolist(),
                    "boundary_canvas_px": boundary_canvas.astype(float).tolist(),
                },
                "boundary_classification": {
                    "arena_edge_margin_canvas_px": boundary_classification[
                        "arena_edge_margin_canvas_px"
                    ],
                    "sensor_edge_margin_camera_px": boundary_classification[
                        "sensor_edge_margin_camera_px"
                    ],
                    "configured_arena_bounds_canvas_px": boundary_classification[
                        "configured_arena_bounds_canvas_px"
                    ],
                    "sample_count": int(len(boundary_camera)),
                    "observed_aperture_sample_count": int(np.count_nonzero(aperture_mask)),
                    "configured_arena_clip_sample_count": int(np.count_nonzero(arena_clip_mask)),
                    "camera_sensor_clip_sample_count": int(np.count_nonzero(sensor_clip_mask)),
                },
                "observed_aperture_boundary": {
                    "status": (
                        "partial_observation"
                        if len(aperture_boundary_camera) >= 5
                        else "insufficient_evidence"
                    ),
                    "boundary_camera_px": aperture_boundary_camera.astype(float).tolist(),
                    "boundary_canvas_px": aperture_boundary_canvas.astype(float).tolist(),
                    "camera_shape_fit_diagnostic_only": (
                        geometry_for_shape(aperture_boundary_camera, aperture_shape)
                        if len(aperture_boundary_camera) >= 5 else None
                    ),
                    "canvas_shape_fit_diagnostic_only": (
                        geometry_for_shape(aperture_boundary_canvas, aperture_shape)
                        if len(aperture_boundary_canvas) >= 5 else None
                    ),
                    "unobserved_boundary_must_not_be_inferred_as_measured": True,
                },
                "distinct_from_experimental_area": True,
                "distinct_from_dish_inner_rim": True,
            },
            "primary_support": {"recipe": primary_recipe, **primary},
            "verification": {"recipe": "verification_dots", **verification},
            "diagnostic_refit": refit,
            "operational_candidate_assessment": operational_candidate_assessment,
            "homography_qc_images": {
                "active_primary_reprojection": str(primary_qc_path.resolve()),
                "active_heldout_reprojection": str(
                    verification_qc_path.resolve()
                ),
            },
            "overlay_path": str(overlay_path.resolve()),
        }
        camera_reports.append(camera_report)
        overall_errors.extend(f"Cam{camera}: {error}" for error in camera_errors)
        reference_comparison_errors.extend(
            f"Cam{camera}: {error}" for error in camera_reference_errors
        )

    operational_status = (
        "passed"
        if camera_reports and all(
            camera.get("operational_candidate_assessment", {}).get("status")
            == "passed"
            for camera in camera_reports
        )
        else "failed"
    )
    report = {
        "schema_id": REPORT_SCHEMA_ID,
        "schema_version": 3,
        "created_utc": utc_now(),
        "status": (
            "pass"
            if not overall_errors and operational_status == "passed"
            else "fail"
        ),
        "errors": overall_errors,
        "commissioning_reference_comparison": {
            "status": (
                "within_tolerance"
                if not reference_comparison_errors
                else "mismatch_detected"
            ),
            "within_operational_tolerance": not reference_comparison_errors,
            "findings": reference_comparison_errors,
            "authority_effect": "diagnostic_only",
            "commissioning_reference_modified": False,
            "interpretation": (
                "The dry commissioning reference remains provenance evidence; "
                "the holder-plane candidate is judged independently."
            ),
        },
        "run_manifest_path": str(args.manifest.resolve()),
        "guided_capture_result_path": str(result_path.resolve()),
        "citrus_canvas_path": str(citrus_path.resolve()),
        "citrus_canvas_sha256": citrus_sha256,
        "citrus_canvas_sha256_before_capture": manifest["inputs"].get(
            "citrus_canvas_sha256_before_capture"
        ),
        "authority_contract": manifest["authority_contract"],
        "citrus_homography_candidate": result.get("homography", {}),
        "coordinate_contract": {
            "camera_native_px": {"origin": "top_left", "positive_x": "right", "positive_y": "down"},
            "final_display_canvas_px": {"origin": "top_left", "positive_x": "right", "positive_y": "down"},
            "homography_direction": "camera_native_px_to_final_display_canvas_px",
        },
        "quality_thresholds": {
            "maximum_active_homography_rms_canvas_px": args.max_rms_canvas_px,
            "maximum_active_homography_point_error_canvas_px": args.max_point_error_canvas_px,
            "minimum_visible_detection_fraction": args.min_visible_detection_fraction,
            "aperture_edge_margin_camera_px": args.edge_margin_camera_px,
            "arena_edge_classification_margin_canvas_px": (
                args.arena_edge_margin_canvas_px
            ),
            "sensor_edge_classification_margin_camera_px": (
                args.sensor_edge_margin_camera_px
            ),
        },
        "camera_results": camera_reports,
        "operational_candidate_assessment": {
            "status": operational_status,
            "camera_count": len(camera_reports),
            "all_cameras_require_explicit_citrus_candidate_fit_and_promotion": True,
            "runtime_authority_changed": False,
        },
        "mutation_summary": {
            "homography_candidates_created": (
                len(result.get("homography", {}).get("targets", []))
                if result.get("homography", {}).get("fit_requested", False)
                else 0
            ),
            "homographies_promoted": 0,
            "citrus_canvas_modified": False,
        },
    }
    report_path = args.manifest.parent / "validation_report.json"
    markdown = args.manifest.parent / "validation_report.md"
    lines = [
        "# Holder Fixture Validation",
        "",
        f"Status: **{report['status'].upper()}**",
        "",
        "The installed holder aperture was characterized separately from the "
        "experimental area and dish inner rim. Active dry homographies were read-only inputs.",
        "",
        "| Camera | Aperture | Active primary RMS | Active verification RMS | Holder-plane candidate verification RMS | Reference comparison | Operational status |",
        "|---|---|---:|---:|---:|---|---|",
    ]
    for camera in camera_reports:
        lines.append(
            f"| {camera['camera_serial']} | {aperture_shape} | "
            f"{camera['primary_support']['active_homography_rms_canvas_px']} | "
            f"{camera['verification']['active_homography_rms_canvas_px']} | "
            f"{camera['operational_candidate_assessment'].get('held_out_verification', {}).get('rms_canvas_px')} | "
            f"{camera['commissioning_reference_comparison']['status']} | "
            f"{camera['status']} |"
        )
    lines.extend([
        "",
        "Operational holder-plane candidate assessment: "
        f"**{report['operational_candidate_assessment']['status'].upper()}**. "
        "This is checksummed quality evidence only; Citrus must still create, "
        "review, and explicitly promote a candidate.",
    ])
    if overall_errors:
        lines.extend(
            ["", "## Operational quality failures", ""]
            + [f"- {error}" for error in overall_errors]
        )
    if reference_comparison_errors:
        lines.extend(
            [
                "",
                "## Dry commissioning reference mismatch (diagnostic)",
                "",
                "These findings do not reject an independently passing holder-plane "
                "candidate and do not mutate the commissioning reference.",
                "",
            ]
            + [f"- {error}" for error in reference_comparison_errors]
        )
    persistence_result = persist_holder_fixture_evidence(
        report,
        manifest,
        result,
        report_path,
        markdown,
        "\n".join(lines) + "\n",
    )
    print(f"{report['status'].upper()}: holder fixture validation report={report_path}")
    print(
        "Persisted holder evidence: "
        f"{persistence_result['camera_observation_count']} camera observations; "
        f"manifest={persistence_result['session_manifest_path']}"
    )
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
