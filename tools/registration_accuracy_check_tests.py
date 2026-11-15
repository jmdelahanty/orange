#!/usr/bin/env python3
"""Math checks for scripts/registration_accuracy_check.py on synthetic data (numpy only)."""

from __future__ import annotations

import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import registration_accuracy_check as rac  # noqa: E402

failures: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)
        print("FAIL " + message)


def close(a: float, b: float, tol: float) -> bool:
    return abs(a - b) <= tol


def test_similarity_recovers_known_transform() -> None:
    rng = np.random.default_rng(1)
    src = rng.uniform(-30, 30, size=(29, 2))
    angle = math.radians(0.7)
    rot = np.array([[math.cos(angle), -math.sin(angle)], [math.sin(angle), math.cos(angle)]])
    dst = (1.004 * (rot @ src.T)).T + np.array([0.35, -0.2])
    fit = rac.fit_similarity(src, dst)
    require(fit["fitted"], "similarity fitted")
    require(close(fit["scale"], 1.004, 1e-9), f"similarity scale {fit['scale']}")
    require(close(fit["rotation_deg"], 0.7, 1e-7), f"similarity rotation {fit['rotation_deg']}")
    require(close(fit["translation"][0], 0.35, 1e-9) and close(fit["translation"][1], -0.2, 1e-9), "similarity shift")
    require(fit["rms_after_fit"] < 1e-9 and not fit["reflection"], "similarity exact, no reflection")
    noisy = dst + rng.normal(0, 0.05, size=dst.shape)
    require(close(rac.fit_similarity(src, noisy)["rms_after_fit"], 0.05 * math.sqrt(2), 0.03), "similarity rms with noise")


def test_affine_needs_three_spread_points() -> None:
    require(not rac.fit_affine(np.zeros((2, 2)), np.zeros((2, 2)))["fitted"], "affine refuses 2 points")
    line = np.array([[0.0, 0.0], [1.0, 1.0], [2.0, 2.0]])
    require(rac.fit_affine(line, line).get("reason") == "collinear", "affine refuses collinear points")
    src = np.array([[0.0, 0.0], [10.0, 0.0], [0.0, 10.0], [7.0, 3.0]])
    a = np.array([[1.01, 0.002], [-0.001, 0.995]])
    fit = rac.fit_affine(src, (a @ src.T).T + [0.5, 0.25])
    require(fit["fitted"] and close(fit["scale_x"], math.hypot(1.01, -0.001), 1e-9), "affine scale_x")
    require(close(fit["translation"][0], 0.5, 1e-9), "affine shift")


def disk_image(size: int, cx: float, cy: float, radius: float, inside: float, outside: float) -> np.ndarray:
    rows, cols = np.mgrid[0:size, 0:size]
    return np.where(np.hypot(cols - cx, rows - cy) <= radius, inside, outside).astype(np.float64)


def test_dot_centroid() -> None:
    signal = disk_image(400, 211.3, 187.6, 30.0, 255.0, 0.0)
    signal += disk_image(400, 380.0, 380.0, 30.0, 255.0, 0.0)  # a neighbour in the corner
    result = rac.measure_dot(signal, (200.0, 200.0), 30.0, 25.0)
    require(result["found"], "dot found")
    require(math.hypot(result["camera_px"][0] - 211.3, result["camera_px"][1] - 187.6) < 0.3,
            f"dot centroid {result['camera_px']}")
    require(not rac.measure_dot(np.zeros((400, 400)), (200.0, 200.0), 30.0, 25.0)["found"], "no dot in a dark image")


def test_edge_support_matches_the_gate_rule() -> None:
    image = disk_image(300, 150.0, 150.0, 100.0, 120.0, 20.0).astype(np.uint8)
    require(close(rac.edge_support_fraction(image, 150.0, 150.0, 100.0), 1.0, 1e-12), "edge support on the true rim")
    require(rac.edge_support_fraction(image, 150.0, 150.0, 115.0) == 0.0, "no edge support 15 px off")
    require(rac.edge_support_fraction(image, 150.0, 150.0, 5.0) == -1.0, "radius <= search is invalid")
    weak = disk_image(300, 150.0, 150.0, 100.0, 30.0, 20.0).astype(np.uint8)  # a 10 DN step
    require(rac.edge_support_fraction(weak, 150.0, 150.0, 100.0) == 0.0, "a 10 DN step is below 12 DN")


def test_radius_band() -> None:
    model = {"s_floor_px_per_mm": 53.0, "k_px": 41000.0, "p_floor_mm": 8.13, "e_mm": 1.5}
    band = rac.radius_band(model)
    require(band["expected_min_px"] < band["expected_max_px"], "band ordered")
    require(close(band["accept_min_px"], band["expected_min_px"] * 0.98, 1e-9), "band -2 %")
    require(close(band["z_low_mm"], 8.13 + 2.8 * (1 - 1 / 1.333), 1e-12), "z_low from the wall seen through water")


def test_crosshair_detection() -> None:
    size, cx, cy, r = 600, 301.4, 297.2, 240.0
    rows, cols = np.mgrid[0:size, 0:size]
    signal = np.zeros((size, size))
    on_cross = ((np.abs(cols - cx) <= 2.0) & (np.abs(rows - cy) <= r)) | ((np.abs(rows - cy) <= 2.0) & (np.abs(cols - cx) <= r))
    on_outline = np.abs(np.hypot(cols - cx, rows - cy) - r) <= 2.0
    signal[on_cross | on_outline] = 200.0
    identity = np.eye(3)
    result = rac.detect_crosshair(signal, identity, (300.0, 300.0), r, 20.0)
    require(result["found"], f"crosshair found ({result.get('reason')})")
    if result["found"]:
        ox, oy = result["centre_camera_px"]
        require(math.hypot(ox - cx, oy - cy) < 0.5, f"crosshair centre {result['centre_camera_px']}")
        require(close(result["angle_between_arms_deg"], 90.0, 0.2), "arms perpendicular")
        outline = result.get("outline_camera_px")
        require(bool(outline) and math.hypot(outline["x"] - cx, outline["y"] - cy) < 1.0 and close(outline["radius"], r, 1.0),
                f"outline fit {outline}")


def test_ransac_circle_rejects_outliers() -> None:
    rng = np.random.default_rng(3)
    t = rng.uniform(0, 2 * math.pi, 300)
    xs, ys = 1000 + 800 * np.cos(t), 900 + 800 * np.sin(t)
    xs[:60] += rng.uniform(25, 60, 60)  # a second edge on one side
    fit = rac.ransac_circle(xs + rng.normal(0, 0.5, 300), ys + rng.normal(0, 0.5, 300))
    require(fit["found"] and math.hypot(fit["x"] - 1000, fit["y"] - 900) < 1.0 and close(fit["radius"], 800, 1.0),
            f"ransac circle {fit.get('x')}, {fit.get('y')}, {fit.get('radius')}")
    require(fit["inliers"] >= 230, "ransac keeps the true points")


def test_two_rims_separates_floor_and_wall_top() -> None:
    size = 900
    rows, cols = np.mgrid[0:size, 0:size]
    rho = np.hypot(cols - 450.0, rows - 450.0)
    image = np.where(rho <= 340, 200.0, np.where(rho <= 360, 90.0, 10.0)).astype(np.uint8)  # floor, wall band, outside
    rims = rac.fit_two_rims(image, (452.0, 448.0, 350.0), inward_px=60, outward_px=60)
    floor, top = rims["floor_edge"], rims["wall_top"]
    require(floor["found"] and close(floor["radius"], 340.5, 2.5), f"floor edge radius {floor.get('radius')}")
    require(top["found"] and close(top["radius"], 360.5, 1.5), f"wall top radius {top.get('radius')}")
    require(all(v["inliers"] > 0 for v in top["per_side"].values()), "every side has wall-top points")


def test_arc_coverage_and_bootstrap_on_a_partial_arc() -> None:
    rng = np.random.default_rng(5)
    full = rng.uniform(0, 2 * math.pi, 400)
    part = rng.uniform(math.radians(-150), math.radians(60), 400)  # 210 deg arc, gap centred at 135 deg
    sig = {}
    for name, t in (("full", full), ("part", part)):
        xs = 2000 + 1500 * np.cos(t) + rng.normal(0, 1.0, t.size)
        ys = 2000 + 1500 * np.sin(t) + rng.normal(0, 1.0, t.size)
        cov = rac.arc_coverage(xs, ys, 2000, 2000)
        sig[name] = rac.bootstrap_circle(xs, ys)["centre_sigma_px"]
        if name == "part":
            require(200 <= cov["degrees_covered"] <= 215, f"partial arc coverage {cov['degrees_covered']}")
            require(140 <= cov["largest_gap_deg"] <= 160 and cov["largest_gap_side"] in ("bottom", "left"),
                    f"gap {cov['largest_gap_deg']} at {cov['largest_gap_centre_deg']} ({cov['largest_gap_side']})")
        else:
            require(cov["degrees_covered"] >= 330, "full arc coverage")
    require(sig["part"] > sig["full"], f"a partial arc has the larger centre uncertainty {sig}")


def test_grid_scenes(tmp: Path) -> None:
    path = tmp / "scenes.json"
    rac.emit_grid_scenes(path, ["arena_1", "arena_2"])
    import json

    scenes = json.loads(path.read_text())["scenes"]
    require(len(scenes) == 2, "two grid scenes")
    for scene in scenes:
        dots = scene["scene_options"]["dots"]
        require(len(dots) <= 64, "Citrus limit of 64 dots per scene")
        require(all(math.hypot(*d["offset_mm"]) + d["diameter_mm"] / 2 <= 35.0 for d in dots), "dots inside 35 mm")


def main() -> int:
    import tempfile

    test_similarity_recovers_known_transform()
    test_affine_needs_three_spread_points()
    test_dot_centroid()
    test_edge_support_matches_the_gate_rule()
    test_radius_band()
    test_crosshair_detection()
    test_ransac_circle_rejects_outliers()
    test_two_rims_separates_floor_and_wall_top()
    test_arc_coverage_and_bootstrap_on_a_partial_arc()
    with tempfile.TemporaryDirectory() as tmp:
        test_grid_scenes(Path(tmp))
    if failures:
        print(f"{len(failures)} failure(s)")
        return 1
    print("registration_accuracy_check_tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
