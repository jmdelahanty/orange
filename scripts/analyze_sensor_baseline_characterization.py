#!/usr/bin/env python3
"""Analyze bounded Mono8 production-baseline dark and flat captures."""

from __future__ import annotations

import argparse
import bisect
import csv
import hashlib
import json
import math
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    import numpy as np
except ImportError as error:  # pragma: no cover - exercised by deployment environment
    raise SystemExit(
        "NumPy is required. Run with: conda run -n juicebox python "
        f"{__file__} <run_manifest.json> ({error})"
    )


MANIFEST_SCHEMA_ID = "orange.sensor_baseline_characterization.run_manifest"
REPORT_SCHEMA_ID = "orange.sensor_baseline_characterization.report"
def utc_now() -> str:
    return (
        datetime.now(timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z")
    )


def utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise ValueError(f"expected object JSON in {path}")
    return payload


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def read_index(path: Path) -> list[dict[str, int]]:
    rows: list[dict[str, int]] = []
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        required = {
            "reference_frame_index",
            "recording_frame_id",
            "timestamp",
            "timestamp_sys",
            "byte_offset",
            "byte_size",
        }
        if set(reader.fieldnames or ()) != required:
            raise ValueError(f"unexpected index columns in {path}: {reader.fieldnames}")
        for raw in reader:
            rows.append({key: int(raw[key]) for key in required})
    for index, row in enumerate(rows):
        if row["reference_frame_index"] != index:
            raise ValueError(f"non-contiguous reference_frame_index in {path}")
    return rows


def validate_applied_pipeline(
    snapshot: dict[str, Any], serial: str, expected_features: dict[str, Any]
) -> tuple[dict[str, Any], list[str]]:
    try:
        pipeline = snapshot["camera_runtime"][serial]["sensor_pipeline"]
    except (KeyError, TypeError) as error:
        raise ValueError(
            f"recording snapshot lacks camera_runtime.{serial}.sensor_pipeline"
        ) from error
    if pipeline.get("schema_id") != "orange.camera.sensor_pipeline_state":
        raise ValueError(f"Cam{serial} has an unrecognized sensor-pipeline schema")
    if pipeline.get("capture_stage") != "post_configuration_pre_stream":
        raise ValueError(
            f"Cam{serial} sensor pipeline was not captured post-configuration/pre-stream"
        )
    features = pipeline.get("features")
    if not isinstance(features, dict):
        raise ValueError(f"Cam{serial} sensor pipeline has no feature object")
    mismatches: list[str] = []
    actual: dict[str, Any] = {}
    for name, expected in expected_features.items():
        feature = features.get(name)
        if not isinstance(feature, dict) or feature.get("status") != "readable":
            mismatches.append(f"{name}=unreadable")
            continue
        value = feature.get("value")
        actual[name] = value
        if value != expected:
            mismatches.append(f"{name}={value!r} expected {expected!r}")
    return actual, mismatches


def downsample_mean(image: np.ndarray, block: int) -> np.ndarray:
    height = (image.shape[0] // block) * block
    width = (image.shape[1] // block) * block
    cropped = image[:height, :width]
    return cropped.reshape(
        height // block, block, width // block, block
    ).mean(axis=(1, 3), dtype=np.float64)


def write_pgm(path: Path, image_u8: np.ndarray) -> None:
    if image_u8.dtype != np.uint8 or image_u8.ndim != 2:
        raise ValueError("PGM image must be a two-dimensional uint8 array")
    header = f"P5\n{image_u8.shape[1]} {image_u8.shape[0]}\n255\n".encode("ascii")
    path.write_bytes(header + image_u8.tobytes(order="C"))


def frame_view(
    raw: np.memmap,
    row: dict[str, int],
    *,
    height: int,
    pitch: int,
    width: int,
) -> tuple[np.ndarray, np.ndarray]:
    start = row["byte_offset"]
    stop = start + row["byte_size"]
    payload = raw[start:stop]
    y_bytes = pitch * height
    if payload.size < y_bytes:
        raise ValueError("NV12 payload is shorter than its declared Y plane")
    y = payload[:y_bytes].reshape(height, pitch)[:, :width]
    uv = payload[y_bytes:]
    return y, uv


def analyze_camera_capture(
    *,
    phase: str,
    serial: str,
    artifacts: dict[str, Any],
    expected_frames: int,
    expected_fps: int,
    expected_features: dict[str, Any],
    output_dir: Path,
) -> dict[str, Any]:
    raw_path = Path(str(artifacts["raw_dump"]))
    index_path = Path(str(artifacts["index"]))
    metadata_path = Path(str(artifacts["metadata"]))
    snapshot_path = Path(str(artifacts["recording_snapshot"]))
    for path in (raw_path, index_path, metadata_path, snapshot_path):
        if not path.is_file():
            raise ValueError(f"missing Cam{serial} artifact: {path}")

    metadata = read_json(metadata_path)
    snapshot = read_json(snapshot_path)
    applied_state, state_mismatches = validate_applied_pipeline(
        snapshot, serial, expected_features
    )
    if state_mismatches:
        raise ValueError(
            f"Cam{serial} did not use the production sensor state: "
            + "; ".join(state_mismatches)
        )

    width = int(metadata.get("width", 0))
    height = int(metadata.get("height", 0))
    pitch = int(metadata.get("pitch", 0))
    frame_size = int(metadata.get("frame_size", 0))
    if metadata.get("pixel_format") != "nv12":
        raise ValueError(f"Cam{serial} reference is not NV12")
    if metadata.get("source_path_flavor") != "mono":
        raise ValueError(f"Cam{serial} reference is not the Mono8 source path")
    if metadata.get("resize_enabled") is not False:
        raise ValueError(f"Cam{serial} reference was resized")
    expected_width = int(expected_features["Width"])
    expected_height = int(expected_features["Height"])
    if width != expected_width or height != expected_height or pitch < width:
        raise ValueError(
            f"Cam{serial} unexpected reference geometry {width}x{height} pitch={pitch}"
        )
    if frame_size != pitch * height * 3 // 2:
        raise ValueError(f"Cam{serial} NV12 frame-size contract mismatch")

    rows = read_index(index_path)
    if len(rows) != expected_frames:
        raise ValueError(
            f"Cam{serial} captured {len(rows)} frames, expected {expected_frames}"
        )
    expected_offset = 0
    for row in rows:
        if row["byte_offset"] != expected_offset or row["byte_size"] != frame_size:
            raise ValueError(f"Cam{serial} raw index byte layout is not contiguous")
        expected_offset += frame_size
    if raw_path.stat().st_size != expected_offset:
        raise ValueError(
            f"Cam{serial} raw size {raw_path.stat().st_size} != indexed {expected_offset}"
        )

    timestamps = [row["timestamp"] for row in rows]
    if any(right <= left for left, right in zip(timestamps, timestamps[1:])):
        raise ValueError(f"Cam{serial} camera timestamps are not strictly increasing")
    expected_period_ns = 1_000_000_000.0 / expected_fps
    cadence_deltas = [right - left for left, right in zip(timestamps, timestamps[1:])]
    cadence_valid = [
        0.5 * expected_period_ns <= delta <= 1.5 * expected_period_ns
        for delta in cadence_deltas
    ]
    if not all(cadence_valid):
        raise ValueError(
            f"Cam{serial} reference sequence is not consecutive at {expected_fps} Hz"
        )

    raw = np.memmap(raw_path, dtype=np.uint8, mode="r")
    frame_means: list[float] = []
    frame_spatial_std: list[float] = []
    sampled: list[np.ndarray] = []
    tile_means: list[np.ndarray] = []
    tile_diff_sums = np.zeros((8, 8), dtype=np.float64)
    tile_diff_squares = np.zeros((8, 8), dtype=np.float64)
    tile_diff_counts = np.zeros((8, 8), dtype=np.int64)
    diff_sum = 0.0
    diff_square_sum = 0.0
    diff_count = 0
    dark_zero_count = 0
    low_count = 0
    high_count = 0
    saturated_count = 0
    total_active_pixels = 0
    uv_min = 255
    uv_max = 0
    preview_mean_sum: np.ndarray | None = None
    preview_diff_square_sum: np.ndarray | None = None
    valid_pair_count = 0

    for pair_start in range(0, len(rows), 2):
        first, first_uv = frame_view(
            raw, rows[pair_start], height=height, pitch=pitch, width=width
        )
        second, second_uv = frame_view(
            raw, rows[pair_start + 1], height=height, pitch=pitch, width=width
        )
        for frame, uv in ((first, first_uv), (second, second_uv)):
            frame_means.append(float(frame.mean(dtype=np.float64)))
            frame_spatial_std.append(float(frame.std(dtype=np.float64)))
            sampled.append(np.asarray(frame[::16, ::16]).reshape(-1).copy())
            tile_means.append(
                frame.reshape(8, height // 8, 8, width // 8).mean(
                    axis=(1, 3), dtype=np.float64
                )
            )
            dark_zero_count += int(np.count_nonzero(frame == 0))
            low_count += int(np.count_nonzero(frame <= 5))
            high_count += int(np.count_nonzero(frame >= 250))
            saturated_count += int(np.count_nonzero(frame == 255))
            total_active_pixels += frame.size
            uv_min = min(uv_min, int(uv.min(initial=255)))
            uv_max = max(uv_max, int(uv.max(initial=0)))
            preview = downsample_mean(frame, 16)
            if preview_mean_sum is None:
                preview_mean_sum = np.zeros_like(preview, dtype=np.float64)
            preview_mean_sum += preview

        diff = second.astype(np.int16) - first.astype(np.int16)
        diff_square = np.square(diff, dtype=np.float32)
        diff_sum += float(diff.sum(dtype=np.float64))
        diff_square_sum += float(diff_square.sum(dtype=np.float64))
        diff_count += diff.size
        tile_diff_sums += diff.reshape(8, height // 8, 8, width // 8).sum(
            axis=(1, 3), dtype=np.float64
        )
        tile_diff_squares += diff_square.reshape(
            8, height // 8, 8, width // 8
        ).sum(axis=(1, 3), dtype=np.float64)
        tile_diff_counts += (height // 8) * (width // 8)
        diff_preview = downsample_mean(diff_square, 16)
        if preview_diff_square_sum is None:
            preview_diff_square_sum = np.zeros_like(diff_preview, dtype=np.float64)
        preview_diff_square_sum += diff_preview
        valid_pair_count += 1

    if uv_min != 128 or uv_max != 128:
        raise ValueError(
            f"Cam{serial} neutral-chroma contract failed: UV range {uv_min}..{uv_max}"
        )
    if diff_count <= 0 or preview_mean_sum is None or preview_diff_square_sum is None:
        raise ValueError(f"Cam{serial} has no valid temporal pairs")

    diff_mean = diff_sum / diff_count
    diff_variance = max(0.0, diff_square_sum / diff_count - diff_mean * diff_mean)
    temporal_sigma = math.sqrt(diff_variance / 2.0)
    tile_mean_array = np.stack(tile_means, axis=0)
    tile_capture_mean = tile_mean_array.mean(axis=0)
    tile_frame_mean_sd = tile_mean_array.std(axis=0)
    tile_diff_mean = tile_diff_sums / tile_diff_counts
    tile_diff_variance = np.maximum(
        0.0,
        tile_diff_squares / tile_diff_counts - np.square(tile_diff_mean),
    )
    tile_temporal_sigma = np.sqrt(tile_diff_variance / 2.0)

    samples = np.concatenate(sampled).astype(np.uint8, copy=False)
    percentiles = {
        str(percentile): float(np.percentile(samples, percentile))
        for percentile in (0.1, 1.0, 50.0, 99.0, 99.9)
    }
    frame_mean_array = np.asarray(frame_means, dtype=np.float64)
    mean_preview = np.clip(
        np.rint(preview_mean_sum / len(rows)), 0, 255
    ).astype(np.uint8)
    sigma_preview_dn = np.sqrt(
        np.maximum(0.0, preview_diff_square_sum / valid_pair_count) / 2.0
    )
    sigma_preview_scale_max_dn = 16.0
    sigma_preview = np.clip(
        np.rint(sigma_preview_dn / sigma_preview_scale_max_dn * 255.0),
        0,
        255,
    ).astype(np.uint8)

    camera_dir = output_dir / phase / f"Cam{serial}"
    camera_dir.mkdir(parents=True, exist_ok=False)
    mean_preview_path = camera_dir / "mean_luma_preview_block16.pgm"
    sigma_preview_path = camera_dir / "temporal_sigma_preview_block16.pgm"
    write_pgm(mean_preview_path, mean_preview)
    write_pgm(sigma_preview_path, sigma_preview)
    tile_csv_path = camera_dir / "tile_metrics.csv"
    with tile_csv_path.open("x", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "phase",
                "camera_serial",
                "tile_y",
                "tile_x",
                "mean_dn",
                "frame_mean_sd_dn",
                "paired_temporal_sigma_dn",
            ]
        )
        for tile_y in range(8):
            for tile_x in range(8):
                writer.writerow(
                    [
                        phase,
                        serial,
                        tile_y,
                        tile_x,
                        f"{tile_capture_mean[tile_y, tile_x]:.9f}",
                        f"{tile_frame_mean_sd[tile_y, tile_x]:.9f}",
                        f"{tile_temporal_sigma[tile_y, tile_x]:.9f}",
                    ]
                )

    warnings: list[str] = []
    zero_fraction = dark_zero_count / total_active_pixels
    high_fraction = high_count / total_active_pixels
    saturated_fraction = saturated_count / total_active_pixels
    if phase == "dark" and zero_fraction > 0.01:
        warnings.append(
            "More than 1% of dark samples are exactly zero; black clipping may bias "
            "the dark-noise estimate."
        )
    if phase == "flat" and high_fraction > 0.005:
        warnings.append(
            "More than 0.5% of flat samples are >=250 DN; the field may be too close "
            "to Mono8 clipping."
        )

    result: dict[str, Any] = {
        "schema_id": "orange.sensor_baseline_characterization.camera_metrics",
        "schema_version": 1,
        "phase": phase,
        "camera_serial": serial,
        "status": "valid_measurement",
        "applied_sensor_pipeline": applied_state,
        "capture": {
            "frames": len(rows),
            "temporal_pairs": valid_pair_count,
            "width": width,
            "height": height,
            "pitch": pitch,
            "frame_size": frame_size,
            "camera_timestamps_ns": timestamps,
            "cadence_deltas_ns": cadence_deltas,
            "cadence_mean_ns": float(np.mean(cadence_deltas)),
            "cadence_min_ns": min(cadence_deltas),
            "cadence_max_ns": max(cadence_deltas),
        },
        "luma": {
            "mean_dn": float(frame_mean_array.mean()),
            "frame_mean_sd_dn": float(frame_mean_array.std()),
            "frame_mean_peak_to_peak_dn": float(np.ptp(frame_mean_array)),
            "mean_frame_spatial_sd_dn": float(np.mean(frame_spatial_std)),
            "sampled_percentiles_dn": percentiles,
            "sample_stride_xy": 16,
            "fraction_eq_0": zero_fraction,
            "fraction_le_5": low_count / total_active_pixels,
            "fraction_ge_250": high_fraction,
            "fraction_eq_255": saturated_fraction,
        },
        "temporal_noise": {
            "method": "adjacent_frame_difference_standard_deviation_divided_by_sqrt_2",
            "paired_temporal_sigma_dn": temporal_sigma,
            "paired_difference_mean_dn": diff_mean,
            "scope": "all_active_pixels",
        },
        "spatial_field": {
            "method": "8x8_tile_means_on_temporal_capture",
            "tile_mean_min_dn": float(tile_capture_mean.min()),
            "tile_mean_max_dn": float(tile_capture_mean.max()),
            "tile_mean_range_dn": float(np.ptp(tile_capture_mean)),
            "tile_mean_cv": float(
                tile_capture_mean.std() / max(abs(tile_capture_mean.mean()), 1e-12)
            ),
            "interpretation": (
                "Observed field nonuniformity; this one-level measurement does not "
                "separate illumination, optics, PRNU, or fixed-pattern effects."
            ),
        },
        "mono_nv12_contract": {"uv_min": uv_min, "uv_max": uv_max, "valid": True},
        "warnings": warnings,
        "artifacts": {
            "raw_dump": str(raw_path.resolve()),
            "raw_dump_sha256": sha256_file(raw_path),
            "index": str(index_path.resolve()),
            "index_sha256": sha256_file(index_path),
            "metadata": str(metadata_path.resolve()),
            "metadata_sha256": sha256_file(metadata_path),
            "recording_snapshot": str(snapshot_path.resolve()),
            "recording_snapshot_sha256": sha256_file(snapshot_path),
            "mean_luma_preview": str(mean_preview_path.resolve()),
            "temporal_sigma_preview": str(sigma_preview_path.resolve()),
            "temporal_sigma_preview_scale": {
                "black_dn": 0.0,
                "white_dn": sigma_preview_scale_max_dn,
            },
            "tile_metrics_csv": str(tile_csv_path.resolve()),
        },
    }
    result_path = camera_dir / "metrics.json"
    write_json_atomic(result_path, result)
    result["artifacts"]["metrics"] = str(result_path.resolve())
    return result


def cross_camera_ptp_summary(
    camera_results: dict[str, dict[str, Any]], expected_fps: int
) -> dict[str, Any]:
    serials = sorted(camera_results)
    if not serials:
        raise ValueError("no cameras available for PTP comparison")
    timestamp_lists = {
        serial: sorted(camera_results[serial]["capture"]["camera_timestamps_ns"])
        for serial in serials
    }
    reference = serials[0]
    spans: list[int] = []
    period_ns = 1_000_000_000.0 / expected_fps
    for timestamp in timestamp_lists[reference]:
        selected = [timestamp]
        valid = True
        for serial in serials[1:]:
            values = timestamp_lists[serial]
            position = bisect.bisect_left(values, timestamp)
            candidates = []
            if position < len(values):
                candidates.append(values[position])
            if position > 0:
                candidates.append(values[position - 1])
            if not candidates:
                valid = False
                break
            nearest = min(candidates, key=lambda value: abs(value - timestamp))
            if abs(nearest - timestamp) > period_ns / 2.0:
                valid = False
                break
            selected.append(nearest)
        if valid:
            spans.append(max(selected) - min(selected))
    if not spans:
        raise ValueError("no cross-camera PTP-aligned frame groups were found")
    max_span = max(spans)
    return {
        "reference_camera": reference,
        "camera_serials": serials,
        "matched_groups": len(spans),
        "median_timestamp_span_ns": float(np.median(spans)),
        "max_timestamp_span_ns": max_span,
        "acceptance_limit_ns": 100000,
        "status": "pass" if max_span <= 100000 else "fail",
    }


def write_markdown_report(path: Path, report: dict[str, Any]) -> None:
    lines = [
        "# Production-Baseline Sensor Characterization",
        "",
        f"Session: `{report['session_id']}`  ",
        f"Status: **{report['status']}**  ",
        f"Analyzed: `{report['created_utc']}`",
        "",
        "This is a one-level Mono8 production baseline, not a photon-transfer curve,",
        "conversion-gain result, PRNU measurement, or camera-setting promotion.",
        "",
        "| Camera | Dark mean | Dark temporal sigma | Flat mean | Flat temporal sigma | Flat-dark signal | Flat >=250 |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for serial in report["camera_serials"]:
        dark = report["phases"]["dark"]["cameras"][serial]
        flat = report["phases"]["flat"]["cameras"][serial]
        comparison = report["comparisons"][serial]
        lines.append(
            "| `{}` | {:.3f} | {:.3f} | {:.3f} | {:.3f} | {:.3f} | {:.4%} |".format(
                serial,
                dark["luma"]["mean_dn"],
                dark["temporal_noise"]["paired_temporal_sigma_dn"],
                flat["luma"]["mean_dn"],
                flat["temporal_noise"]["paired_temporal_sigma_dn"],
                comparison["flat_minus_dark_mean_dn"],
                flat["luma"]["fraction_ge_250"],
            )
        )
    lines.extend(["", "## Synchronization", ""])
    for phase in ("dark", "flat"):
        ptp = report["phases"][phase]["ptp_grouping"]
        lines.append(
            f"- `{phase}`: {ptp['matched_groups']} matched groups; maximum "
            f"timestamp span `{ptp['max_timestamp_span_ns']} ns` "
            f"(status `{ptp['status']}`)."
        )
    lines.extend(
        [
            "",
            "## Interpretation Boundary",
            "",
            "- Temporal sigma uses adjacent-frame differences divided by `sqrt(2)`.",
            "- Flat-field tile variation combines illumination, optics, sensor response, and fixed pattern.",
            "- A single flat level cannot establish shot-noise limitation or electron-per-DN conversion gain.",
            "- These results do not measure fish/background contrast; that remains a separate optical experiment.",
            "- No setting is automatically accepted or promoted by this report.",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def analyze(manifest_path: Path) -> dict[str, Any]:
    manifest = read_json(manifest_path)
    if manifest.get("schema_id") != MANIFEST_SCHEMA_ID:
        raise ValueError(f"not a sensor-baseline manifest: {manifest_path}")
    phases = manifest.get("phases", {})
    for phase in ("dark", "flat"):
        if phases.get(phase, {}).get("status") != "captured":
            raise ValueError(f"phase '{phase}' has not completed")

    session_dir = Path(str(manifest.get("session_dir", manifest_path.parent))).resolve()
    analysis_id = "analysis_" + utc_stamp()
    analysis_dir = session_dir / "analysis" / analysis_id
    analysis_dir.mkdir(parents=True, exist_ok=False)
    camera_serials = [
        str(value)
        for value in manifest["production_profile"]["camera_serials"]
    ]
    expected_frames = int(
        manifest["capture_contract"]["frames_per_camera_per_phase"]
    )
    expected_fps = int(
        manifest["production_profile"]["expected_camera_state"]["frame_rate"]
    )
    expected_state = manifest["production_profile"]["expected_camera_state"]
    expected_features = {
        "Width": int(expected_state["width"]),
        "Height": int(expected_state["height"]),
        "FrameRate": expected_fps,
        "Exposure": int(expected_state["exposure"]),
        "Gain": int(expected_state["gain"]),
        "AutoGain": bool(expected_state["AutoGain"]),
        "PGAGain": int(expected_state["PGAGain"]),
        "Offset": int(expected_state["Offset"]),
        "LUTEnable": bool(expected_state["LUTEnable"]),
        "ADC": str(expected_state["ADC"]),
        "DualADC": bool(expected_state["DualADC"]),
        "PixelFormat": str(expected_state["pixel_format"]),
    }

    report: dict[str, Any] = {
        "schema_id": REPORT_SCHEMA_ID,
        "schema_version": 1,
        "analysis_id": analysis_id,
        "session_id": manifest["session_id"],
        "created_utc": utc_now(),
        "status": "analyzing",
        "camera_serials": camera_serials,
        "source_manifest": str(manifest_path.resolve()),
        "source_manifest_sha256_before_analysis": sha256_file(manifest_path),
        "scope": "production_baseline_only",
        "phases": {},
        "comparisons": {},
        "warnings": [],
    }
    for phase in ("dark", "flat"):
        phase_artifacts = phases[phase].get("artifacts", {})
        snapshot = phase_artifacts.get("recording_snapshot")
        camera_artifacts = phase_artifacts.get("cameras", {})
        phase_results: dict[str, dict[str, Any]] = {}
        for serial in camera_serials:
            artifacts = dict(camera_artifacts.get(serial, {}))
            artifacts["recording_snapshot"] = snapshot
            phase_results[serial] = analyze_camera_capture(
                phase=phase,
                serial=serial,
                artifacts=artifacts,
                expected_frames=expected_frames,
                expected_fps=expected_fps,
                expected_features=expected_features,
                output_dir=analysis_dir,
            )
            report["warnings"].extend(
                f"Cam{serial} {phase}: {warning}"
                for warning in phase_results[serial]["warnings"]
            )
        ptp = cross_camera_ptp_summary(phase_results, expected_fps)
        if ptp["status"] != "pass":
            raise ValueError(
                f"{phase} cross-camera PTP span {ptp['max_timestamp_span_ns']} ns "
                "exceeds 100000 ns"
            )
        report["phases"][phase] = {
            "physical_condition": phases[phase].get("physical_condition", {}),
            "cameras": phase_results,
            "ptp_grouping": ptp,
        }

    for serial in camera_serials:
        dark = report["phases"]["dark"]["cameras"][serial]
        flat = report["phases"]["flat"]["cameras"][serial]
        signal = flat["luma"]["mean_dn"] - dark["luma"]["mean_dn"]
        report["comparisons"][serial] = {
            "flat_minus_dark_mean_dn": signal,
            "flat_temporal_sigma_dn": flat["temporal_noise"][
                "paired_temporal_sigma_dn"
            ],
            "dark_temporal_sigma_dn": dark["temporal_noise"][
                "paired_temporal_sigma_dn"
            ],
            "flat_signal_over_flat_temporal_sigma": (
                signal
                / max(
                    flat["temporal_noise"]["paired_temporal_sigma_dn"], 1e-12
                )
            ),
            "interpretation": (
                "Uniform-field signal diagnostic only; not fish CNR and not "
                "electron-per-DN conversion gain."
            ),
        }

    report["status"] = "valid_baseline"
    report_path = analysis_dir / "report.json"
    markdown_path = analysis_dir / "report.md"
    report["artifacts"] = {
        "report_json": str(report_path.resolve()),
        "report_markdown": str(markdown_path.resolve()),
    }
    write_json_atomic(report_path, report)
    write_markdown_report(markdown_path, report)

    manifest["analysis"] = {
        "status": "valid_baseline",
        "analysis_id": analysis_id,
        "completed_utc": utc_now(),
        "report_json": str(report_path.resolve()),
        "report_json_sha256": sha256_file(report_path),
        "report_markdown": str(markdown_path.resolve()),
    }
    manifest["status"] = "complete"
    manifest["updated_utc"] = utc_now()
    write_json_atomic(manifest_path, manifest)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze a production-baseline sensor characterization session."
    )
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    manifest_path = args.manifest.resolve()
    if manifest_path.is_dir():
        manifest_path = manifest_path / "run_manifest.json"
    try:
        report = analyze(manifest_path)
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        print(f"Sensor baseline analysis failed: {error}", file=sys.stderr)
        return 1
    print(f"Sensor baseline analysis: {report['status']}")
    print(f"Report: {report['artifacts']['report_json']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
