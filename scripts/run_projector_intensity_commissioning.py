#!/usr/bin/env python3
"""Run a resumable projected-grid intensity commissioning sweep.

Each sample is one fenced, full-resolution grouped still. Repeats are never
temporally averaged; this is intentional so the report can measure centroid
jitter and intermittent pattern-detection failures.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA_ID = "orange.projector_intensity_commissioning.run_manifest"


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def parse_int_csv(value: str, minimum: int, maximum: int) -> list[int]:
    values: list[int] = []
    for text in value.split(","):
        text = text.strip()
        if not text:
            continue
        number = int(text)
        if number < minimum or number > maximum:
            raise argparse.ArgumentTypeError(
                f"{number} is outside the allowed range {minimum}..{maximum}"
            )
        if number not in values:
            values.append(number)
    if not values:
        raise argparse.ArgumentTypeError("at least one value is required")
    return values


def parse_camera_csv(value: str) -> list[str]:
    cameras = [item.strip() for item in value.split(",") if item.strip()]
    if not cameras or any(not camera.isdigit() for camera in cameras):
        raise argparse.ArgumentTypeError("cameras must be comma-separated numeric serials")
    return list(dict.fromkeys(cameras))


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def load_pass_result(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return payload if isinstance(payload, dict) and payload.get("status") == "pass" else None


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description="Capture and analyze opaque grayscale projector commissioning frames."
    )
    parser.add_argument("--execute", action="store_true", help="control the real display/cameras")
    parser.add_argument(
        "--confirm-unobstructed-dry-shelf",
        action="store_true",
        help="confirm holder, dishes, water, and camera filters are removed",
    )
    parser.add_argument("--levels", default="64,128,192,255")
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--cameras", default="2010093,2010094,2010095,2010096")
    parser.add_argument("--frame-rate-hz", type=int, default=5)
    parser.add_argument("--exposure-us", type=int, default=100000)
    parser.add_argument("--timeout-seconds", type=int, default=240)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument(
        "--legacy-one-shot",
        action="store_true",
        help="restart Orange/Citrus for every sample (diagnostic compatibility only)",
    )
    parser.add_argument("--analyze-only", type=Path, metavar="MANIFEST")
    args = parser.parse_args()

    if args.analyze_only is not None:
        return subprocess.call(
            [sys.executable, str(repo_root / "scripts/analyze_projector_intensity_commissioning.py"),
             str(args.analyze_only)]
        )

    try:
        levels = parse_int_csv(args.levels, 1, 255)
        cameras = parse_camera_csv(args.cameras)
    except (ValueError, argparse.ArgumentTypeError) as error:
        parser.error(str(error))
    if args.repeats < 2:
        parser.error("--repeats must be at least 2 to measure stability")
    if args.frame_rate_hz <= 0 or args.exposure_us <= 0 or args.timeout_seconds <= 0:
        parser.error("frame rate, exposure, and timeout must be positive")
    if args.execute and not args.confirm_unobstructed_dry_shelf:
        parser.error("--execute requires --confirm-unobstructed-dry-shelf")

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output_dir = args.output_dir or Path(
        f"/home/jeremy/orange_data/calibrations/commissioning/projector_intensity_{stamp}"
    )
    manifest_path = output_dir / "run_manifest.json"
    if manifest_path.exists() and not args.resume:
        parser.error(f"output already exists; use --resume: {manifest_path}")

    if args.resume and manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("schema_id") != SCHEMA_ID:
            parser.error(f"not a projector intensity manifest: {manifest_path}")
    else:
        manifest = {
            "schema_id": SCHEMA_ID,
            "schema_version": 2,
            "created_utc": utc_now(),
            "updated_utc": utc_now(),
            "status": "planned",
            "fixture_state": {
                "state_id": "unobstructed_dry_shelf",
                "holder_installed": False,
                "dish_installed": False,
                "water_present": False,
                "camera_filters_removed": True,
                "operator_confirmed": bool(args.confirm_unobstructed_dry_shelf),
            },
            "projection": {
                "recipe": "homography_grid",
                "foreground_command_encoding": "framebuffer_rgb_u8",
                "alpha_semantics": "opaque",
                "levels_gray_u8": levels,
            },
            "capture": {
                "individual_frames_only": True,
                "temporal_averaging": False,
                "invocation_mode": (
                    "one_process_per_sample" if args.legacy_one_shot else "one_process_per_sweep"
                ),
                "session_policy": (
                    "one_session_per_sample" if args.legacy_one_shot else "one_session_per_sweep"
                ),
                "arena_outline_reference": {
                    "included": not args.legacy_one_shot,
                    "center_fiducial": True,
                    "recipe": "arena_outline",
                    "purpose": "arena_projection",
                },
                "repeats_per_level": args.repeats,
                "camera_serials": cameras,
                "frame_rate_hz": args.frame_rate_hz,
                "exposure_us": args.exposure_us,
                "synchronization": {
                    "sync_mode": "ptp_gate",
                    "startup_at_calibration_timing": True,
                    "max_group_camera_timestamp_span_ns": 100000,
                },
            },
            "runs": [],
        }

    print("Projector intensity commissioning plan:")
    print(f"  levels={levels} repeats={args.repeats} cameras={','.join(cameras)}")
    print(f"  opaque RGB, {args.exposure_us} us at {args.frame_rate_hz} fps")
    print("  fixture=unobstructed dry shelf; holder/dishes/water/filters removed")
    print(
        "  sequence=arena outline + center fiducial, then grayscale grids; "
        + ("one process/one session" if not args.legacy_one_shot else "legacy process-per-sample")
    )
    print(f"  output={output_dir}")
    if not args.execute:
        print("Dry-run only. Add --execute --confirm-unobstructed-dry-shelf to capture.")
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)
    manifest["status"] = "capturing"
    manifest["updated_utc"] = utc_now()
    write_json_atomic(manifest_path, manifest)

    guided_script = repo_root / "scripts/run_gui_guided_capture_smoke.sh"
    if not args.legacy_one_shot:
        result_path = output_dir / "sweep_result.json"
        result = load_pass_result(result_path) if args.resume else None
        if result is None:
            command = [
                str(guided_script),
                "--execute",
                "--profile", "unobstructed_canvas_commissioning",
                "--recipe", "homography_grid",
                "--purpose", "homography_grid",
                "--cameras", ",".join(cameras),
                "--frame-count", "1",
                "--sweep-foreground-grays-u8", ",".join(map(str, levels)),
                "--sweep-repeats", str(args.repeats),
                "--include-arena-outline-reference",
                "--calibration-frame-rate-hz", str(args.frame_rate_hz),
                "--calibration-exposure-us", str(args.exposure_us),
                "--save",
                "--timeout-seconds", str(args.timeout_seconds),
                "--result-json", str(result_path),
            ]
            print(
                f"CAPTURE SWEEP: {len(levels) * args.repeats} grouped samples "
                "in one Orange/Citrus process and one calibration session",
                flush=True,
            )
            completed = subprocess.run(command, cwd=repo_root, check=False)
            result = load_pass_result(result_path)
            if completed.returncode != 0 or result is None:
                manifest["status"] = "capture_failed"
                manifest["updated_utc"] = utc_now()
                write_json_atomic(manifest_path, manifest)
                print(
                    "Sweep capture failed. A partial result remains for diagnosis; "
                    "--resume reruns the complete atomic sweep.",
                    file=sys.stderr,
                )
                return completed.returncode or 1
        else:
            print("RESUME: complete single-process sweep already passed")

        samples = result.get("samples", []) if isinstance(result, dict) else []
        outline_samples = [
            sample for sample in samples
            if isinstance(sample, dict) and sample.get("recipe") == "arena_outline"
        ]
        grid_samples = [
            sample for sample in samples
            if isinstance(sample, dict) and sample.get("recipe") == "homography_grid"
        ]
        manifest["reference_captures"] = [
            {
                "recipe": "arena_outline",
                "purpose": "arena_projection",
                "center_fiducial": True,
                "sample_index": int(sample.get("sample_index", index)),
                "result_json": str(result_path.resolve()),
                "status": "pass",
            }
            for index, sample in enumerate(outline_samples)
        ]
        manifest["runs"] = [
            {
                "foreground_gray_u8": int(sample["foreground_gray_u8"]),
                "repeat_index": int(sample["repeat_index"]),
                "sample_index": int(sample.get("sample_index", index)),
                "result_json": str(result_path.resolve()),
                "completed_utc": utc_now(),
                "process_returncode": 0,
                "status": "pass",
            }
            for index, sample in enumerate(grid_samples)
        ]
        manifest["calibration_session"] = {
            "session_id": result.get("persistence", {}).get("session_id"),
            "session_dir": result.get("persistence", {}).get("session_dir"),
        }
        expected_count = len(levels) * args.repeats
        if len(manifest["runs"]) != expected_count or len(outline_samples) != 1:
            manifest["status"] = "capture_failed"
            write_json_atomic(manifest_path, manifest)
            print(
                f"Sweep result has {len(manifest['runs'])} grid samples and "
                f"{len(outline_samples)} arena outlines; expected {expected_count} and 1",
                file=sys.stderr,
            )
            return 1
        manifest["updated_utc"] = utc_now()
        write_json_atomic(manifest_path, manifest)
    else:
        existing_by_key = {
            (int(run.get("foreground_gray_u8", -1)), int(run.get("repeat_index", -1))): run
            for run in manifest.get("runs", [])
            if isinstance(run, dict)
        }
        for gray in levels:
            for repeat_index in range(1, args.repeats + 1):
                result_path = output_dir / f"gray_{gray:03d}" / f"repeat_{repeat_index:02d}.json"
                previous = existing_by_key.get((gray, repeat_index))
                if args.resume and previous and load_pass_result(result_path) is not None:
                    print(f"RESUME: gray={gray} repeat={repeat_index} already passed")
                    continue
                result_path.parent.mkdir(parents=True, exist_ok=True)
                command = [
                    str(guided_script),
                    "--execute",
                    "--profile", "unobstructed_canvas_commissioning",
                    "--recipe", "homography_grid",
                    "--purpose", "homography_grid",
                    "--cameras", ",".join(cameras),
                    "--frame-count", "1",
                    "--foreground-gray-u8", str(gray),
                    "--calibration-frame-rate-hz", str(args.frame_rate_hz),
                    "--calibration-exposure-us", str(args.exposure_us),
                    "--save",
                    "--timeout-seconds", str(args.timeout_seconds),
                    "--result-json", str(result_path),
                ]
                print(f"CAPTURE: gray={gray} repeat={repeat_index}/{args.repeats}", flush=True)
                completed = subprocess.run(command, cwd=repo_root, check=False)
                run_entry = {
                    "foreground_gray_u8": gray,
                    "repeat_index": repeat_index,
                    "result_json": str(result_path.resolve()),
                    "completed_utc": utc_now(),
                    "process_returncode": completed.returncode,
                    "status": "pass" if completed.returncode == 0 else "fail",
                }
                if previous is None:
                    manifest["runs"].append(run_entry)
                else:
                    previous.clear()
                    previous.update(run_entry)
                manifest["updated_utc"] = utc_now()
                write_json_atomic(manifest_path, manifest)
                if completed.returncode != 0:
                    manifest["status"] = "capture_failed"
                    write_json_atomic(manifest_path, manifest)
                    return completed.returncode or 1

    manifest["status"] = "captured"
    manifest["updated_utc"] = utc_now()
    write_json_atomic(manifest_path, manifest)
    analyzer = repo_root / "scripts/analyze_projector_intensity_commissioning.py"
    completed = subprocess.run([sys.executable, str(analyzer), str(manifest_path)], check=False)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
