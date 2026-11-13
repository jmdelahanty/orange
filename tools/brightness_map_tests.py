#!/usr/bin/env python3
"""Synthetic checks for scripts/brightness_map.py.

Builds a three-arena canvas, one camera per arena (one camera y-flipped, like
omnifin0), calibration sessions at two gray levels and active homography
pointers, then checks the levels, the shape comparison, the facing-edge
ratios (a camera 10 % more sensitive must show up as 1/1.1 and 1.1) and the
refusals. Needs numpy, PIL and matplotlib (run with the juicebox env).
"""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import brightness_map as tool  # noqa: E402

CANVAS_PX_PER_MM = 4.0
ARENA_SIZE = 120
CENTERS = {"arena_1": (100, 100), "arena_2": (260, 100), "arena_3": (420, 100)}
SERIALS = {"arena_1": "1001", "arena_2": "1002", "arena_3": "1003"}
CAMERA_GAIN = {"arena_1": 1.0, "arena_2": 1.1, "arena_3": 1.0}
FLIP_Y = {"arena_1": True, "arena_2": False, "arena_3": True}
IMAGE_SIZE = 400
SCALE = 0.33  # canvas px per camera px
BLACK_LEVEL = 3


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def homography(arena_id: str) -> np.ndarray:
    cx, cy = CENTERS[arena_id]
    if FLIP_Y[arena_id]:
        return np.array([[SCALE, 0.0, cx - 66.0], [0.0, -SCALE, cy + 66.0], [0.0, 0.0, 1.0]])
    return np.array([[SCALE, 0.0, cx - 66.0], [0.0, SCALE, cy - 66.0], [0.0, 0.0, 1.0]])


def field(canvas_y: np.ndarray, level_scale: float) -> np.ndarray:
    # Smooth projector field; constant along canvas x, so facing strips at
    # the same canvas y see the same light and the ratio is the gain ratio.
    return level_scale * (80.0 + 40.0 * ((canvas_y - 100.0) / 60.0) ** 2)


def camera_images(arena_id: str, level_scale: float) -> tuple[np.ndarray, np.ndarray]:
    rows, cols = np.mgrid[0:IMAGE_SIZE, 0:IMAGE_SIZE].astype(np.float64)
    x, y = tool.map_points(homography(arena_id), cols, rows)
    cx, cy = CENTERS[arena_id]
    ox, oy = cx - ARENA_SIZE // 2, cy - ARENA_SIZE // 2
    inside = (x >= ox - 0.5) & (x < ox + ARENA_SIZE - 0.5) & (y >= oy - 0.5) & (y < oy + ARENA_SIZE - 0.5)
    signal = np.where(inside, CAMERA_GAIN[arena_id] * field(y, level_scale), 0.0)
    black = np.full((IMAGE_SIZE, IMAGE_SIZE), BLACK_LEVEL, dtype=np.uint8)
    uniform = np.clip(np.rint(BLACK_LEVEL + signal), 0, 255).astype(np.uint8)
    return black, uniform


def write_canvas(rig_dir: Path, direction: str = tool.HOMOGRAPHY_DIRECTION) -> Path:
    arenas = {}
    for arena_id, (cx, cy) in CENTERS.items():
        arenas[arena_id] = {
            "active_camera_id": SERIALS[arena_id],
            "camera_calibrations": [{
                "camera_id": SERIALS[arena_id],
                "arena_center_x_px": cx,
                "arena_center_y_px": cy,
                "arena_width_px": ARENA_SIZE,
                "arena_height_px": ARENA_SIZE,
                "pixels_per_mm_projector": CANVAS_PX_PER_MM,
            }],
        }
    canvas_path = rig_dir / "canvas.json"
    canvas_path.write_text(json.dumps({"canvas_name": "synthetic", "arenas": arenas}), encoding="utf-8")
    canvas_sha = "sha256:" + tool.sha256_file(canvas_path)
    calibration_dir = rig_dir / "calibration_artifacts"
    calibration_dir.mkdir()
    side = {"origin": "top_left", "first_pixel_center": [0.0, 0.0], "positive_x": "right", "positive_y": "down"}
    for arena_id in CENTERS:
        pointer = {
            "schema_id": "citrus.calibration.active_homography",
            "schema_version": 1,
            "status": "accepted",
            "arena_id": arena_id,
            "camera_id": SERIALS[arena_id],
            "homography_direction": direction,
            "homography_matrix": homography(arena_id).tolist(),
            "canvas_checksum_at_acceptance": canvas_sha,
            "coordinate_contract": {
                "homography_direction": direction,
                "source": dict(side, space="camera_native_px"),
                "destination": dict(side, space="final_display_canvas_px"),
            },
        }
        (calibration_dir / f"homography_active_{arena_id}_{SERIALS[arena_id]}.json").write_text(
            json.dumps(pointer), encoding="utf-8"
        )
    return canvas_path


def write_session(sessions_root: Path, session_id: str, gray: int | None, exposure_us: float, level_scale: float) -> Path:
    session_dir = sessions_root / session_id
    order = []
    for arena_id in CENTERS:
        serial = SERIALS[arena_id]
        artifact_id = f"Cam{serial}_{arena_id}"
        order.append(artifact_id)
        artifact_dir = session_dir / "artifacts" / artifact_id
        (artifact_dir / "captures").mkdir(parents=True)
        black, uniform = camera_images(arena_id, level_scale)
        Image.fromarray(black).save(artifact_dir / "captures" / "black.png")
        Image.fromarray(uniform).save(artifact_dir / "captures" / "uniform.png")
        capture = {"exposure_us": exposure_us, "gain": 256.0}
        uniform_entry = {"role": "uniform_gray_reference", "path": "captures/uniform.png", "capture": capture}
        if gray is not None:
            cx, cy = CENTERS[arena_id]
            uniform_entry["citrus_calibration_scene_post_capture"] = {"resolved_targets": [{
                "arena_id": arena_id,
                "arena": {
                    "origin_final_display_canvas_px": {"x": cx - ARENA_SIZE // 2, "y": cy - ARENA_SIZE // 2},
                    "size_px": {"width": ARENA_SIZE, "height": ARENA_SIZE},
                },
                "uniform_illumination": {"gray_u8": gray},
            }]}
        image_set = {
            "schema_id": "orange.calibration.image_set",
            "camera": {"serial": serial},
            "rig_context": {"arena_id": arena_id, "camera_id": serial},
            "images": [
                {"role": "black_reference", "path": "captures/black.png", "capture": capture},
                uniform_entry,
            ],
        }
        (artifact_dir / "image_set.json").write_text(json.dumps(image_set), encoding="utf-8")
    index = {
        "artifact_order": order,
        "artifacts_by_id": {a: {"artifact_schema_id": "orange.calibration.image_set"} for a in order},
    }
    (session_dir / "session_index.json").write_text(json.dumps(index), encoding="utf-8")
    return session_dir


def run(argv: list[str]) -> tuple[int, str]:
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        code = tool.main(argv)
    return code, stdout.getvalue() + stderr.getvalue()


def test_geometry_helpers() -> None:
    layout = {
        a: tool.ArenaLayout(a, SERIALS[a], (float(c[0]), float(c[1])), (ARENA_SIZE, ARENA_SIZE),
                            (c[0] - ARENA_SIZE // 2, c[1] - ARENA_SIZE // 2), CANVAS_PX_PER_MM)
        for a, c in CENTERS.items()
    }
    pairs = tool.neighbour_pairs(layout)
    require(pairs == [("arena_1", "right", "arena_2", "left"), ("arena_2", "right", "arena_3", "left")],
            f"neighbour pairs {pairs}")
    # A vertical neighbour faces bottom/top.
    layout["arena_9"] = tool.ArenaLayout("arena_9", "9", (100.0, 260.0), (ARENA_SIZE, ARENA_SIZE),
                                         (40, 200), CANVAS_PX_PER_MM)
    pairs = tool.neighbour_pairs(layout)
    require(("arena_1", "bottom", "arena_9", "top") in pairs, f"vertical pair missing: {pairs}")
    geometry = tool.facing_edge_geometry(layout, ("arena_1", "right", "arena_2", "left"), 10.0)
    require(abs(geometry["gap_mm"] - 10.0) < 1e-9, f"gap {geometry['gap_mm']}")
    require(geometry["tangent_window_canvas_px"] == [59.5, 139.5], f"window {geometry['tangent_window_canvas_px']}")
    # The 2026-11-13 lit-mask rule.
    signal = np.zeros((10, 10), dtype=np.int16)
    signal[2:8, 2:8] = 100
    mask, threshold = tool.lit_mask(signal)
    require(threshold == 50.0 and int(mask.sum()) == 36, f"lit mask {threshold} {mask.sum()}")


def test_end_to_end(tmp: Path) -> None:
    rig = tmp / "rig"
    rig.mkdir(parents=True)
    canvas = write_canvas(rig)
    sessions = tmp / "sessions"
    write_session(sessions, "calsess_high", 200, 400.0, 1.0)
    write_session(sessions, "calsess_low", 100, 2000.0, 0.5)
    out = tmp / "out"
    base = ["--sessions-root", str(sessions), "--citrus-config", str(canvas), "--edge-half-length-mm", "10"]
    code, text = run(base + ["--session", "calsess_high", "--session", "calsess_low", "--out", str(out)])
    require(code == tool.EXIT_OK, f"run failed ({code}): {text}")
    report = json.loads((out / tool.OUTPUT_JSON).read_text(encoding="utf-8"))
    require(report["schema_id"] == tool.SCHEMA_ID and report["schema_version"] == 1, "schema")
    require((out / tool.OUTPUT_RELATIVE_PNG).stat().st_size > 0 and (out / tool.OUTPUT_EDGES_PNG).stat().st_size > 0, "pngs")
    require(report["summary"]["levels_gray_u8"] == [100, 200], f"levels {report['summary']['levels_gray_u8']}")
    session_inputs = {s["session_id"]: s for s in report["inputs"]["sessions"]}
    require(session_inputs["calsess_low"]["exposure_us"] == 2000.0 and session_inputs["calsess_low"]["gray_u8"] == 100, "level metadata")
    require(all(len(c["uniform_gray_reference"]["sha256"]) == 64 for s in session_inputs.values() for c in s["cameras"]), "image sha256")
    require(all(h["canvas_checksum_matches_citrus_config"] for h in report["inputs"]["homographies"]), "canvas checksum")

    levels = report["levels"]
    # Camera 1002 is 10 % more sensitive; medians follow.
    ratio = levels["200"]["1002"]["median_above_black"] / levels["200"]["1001"]["median_above_black"]
    require(abs(ratio - 1.1) < 0.02, f"median ratio {ratio}")
    for serial in SERIALS.values():
        record = levels["200"][serial]
        require(record["saturated_fraction"] == 0.0, "saturation")
        require(1.3 < record["max_over_min"] < 1.6, f"max/min {record['max_over_min']}")
        require(len(record["block_grid_relative"]) == 12, "block grid")
        # Same field shape at half the level: shape difference ~ 0.
        diff = report["shape_max_abs_diff_vs_highest_level"][serial]["100"]
        require(diff < 0.02, f"shape diff {serial} {diff}")
        require(report["shape_max_abs_diff_vs_highest_level"][serial]["200"] == 0.0, "self diff")

    facing = {f"{e['arena_a']}|{e['arena_b']}": e for e in report["facing_edges"]}
    require(set(facing) == {"arena_1|arena_2", "arena_2|arena_3"}, f"pairs {set(facing)}")
    for key, expected in (("arena_1|arena_2", 1 / 1.1), ("arena_2|arena_3", 1.1)):
        for level in ("100", "200"):
            data = facing[key]["levels"][level]
            require(data["status"] == "ok", f"{key} {level} status {data['invalid_reasons']}")
            require(abs(data["ratio_a_over_b"] - expected) < 0.01, f"{key} {level} ratio {data['ratio_a_over_b']} != {expected}")
            require(data["lit_fraction_a"] == 1.0 and data["lit_fraction_b"] == 1.0, f"{key} lit fraction")
            require(data["pixels_a"] > 1000 and data["pixels_b"] > 1000, f"{key} strip pixels")
    require(facing["arena_1|arena_2"]["side_a"] == "right" and facing["arena_1|arena_2"]["side_b"] == "left", "sides")

    # Never overwrite.
    code, text = run(base + ["--session", "calsess_high", "--out", str(out)])
    require(code == tool.EXIT_OUTPUT_EXISTS and "refusing to overwrite" in text, f"overwrite not refused: {code} {text}")


def test_refusals(tmp: Path) -> None:
    rig = tmp / "rig"
    rig.mkdir(parents=True)
    canvas = write_canvas(rig)
    sessions = tmp / "sessions"
    write_session(sessions, "calsess_nogray", None, 400.0, 1.0)
    write_session(sessions, "calsess_missing", 128, 400.0, 1.0)
    base = ["--sessions-root", str(sessions), "--citrus-config", str(canvas)]

    code, text = run(base + ["--session", "calsess_nogray", "--out", str(tmp / "o1")])
    require(code == tool.EXIT_INPUT and "--gray" in text, f"missing gray not refused: {code} {text}")
    code, text = run(base + ["--session", "calsess_nogray", "--gray", "50", "--out", str(tmp / "o2")])
    require(code == tool.EXIT_OK, f"--gray fallback failed: {code} {text}")
    report = json.loads((tmp / "o2" / tool.OUTPUT_JSON).read_text(encoding="utf-8"))
    require(report["inputs"]["sessions"][0]["gray_source"] == "cli", "gray source")
    code, text = run(base + ["--session", "calsess_missing", "--gray", "50", "--out", str(tmp / "o3")])
    require(code == tool.EXIT_INPUT and "says 128" in text, f"conflicting --gray not refused: {code} {text}")

    (sessions / "calsess_missing" / "artifacts" / "Cam1002_arena_2" / "captures" / "uniform.png").unlink()
    code, text = run(base + ["--session", "calsess_missing", "--out", str(tmp / "o4")])
    require(code == tool.EXIT_INPUT and "missing images" in text and "uniform.png" in text, f"missing image: {code} {text}")
    require(not (tmp / "o4").exists(), "output written despite missing image")

    rig2 = tmp / "rig2"
    rig2.mkdir()
    canvas2 = write_canvas(rig2, direction="final_display_canvas_px_to_camera_native_px")
    code, text = run(["--sessions-root", str(sessions), "--citrus-config", str(canvas2),
                      "--session", "calsess_nogray", "--gray", "50", "--out", str(tmp / "o5")])
    require(code == tool.EXIT_INPUT and "direction" in text, f"wrong homography direction not refused: {code} {text}")


def main() -> int:
    test_geometry_helpers()
    with tempfile.TemporaryDirectory(prefix="brightness_map_tests_") as tmp:
        test_end_to_end(Path(tmp) / "e2e")
    with tempfile.TemporaryDirectory(prefix="brightness_map_tests_") as tmp:
        test_refusals(Path(tmp))
    print("brightness_map_tests: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
