#!/usr/bin/env python3
"""Summarize image brightness across an aperture/iris sweep.

The script expects filenames that include `_focus-<n>` and `_iris-<n>`, for
example:

  e-70_focus-3100_iris-0.tif
  e-70_focus-3100_iris22.tif

It computes per-file grayscale statistics, groups them by iris command, and
reports relative transmission versus a reference iris command. If you provide a
reference f-number, it also estimates effective f-number under the simple
assumption that image brightness is proportional to aperture area.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageOps, ImageStat


IMAGE_EXTENSIONS = {".tif", ".tiff", ".png", ".jpg", ".jpeg"}
FILENAME_RE = re.compile(
    r"_focus-?(?P<focus>\d+)_iris-?(?P<iris>\d+)\.[^.]+$",
    re.IGNORECASE,
)


@dataclass
class Roi:
    left: int
    top: int
    width: int
    height: int


@dataclass
class Measurement:
    path: Path
    iris_cmd: int
    focus_cmd: int | None
    width: int
    height: int
    pixel_count: int
    mean: float
    median: float
    p05: float
    p95: float
    stddev: float
    minimum: int
    maximum: int
    black_fraction: float
    white_fraction: float


@dataclass
class GroupSummary:
    iris_cmd: int
    count: int
    mean_mean: float
    mean_median: float
    mean_p05: float
    mean_p95: float
    mean_black_fraction: float
    mean_white_fraction: float
    files: list[str]


@dataclass
class MeasurementResult:
    measurements: list[Measurement]
    skipped: list[tuple[Path, str]]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Measure brightness across images captured at different iris commands."
    )
    parser.add_argument(
        "paths",
        nargs="+",
        help="Image file(s) or directories to scan.",
    )
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="Recursively scan directories for image files.",
    )
    parser.add_argument(
        "--roi",
        default="",
        help="Optional ROI as x,y,w,h in input-pixel coordinates.",
    )
    parser.add_argument(
        "--center-fraction",
        type=float,
        default=0.0,
        help="Optional centered ROI fraction in (0,1]; ignored if --roi is set.",
    )
    parser.add_argument(
        "--reference-iris",
        type=int,
        default=None,
        help="Iris command used as the brightness reference. Default: minimum iris command found.",
    )
    parser.add_argument(
        "--reference-f-number",
        type=float,
        default=None,
        help="If set, estimate effective f-number relative to the reference iris command.",
    )
    parser.add_argument(
        "--csv-out",
        default="",
        help="Optional path for per-file CSV output.",
    )
    parser.add_argument(
        "--grouped-csv-out",
        default="",
        help="Optional path for grouped-by-iris CSV output.",
    )
    parser.add_argument(
        "--plot-out",
        default="",
        help="Optional path for a grouped summary plot (requires matplotlib).",
    )
    return parser.parse_args()


def collect_paths(paths: Iterable[str], recursive: bool) -> list[Path]:
    out: list[Path] = []
    for raw in paths:
        path = Path(raw).expanduser()
        if path.is_file():
            if path.suffix.lower() in IMAGE_EXTENSIONS:
                out.append(path)
            continue
        if not path.is_dir():
            continue

        iterator = path.rglob("*") if recursive else path.iterdir()
        for child in iterator:
            if child.is_file() and child.suffix.lower() in IMAGE_EXTENSIONS:
                out.append(child)

    return sorted(set(out))


def parse_metadata(path: Path) -> tuple[int, int | None]:
    match = FILENAME_RE.search(path.name)
    if not match:
        raise ValueError(
            f"could not parse iris command from filename: {path.name!r}; "
            "expected *_focus-<n>_iris-<n>.<ext> or *_focus-<n>_iris<n>.<ext>"
        )
    iris_cmd = int(match.group("iris"))
    focus_cmd = int(match.group("focus")) if match.group("focus") else None
    return iris_cmd, focus_cmd


def parse_roi_arg(value: str) -> Roi | None:
    if not value:
        return None
    parts = value.split(",")
    if len(parts) != 4:
        raise ValueError("--roi must be formatted as x,y,w,h")
    left, top, width, height = (int(p.strip()) for p in parts)
    if width <= 0 or height <= 0:
        raise ValueError("--roi width and height must be positive")
    return Roi(left, top, width, height)


def make_center_roi(width: int, height: int, fraction: float) -> Roi | None:
    if fraction <= 0:
        return None
    if fraction > 1:
        raise ValueError("--center-fraction must be in (0,1]")
    roi_width = max(1, int(round(width * fraction)))
    roi_height = max(1, int(round(height * fraction)))
    left = (width - roi_width) // 2
    top = (height - roi_height) // 2
    return Roi(left, top, roi_width, roi_height)


def clamp_roi(roi: Roi, width: int, height: int) -> Roi:
    left = min(max(roi.left, 0), width)
    top = min(max(roi.top, 0), height)
    right = min(max(left + roi.width, left + 1), width)
    bottom = min(max(top + roi.height, top + 1), height)
    return Roi(left, top, right - left, bottom - top)


def percentile_from_histogram(hist: list[int], total: int, fraction: float) -> float:
    if total <= 0:
        return 0.0
    target = max(0, min(total - 1, int(math.ceil(total * fraction) - 1)))
    running = 0
    for value, count in enumerate(hist):
        running += count
        if running > target:
            return float(value)
    return float(len(hist) - 1)


def open_grayscale_image(path: Path, roi: Roi | None, center_fraction: float) -> Image.Image:
    image = Image.open(path)
    if image.mode != "L":
        image = ImageOps.grayscale(image)
    if roi is None and center_fraction > 0:
        roi = make_center_roi(image.width, image.height, center_fraction)
    if roi is not None:
        bounded = clamp_roi(roi, image.width, image.height)
        image = image.crop(
            (
                bounded.left,
                bounded.top,
                bounded.left + bounded.width,
                bounded.top + bounded.height,
            )
        )
    return image


def measure_image(path: Path, roi: Roi | None, center_fraction: float) -> Measurement:
    iris_cmd, focus_cmd = parse_metadata(path)
    image = open_grayscale_image(path, roi, center_fraction)
    hist = image.histogram()
    if len(hist) != 256:
        raise RuntimeError(
            f"{path.name}: expected 8-bit grayscale histogram, got {len(hist)} bins"
        )

    stat = ImageStat.Stat(image)
    minimum, maximum = image.getextrema()
    pixel_count = image.width * image.height

    return Measurement(
        path=path,
        iris_cmd=iris_cmd,
        focus_cmd=focus_cmd,
        width=image.width,
        height=image.height,
        pixel_count=pixel_count,
        mean=stat.mean[0],
        median=percentile_from_histogram(hist, pixel_count, 0.50),
        p05=percentile_from_histogram(hist, pixel_count, 0.05),
        p95=percentile_from_histogram(hist, pixel_count, 0.95),
        stddev=stat.stddev[0],
        minimum=int(minimum),
        maximum=int(maximum),
        black_fraction=hist[0] / pixel_count if pixel_count else 0.0,
        white_fraction=hist[-1] / pixel_count if pixel_count else 0.0,
    )


def measure_images(paths: list[Path], roi: Roi | None, center_fraction: float) -> MeasurementResult:
    measurements: list[Measurement] = []
    skipped: list[tuple[Path, str]] = []

    for path in paths:
        try:
            measurements.append(measure_image(path, roi, center_fraction))
        except Exception as exc:
            skipped.append((path, str(exc)))

    return MeasurementResult(measurements=measurements, skipped=skipped)


def summarize_groups(measurements: list[Measurement]) -> list[GroupSummary]:
    grouped: dict[int, list[Measurement]] = defaultdict(list)
    for measurement in measurements:
        grouped[measurement.iris_cmd].append(measurement)

    summaries: list[GroupSummary] = []
    for iris_cmd, entries in grouped.items():
        summaries.append(
            GroupSummary(
                iris_cmd=iris_cmd,
                count=len(entries),
                mean_mean=sum(e.mean for e in entries) / len(entries),
                mean_median=sum(e.median for e in entries) / len(entries),
                mean_p05=sum(e.p05 for e in entries) / len(entries),
                mean_p95=sum(e.p95 for e in entries) / len(entries),
                mean_black_fraction=sum(e.black_fraction for e in entries) / len(entries),
                mean_white_fraction=sum(e.white_fraction for e in entries) / len(entries),
                files=[e.path.name for e in sorted(entries, key=lambda item: item.path.name)],
            )
        )
    summaries.sort(key=lambda item: (item.iris_cmd, item.files))
    return summaries


def format_float(value: float, digits: int = 3) -> str:
    return f"{value:.{digits}f}"


def print_summary(
    summaries: list[GroupSummary],
    reference_iris: int,
    reference_mean: float,
    reference_f_number: float | None,
) -> None:
    print("Aperture sweep summary")
    print(f"  reference iris command: {reference_iris}")
    print(f"  reference mean brightness: {format_float(reference_mean)}")
    if reference_f_number is not None:
        print(
            "  estimated f-number assumes brightness is proportional to aperture area"
            f" and iris {reference_iris} corresponds to f/{format_float(reference_f_number, 2)}"
        )

    duplicate_groups = [s for s in summaries if s.count > 1]
    if duplicate_groups:
        print("  duplicate iris commands detected:")
        for summary in duplicate_groups:
            print(f"    iris {summary.iris_cmd}: {', '.join(summary.files)}")

    header = (
        " iris  n      mean    rel_ref  delta_ev"
        + ("   est_f#" if reference_f_number is not None else "")
        + "    median      p05      p95   black%   white%"
    )
    print()
    print(header)
    print("-" * len(header))
    for summary in summaries:
        rel_ref = summary.mean_mean / reference_mean if reference_mean > 0 else float("nan")
        delta_ev = math.log2(reference_mean / summary.mean_mean) if summary.mean_mean > 0 else float("nan")
        row = (
            f"{summary.iris_cmd:5d}"
            f"{summary.count:3d}"
            f"{summary.mean_mean:10.3f}"
            f"{rel_ref:10.4f}"
            f"{delta_ev:10.3f}"
        )
        if reference_f_number is not None:
            est_f_number = reference_f_number * math.sqrt(reference_mean / summary.mean_mean)
            row += f"{est_f_number:10.3f}"
        row += (
            f"{summary.mean_median:10.3f}"
            f"{summary.mean_p05:10.3f}"
            f"{summary.mean_p95:10.3f}"
            f"{summary.mean_black_fraction * 100.0:9.3f}"
            f"{summary.mean_white_fraction * 100.0:9.3f}"
        )
        print(row)


def write_per_file_csv(path: Path, measurements: list[Measurement], reference_mean: float, reference_f_number: float | None) -> None:
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "file",
                "iris_cmd",
                "focus_cmd",
                "width",
                "height",
                "pixel_count",
                "mean",
                "median",
                "p05",
                "p95",
                "stddev",
                "min",
                "max",
                "black_fraction",
                "white_fraction",
                "relative_to_reference",
                "delta_ev_from_reference",
                "estimated_f_number",
            ]
        )
        for measurement in sorted(measurements, key=lambda item: (item.iris_cmd, item.path.name)):
            rel_ref = measurement.mean / reference_mean if reference_mean > 0 else float("nan")
            delta_ev = math.log2(reference_mean / measurement.mean) if measurement.mean > 0 else float("nan")
            est_f_number = ""
            if reference_f_number is not None and measurement.mean > 0:
                est_f_number = reference_f_number * math.sqrt(reference_mean / measurement.mean)
            writer.writerow(
                [
                    measurement.path.name,
                    measurement.iris_cmd,
                    measurement.focus_cmd if measurement.focus_cmd is not None else "",
                    measurement.width,
                    measurement.height,
                    measurement.pixel_count,
                    format_float(measurement.mean),
                    format_float(measurement.median),
                    format_float(measurement.p05),
                    format_float(measurement.p95),
                    format_float(measurement.stddev),
                    measurement.minimum,
                    measurement.maximum,
                    format_float(measurement.black_fraction, 6),
                    format_float(measurement.white_fraction, 6),
                    format_float(rel_ref, 6),
                    format_float(delta_ev, 6),
                    format_float(est_f_number, 6) if est_f_number != "" else "",
                ]
            )


def write_grouped_csv(path: Path, summaries: list[GroupSummary], reference_mean: float, reference_f_number: float | None) -> None:
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "iris_cmd",
                "count",
                "mean_brightness",
                "median_brightness",
                "p05_brightness",
                "p95_brightness",
                "black_fraction",
                "white_fraction",
                "relative_to_reference",
                "delta_ev_from_reference",
                "estimated_f_number",
                "files",
            ]
        )
        for summary in summaries:
            rel_ref = summary.mean_mean / reference_mean if reference_mean > 0 else float("nan")
            delta_ev = math.log2(reference_mean / summary.mean_mean) if summary.mean_mean > 0 else float("nan")
            est_f_number = ""
            if reference_f_number is not None and summary.mean_mean > 0:
                est_f_number = reference_f_number * math.sqrt(reference_mean / summary.mean_mean)
            writer.writerow(
                [
                    summary.iris_cmd,
                    summary.count,
                    format_float(summary.mean_mean),
                    format_float(summary.mean_median),
                    format_float(summary.mean_p05),
                    format_float(summary.mean_p95),
                    format_float(summary.mean_black_fraction, 6),
                    format_float(summary.mean_white_fraction, 6),
                    format_float(rel_ref, 6),
                    format_float(delta_ev, 6),
                    format_float(est_f_number, 6) if est_f_number != "" else "",
                    ";".join(summary.files),
                ]
            )


def write_plot(path: Path, summaries: list[GroupSummary], reference_mean: float, reference_f_number: float | None) -> None:
    try:
        import matplotlib.pyplot as plt
    except Exception as exc:
        raise RuntimeError(f"matplotlib is required for --plot-out ({exc})") from exc

    iris_values = [summary.iris_cmd for summary in summaries]
    rel_values = [summary.mean_mean / reference_mean if reference_mean > 0 else float("nan") for summary in summaries]
    ev_values = [math.log2(reference_mean / summary.mean_mean) if summary.mean_mean > 0 else float("nan") for summary in summaries]

    fig, ax_left = plt.subplots(figsize=(9, 5))
    ax_left.plot(iris_values, rel_values, marker="o", color="#1f77b4", label="Relative brightness")
    ax_left.set_xlabel("Iris command")
    ax_left.set_ylabel("Relative brightness vs reference", color="#1f77b4")
    ax_left.tick_params(axis="y", labelcolor="#1f77b4")
    ax_left.grid(True, alpha=0.3)

    ax_right = ax_left.twinx()
    ax_right.plot(iris_values, ev_values, marker="s", color="#d62728", label="Delta EV")
    ax_right.set_ylabel("Delta EV from reference", color="#d62728")
    ax_right.tick_params(axis="y", labelcolor="#d62728")

    if reference_f_number is not None:
        f_values = [
            reference_f_number * math.sqrt(reference_mean / summary.mean_mean)
            if summary.mean_mean > 0 else float("nan")
            for summary in summaries
        ]
        ax_f = ax_left.twinx()
        ax_f.spines.right.set_position(("axes", 1.12))
        ax_f.plot(iris_values, f_values, marker="^", color="#2ca02c", label="Estimated f-number")
        ax_f.set_ylabel("Estimated f-number", color="#2ca02c")
        ax_f.tick_params(axis="y", labelcolor="#2ca02c")

    fig.suptitle("Aperture brightness sweep")
    fig.tight_layout()
    fig.savefig(path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def main() -> int:
    args = parse_args()

    try:
        roi = parse_roi_arg(args.roi)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    try:
        image_paths = collect_paths(args.paths, args.recursive)
        if not image_paths:
            raise ValueError("no image files found")

        result = measure_images(image_paths, roi, args.center_fraction)
        measurements = result.measurements
        if not measurements:
            raise ValueError("all image files were unreadable")
        summaries = summarize_groups(measurements)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if result.skipped:
        print("Skipped unreadable files:", file=sys.stderr)
        for path, reason in result.skipped:
            print(f"  {path}: {reason}", file=sys.stderr)
        print(file=sys.stderr)

    available_iris_values = {summary.iris_cmd for summary in summaries}
    reference_iris = args.reference_iris
    if reference_iris is None:
        reference_iris = min(available_iris_values)
    if reference_iris not in available_iris_values:
        print(
            f"error: reference iris command {reference_iris} not found in input set",
            file=sys.stderr,
        )
        return 2

    reference_summary = next(summary for summary in summaries if summary.iris_cmd == reference_iris)
    reference_mean = reference_summary.mean_mean

    print_summary(summaries, reference_iris, reference_mean, args.reference_f_number)

    if args.csv_out:
        write_per_file_csv(Path(args.csv_out), measurements, reference_mean, args.reference_f_number)
        print(f"\nWrote per-file CSV: {args.csv_out}")
    if args.grouped_csv_out:
        write_grouped_csv(Path(args.grouped_csv_out), summaries, reference_mean, args.reference_f_number)
        print(f"Wrote grouped CSV: {args.grouped_csv_out}")
    if args.plot_out:
        try:
            write_plot(Path(args.plot_out), summaries, reference_mean, args.reference_f_number)
        except Exception as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        print(f"Wrote plot: {args.plot_out}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
