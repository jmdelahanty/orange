#!/usr/bin/env python3
"""Does the registration match what the cameras see? A read-only check.

    registration_accuracy_check.py --session <calsess id|dir> [--rim-observation <observation.json> ...]
                                   [--out <dir>] [--max-residual-mm 1.0]
    registration_accuracy_check.py --emit-grid-scenes <scenes.json>      # the rig capture's dot grid

Two checks per camera, from a guided-capture session made with --static-dots-scenes
(scripts/run_gui_guided_capture_smoke.sh; needs the NIR filters off, since the cameras must
see the projector):

A. Projection ("translations"). Every dot Citrus drew (its scene status: center_canvas_px,
   radius_px) is taken to the camera with the inverse of the active Citrus homography
   (camera native px -> final display canvas px, the pointer files scripts/dot_contrast.py and
   scripts/brightness_map.py use). The dot's actual centroid is measured near that prediction
   (image minus the session's black_reference, intensity-weighted, saturated disks allowed) and
   mapped back to the canvas. The residual is reported in camera px, canvas px and mm at the
   projection plane (canvas px / the arena's pixels_per_mm_projector). Per camera, a similarity
   (and, with 3 or more spread points, an affine) correction from commanded to measured canvas
   points says whether the homography is off by a shift, a rotation or a scale, i.e. whether it
   needs a refit.

B. Dish registration. For each camera's active physical registration pointer
   (~/orange_data/calibrations/active/physical_dish_registration/Cam<serial>.json, or a rim
   observation given with --rim-observation) the accepted inner-rim circle is checked on a lit
   frame: edge support exactly as the C++ gate computes it (src/dish_rim_auto_accept.cpp,
   orange_dish_rim_auto_accept_v1: 720 points, strongest 1-px radial step within +/-6 px,
   >= 12 DN, pass >= 0.40), the radius against the physical band (80 mm design inner
   diameter x the fish-plane scale at the rim, +/-2 %), and the dish centre mapped through
   the homography into the canvas against the arena's experimental-area centre, in mm.

C. Crosshair (recipe experimental_area_center_and_outline: Citrus draws each arena's
   experimental-area outline circle and a full-diameter centre cross, 2 canvas px wide, red).
   The cross centre is found directly in the camera image (black-subtracted): each arm is a
   weighted total-least-squares line fit, the centre is their intersection. It gives
   - dish placement, the PRIMARY number: the fitted rim centre against the observed cross, in
     camera px and in mm with the fish-plane scale at the rim (also at the projection film).
     It does not use the homography;
   - the homography at the centre: the observed cross against its homography prediction, mm;
   - the projected outline circle (algebraic fit), overlaid with the rim circle.

Frames are found by the Citrus scene status recipe_id saved with each image, not by image
role, so a recipe sequence's role labels do not matter.

Writes <out>/registration_accuracy.json and downsampled overlay PNGs; never writes anywhere
else. Default --out: ~/orange_data/registration_accuracy/<UTC stamp>/.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import brightness_map as bm  # noqa: E402
import dot_contrast as dc  # noqa: E402
from orange_paths import orange_data_root  # noqa: E402

SCHEMA_ID = "orange.diagnostic.registration_accuracy"
SCHEMA_VERSION = 1
DOT_ROLE = dc.DOT_ROLE
BLACK_ROLE = "black_reference"
CROSSHAIR_RECIPE = "experimental_area_center_and_outline"
# Image roles used when an image carries no Citrus scene status.
ROLE_FALLBACK = {"black_reference": ("black_reference",), "static_dots": (DOT_ROLE,),
                 "uniform_gray": ("uniform_gray_reference",), CROSSHAIR_RECIPE: ("crosshair_on",)}

# The C++ gate (orange_dish_rim_auto_accept_v1), kept identical here.
EDGE_SAMPLES = 720
EDGE_SEARCH_PX = 6.0
EDGE_MIN_STEP_DN = 12.0
EDGE_MIN_FRACTION = 0.40
DISH_INNER_DIAMETER_MM = 80.0
# Calipered 2026-11-15 (Jeremy; 80.4 / 79.84 / 80.2 mm, consistent across angles). Used for the
# fixed-radius fits here; the gate's radius band keeps the design 80 mm (orange_dish_rim_auto_accept_v1).
DISH_CALIPERED_DIAMETER_MM = 80.15
DISH_CALIPERED_DIAMETER_SIGMA_MM = 0.3
DISH_WALL_HEIGHT_MM = 2.8
WATER_INDEX = 1.333
RADIUS_TOLERANCE_FRACTION = 0.02
FRESHNESS_HOURS = 24.0


# --------------------------------------------------------------------------
# Geometry (also used by tools/registration_accuracy_check_tests.py)


def fit_similarity(src: np.ndarray, dst: np.ndarray) -> dict[str, Any]:
    """Least-squares similarity dst ~ s R src + t (Umeyama). src, dst: (n, 2)."""
    src = np.asarray(src, np.float64)
    dst = np.asarray(dst, np.float64)
    n = len(src)
    if n < 2:
        return {"n": n, "fitted": False}
    mu_s, mu_d = src.mean(0), dst.mean(0)
    a, b = src - mu_s, dst - mu_d
    var_s = (a ** 2).sum() / n
    if var_s <= 0:
        return {"n": n, "fitted": False}
    cov = b.T @ a / n
    u, d, vt = np.linalg.svd(cov)
    sign = np.eye(2)
    if np.linalg.det(u) * np.linalg.det(vt) < 0:
        sign[1, 1] = -1.0
    rot = u @ sign @ vt
    scale = float(np.trace(np.diag(d) @ sign) / var_s)
    trans = mu_d - scale * rot @ mu_s
    pred = (scale * (rot @ src.T)).T + trans
    rms = float(np.sqrt(((pred - dst) ** 2).sum(1).mean()))
    return {"n": n, "fitted": True, "scale": scale, "rotation_deg": math.degrees(math.atan2(rot[1, 0], rot[0, 0])),
            "translation": trans.tolist(), "rms_after_fit": rms, "reflection": bool(sign[1, 1] < 0)}


def fit_affine(src: np.ndarray, dst: np.ndarray) -> dict[str, Any]:
    """Least-squares affine dst ~ A src + t; needs 3 or more non-collinear points."""
    src = np.asarray(src, np.float64)
    dst = np.asarray(dst, np.float64)
    n = len(src)
    if n < 3:
        return {"n": n, "fitted": False}
    design = np.hstack([src, np.ones((n, 1))])
    if np.linalg.matrix_rank(design) < 3:
        return {"n": n, "fitted": False, "reason": "collinear"}
    coef, *_ = np.linalg.lstsq(design, dst, rcond=None)
    pred = design @ coef
    a = coef[:2].T
    sx, sy = float(np.hypot(*a[:, 0])), float(np.hypot(*a[:, 1]))
    return {"n": n, "fitted": True, "matrix": a.tolist(), "translation": coef[2].tolist(),
            "scale_x": sx, "scale_y": sy, "shear_deg": math.degrees(math.asin(
                max(-1.0, min(1.0, float(a[:, 0] @ a[:, 1]) / (sx * sy))))),
            "rms_after_fit": float(np.sqrt(((pred - dst) ** 2).sum(1).mean()))}


def bilinear(gray: np.ndarray, x: float, y: float) -> float | None:
    h, w = gray.shape
    if not (x >= 0.0 and y >= 0.0 and x <= w - 1.0 and y <= h - 1.0):
        return None
    x0, y0 = min(int(x), w - 2), min(int(y), h - 2)
    fx, fy = x - x0, y - y0
    g = gray
    return float((1 - fx) * (1 - fy) * g[y0, x0] + fx * (1 - fy) * g[y0, x0 + 1] +
                 (1 - fx) * fy * g[y0 + 1, x0] + fx * fy * g[y0 + 1, x0 + 1])


def edge_support_fraction(gray: np.ndarray, cx: float, cy: float, radius: float) -> float:
    """dish_rim_edge_support_fraction (src/dish_rim_auto_accept.cpp), point for point."""
    if gray.ndim != 2 or gray.shape[0] < 2 or gray.shape[1] < 2 or not all(map(math.isfinite, (cx, cy, radius))) \
            or radius <= EDGE_SEARCH_PX:
        return -1.0
    g = gray.astype(np.float64)
    supported = 0
    offsets = np.arange(-EDGE_SEARCH_PX, EDGE_SEARCH_PX + 1e-9, 1.0)
    for index in range(EDGE_SAMPLES):
        angle = 2.0 * math.pi * (index + 0.5) / EDGE_SAMPLES
        ux, uy = math.cos(angle), math.sin(angle)
        values = []
        for offset in offsets:
            v = bilinear(g, cx + (radius + offset) * ux, cy + (radius + offset) * uy)
            if v is None:
                values = None
                break
            values.append(v)
        if values is not None and max(abs(b - a) for a, b in zip(values, values[1:])) >= EDGE_MIN_STEP_DN:
            supported += 1
    return supported / EDGE_SAMPLES


def scale_at(model: dict[str, float], z_mm: float) -> float:
    return 1.0 / (1.0 / model["s_floor_px_per_mm"] - (z_mm - model["p_floor_mm"] - model["e_mm"]) / model["k_px"])


def radius_band(model: dict[str, float]) -> dict[str, float]:
    z_low = model["p_floor_mm"] + DISH_WALL_HEIGHT_MM * (1.0 - 1.0 / WATER_INDEX)
    z_high = model["p_floor_mm"] + DISH_WALL_HEIGHT_MM
    r_mm = DISH_INNER_DIAMETER_MM / 2.0
    lo, hi = r_mm * scale_at(model, z_low), r_mm * scale_at(model, z_high)
    return {"z_low_mm": z_low, "z_high_mm": z_high, "expected_min_px": lo, "expected_max_px": hi,
            "accept_min_px": lo * (1 - RADIUS_TOLERANCE_FRACTION), "accept_max_px": hi * (1 + RADIUS_TOLERANCE_FRACTION),
            "px_per_mm_at_rim": (scale_at(model, z_low) + scale_at(model, z_high)) / 2.0}


def scale_model(fish_plane: dict[str, Any], serial: str) -> dict[str, float] | None:
    camera = (fish_plane.get("cameras") or {}).get(serial) or {}
    fit = camera.get("fit") or {}
    params = fish_plane.get("parameters") or {}

    def pick(key: str, *blocks: dict[str, Any]) -> float | None:
        for block in blocks:
            if isinstance(block.get(key), (int, float)) and math.isfinite(block[key]):
                return float(block[key])
        return None

    values = {"s_floor_px_per_mm": pick("s_floor_px_per_mm", camera, fit), "k_px": pick("k_px", camera, fit),
              "p_floor_mm": pick("p_floor_mm", params), "e_mm": pick("e_mm", params)}
    if any(v is None for v in values.values()) or values["s_floor_px_per_mm"] <= 0 or values["k_px"] <= 0:
        return None
    return values  # type: ignore[return-value]


def measure_dot(signal: np.ndarray, predicted: tuple[float, float], radius_cam: float, search_cam: float) -> dict[str, Any]:
    """Intensity-weighted centroid of the bright disk near the prediction (numpy only).

    In a window of 1.5 x the expected camera radius + the search radius around the prediction,
    pixels at >= half the window peak are kept; the centroid is then re-taken three times
    within 1.3 x the expected radius of the current centroid, so a neighbouring dot or a
    reflection at the window's edge does not pull it."""
    h, w = signal.shape
    half = int(math.ceil(radius_cam * 1.5 + search_cam))
    x0, x1 = max(0, int(predicted[0]) - half), min(w, int(predicted[0]) + half + 1)
    y0, y1 = max(0, int(predicted[1]) - half), min(h, int(predicted[1]) + half + 1)
    if x1 - x0 < 8 or y1 - y0 < 8:
        return {"found": False, "reason": "prediction_outside_image"}
    win = signal[y0:y1, x0:x1]
    peak = float(np.percentile(win, 99.9))
    if peak <= 10.0:
        return {"found": False, "reason": "no_signal_near_prediction", "window_peak_dn": round(peak, 2)}
    lit = win >= 0.5 * peak
    rows, cols = np.mgrid[0:win.shape[0], 0:win.shape[1]]
    # Start at the lit pixel cluster nearest the prediction: the brightest lit pixel within
    # radius + search of it.
    px, py = predicted[0] - x0, predicted[1] - y0
    near = lit & (np.hypot(cols - px, rows - py) <= radius_cam + search_cam)
    if not near.any():
        return {"found": False, "reason": "no_lit_pixels_near_prediction", "window_peak_dn": round(peak, 2)}
    weights = np.where(near, win, 0.0)
    cx, cy = (weights * cols).sum() / weights.sum(), (weights * rows).sum() / weights.sum()
    keep = near
    for _ in range(3):
        keep = lit & (np.hypot(cols - cx, rows - cy) <= 1.3 * radius_cam)
        if not keep.any():
            return {"found": False, "reason": "centroid_lost"}
        weights = np.where(keep, win, 0.0)
        cx, cy = (weights * cols).sum() / weights.sum(), (weights * rows).sum() / weights.sum()
    expected_area = math.pi * radius_cam ** 2
    return {"found": True, "camera_px": [x0 + float(cx), y0 + float(cy)], "blob_area_px": int(keep.sum()),
            "expected_area_px": round(expected_area, 1), "saturated_px": int((win[keep] >= 254).sum()),
            "window_peak_dn": round(peak, 2)}


def fit_line_weighted(xs: np.ndarray, ys: np.ndarray, w: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Weighted total-least-squares line: (point, unit direction)."""
    total = w.sum()
    mx, my = (w * xs).sum() / total, (w * ys).sum() / total
    dx, dy = xs - mx, ys - my
    cov = np.array([[(w * dx * dx).sum(), (w * dx * dy).sum()], [(w * dx * dy).sum(), (w * dy * dy).sum()]]) / total
    values, vectors = np.linalg.eigh(cov)
    return np.array([mx, my]), vectors[:, int(np.argmax(values))]


def intersect_lines(p1: np.ndarray, d1: np.ndarray, p2: np.ndarray, d2: np.ndarray) -> np.ndarray | None:
    det = d1[0] * (-d2[1]) - d1[1] * (-d2[0])
    if abs(det) < 1e-9:
        return None
    rhs = p2 - p1
    t = (rhs[0] * (-d2[1]) - rhs[1] * (-d2[0])) / det
    return p1 + t * d1


def fit_circle_weighted(xs: np.ndarray, ys: np.ndarray, w: np.ndarray) -> tuple[float, float, float]:
    """Weighted algebraic (Kasa) circle fit: (cx, cy, r)."""
    a = np.column_stack([xs, ys, np.ones_like(xs)]) * np.sqrt(w)[:, None]
    b = (xs * xs + ys * ys) * np.sqrt(w)
    sol, *_ = np.linalg.lstsq(a, b, rcond=None)
    cx, cy = sol[0] / 2.0, sol[1] / 2.0
    return float(cx), float(cy), float(math.sqrt(max(0.0, sol[2] + cx * cx + cy * cy)))


def _band_pixels(signal: np.ndarray, p: np.ndarray, d: np.ndarray, half_width: float,
                 t_ranges: list[tuple[float, float]]) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Pixels within half_width of the line p + t d, with t in any of t_ranges."""
    h, w = signal.shape
    ends = [p + d * t for rng in t_ranges for t in rng]
    xs_all = [e[0] for e in ends]
    ys_all = [e[1] for e in ends]
    x0 = max(0, int(min(xs_all) - half_width - 2)); x1 = min(w, int(max(xs_all) + half_width + 3))
    y0 = max(0, int(min(ys_all) - half_width - 2)); y1 = min(h, int(max(ys_all) + half_width + 3))
    if x1 <= x0 or y1 <= y0:
        return np.zeros(0), np.zeros(0), np.zeros(0)
    rows, cols = np.mgrid[y0:y1, x0:x1]
    rel_x, rel_y = cols - p[0], rows - p[1]
    t = rel_x * d[0] + rel_y * d[1]
    dist = np.abs(rel_x * (-d[1]) + rel_y * d[0])
    keep = dist <= half_width
    in_range = np.zeros_like(keep)
    for lo, hi in t_ranges:
        in_range |= (t >= lo) & (t <= hi)
    keep &= in_range
    return cols[keep].astype(np.float64), rows[keep].astype(np.float64), signal[y0:y1, x0:x1][keep]


def detect_crosshair(signal: np.ndarray, inverse: np.ndarray, centre_canvas: tuple[float, float],
                     radius_canvas: float, search_cam: float) -> dict[str, Any]:
    """The projected cross centre and outline circle in camera px, found in the image."""
    ex, ey = centre_canvas
    arms = {}
    for name, (ux, uy) in (("horizontal", (1.0, 0.0)), ("vertical", (0.0, 1.0))):
        ts = np.linspace(-radius_canvas, radius_canvas, 41)
        cx, cy = bm.map_points(inverse, ex + ux * ts, ey + uy * ts)
        p, d = fit_line_weighted(cx, cy, np.ones_like(cx))
        length = float(np.hypot(cx[-1] - cx[0], cy[-1] - cy[0]))
        # Arm pixels: away from the centre (the other arm) and from the outline circle.
        t_ranges = [(-0.88 * length / 2, -0.12 * length / 2), (0.12 * length / 2, 0.88 * length / 2)]
        line_p, line_d = p, d
        half = search_cam
        for _ in range(3):
            xs, ys, vs = _band_pixels(signal, line_p, line_d, half, t_ranges)
            if vs.size < 50:
                return {"found": False, "reason": f"{name}_arm_no_pixels"}
            peak = float(np.percentile(vs, 99.5))
            if peak <= 10.0:
                return {"found": False, "reason": f"{name}_arm_no_signal", "peak_dn": peak}
            sel = vs >= 0.5 * peak
            if sel.sum() < 50:
                return {"found": False, "reason": f"{name}_arm_too_few_lit_pixels"}
            new_p, new_d = fit_line_weighted(xs[sel], ys[sel], vs[sel])
            # Keep the along-line parameterisation anchored at the predicted centre.
            if new_d @ line_d < 0:
                new_d = -new_d
            centre_on_line = new_p + new_d * ((p - new_p) @ new_d)
            line_p, line_d = centre_on_line, new_d
            width = float(np.sqrt(((((xs[sel] - new_p[0]) * -new_d[1] + (ys[sel] - new_p[1]) * new_d[0]) ** 2) * vs[sel]).sum()
                                  / vs[sel].sum()))
            half = max(4.0 * width + 4.0, 12.0)
        arms[name] = {"point": line_p.tolist(), "direction": line_d.tolist(), "lit_pixels": int(sel.sum()),
                      "rms_half_width_px": round(width, 2), "peak_dn": round(peak, 1)}
    hp, hd = np.array(arms["horizontal"]["point"]), np.array(arms["horizontal"]["direction"])
    vp, vd = np.array(arms["vertical"]["point"]), np.array(arms["vertical"]["direction"])
    centre = intersect_lines(hp, hd, vp, vd)
    if centre is None:
        return {"found": False, "reason": "arms_parallel", "arms": arms}
    angle = math.degrees(math.acos(min(1.0, abs(float(hd @ vd)))))
    result: dict[str, Any] = {"found": True, "centre_camera_px": centre.tolist(), "arms": arms,
                              "angle_between_arms_deg": round(angle, 4)}
    # Outline: lit pixels near the predicted circle, away from the arms.
    angles = np.linspace(0, 2 * math.pi, 180, endpoint=False)
    ox, oy = bm.map_points(inverse, ex + radius_canvas * np.cos(angles), ey + radius_canvas * np.sin(angles))
    pcx, pcy, pr = fit_circle_weighted(ox, oy, np.ones_like(ox))
    h, w = signal.shape
    x0, x1 = max(0, int(pcx - pr - search_cam - 2)), min(w, int(pcx + pr + search_cam + 3))
    y0, y1 = max(0, int(pcy - pr - search_cam - 2)), min(h, int(pcy + pr + search_cam + 3))
    rows, cols = np.mgrid[y0:y1:2, x0:x1:2]  # every other pixel: plenty for a circle fit
    rho = np.hypot(cols - pcx, rows - pcy)
    near = np.abs(rho - pr) <= search_cam
    for arm in (arms["horizontal"], arms["vertical"]):
        ap, ad = np.array(arm["point"]), np.array(arm["direction"])
        dist = np.abs((cols - ap[0]) * -ad[1] + (rows - ap[1]) * ad[0])
        near &= dist > 4.0 * arm["rms_half_width_px"] + 20.0
    vals = signal[rows[near], cols[near]]
    if vals.size >= 200:
        peak = float(np.percentile(vals, 99.5))
        sel = vals >= 0.5 * peak
        if sel.sum() >= 100 and peak > 10.0:
            fcx, fcy, fr = fit_circle_weighted(cols[near][sel].astype(np.float64), rows[near][sel].astype(np.float64),
                                               vals[sel])
            result["outline_camera_px"] = {"x": fcx, "y": fcy, "radius": fr, "lit_pixels": int(sel.sum())}
    result["predicted_outline_camera_px"] = {"x": pcx, "y": pcy, "radius": pr}
    return result


def bilinear_many(gray: np.ndarray, xs: np.ndarray, ys: np.ndarray) -> np.ndarray:
    """Vectorised bilinear sample; NaN outside the image."""
    h, w = gray.shape
    inside = (xs >= 0) & (ys >= 0) & (xs <= w - 1) & (ys <= h - 1)
    x = np.clip(xs, 0, w - 1.000001)
    y = np.clip(ys, 0, h - 1.000001)
    x0, y0 = np.minimum(x.astype(int), w - 2), np.minimum(y.astype(int), h - 2)
    fx, fy = x - x0, y - y0
    g = gray.astype(np.float64)
    out = ((1 - fx) * (1 - fy) * g[y0, x0] + fx * (1 - fy) * g[y0, x0 + 1] +
           (1 - fx) * fy * g[y0 + 1, x0] + fx * fy * g[y0 + 1, x0 + 1])
    return np.where(inside, out, np.nan)


def detect_rim(gray: np.ndarray, cx0: float, cy0: float, r0: float, search_fraction: float = 0.08,
               angles: int = 720) -> dict[str, Any]:
    """Today's inner rim on a lit (uniform gray) frame: on each of `angles` rays from the initial
    centre, the strongest bright-to-dark radial step within r0 x (1 +/- search_fraction) (profile
    smoothed over 5 px), then a weighted circle fit with two rounds of 3 x MAD outlier rejection."""
    theta = 2 * math.pi * (np.arange(angles) + 0.5) / angles
    radii = np.arange(r0 * (1 - search_fraction), r0 * (1 + search_fraction), 1.0)
    xs = cx0 + np.outer(np.cos(theta), radii)
    ys = cy0 + np.outer(np.sin(theta), radii)
    prof = bilinear_many(gray, xs, ys)
    kernel = np.ones(5) / 5.0
    edge_x, edge_y, strength = [], [], []
    for i in range(angles):
        row = prof[i]
        if np.isnan(row).any():
            continue
        smooth = np.convolve(row, kernel, mode="same")
        grad = np.diff(smooth)[3:-3]
        j = int(np.argmin(grad))
        if -grad[j] < 1.0:  # under 1 DN per px after smoothing: no edge on this ray
            continue
        r = radii[j + 3] + 0.5
        edge_x.append(cx0 + r * math.cos(theta[i]))
        edge_y.append(cy0 + r * math.sin(theta[i]))
        strength.append(-grad[j])
    if len(edge_x) < 60:
        return {"found": False, "reason": "too_few_edge_points", "edge_points": len(edge_x)}
    ex, ey, ew = np.array(edge_x), np.array(edge_y), np.array(strength)
    keep = np.ones(len(ex), bool)
    for _ in range(3):
        cx, cy, r = fit_circle_weighted(ex[keep], ey[keep], ew[keep])
        resid = np.hypot(ex - cx, ey - cy) - r
        mad = float(np.median(np.abs(resid[keep] - np.median(resid[keep])))) or 0.5
        keep = np.abs(resid) <= max(3.0 * 1.4826 * mad, 2.0)
    cx, cy, r = fit_circle_weighted(ex[keep], ey[keep], ew[keep])
    resid = np.hypot(ex[keep] - cx, ey[keep] - cy) - r
    return {"found": True, "x": cx, "y": cy, "radius": r, "edge_points": int(len(ex)), "inliers": int(keep.sum()),
            "rms_residual_px": float(np.sqrt((resid ** 2).mean())),
            "median_step_dn_per_px_smoothed": round(float(np.median(ew[keep])), 2),
            "initial": {"x": cx0, "y": cy0, "radius": r0}}


SIDES = (("right", -45.0, 45.0), ("bottom", 45.0, 135.0), ("left", 135.0, 225.0), ("top", 225.0, 315.0))


def side_of(theta: np.ndarray) -> np.ndarray:
    deg = np.degrees(theta) % 360.0
    out = np.full(deg.shape, "right", dtype=object)
    for name, lo, hi in SIDES[1:]:
        out[(deg >= lo) & (deg < hi)] = name
    return out


def ransac_circle(xs: np.ndarray, ys: np.ndarray, tol: float = 3.0, iterations: int = 600,
                  seed: int = 7) -> dict[str, Any]:
    """RANSAC circle (3-point samples), then iterative refits on the inliers."""
    n = len(xs)
    if n < 12:
        return {"found": False, "reason": "too_few_points", "points": int(n)}
    rng = np.random.default_rng(seed)
    best = None
    for _ in range(iterations):
        i = rng.choice(n, 3, replace=False)
        try:
            cx, cy, r = fit_circle_weighted(xs[i], ys[i], np.ones(3))
        except np.linalg.LinAlgError:
            continue
        if not (math.isfinite(r) and r > 100):
            continue
        inl = np.abs(np.hypot(xs - cx, ys - cy) - r) <= tol
        if best is None or inl.sum() > best.sum():
            best = inl
    if best is None or best.sum() < 12:
        return {"found": False, "reason": "no_consensus", "points": int(n)}
    inl = best
    for _ in range(4):
        cx, cy, r = fit_circle_weighted(xs[inl], ys[inl], np.ones(int(inl.sum())))
        inl = np.abs(np.hypot(xs - cx, ys - cy) - r) <= tol
    cx, cy, r = fit_circle_weighted(xs[inl], ys[inl], np.ones(int(inl.sum())))
    resid = np.hypot(xs - cx, ys - cy) - r
    return {"found": True, "x": cx, "y": cy, "radius": r, "points": int(n), "inliers": int(inl.sum()),
            "inlier_tolerance_px": tol, "residual_px_median_abs_inliers": float(np.median(np.abs(resid[inl]))),
            "residual_px_max_abs_inliers": float(np.max(np.abs(resid[inl]))),
            "residual_px_median_abs_all": float(np.median(np.abs(resid))), "_inlier_mask": inl, "_resid": resid}


def arc_coverage(xs: np.ndarray, ys: np.ndarray, cx: float, cy: float, min_gap_deg: float = 5.0) -> dict[str, Any]:
    """Degrees of the circle with points: 360 minus every gap between neighbouring points wider
    than min_gap_deg (the 720 rays are 0.5 deg apart), and the largest gap with its location."""
    if len(xs) == 0:
        return {"degrees_covered": 0.0, "largest_gap_deg": 360.0, "largest_gap_centre_deg": None, "largest_gap_side": None}
    ang = np.sort(np.degrees(np.arctan2(ys - cy, xs - cx)) % 360.0)
    gaps = np.diff(np.concatenate([ang, [ang[0] + 360.0]]))
    k = int(np.argmax(gaps))
    largest = float(gaps[k])
    centre = (ang[k] + largest / 2.0) % 360.0
    covered = 360.0 - float(gaps[gaps > min_gap_deg].sum())
    return {"degrees_covered": round(covered, 1), "largest_gap_deg": round(largest, 1),
            "largest_gap_centre_deg": round(centre, 1), "largest_gap_side": str(side_of(np.array([math.radians(centre)]))[0]),
            "min_gap_counted_deg": min_gap_deg, "angle_convention": "image axes: 0 deg = +x (right), 90 deg = +y (down)"}


def bootstrap_circle(xs: np.ndarray, ys: np.ndarray, resamples: int = 300, seed: int = 11) -> dict[str, float]:
    """1-sigma centre and radius uncertainty of the circle fit by bootstrap over the inliers."""
    rng = np.random.default_rng(seed)
    n = len(xs)
    fits = []
    for _ in range(resamples):
        i = rng.integers(0, n, n)
        fits.append(fit_circle_weighted(xs[i], ys[i], np.ones(n)))
    arr = np.array(fits)
    return {"x_sigma_px": float(arr[:, 0].std()), "y_sigma_px": float(arr[:, 1].std()),
            "radius_sigma_px": float(arr[:, 2].std()),
            "centre_sigma_px": float(math.hypot(arr[:, 0].std(), arr[:, 1].std())), "resamples": resamples}


EDGE_SIGMA_PX = 2.0
EDGE_HALF_WIDTH_PX = int(3 * EDGE_SIGMA_PX)  # the derivative-of-Gaussian operator's half-width


def edge_steps(row: np.ndarray, threshold: float) -> tuple[np.ndarray, np.ndarray]:
    """Bright-to-dark steps along a 1 px-sampled profile: Gaussian-smoothed derivative (sigma
    EDGE_SIGMA_PX), local minima at least `threshold` DN/px deep, refined to sub-pixel by a
    parabola. Only positions with EDGE_HALF_WIDTH_PX real samples on BOTH sides are used, so no
    padding enters a step. Returns (positions in samples, step strengths)."""
    k = np.arange(-EDGE_HALF_WIDTH_PX, EDGE_HALF_WIDTH_PX + 1, dtype=np.float64)
    dg = -k * np.exp(-0.5 * (k / EDGE_SIGMA_PX) ** 2)
    dg /= np.abs(dg * k).sum()  # unit response to a unit ramp
    if row.size < 2 * EDGE_HALF_WIDTH_PX + 3:
        return np.zeros(0), np.zeros(0)
    # valid correlation: grad[j] is centred on sample j + EDGE_HALF_WIDTH_PX
    grad = np.correlate(row, dg[::-1], mode="valid")
    pos, strength = [], []
    for j in range(1, grad.size - 1):
        if grad[j] <= grad[j - 1] and grad[j] < grad[j + 1] and -grad[j] >= threshold:
            a, b, c = grad[j - 1], grad[j], grad[j + 1]
            den = a - 2 * b + c
            off = 0.5 * (a - c) / den if den != 0 else 0.0
            pos.append(j + EDGE_HALF_WIDTH_PX + max(-0.5, min(0.5, off)))
            strength.append(-b)
    return np.array(pos), np.array(strength)


def fit_two_rims(gray: np.ndarray, init: tuple[float, float, float], inward_px: float = 80.0,
                 outward_px: float = 110.0, border_margin_px: float = 0.0, angles: int = EDGE_SAMPLES) -> dict[str, Any]:
    """The lit floor's edge and the wall top (inner rim at the top), fitted separately.

    On each of `angles` rays from the initial centre the radial profile r0 - inward_px ..
    r0 + outward_px (stopping border_margin_px inside the frame, so the frame edge is never
    taken for a rim) is smoothed over 5 px; bright-to-dark steps of at least max(1.5 DN/px,
    30 % of the ray's strongest) are kept. The innermost step is the floor's edge candidate,
    the outermost the wall-top candidate (the same point when a ray has one step). Each set gets
    its own RANSAC circle (3 px tolerance)."""
    cx0, cy0, r0 = init
    h, w = gray.shape
    theta = 2 * math.pi * (np.arange(angles) + 0.5) / angles
    radii = np.arange(r0 - inward_px, r0 + outward_px + 1e-9, 1.0)
    xs = cx0 + np.outer(np.cos(theta), radii)
    ys = cy0 + np.outer(np.sin(theta), radii)
    inside = (xs >= border_margin_px) & (ys >= border_margin_px) & (xs <= w - 1 - border_margin_px) & \
        (ys <= h - 1 - border_margin_px)
    prof = bilinear_many(gray, np.where(inside, xs, -1.0), np.where(inside, ys, -1.0))
    floor_r, top_r = np.full(angles, np.nan), np.full(angles, np.nan)
    steps: list[np.ndarray] = [np.zeros(0)] * angles
    step_strength: list[np.ndarray] = [np.zeros(0)] * angles
    truncated = np.zeros(angles, bool)
    contrast = np.full(angles, np.nan)
    for i in range(angles):
        valid = ~np.isnan(prof[i])
        # the window is clipped at the border exclusion (never discarded for crossing it); samples
        # within border_margin_px of the frame are NaN, so an edge point is always >= that inside
        n_valid = int(np.argmin(valid)) if not valid.all() else len(valid)
        truncated[i] = n_valid < len(valid)
        if n_valid < 30:
            continue
        row = prof[i][:n_valid]
        # Threshold relative to this ray's own contrast (floor level minus the dark level), so a
        # dim side with a gradual edge (2010096 right: 43 DN floor, 90 px ramp) is not lost.
        level_in = float(np.median(row[:20]))
        level_out = float(np.median(row[-15:])) if n_valid >= 60 else float(row.min())
        contrast[i] = level_in - level_out
        if contrast[i] < 8.0:
            continue
        pos, strength = edge_steps(row, max(0.4, 0.012 * contrast[i]))
        if pos.size == 0:
            continue
        r_steps = radii[0] + pos
        floor_r[i], top_r[i] = r_steps.min(), r_steps.max()
        steps[i] = r_steps
        step_strength[i] = strength
    out: dict[str, Any] = {"initial": {"x": cx0, "y": cy0, "radius": r0}, "rays": angles,
                           "rays_window_clipped_at_border": int(truncated.sum()),
                           "edge_rule": {"operator": f"derivative of Gaussian, sigma {EDGE_SIGMA_PX} px, half-width "
                                                     f"{EDGE_HALF_WIDTH_PX} px, sub-pixel parabola",
                                         "step_threshold": "max(0.4 DN/px, 1.2 % of the ray's floor-to-dark contrast per px)",
                                         "min_contrast_dn": 8.0,
                                         "border_exclusion_px": EDGE_HALF_WIDTH_PX,
                                         "border_rule": "rays are sampled up to the frame; an edge needs the operator's "
                                                        f"{EDGE_HALF_WIDTH_PX} px of real samples on both sides, so it is "
                                                        f"measurable down to {EDGE_HALF_WIDTH_PX} px from the frame"},
                           "_steps": (theta, steps, cx0, cy0), "_strength": step_strength, "_contrast": contrast,
                           "_shape": (h, w), "_margin": float(EDGE_HALF_WIDTH_PX)}
    sides = side_of(theta)
    for name, rr in (("floor_edge", floor_r), ("wall_top", top_r)):
        ok = ~np.isnan(rr)
        px, py = cx0 + rr[ok] * np.cos(theta[ok]), cy0 + rr[ok] * np.sin(theta[ok])
        fit = ransac_circle(px, py)
        if fit.get("found"):
            inl, resid = fit.pop("_inlier_mask"), fit.pop("_resid")
            per_side = {}
            for side, _, _ in SIDES:
                sel = sides[ok] == side
                si = sel & inl
                per_side[side] = ({"edge_points": int(sel.sum()), "inliers": int(si.sum()),
                                   "median_residual_px": float(np.median(resid[si])) if si.any() else None,
                                   "note": None if si.any() else "NO EDGE POINTS on this side (inliers)"})
            fit["per_side"] = per_side
            fit["arc_coverage"] = arc_coverage(px[inl], py[inl], fit["x"], fit["y"])
            fit["uncertainty"] = bootstrap_circle(px[inl], py[inl])
            fit["_points"] = (px, py, inl)
        out[name] = fit
    return out


def _intervals(deg: np.ndarray, step: float) -> list[list[float]]:
    """Merge sorted ray angles (deg) into [start, end] intervals."""
    if deg.size == 0:
        return []
    deg = np.sort(deg)
    out = [[float(deg[0]), float(deg[0])]]
    for d in deg[1:]:
        if d - out[-1][1] <= step * 1.5:
            out[-1][1] = float(d)
        else:
            out.append([float(d), float(d)])
    if len(out) > 1 and out[0][0] <= step and out[-1][1] >= 360 - step * 1.5:  # wrap
        out[0][0] = out[-1][0] - 360.0
        out.pop()
    return [[round(a, 1), round(b, 1)] for a, b in out]


def coverage_loss(theta: np.ndarray, used_rays: set[int], steps: list[np.ndarray], cx: float, cy: float,
                  radius: float, shape: tuple[int, int], margin: float) -> dict[str, Any]:
    """Why each ray has no point on the fit: (a) the rim is outside the frame there, (b) it is
    inside the frame but within the border exclusion, (c) no edge was found (lighting, glare),
    or (d) an edge was found but not near the fit (rejected)."""
    h, w = shape
    step_deg = 360.0 / len(theta)
    classes: dict[str, list[float]] = {"outside_frame": [], "border_exclusion": [], "no_edge_found": [],
                                       "edge_not_near_fit": []}
    for i, t in enumerate(theta):
        if i in used_rays:
            continue
        x, y = cx + radius * math.cos(t), cy + radius * math.sin(t)
        deg = math.degrees(t) % 360.0
        if not (0 <= x <= w - 1 and 0 <= y <= h - 1):
            classes["outside_frame"].append(deg)
        elif not (margin <= x <= w - 1 - margin and margin <= y <= h - 1 - margin):
            classes["border_exclusion"].append(deg)
        elif steps[i].size == 0:
            classes["no_edge_found"].append(deg)
        else:
            classes["edge_not_near_fit"].append(deg)
    return {k: {"degrees": round(len(v) * step_deg, 1), "angles_deg": _intervals(np.array(v), step_deg)}
            for k, v in classes.items()} | {"angle_convention": "image axes from the fit centre: 0 deg = +x (right), "
                                                                 "90 deg = +y (down)"}


def fixed_radius_fit(theta: np.ndarray, steps: list[np.ndarray], cx0: float, cy0: float, radius: float,
                     tol_px: float = 15.0, iterations: int = 6,
                     strengths: list[np.ndarray] | None = None) -> dict[str, Any]:
    """Centre-only circle fit with a known radius. Each ray contributes the edge step nearest the
    expected radius from the current centre (within tol_px); the centre solves
    min sum (|p - c| - radius)^2 by Gauss-Newton with 3 x MAD trimming; bootstrap uncertainty."""
    cands_x, cands_y, ray, cand_strength = [], [], [], []
    for i, rr in enumerate(steps):
        for k, r in enumerate(rr):
            cands_x.append(cx0 + r * math.cos(theta[i]))
            cands_y.append(cy0 + r * math.sin(theta[i]))
            ray.append(i)
            cand_strength.append(float(strengths[i][k]) if strengths is not None and len(strengths[i]) > k else float("nan"))
    if not cands_x:
        return {"found": False, "reason": "no_edge_steps"}
    px, py, ray, cand_strength = np.array(cands_x), np.array(cands_y), np.array(ray), np.array(cand_strength)
    c = np.array([cx0, cy0])

    def select(c: np.ndarray) -> np.ndarray:
        dist = np.abs(np.hypot(px - c[0], py - c[1]) - radius)
        keep = np.zeros(len(px), bool)
        for i in np.unique(ray):
            k = np.where(ray == i)[0]
            j = k[int(np.argmin(dist[k]))]
            if dist[j] <= tol_px:
                keep[j] = True
        return keep

    def solve(x: np.ndarray, y: np.ndarray, c: np.ndarray) -> np.ndarray:
        for _ in range(20):
            dx, dy = x - c[0], y - c[1]
            d = np.hypot(dx, dy)
            res = d - radius
            jac = -np.column_stack([dx / d, dy / d])
            step, *_ = np.linalg.lstsq(jac, -res, rcond=None)
            c = c + step
            if np.hypot(*step) < 1e-4:
                break
        return c

    keep = select(c)
    for _ in range(iterations):
        if keep.sum() < 12:
            return {"found": False, "reason": "too_few_points_near_expected_radius", "points": int(keep.sum())}
        c = solve(px[keep], py[keep], c)
        res = np.hypot(px[keep] - c[0], py[keep] - c[1]) - radius
        mad = float(np.median(np.abs(res - np.median(res)))) or 0.5
        keep_new = select(c)
        res_all = np.hypot(px - c[0], py - c[1]) - radius
        keep_new &= np.abs(res_all - np.median(res)) <= max(3 * 1.4826 * mad, 2.0)
        if np.array_equal(keep_new, keep):
            break
        keep = keep_new
    x, y = px[keep], py[keep]
    used_rays = set(int(r) for r in ray[keep])
    c = solve(x, y, c)
    res = np.hypot(x - c[0], y - c[1]) - radius
    rng = np.random.default_rng(13)
    boots = np.array([solve(x[i], y[i], c) for i in (rng.integers(0, len(x), len(x)) for _ in range(200))])
    return {"found": True, "x": float(c[0]), "y": float(c[1]), "radius_fixed_px": radius, "points": int(len(x)),
            "residual_px_median_abs": float(np.median(np.abs(res))), "residual_px_max_abs": float(np.max(np.abs(res))),
            "residual_px_median_signed": float(np.median(res)),
            "uncertainty": {"x_sigma_px": float(boots[:, 0].std()), "y_sigma_px": float(boots[:, 1].std()),
                            "centre_sigma_px": float(math.hypot(boots[:, 0].std(), boots[:, 1].std())), "resamples": 200},
            "arc_coverage": arc_coverage(x, y, float(c[0]), float(c[1])), "_points": (x, y, np.ones(len(x), bool)),
            "_used_rays": used_rays, "_kept_rays": ray[keep], "_kept_strength": cand_strength[keep]}


def parallax_check(floor: dict[str, Any], top: dict[str, Any], model: dict[str, float] | None,
                   principal: tuple[float, float]) -> dict[str, Any] | None:
    """Both circles share one physical centre; they sit at different heights, so their image
    centres differ by parallax. Fish-plane model: image = p + s(z) X (p the principal point)."""
    if not (model and floor.get("found") and top.get("found")):
        return None
    band = radius_band(model)
    s_low, s_high, s0 = scale_at(model, band["z_low_mm"]), scale_at(model, band["z_high_mm"]), scale_at(model, 0.0)
    p = np.array(principal)
    c_floor, c_top = np.array([floor["x"], floor["y"]]), np.array([top["x"], top["y"]])
    x_from_floor = (c_floor - p) / s_low      # dish centre in mm (floor-corner height)
    predicted_top = p + s_high * x_from_floor
    film_from_floor = p + s0 * x_from_floor
    film_from_top = p + s0 * (c_top - p) / s_high
    return {
        "principal_point_px": list(principal), "principal_point_source": "image centre (not measured)",
        "heights_mm": {"floor_corner_seen_through_water": band["z_low_mm"], "wall_top": band["z_high_mm"]},
        "px_per_mm": {"floor_corner": s_low, "wall_top": s_high, "film": s0},
        "centre_difference_measured_px": (c_top - c_floor).tolist(),
        "centre_difference_predicted_px": (predicted_top - c_floor).tolist(),
        "radius_ratio_measured": top["radius"] / floor["radius"], "radius_ratio_predicted": s_high / s_low,
        "radius_difference_measured_px": top["radius"] - floor["radius"],
        "radius_difference_predicted_px": floor["radius"] * (s_high / s_low - 1.0),
        "floor_centre_at_film_px": film_from_floor.tolist(), "top_centre_at_film_px": film_from_top.tolist(),
        "film_centres_disagree_px": float(np.hypot(*(film_from_floor - film_from_top))),
    }


def _font(size: int) -> Any:
    from PIL import ImageFont

    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def draw_dish_figures(out: Path, serial: str, gray: np.ndarray, rims: dict[str, Any] | None,
                      cross: list[float] | None, placement: dict[str, Any] | None,
                      registration: dict[str, Any] | None) -> list[str]:
    """Cam<serial>_dish_rim.png (whole frame at 1/4, legend strip below) and
    Cam<serial>_dish_rim_zoom.png (native-resolution crops at the top, right, bottom, left)."""
    from PIL import Image, ImageDraw

    FLOOR, TOP, REG, CROSS, FIXED = (0, 255, 0), (255, 0, 255), (255, 200, 0), (0, 230, 255), (255, 255, 255)
    floor = (rims or {}).get("floor_edge") or {}
    top = (rims or {}).get("wall_top") or {}
    fixed_top = ((rims or {}).get("fixed") or {}).get("wall_top") or {}
    written = []
    factor = 4
    f = float(factor)
    small = downsampled_rgb(gray, factor)
    draw = ImageDraw.Draw(small)
    font = _font(18)

    def circle(d: Any, c: dict[str, Any], colour: tuple[int, int, int], scale: float, ox: float = 0.0, oy: float = 0.0,
               width: int = 3) -> None:
        x, y, r = (c["x"] - ox) / scale, (c["y"] - oy) / scale, c["radius"] / scale
        d.ellipse([x - r, y - r, x + r, y + r], outline=colour, width=width)

    legend = []
    for fit, colour, label in ((floor, FLOOR, "floor edge fit (diagnostic)"),
                               (top, TOP, "free wall top fit (cross-check)")):
        if fit.get("reason") == "floor_diagnostic_off":
            continue
        if fit.get("found"):
            circle(draw, fit, colour, f)
            px, py, inl = fit["_points"]
            for k in range(0, len(px), 3):
                if inl[k]:
                    draw.point((px[k] / f, py[k] / f), fill=colour)
            draw.line([((fit["x"] - 60) / f, fit["y"] / f), ((fit["x"] + 60) / f, fit["y"] / f)], fill=colour, width=3)
            draw.line([(fit["x"] / f, (fit["y"] - 60) / f), (fit["x"] / f, (fit["y"] + 60) / f)], fill=colour, width=3)
            missing = [side for side, v in fit["per_side"].items() if not v["inliers"]]
            legend.append((colour, label + f": r {fit['radius']:.0f} px, {fit['inliers']}/{fit['points']} inliers"
                           + (f"; NO EDGE POINTS: {', '.join(missing)}" if missing else "")))
        else:
            legend.append((colour, label + ": NOT FOUND"))
    if fixed_top.get("found"):
        circle(draw, {"x": fixed_top["x"], "y": fixed_top["y"], "radius": fixed_top["radius_fixed_px"]}, FIXED, f, width=1)
        draw.ellipse([(fixed_top["x"] - 40) / f, (fixed_top["y"] - 40) / f, (fixed_top["x"] + 40) / f,
                      (fixed_top["y"] + 40) / f], outline=FIXED, width=2)
        legend.append((FIXED, f"wall top, radius fixed by the calipered 80.15 mm (r {fixed_top['radius_fixed_px']:.0f} px): "
                              f"THE dish centre, {fixed_top['points']} pts, arc {fixed_top['arc_coverage']['degrees_covered']} deg"))
    if registration:
        circle(draw, registration, REG, f, width=2)
        legend.append((REG, registration["label"]))
    if cross:
        x, y = cross
        draw.ellipse([(x - 60) / f, (y - 60) / f, (x + 60) / f, (y + 60) / f], outline=CROSS, width=3)
        legend.append((CROSS, "observed projected crosshair centre"))
        if fixed_top.get("found"):
            ex, ey = x + 10 * (fixed_top["x"] - x), y + 10 * (fixed_top["y"] - y)
            draw.line([(x / f, y / f), (ex / f, ey / f)], fill=CROSS, width=3)
            if placement and placement.get("distance_mm") is not None:
                dx, dy = placement["offset_mm"]
                label = (f"dish centre - cross: {placement['distance_mm']:.2f} +/- {placement.get('distance_sigma_mm', float('nan')):.2f}"
                         f" mm ({dx:+.2f}, {dy:+.2f}) [arrow drawn x10]")
                tx = min(max(4.0, ex / f + 8), small.width - 9.5 * len(label) - 4)
                ty = min(max(4.0, ey / f + 4), small.height - 26)
                draw.text((tx, ty), label, fill=CROSS, font=font)
    strip = 16 + 26 * (len(legend) + 1)
    figure = Image.new("RGB", (small.width, small.height + strip), (0, 0, 0))
    figure.paste(small, (0, 0))
    fd = ImageDraw.Draw(figure)
    y0 = small.height + 8
    fd.text((12, y0), f"Cam{serial}: dish rim, {gray.shape[1]} px frame shown at 1/{factor}", fill=(255, 255, 255), font=font)
    y0 += 26
    for colour, text in legend:
        fd.line([(12, y0 + 11), (44, y0 + 11)], fill=colour, width=4)
        fd.text((52, y0), text, fill=colour, font=font)
        y0 += 26
    figure.save(out / f"Cam{serial}_dish_rim.png")
    written.append(f"Cam{serial}_dish_rim.png")

    ref = top if top.get("found") else floor
    if not ref.get("found"):
        return written
    half = 160
    tiles = []
    for name, angle in (("top", -90.0), ("right", 0.0), ("bottom", 90.0), ("left", 180.0)):
        a = math.radians(angle)
        # Centre the crop between the two edges on this side.
        rmid = ref["radius"] - (0.5 * (top["radius"] - floor["radius"]) if (top.get("found") and floor.get("found")) else 10)
        px0, py0 = ref["x"] + rmid * math.cos(a), ref["y"] + rmid * math.sin(a)
        x0, y0c = int(round(px0)) - half, int(round(py0)) - half
        crop = np.zeros((2 * half, 2 * half), np.uint8)
        sx0, sy0 = max(0, x0), max(0, y0c)
        sx1, sy1 = min(gray.shape[1], x0 + 2 * half), min(gray.shape[0], y0c + 2 * half)
        crop[sy0 - y0c:sy1 - y0c, sx0 - x0:sx1 - x0] = gray[sy0:sy1, sx0:sx1]
        lo, hi = np.percentile(crop, 1), np.percentile(crop, 99.5)
        stretched = np.clip((crop.astype(np.float64) - lo) / max(1.0, hi - lo) * 255, 0, 255).astype(np.uint8)
        tile = Image.fromarray(stretched).convert("RGB").resize((4 * half, 4 * half), Image.NEAREST)
        td = ImageDraw.Draw(tile)
        notes = []
        fixed_circle = ({"x": fixed_top["x"], "y": fixed_top["y"], "radius": fixed_top["radius_fixed_px"], "found": True,
                         "_points": fixed_top["_points"]} if fixed_top.get("found") else None)
        for fit, colour, label in ((registration, REG, None), (floor, FLOOR, "floor"), (top, TOP, "free wall top"),
                                   (fixed_circle, FIXED, "fixed-radius wall top")):
            if not fit or (label and not fit.get("found")):
                continue
            pts = [(((fit["x"] + fit["radius"] * math.cos(t)) - x0) * 2, ((fit["y"] + fit["radius"] * math.sin(t)) - y0c) * 2)
                   for t in np.linspace(a - 0.12, a + 0.12, 400)]
            td.line(pts, fill=colour, width=2)
            if label:
                px, py, inl = fit["_points"]
                shown = 0
                for qx, qy, ok in zip(px, py, inl):
                    if ok and 0 <= qx - x0 < 2 * half and 0 <= qy - y0c < 2 * half:
                        cx2, cy2 = (qx - x0) * 2, (qy - y0c) * 2
                        td.ellipse([cx2 - 4, cy2 - 4, cx2 + 4, cy2 + 4], outline=colour, width=2)
                        shown += 1
                if shown == 0:
                    notes.append(f"no {label} edge points here")
        if x0 < 0 or y0c < 0 or x0 + 2 * half > gray.shape[1] or y0c + 2 * half > gray.shape[0]:
            notes.append("black = outside the frame")
        td.rectangle([0, 0, 4 * half, 26 + 24 * len(notes)], fill=(0, 0, 0))
        td.text((6, 3), f"{name}: {2 * half} x {2 * half} native px (shown x2)", fill=(255, 255, 255), font=font)
        for k, note in enumerate(notes):
            td.text((6, 27 + 24 * k), note.upper() if note.startswith("no ") else note, fill=(255, 80, 80), font=font)
        tiles.append(tile)
    sheet = Image.new("RGB", (8 * half, 8 * half + 60), (0, 0, 0))
    for k, tile in enumerate(tiles):
        sheet.paste(tile, ((k % 2) * 4 * half, (k // 2) * 4 * half))
    sd = ImageDraw.Draw(sheet)
    key = [(FIXED, "fixed-radius wall top (THE placement)"), (TOP, "free wall top (cross-check)")]
    if floor.get("found"):
        key.append((FLOOR, "floor edge (diagnostic)"))
    if registration:
        key.append((REG, registration["label"]))
    yk = 8 * half + 8
    x = 8
    for colour, text in key:
        sd.line([(x, yk + 14), (x + 30, yk + 14)], fill=colour, width=4)
        sd.text((x + 36, yk + 4), text, fill=colour, font=font)
        x += 60 + int(9.5 * len(text))
    sheet.save(out / f"Cam{serial}_dish_rim_zoom.png")
    written.append(f"Cam{serial}_dish_rim_zoom.png")
    return written


def frames_by_recipe(artifact: Path) -> dict[str, list[dict[str, Any]]]:
    """Images of one camera grouped by the Citrus scene recipe they were captured under."""
    image_set = json.loads((artifact / "image_set.json").read_text(encoding="utf-8"))
    out: dict[str, list[dict[str, Any]]] = {}
    for image in image_set.get("images", []):
        recipe = (image.get("citrus_calibration_scene_pre_capture") or {}).get("recipe_id")
        if not recipe:
            for name, roles in ROLE_FALLBACK.items():
                if image.get("role") in roles:
                    recipe = name
        if recipe:
            out.setdefault(str(recipe), []).append(image)
    return out


def experimental_area_from_status(image: dict[str, Any], arena_id: str) -> dict[str, Any] | None:
    status = image.get("citrus_calibration_scene_pre_capture") or {}
    for target in status.get("resolved_targets") or []:
        if target.get("arena_id") == arena_id:
            area = target.get("experimental_area") or {}
            centre = area.get("center_final_display_canvas_px") or {}
            if "x" in centre and area.get("radius_px"):
                return {"centre_canvas_px": [float(centre["x"]), float(centre["y"])], "radius_canvas_px": float(area["radius_px"]),
                        "radius_mm": area.get("radius_mm"), "source": "citrus_scene_status.resolved_targets"}
    return None


# --------------------------------------------------------------------------


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def parse_utc(text: str) -> datetime | None:
    try:
        return datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    except (TypeError, ValueError):
        return None


def sha256_token(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def local_scale(inverse: np.ndarray, centre: tuple[float, float], radius: float) -> float:
    """Camera px per canvas px around a canvas point (mean of four directions)."""
    xs = np.array([centre[0] + radius, centre[0] - radius, centre[0], centre[0]])
    ys = np.array([centre[1], centre[1], centre[1] + radius, centre[1] - radius])
    cx, cy = bm.map_points(inverse, xs, ys)
    c0x, c0y = bm.map_points(inverse, np.array([centre[0]]), np.array([centre[1]]))
    return float(np.mean(np.hypot(cx - c0x[0], cy - c0y[0])) / radius)


def arena_geometry(scene_status: dict[str, Any], arena_id: str, layout: bm.ArenaLayout | None) -> dict[str, Any]:
    for arena in (scene_status.get("arenas") or []):
        if arena.get("arena_id") == arena_id:
            origin = arena.get("origin_canvas_px") or [0, 0]
            centre = arena.get("experimental_area_center_arena_px") or [0, 0]
            return {"source": "citrus_scene_status", "pixels_per_mm_projector": float(arena["pixels_per_mm_projector"]),
                    "experimental_area_center_canvas_px": [origin[0] + centre[0], origin[1] + centre[1]],
                    "experimental_area_shape": arena.get("experimental_area_shape")}
    if layout is not None:
        return {"source": "citrus_canvas_config", "pixels_per_mm_projector": layout.pixels_per_mm_projector,
                "experimental_area_center_canvas_px": list(layout.center), "experimental_area_shape": None}
    return {}


def downsampled_rgb(gray: np.ndarray, factor: int) -> Any:
    from PIL import Image

    small = gray[::factor, ::factor]
    lo, hi = np.percentile(small, 1), np.percentile(small, 99.5)
    stretched = np.clip((small.astype(np.float64) - lo) / max(1.0, hi - lo) * 255.0, 0, 255).astype(np.uint8)
    return Image.fromarray(stretched).convert("RGB")


def emit_grid_scenes(path: Path, arenas: list[str]) -> None:
    """Two static_dots scenes per arena: a centre + inner ring + outer ring grid of 3 mm dots.

    15 dots per arena (60 in total, Citrus's limit is 64 per scene), all inside a 35 mm radius
    of the experimental-area centre (the 80 mm dish), at least 9 mm apart. Scene 2 is scene 1
    rotated by 22.5 degrees, so the pair samples 29 distinct points per arena."""
    scenes = []
    for scene_index, rotation in enumerate((0.0, 22.5), start=1):
        dots = []
        for arena_id in arenas:
            points = [(0.0, 0.0)]
            points += [(18.0 * math.cos(math.radians(rotation + 45 * k)), 18.0 * math.sin(math.radians(rotation + 45 * k)))
                       for k in range(8)]
            points += [(32.0 * math.cos(math.radians(rotation + 30 + 60 * k)), 32.0 * math.sin(math.radians(rotation + 30 + 60 * k)))
                       for k in range(6)]
            for index, (x, y) in enumerate(points):
                if scene_index == 2 and index == 0:
                    continue  # the centre is already in scene 1
                dots.append({"dot_id": f"{arena_id}_g{scene_index}_{index:02d}", "arena_id": arena_id,
                             "offset_mm": [round(x, 3), round(y, 3)], "diameter_mm": 3.0, "color": {"gray_u8": 255}})
        scenes.append({"label": f"registration_grid_{scene_index}", "scene_options": {"background": {"gray_u8": 0}, "dots": dots}})
    path.write_text(json.dumps({"scenes": scenes}, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {path}: {len(scenes)} scenes, {sum(len(s['scene_options']['dots']) for s in scenes)} dots")


# --------------------------------------------------------------------------
# Compare mode: before (base geometry) and after (daily registration applied)


def load_applied_translation(path: Path | None) -> dict[str, Any]:
    """Per arena, the integer canvas translation a Citrus daily registration applies."""
    if path is None:
        return {"source": None, "note": "no --daily-registration given: the applied translation is not read, "
                                        "only inferred from the scene status (below)"}
    doc = json.loads(path.read_text(encoding="utf-8"))
    out: dict[str, Any] = {"source": str(path), "sha256": sha256_token(path), "schema_id": doc.get("schema_id")}
    registration = doc
    if doc.get("schema_id") == "citrus.calibration.daily_registration_runtime_selection":
        out["mode"] = doc.get("mode")
        out["selected_at_utc"] = doc.get("selected_at_utc")
        ref = doc.get("registration")
        if doc.get("mode") != "selected_daily_registration" or not ref:
            out["arenas"] = {}
            out["note"] = f"runtime selection mode {doc.get('mode')!r}: no daily translation selected"
            return out
        reg_path = Path(str((ref or {}).get("path") or (ref or {}).get("registration_path") or ""))
        if not reg_path.is_file():
            out["arenas"] = {}
            out["note"] = f"the selection names a registration that was not found: {ref}"
            return out
        registration = json.loads(reg_path.read_text(encoding="utf-8"))
        out["registration_path"] = str(reg_path)
        out["registration_sha256"] = sha256_token(reg_path)
    out["registration_id"] = registration.get("registration_id")
    out["valid_until_utc"] = registration.get("valid_until_utc")
    arenas = {}
    for target in registration.get("targets") or []:
        final = target.get("final_applied_translation_canvas_px") or {}
        auto = target.get("automatic_translation_canvas_px") or {}
        tx = final.get("x", final.get("applied_x", auto.get("applied_x")))
        ty = final.get("y", final.get("applied_y", auto.get("applied_y")))
        arenas[target.get("arena_id")] = {
            "camera_id": target.get("camera_id"), "applied_canvas_px": [tx, ty],
            "requested_canvas_px": [auto.get("requested_x"), auto.get("requested_y")],
            "rounding_residual_canvas_px": [auto.get("rounding_residual_x"), auto.get("rounding_residual_y")],
            "alignment_basis": target.get("alignment_basis")}
    out["arenas"] = arenas
    return out


def _session_view(report: dict[str, Any], serial: str) -> dict[str, Any]:
    c = report["cameras"].get(serial) or {}
    x = c.get("crosshair") or {}
    t = (c.get("dish") or {}).get("today") or {}
    p = c.get("projection") or {}
    return {"cross": x.get("centre_camera_px") if x.get("found") else None,
            "experimental_area_centre_canvas_px": (x.get("experimental_area") or {}).get("centre_canvas_px"),
            "crosshair_path": x.get("path"), "arena_id": c.get("arena_id"),
            "floor": None,  # the floor edge is a diagnostic only (Jeremy 2026-11-15)
            "top": t.get("wall_top") if (t.get("wall_top") or {}).get("found") else None,
            "placement": x.get("dish_placement") or {}, "homography_at_centre": x.get("homography_at_centre") or {},
            "top_fixed": t.get("wall_top_fixed_radius") if (t.get("wall_top_fixed_radius") or {}).get("found") else None,
            "dots": {"found": p.get("found"), "drawn": p.get("drawn"), "median_mm": p.get("residual_mm_median"),
                     "max_mm": p.get("residual_mm_max")},
            "parallax": t.get("parallax") or {}}


def _centre_sigma_px(fit: dict[str, Any] | None) -> float | None:
    """1-sigma centre uncertainty of a circle fit: the bootstrap value (it grows on a partial arc)."""
    if not fit:
        return None
    return (fit.get("uncertainty") or {}).get("centre_sigma_px")


def _r(value: Any) -> Any:
    return round(value, 2) if isinstance(value, float) else value


def compare_sessions(args: argparse.Namespace) -> int:
    now = utc_now()
    out = args.out or (orange_data_root() / "registration_accuracy" / (now.strftime("%Y%m%dT%H%M%SZ") + "_compare"))
    out.mkdir(parents=True, exist_ok=True)
    common = ["--sessions-root", str(args.sessions_root), "--citrus-config", str(args.citrus_config),
              "--fish-plane-scale", str(args.fish_plane_scale), "--active-pointer-dir", str(args.active_pointer_dir),
              "--max-residual-mm", str(args.max_residual_mm), "--max-dish-offset-mm", str(args.max_dish_offset_mm),
              "--search-mm", str(args.search_mm)]
    for path in args.rim_observation:
        common += ["--rim-observation", str(path)]
    reports = {}
    for stage, session in (("before", args.before), ("after", args.after)):
        print(f"== {stage}: {session}")
        main(["--session", session, "--out", str(out / stage)] + common)
        reports[stage] = json.loads((out / stage / "registration_accuracy.json").read_text(encoding="utf-8"))
    applied = load_applied_translation(args.daily_registration)
    calibration_dir = args.citrus_config.parent / "calibration_artifacts"
    canvas_sha = dc.sha256_file(args.citrus_config).split(":", 1)[1]
    layout = bm.load_layout(args.citrus_config)
    fish_plane = json.loads(args.fish_plane_scale.read_text(encoding="utf-8")) if args.fish_plane_scale.is_file() else {}
    cameras: dict[str, Any] = {}
    for serial in sorted(set(reports["before"]["cameras"]) | set(reports["after"]["cameras"])):
        before, after = _session_view(reports["before"], serial), _session_view(reports["after"], serial)
        arena_id = before["arena_id"] or after["arena_id"]
        model = scale_model(fish_plane, serial) if fish_plane else None
        s_low = scale_at(model, radius_band(model)["z_low_mm"]) if model else None
        ppm = layout[arena_id].pixels_per_mm_projector if arena_id in layout else None
        row: dict[str, Any] = {"arena_id": arena_id}
        # (a) before
        row["before"] = {"basis": before["placement"].get("basis"),
                         "dish_vs_cross_mm": before["placement"].get("offset_mm"),
                         "distance_mm": before["placement"].get("distance_mm"),
                         "distance_sigma_mm": before["placement"].get("distance_sigma_mm"),
                         "arc_coverage_deg": before["placement"].get("arc_coverage_deg"),
                         "cross_checks_mm": {k: (v or {}).get("distance_mm") for k, v in
                                             (before["placement"].get("cross_checks") or {}).items()},
                         "homography_at_centre_mm": before["homography_at_centre"].get("residual_mm_norm"),
                         "dots": before["dots"]}
        # (b) applied translation: the registration file, and what the scene status shows
        ec_b, ec_a = before["experimental_area_centre_canvas_px"], after["experimental_area_centre_canvas_px"]
        if ec_b and ec_a:
            moved = [ec_a[0] - ec_b[0], ec_a[1] - ec_b[1]]
            status_note = ("the after-session's scene status shows the experimental-area centre moved by "
                           f"({moved[0]:+.1f}, {moved[1]:+.1f}) canvas px" if any(abs(v) > 1e-6 for v in moved) else
                           "the after-session's scene status shows the SAME experimental-area centre as before: no "
                           "daily translation was active in it")
        else:
            moved = None
            status_note = ("the scene status does not show the experimental-area centre for one of the sessions, so "
                           "it does not say whether a daily translation was active")
        reg = (applied.get("arenas") or {}).get(arena_id)
        translation: dict[str, Any] = {"from_scene_status_canvas_px": moved, "scene_status_note": status_note,
                                       "from_daily_registration": reg, "daily_registration_note": applied.get("note")}
        t_canvas = (reg or {}).get("applied_canvas_px") if reg else moved
        if t_canvas and None not in t_canvas and ppm:
            translation["applied_mm"] = [t_canvas[0] / ppm, t_canvas[1] / ppm]
            translation["applied_basis"] = "daily registration file" if reg else "scene status centre change"
            if reg and moved is not None and (abs(moved[0] - t_canvas[0]) > 0.5 or abs(moved[1] - t_canvas[1]) > 0.5):
                translation["disagreement"] = "the registration's translation and the scene status's centre change differ"
            try:
                homography = bm.load_homography(calibration_dir, arena_id, serial, canvas_sha)
                inverse = np.linalg.inv(homography.matrix)
                ec = ec_b or [layout[arena_id].center[0], layout[arena_id].center[1]]
                xs, ys = bm.map_points(inverse, np.array([ec[0], ec[0] + t_canvas[0]]), np.array([ec[1], ec[1] + t_canvas[1]]))
                translation["applied_camera_px"] = [float(xs[1] - xs[0]), float(ys[1] - ys[0])]
            except bm.InputError as error:
                translation["applied_camera_px_error"] = str(error)
        row["applied_translation"] = translation
        # predicted after = before offset minus the cross's move (camera px)
        bp, ap = before["placement"], after["placement"]
        s_top_pred = scale_at(model, radius_band(model)["z_high_mm"]) if model else None
        if bp.get("offset_camera_px") and translation.get("applied_camera_px") and s_top_pred:
            pred = [bp["offset_camera_px"][0] - translation["applied_camera_px"][0],
                    bp["offset_camera_px"][1] - translation["applied_camera_px"][1]]
            row["predicted_after_mm"] = [pred[0] / s_top_pred, pred[1] / s_top_pred]
            row["predicted_after_distance_mm"] = math.hypot(*row["predicted_after_mm"])
        # (c) after and (d) residual
        row["after"] = {"basis": ap.get("basis"), "dish_vs_cross_mm": ap.get("offset_mm"), "distance_mm": ap.get("distance_mm"),
                        "distance_sigma_mm": ap.get("distance_sigma_mm"), "arc_coverage_deg": ap.get("arc_coverage_deg"),
                        "film_corrected_distance_mm": ap.get("film_corrected_distance_mm"),
                        "cross_checks_mm": {k: (v or {}).get("distance_mm") for k, v in (ap.get("cross_checks") or {}).items()},
                        "homography_at_centre_mm": after["homography_at_centre"].get("residual_mm_norm"),
                        "dots": after["dots"]}
        sig = [v for v in (_centre_sigma_px(after["top_fixed"]),) if v is not None]
        s_top = scale_at(model, radius_band(model)["z_high_mm"]) if model else None
        if s_top and sig:
            quant_mm = (math.sqrt(2) * 0.5 / ppm) if ppm else 0.0
            noise_mm = math.hypot(sig[0] / s_top, quant_mm)
            row["residual_after"] = {
                "distance_mm": ap.get("distance_mm"),
                "noise_floor_mm": noise_mm,
                "noise_floor_parts": {"fixed_wall_top_centre_1sigma_mm_bootstrap": sig[0] / s_top,
                                      "integer_canvas_translation_rounding_mm": quant_mm},
                "cross_check_spread_mm": (max(abs(ap["distance_mm"] - v) for v in
                                              [(c or {}).get("distance_mm") for c in (ap.get("cross_checks") or {}).values()]
                                              if v is not None)
                                          if ap.get("distance_mm") is not None and any(
                                              (c or {}).get("distance_mm") is not None for c in
                                              (ap.get("cross_checks") or {}).values()) else None),
                "limit_mm": args.max_after_mm,
                "pass": bool(ap.get("distance_mm") is not None and ap["distance_mm"] <= args.max_after_mm),
                "note": "noise floor: the fixed-radius wall-top fit's centre uncertainty (bootstrap) and the 0.5 canvas "
                        "px rounding of an integer translation; the spread to the cross-checks (free fits, floor "
                        "edge) is a separate, systematic term"}
        cameras[serial] = row
        draw_compare_figure(out, serial, reports, before, after, s_low, row)
    report = {"schema_id": SCHEMA_ID + "_compare", "schema_version": 1,
              "created_utc": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
              "before": {"session": args.before, "report": str(out / "before" / "registration_accuracy.json")},
              "after": {"session": args.after, "report": str(out / "after" / "registration_accuracy.json")},
              "daily_registration": {k: v for k, v in applied.items() if k != "arenas"},
              "limits": {"max_after_mm": args.max_after_mm,
                         "why": "0.5 mm = about 2 canvas px: an integer-pixel translation can leave up to 0.12 mm, and "
                                "the fits' centre noise is well under 0.1 mm"},
              "cameras": cameras}
    (out / "registration_compare.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {out / 'registration_compare.json'}")
    for serial, row in cameras.items():
        b, a, t = row["before"], row["after"], row["applied_translation"]
        r = row.get("residual_after") or {}
        print(f"Cam{serial}: before {b.get('distance_mm', float('nan')):.2f} +/- {b.get('distance_sigma_mm') or float('nan'):.2f} mm | applied "
              f"{t.get('applied_mm') and [round(v, 2) for v in t['applied_mm']]} mm ({t.get('applied_basis', 'none')}) | after "
              f"{a.get('distance_mm', float('nan')):.2f} +/- {a.get('distance_sigma_mm') or float('nan'):.2f} mm (noise floor {r.get('noise_floor_mm', float('nan')):.2f}, "
              f"limit {args.max_after_mm}) {'PASS' if r.get('pass') else 'FAIL'} | dots max {_r(b['dots']['max_mm'])} -> "
              f"{_r(a['dots']['max_mm'])} mm | homography at centre {_r(b['homography_at_centre_mm'])} -> "
              f"{_r(a['homography_at_centre_mm'])} mm")
        print("    " + t["scene_status_note"] + (f"; {t['daily_registration_note']}" if t.get("daily_registration_note") else ""))
    return 0


def draw_compare_figure(out: Path, serial: str, reports: dict[str, Any], before: dict[str, Any],
                        after: dict[str, Any], s_low: float | None, row: dict[str, Any]) -> None:
    """Cam<serial>_compare.png: before | after, each the crosshair frame at 1/8 with the cross,
    the floor-edge and wall-top centres, and an inset at native resolution (1 image px = 1 camera
    px) around the cross with the offset arrow to scale and a 1 mm scale bar."""
    from PIL import Image, ImageDraw

    font = _font(16)
    panels = []
    for stage, view in (("BEFORE", before), ("AFTER", after)):
        rep = reports[stage.lower()]
        session_dir = Path(rep["inputs"]["session_dir"])
        arena = view["arena_id"]
        path = session_dir / "artifacts" / f"Cam{serial}_{arena}" / str(view["crosshair_path"] or "")
        if not path.is_file():
            panel = Image.new("RGB", (564, 640), (0, 0, 0))
            ImageDraw.Draw(panel).text((10, 10), f"{stage}: no crosshair frame", fill=(255, 80, 80), font=font)
            panels.append(panel)
            continue
        gray = bm.load_u8(path)
        factor = 8
        img = downsampled_rgb(gray, factor)
        d = ImageDraw.Draw(img)
        cross, floor, top = view["cross"], view["floor"], view["top"]
        if cross:
            d.ellipse([cross[0] / factor - 6, cross[1] / factor - 6, cross[0] / factor + 6, cross[1] / factor + 6],
                      outline=(0, 230, 255), width=2)
        # inset around the cross: 480 x 480 native camera px, shown at 0.75 (arrows and the scale bar
        # use the same factor, so everything in it is to scale)
        half, shown = 240, 0.75
        inset = None
        if cross:
            cx, cy = int(round(cross[0])), int(round(cross[1]))
            crop = np.zeros((2 * half, 2 * half), np.uint8)
            x0, y0 = cx - half, cy - half
            sx0, sy0, sx1, sy1 = max(0, x0), max(0, y0), min(gray.shape[1], x0 + 2 * half), min(gray.shape[0], y0 + 2 * half)
            crop[sy0 - y0:sy1 - y0, sx0 - x0:sx1 - x0] = gray[sy0:sy1, sx0:sx1]
            size = int(2 * half * shown)
            inset = Image.fromarray(crop).convert("RGB").resize((size, size), Image.BILINEAR)
            idr = ImageDraw.Draw(inset)
            c0 = half * shown
            idr.ellipse([c0 - 6, c0 - 6, c0 + 6, c0 + 6], outline=(0, 230, 255), width=2)
            for fit, colour in ((floor, (0, 255, 0)), (top, (255, 0, 255)), (view.get("top_fixed"), (255, 255, 255))):
                if fit:
                    fx, fy = (fit["x"] - x0) * shown, (fit["y"] - y0) * shown
                    idr.line([(c0, c0), (fx, fy)], fill=colour, width=2)
                    idr.line([(fx - 7, fy), (fx + 7, fy)], fill=colour, width=2)
                    idr.line([(fx, fy - 7), (fx, fy + 7)], fill=colour, width=2)
            if s_low:
                idr.line([(10, size - 12), (10 + s_low * shown, size - 12)], fill=(255, 255, 255), width=3)
                idr.text((12, size - 34), "1 mm", fill=(255, 255, 255), font=font)
            idr.rectangle([0, 0, size - 1, size - 1], outline=(255, 255, 255))
        dist = view["placement"].get("distance_mm")
        lines = [f"{stage}: {rep['inputs']['session_dir'].split('/')[-1]}",
                 (f"dish (fixed-radius wall top) - cross: {dist:.2f} +/- {view['placement'].get('distance_sigma_mm') or float('nan'):.2f} mm"
                  f" (arc {view['placement'].get('arc_coverage_deg')} deg)") if dist is not None
                 else "dish centre: not measured",
                 "inset below: 480 x 480 camera px around the cross, shown at 0.75x;",
                 "arrows and the 1 mm bar are to that same scale (no exaggeration)",
                 "cyan: cross; white: fixed-radius wall top (the measurement);",
                 "magenta: free wall top (cross-check)"]
        inset_h = inset.height + 10 if inset is not None else 0
        figure = Image.new("RGB", (img.width, img.height + inset_h + 24 * len(lines) + 10), (0, 0, 0))
        figure.paste(img, (0, 0))
        if inset is not None:
            figure.paste(inset, ((img.width - inset.width) // 2, img.height + 5))
        fd = ImageDraw.Draw(figure)
        for k, text in enumerate(lines):
            fd.text((8, img.height + inset_h + 6 + 24 * k), text, fill=(255, 255, 255), font=font)
        panels.append(figure)
    width = sum(p.width for p in panels) + 10
    height = max(p.height for p in panels) + 34
    sheet = Image.new("RGB", (width, height), (0, 0, 0))
    x = 0
    for p in panels:
        sheet.paste(p, (x, 34))
        x += p.width + 10
    t = row["applied_translation"]
    note = t.get("daily_registration_note") or ""
    ImageDraw.Draw(sheet).text((8, 8), f"Cam{serial}: applied translation "
                               f"{t.get('applied_mm') and [round(v, 2) for v in t['applied_mm']]} mm "
                               f"({t.get('applied_basis', 'none read')}). {note}", fill=(255, 200, 0), font=font)
    sheet.save(out / f"Cam{serial}_compare.png")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--session")
    parser.add_argument("--sessions-root", type=Path, default=orange_data_root() / "calibrations" / "sessions")
    parser.add_argument("--citrus-config", type=Path,
                        default=Path("/home/jeremy/citrus/targets/rigs/omnifin0/shadow/shadow.json"))
    parser.add_argument("--fish-plane-scale", type=Path, default=orange_data_root() / "calibrations" / "commissioning" /
                        "omnifin0_20261113T230405Z_wet_commissioning_holder_check_fish_plane_scale_" / "steps" /
                        "08_fish_plane_scale" / "fish_plane_scale.json")
    parser.add_argument("--active-pointer-dir", type=Path,
                        default=orange_data_root() / "calibrations" / "active" / "physical_dish_registration")
    parser.add_argument("--rim-observation", type=Path, action="append", default=[],
                        help="a dish_top_rim_observation to check instead of the active pointer (repeatable)")
    parser.add_argument("--max-residual-mm", type=float, default=1.0)
    parser.add_argument("--max-dish-offset-mm", type=float, default=2.0)
    parser.add_argument("--search-mm", type=float, default=4.0, help="search radius around each prediction")
    parser.add_argument("--overlay-factor", type=int, default=8)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--emit-grid-scenes", type=Path, help="write the rig capture's dot-grid scenes and exit")
    parser.add_argument("--arenas", default="arena_1,arena_2,arena_3,arena_4")
    parser.add_argument("--floor-diagnostic", action="store_true",
                        help="also fit the lit floor's edge (free and fixed radius) and the floor/top parallax check; "
                             "a diagnostic only, never in the placement or pass/fail (Jeremy 2026-11-15: the wall top is "
                             "the reference)")
    parser.add_argument("--before", help="compare mode: the session with base geometry")
    parser.add_argument("--after", help="compare mode: the session after the daily registration was applied")
    parser.add_argument("--daily-registration", type=Path,
                        help="Citrus daily_registration_runtime_selection.json or a registration.json (the applied "
                             "translation); without it the translation is only inferred from the scene status")
    parser.add_argument("--max-after-mm", type=float, default=0.5,
                        help="compare mode: limit on the dish-vs-centre residual after correction")
    args = parser.parse_args(argv)

    if args.before or args.after:
        if not (args.before and args.after):
            parser.error("--before and --after go together")
        return compare_sessions(args)
    if args.emit_grid_scenes:
        emit_grid_scenes(args.emit_grid_scenes, [a for a in args.arenas.split(",") if a])
        return 0
    if not args.session:
        parser.error("--session is required (or --emit-grid-scenes)")

    now = utc_now()
    out = args.out or (orange_data_root() / "registration_accuracy" / now.strftime("%Y%m%dT%H%M%SZ"))
    out.mkdir(parents=True, exist_ok=True)
    session = bm.resolve_session_dir(args.session, args.sessions_root)
    canvas_sha = dc.sha256_file(args.citrus_config).split(":", 1)[1]
    calibration_dir = args.citrus_config.parent / "calibration_artifacts"
    layout = bm.load_layout(args.citrus_config)
    fish_plane = json.loads(args.fish_plane_scale.read_text(encoding="utf-8")) if args.fish_plane_scale.is_file() else {}
    observations_by_serial: dict[str, Path] = {}
    for path in args.rim_observation:
        doc = json.loads(path.read_text(encoding="utf-8"))
        observations_by_serial[str((doc.get("camera") or {}).get("serial"))] = path

    warnings: list[str] = []
    cameras: dict[str, Any] = {}
    all_residuals_mm: list[float] = []

    for serial, artifact in dc.camera_dirs(session).items():
        arena_id = artifact.name.split("_", 1)[1]
        entry: dict[str, Any] = {"arena_id": arena_id}
        cameras[serial] = entry
        try:
            homography = bm.load_homography(calibration_dir, arena_id, serial, canvas_sha)
        except bm.InputError as error:
            entry["error"] = str(error)
            warnings.append(f"Cam{serial}: {error}")
            continue
        entry["homography"] = {k: homography.provenance.get(k) for k in (
            "pointer_path", "pointer_sha256", "candidate_id", "accepted_at_utc", "canvas_checksum_matches_citrus_config")}
        if not homography.provenance.get("canvas_checksum_matches_citrus_config"):
            warnings.append(f"Cam{serial}: the active homography was accepted on a different canvas checksum")
        matrix = homography.matrix
        inverse = np.linalg.inv(matrix)
        frames = frames_by_recipe(artifact)
        black_images = frames.get("black_reference", [])
        dot_images = frames.get("static_dots", [])
        entry["frames"] = {recipe: [im.get("path") for im in images] for recipe, images in frames.items()}
        black = bm.load_u8(artifact / black_images[-1]["path"]).astype(np.float64) if black_images else None
        if black is None:
            warnings.append(f"Cam{serial}: no black_reference frame; centroids use the raw image")
        scene_status_any: dict[str, Any] = {}

        # ---- A. projection
        dots_out: list[dict[str, Any]] = []
        overlay = None
        for image in dot_images:
            pixels = bm.load_u8(artifact / image["path"])
            signal = pixels.astype(np.float64) - (black if black is not None else 0.0)
            np.clip(signal, 0, None, out=signal)
            dots, info = dc.drawn_dots(image)
            status = (image.get("citrus_calibration_scene_pre_capture") or {}).get("static_dots") or {}
            scene_status_any = scene_status_any or status
            if not info["presented"] or not info["same_scene_pre_post"]:
                warnings.append(f"Cam{serial} {dc.scene_label(image)}: scene not presented or changed during capture")
            geometry = arena_geometry(status, arena_id, layout.get(arena_id))
            ppm = geometry.get("pixels_per_mm_projector") or layout[arena_id].pixels_per_mm_projector
            if overlay is None:
                overlay = downsampled_rgb(pixels, args.overlay_factor)
            for dot in dots:
                if dot.get("arena_id") != arena_id:
                    continue
                centre = (float(dot["center_canvas_px"][0]), float(dot["center_canvas_px"][1]))
                radius = float(dot["radius_px"])
                px, py = bm.map_points(inverse, np.array([centre[0]]), np.array([centre[1]]))
                cam_per_canvas = local_scale(inverse, centre, radius)
                measured = measure_dot(signal, (float(px[0]), float(py[0])), radius * cam_per_canvas,
                                       args.search_mm * ppm * cam_per_canvas)
                row: dict[str, Any] = {"scene": dc.scene_label(image), "dot_id": dot.get("dot_id"),
                                       "commanded_canvas_px": list(centre), "radius_canvas_px": radius,
                                       "predicted_camera_px": [float(px[0]), float(py[0])],
                                       "camera_px_per_canvas_px": round(cam_per_canvas, 4),
                                       "pixels_per_mm_projector": ppm, **measured}
                if measured.get("found"):
                    mx, my = measured["camera_px"]
                    cx, cy = bm.map_points(matrix, np.array([mx]), np.array([my]))
                    d_canvas = (float(cx[0]) - centre[0], float(cy[0]) - centre[1])
                    row["measured_canvas_px"] = [float(cx[0]), float(cy[0])]
                    row["residual_camera_px"] = [mx - float(px[0]), my - float(py[0])]
                    row["residual_canvas_px"] = list(d_canvas)
                    row["residual_mm"] = [d_canvas[0] / ppm, d_canvas[1] / ppm]
                    row["residual_mm_norm"] = math.hypot(*row["residual_mm"])
                    all_residuals_mm.append(row["residual_mm_norm"])
                    if measured.get("blob_area_px", 0) < 0.5 * measured.get("expected_area_px", 0):
                        warnings.append(f"Cam{serial} {dot.get('dot_id')}: blob much smaller than the drawn disk")
                else:
                    warnings.append(f"Cam{serial} {dot.get('dot_id')}: not found ({measured.get('reason')})")
                dots_out.append(row)
        found = [d for d in dots_out if d.get("found")]
        norms = [d["residual_mm_norm"] for d in found]
        projection: dict[str, Any] = {"dots": dots_out, "found": len(found), "drawn": len(dots_out)}
        if found:
            ppm = found[0]["pixels_per_mm_projector"]
            # About the experimental-area centre, so the shift is the offset at the dish centre.
            ec = np.array((arena_geometry(scene_status_any, arena_id, layout.get(arena_id)) or {}).get(
                "experimental_area_center_canvas_px") or [0.0, 0.0], dtype=np.float64)
            src = (np.array([d["commanded_canvas_px"] for d in found]) - ec) / ppm
            dst = (np.array([d["measured_canvas_px"] for d in found]) - ec) / ppm
            similarity = fit_similarity(src, dst)
            if similarity.get("fitted"):
                similarity["translation_mm"] = similarity.pop("translation")
                similarity["rms_after_fit_mm"] = similarity.pop("rms_after_fit")
                similarity["note"] = "commanded -> measured, projection-plane mm about the experimental-area centre" + (
                    "; exact with 2 points (no residual left to judge)" if similarity["n"] == 2 else "")
            affine = fit_affine(src, dst)
            if affine.get("fitted"):
                affine["translation_mm"] = affine.pop("translation")
                affine["rms_after_fit_mm"] = affine.pop("rms_after_fit")
            projection.update({
                "residual_mm_median": float(np.median(norms)), "residual_mm_max": float(np.max(norms)),
                "mean_residual_mm": np.mean([d["residual_mm"] for d in found], axis=0).tolist(),
                "pass": bool(len(found) == len(dots_out) and max(norms) <= args.max_residual_mm),
                "similarity": similarity, "affine": affine})
        entry["projection"] = projection

        # ---- B. dish registration (and today's rim, measured)
        dish: dict[str, Any] = {}
        uniform_images = frames.get("uniform_gray", [])
        uniform_gray = bm.load_u8(artifact / uniform_images[-1]["path"]) if uniform_images else None
        observation_path = observations_by_serial.get(serial)
        pointer_path = args.active_pointer_dir / f"Cam{serial}.json"
        if observation_path is None and pointer_path.is_file():
            pointer = json.loads(pointer_path.read_text(encoding="utf-8"))
            selection = pointer.get("selection") or {}
            dish["active_pointer"] = {"path": str(pointer_path), "sha256": sha256_token(pointer_path),
                                      "schema_version": pointer.get("schema_version"),
                                      "selected_at_utc": pointer.get("selected_at_utc"),
                                      "selection_policy": pointer.get("selection_policy"),
                                      "freshness": pointer.get("freshness")}
            observation_path = Path(str(selection.get("observation_path") or ""))
            stated = selection.get("observation_sha256")
            if observation_path.is_file() and stated and stated != sha256_token(observation_path):
                warnings.append(f"Cam{serial}: the active pointer's observation sha256 does not match the file")
                dish["observation_sha256_matches_pointer"] = False
        if observation_path is None or not observation_path.is_file():
            dish["status"] = "no_registration"
        else:
            observation = json.loads(observation_path.read_text(encoding="utf-8"))
            boundary = observation.get("accepted_inner_rim_boundary") or {}
            geometry = boundary.get("geometry") or {}
            centre = geometry.get("center_px") or {}
            cx, cy, radius = float(centre.get("x", float("nan"))), float(centre.get("y", float("nan"))), \
                float(geometry.get("radius_px", float("nan")))
            created = parse_utc(str(observation.get("created_utc") or ""))
            valid_until = created + timedelta(hours=FRESHNESS_HOURS) if created else None
            dish.update({"observation_path": str(observation_path), "observation_sha256": sha256_token(observation_path),
                         "schema_version": observation.get("schema_version"), "created_utc": observation.get("created_utc"),
                         "fresh_now": bool(valid_until and now <= valid_until),
                         "valid_until_utc": valid_until.strftime("%Y-%m-%dT%H:%M:%SZ") if valid_until else None,
                         "accepted_circle_camera_px": {"x": cx, "y": cy, "radius": radius}})
            # A lit frame of this session (uniform gray), else the observation's own source frame.
            rim_frame = None
            if uniform_images:
                rim_frame = {"source": "session:uniform_gray", "path": str(artifact / uniform_images[-1]["path"])}
            if rim_frame is None:
                for source in observation.get("source_frames") or []:
                    candidate = observation_path.parent / str(source.get("path") or "")
                    if candidate.is_file():
                        rim_frame = {"source": "observation_source_frame", "path": str(candidate)}
                        warnings.append(f"Cam{serial}: no lit frame in the session; rim checked on the observation's "
                                        f"own source frame ({observation.get('created_utc')}), not on today's dish placement")
                        break
            if rim_frame is not None:
                gray = bm.load_u8(Path(rim_frame["path"]))
                support = edge_support_fraction(gray, cx, cy, radius)
                rim_frame.update({"edge_support_fraction": round(support, 6),
                                  "edge_support_pass": bool(support >= EDGE_MIN_FRACTION)})
                dish["rim_frame"] = rim_frame
            dish["artifact_id"] = observation.get("artifact_id")
            model = scale_model(fish_plane, serial) if fish_plane else None
            if model:
                band = radius_band(model)
                dish["radius_band_px"] = {k: round(v, 3) for k, v in band.items()}
                dish["radius_pass"] = bool(band["accept_min_px"] <= radius <= band["accept_max_px"])
            else:
                warnings.append(f"Cam{serial}: no fish-plane scale model; radius band not checked")
            mx, my = bm.map_points(matrix, np.array([cx]), np.array([cy]))
            arena = arena_geometry(scene_status_any, arena_id, layout.get(arena_id))
            if arena:
                ec = arena["experimental_area_center_canvas_px"]
                ppm = arena["pixels_per_mm_projector"]
                offset_mm = [(float(mx[0]) - ec[0]) / ppm, (float(my[0]) - ec[1]) / ppm]
                dish["centre_in_canvas"] = {
                    "dish_centre_canvas_px": [float(mx[0]), float(my[0])], "experimental_area_center_canvas_px": ec,
                    "offset_mm": offset_mm, "offset_mm_norm": math.hypot(*offset_mm),
                    "pass": bool(math.hypot(*offset_mm) <= args.max_dish_offset_mm), "arena_source": arena["source"],
                    "note": "the rim sits about 9-11 mm above the projection plane; at the dish centre (near the "
                            "principal point) that parallax is well under 0.1 mm"}
            dish["status"] = "checked"
        entry["dish"] = dish

        # ---- C. crosshair
        cross_images = frames.get(CROSSHAIR_RECIPE, [])
        crosshair: dict[str, Any] = {}
        if not cross_images:
            crosshair["status"] = "no_crosshair_frame"
        else:
            image = cross_images[-1]
            pixels = bm.load_u8(artifact / image["path"])
            signal = pixels.astype(np.float64) - (black if black is not None else 0.0)
            np.clip(signal, 0, None, out=signal)
            area = experimental_area_from_status(image, arena_id)
            if area is None:
                lay = layout.get(arena_id)
                area = {"centre_canvas_px": list(lay.center), "radius_canvas_px": 40.0 * lay.pixels_per_mm_projector,
                        "radius_mm": 40.0, "source": "citrus_canvas_config_assumed_40mm"} if lay else None
            if area is None:
                crosshair["status"] = "no_experimental_area"
            else:
                ppm = layout[arena_id].pixels_per_mm_projector if arena_id in layout else 4.23
                centre_c = (area["centre_canvas_px"][0], area["centre_canvas_px"][1])
                cam_per_canvas = local_scale(inverse, centre_c, 10.0)
                found = detect_crosshair(signal, inverse, centre_c, area["radius_canvas_px"],
                                         args.search_mm * ppm * cam_per_canvas)
                crosshair.update({"status": "checked" if found.get("found") else "not_found", "path": image["path"],
                                  "experimental_area": area, **found})
                if found.get("found"):
                    ox, oy = found["centre_camera_px"]
                    pcx, pcy = bm.map_points(inverse, np.array([centre_c[0]]), np.array([centre_c[1]]))
                    mcx, mcy = bm.map_points(matrix, np.array([ox]), np.array([oy]))
                    d_canvas = (float(mcx[0]) - centre_c[0], float(mcy[0]) - centre_c[1])
                    crosshair["predicted_centre_camera_px"] = [float(pcx[0]), float(pcy[0])]
                    crosshair["homography_at_centre"] = {
                        "residual_camera_px": [ox - float(pcx[0]), oy - float(pcy[0])],
                        "residual_mm": [d_canvas[0] / ppm, d_canvas[1] / ppm],
                        "residual_mm_norm": math.hypot(d_canvas[0] / ppm, d_canvas[1] / ppm),
                        "pass": bool(math.hypot(d_canvas[0] / ppm, d_canvas[1] / ppm) <= args.max_residual_mm)}
                    model = scale_model(fish_plane, serial) if fish_plane else None
                    # Today's rim, measured on the uniform-gray frame. Start from the registered
                    # fit, else from the projected outline scaled to the dish (inner radius 40 mm,
                    # rim above the film).
                    if uniform_gray is not None:
                        reg = dish.get("accepted_circle_camera_px")
                        outline = found.get("outline_camera_px") or found.get("predicted_outline_camera_px")
                        if reg and all(math.isfinite(v) for v in reg.values()):
                            init = (reg["x"], reg["y"], reg["radius"])
                        else:
                            ratio = (DISH_INNER_DIAMETER_MM / 2.0) / float(area.get("radius_mm") or 40.0)
                            if model:
                                ratio *= radius_band(model)["px_per_mm_at_rim"] / scale_at(model, 0.0)
                            init = (outline["x"], outline["y"], outline["radius"] * ratio)
                        observed = detect_rim(uniform_gray, *init)
                        if observed.get("found"):
                            observed["edge_support_fraction"] = round(edge_support_fraction(
                                uniform_gray, observed["x"], observed["y"], observed["radius"]), 6)
                            observed["edge_support_pass"] = bool(observed["edge_support_fraction"] >= EDGE_MIN_FRACTION)
                            if model:
                                band = radius_band(model)
                                observed["radius_pass"] = bool(band["accept_min_px"] <= observed["radius"] <= band["accept_max_px"])
                                observed["radius_mm_at_rim"] = observed["radius"] / band["px_per_mm_at_rim"]
                        dish["observed_rim"] = observed

                    def placement(circle: dict[str, float], label: str) -> dict[str, Any]:
                        dx, dy = circle["x"] - ox, circle["y"] - oy
                        out_p: dict[str, Any] = {"circle": label, "rim_centre_minus_crosshair_camera_px": [dx, dy],
                                                 "distance_camera_px": math.hypot(dx, dy)}
                        if model:
                            band = radius_band(model)
                            s_rim, s_film = band["px_per_mm_at_rim"], scale_at(model, 0.0)
                            out_p.update({
                                "offset_mm_at_rim_scale": [dx / s_rim, dy / s_rim],
                                "distance_mm_at_rim_scale": math.hypot(dx, dy) / s_rim,
                                "offset_mm_at_film_scale": [dx / s_film, dy / s_film],
                                "px_per_mm_rim": s_rim, "px_per_mm_film": s_film,
                                "pass": bool(math.hypot(dx, dy) / s_rim <= args.max_dish_offset_mm)})
                        return out_p

                    observed = dish.get("observed_rim") or {}
                    if observed.get("found"):
                        crosshair["dish_placement"] = {
                            **placement(observed, "observed_rim_today"),
                            "note": "PRIMARY: today's rim (fitted on this session's uniform-gray frame) against the "
                                    "cross Citrus drew, both measured in camera px; no homography involved. mm with "
                                    "the fish-plane scale at the rim (mean of the band's z_low/z_high); film-plane "
                                    "value for comparison"}
                    reg = dish.get("accepted_circle_camera_px")
                    if reg and all(math.isfinite(v) for v in reg.values()):
                        crosshair["registered_rim_placement"] = placement(reg, "active_registration_fit")
                        if observed.get("found") and model:
                            s_rim = radius_band(model)["px_per_mm_at_rim"]
                            ddx, ddy, ddr = reg["x"] - observed["x"], reg["y"] - observed["y"], reg["radius"] - observed["radius"]
                            dish["registration_vs_today"] = {
                                "centre_offset_camera_px": [ddx, ddy], "centre_offset_mm": math.hypot(ddx, ddy) / s_rim,
                                "radius_difference_px": ddr,
                                "note": "the active registration's circle against today's measured rim: whether the "
                                        "dish has moved since it was registered"}
                    outline = found.get("outline_camera_px")
                    if outline and observed.get("found") and model:
                        crosshair["outline_vs_rim"] = {
                            "centre_offset_camera_px": [observed["x"] - outline["x"], observed["y"] - outline["y"]],
                            "outline_radius_mm_film": outline["radius"] / scale_at(model, 0.0),
                            "outline_radius_mm_commanded": area.get("radius_mm"),
                            "rim_radius_mm_rim": observed["radius"] / radius_band(model)["px_per_mm_at_rim"],
                            "note": "outline on the projection film, rim ~9-11 mm higher: each in its own plane's mm"}
                # Overlay: cross arms, observed centre, projected outline, rim.
                from PIL import ImageDraw

                cross_overlay = downsampled_rgb(pixels, args.overlay_factor)
                draw = ImageDraw.Draw(cross_overlay)
                f = float(args.overlay_factor)
                if found.get("found"):
                    for arm in found["arms"].values():
                        ap, ad = np.array(arm["point"]), np.array(arm["direction"])
                        a0, a1 = ap - ad * 2500, ap + ad * 2500
                        draw.line([(a0[0] / f, a0[1] / f), (a1[0] / f, a1[1] / f)], fill=(0, 200, 255))
                    ox, oy = found["centre_camera_px"]
                    draw.ellipse([(ox - 40) / f, (oy - 40) / f, (ox + 40) / f, (oy + 40) / f], outline=(255, 0, 0))
                    pc = crosshair.get("predicted_centre_camera_px")
                    if pc:
                        draw.line([((pc[0] - 60) / f, pc[1] / f), ((pc[0] + 60) / f, pc[1] / f)], fill=(0, 255, 0))
                        draw.line([(pc[0] / f, (pc[1] - 60) / f), (pc[0] / f, (pc[1] + 60) / f)], fill=(0, 255, 0))
                    outline = found.get("outline_camera_px")
                    if outline:
                        r = outline["radius"]
                        draw.ellipse([(outline["x"] - r) / f, (outline["y"] - r) / f, (outline["x"] + r) / f,
                                      (outline["y"] + r) / f], outline=(0, 200, 255))
                rim = dish.get("accepted_circle_camera_px")
                if rim and all(math.isfinite(v) for v in rim.values()):
                    r = rim["radius"]
                    draw.ellipse([(rim["x"] - r) / f, (rim["y"] - r) / f, (rim["x"] + r) / f, (rim["y"] + r) / f],
                                 outline=(255, 220, 0))
                cross_overlay.save(out / f"Cam{serial}_crosshair.png")
            if crosshair.get("status") != "checked":
                warnings.append(f"Cam{serial}: crosshair {crosshair.get('status')} ({crosshair.get('reason', '')})")
        entry["crosshair"] = crosshair

        # ---- D. today's dish: two edges, fitted separately (every camera with a lit frame)
        if uniform_gray is not None:
            first = dish.get("observed_rim")
            if first is None or not first.get("found"):
                reg0 = dish.get("accepted_circle_camera_px")
                if reg0 and all(math.isfinite(v) for v in reg0.values()):
                    first = detect_rim(uniform_gray, reg0["x"], reg0["y"], reg0["radius"])
            model = scale_model(fish_plane, serial) if fish_plane else None
            rims = None
            if first and first.get("found"):
                dish["first_pass_single_circle"] = {k: v for k, v in first.items() if k != "radial_edges"}
                dish["first_pass_single_circle"]["note"] = ("first pass only (a single circle can mix the floor edge "
                                                            "and the wall top); the two fits below replace it")
                dish.pop("observed_rim", None)
                rims = fit_two_rims(uniform_gray, (first["x"], first["y"], first["radius"]))
                s_low = s_high = s0 = None
                if model:
                    band = radius_band(model)
                    s_low, s_high = scale_at(model, band["z_low_mm"]), scale_at(model, band["z_high_mm"])
                    s0 = scale_at(model, 0.0)
                report_rims: dict[str, Any] = {k: v for k, v in rims.items()
                                               if k not in ("floor_edge", "wall_top", "fixed") and not k.startswith("_")}
                if not args.floor_diagnostic:
                    rims["floor_edge"] = {"found": False, "reason": "floor_diagnostic_off"}
                for name, scale in (("floor_edge", s_low), ("wall_top", s_high)):
                    fit = rims[name]
                    if fit.get("reason") == "floor_diagnostic_off":
                        continue
                    shown = {k: v for k, v in fit.items() if not k.startswith("_")}
                    if fit.get("found") and scale:
                        shown["radius_mm"] = fit["radius"] / scale
                        shown["residual_mm_median_abs_inliers"] = fit["residual_px_median_abs_inliers"] / scale
                        shown["residual_mm_max_abs_inliers"] = fit["residual_px_max_abs_inliers"] / scale
                        shown["px_per_mm_used"] = scale
                        u = fit.get("uncertainty") or {}
                        shown["uncertainty_mm"] = {"centre_sigma_mm": u.get("centre_sigma_px", float("nan")) / scale,
                                                   "radius_sigma_mm": u.get("radius_sigma_px", float("nan")) / scale}
                        shown["edge_support_gate_rule"] = round(edge_support_fraction(
                            uniform_gray, fit["x"], fit["y"], fit["radius"]), 6)
                    report_rims[name] = shown
                # Fixed-radius, centre-only fits: radius = calipered inner diameter / 2 x the scale at
                # each edge's plane.
                fixed: dict[str, Any] = {}
                if model:
                    theta_r, steps_r, ix, iy = rims["_steps"]
                    r_mm = DISH_CALIPERED_DIAMETER_MM / 2.0
                    edges = (("floor_edge", s_low), ("wall_top", s_high)) if args.floor_diagnostic else (("wall_top", s_high),)
                    for name, scale in edges:
                        free = rims[name]
                        fit = fixed_radius_fit(theta_r, steps_r, ix, iy, r_mm * scale, strengths=rims["_strength"])
                        if fit.get("found"):
                            fit["coverage_loss"] = coverage_loss(theta_r, fit["_used_rays"], steps_r, ix, iy,
                                                                 r_mm * scale, rims["_shape"], rims["_margin"])
                            # Step strength and floor-to-dark contrast per side (vignetting near the frame)
                            kept_sides = side_of(theta_r[fit["_kept_rays"]])
                            all_sides = side_of(theta_r)
                            overall = float(np.nanmedian(fit["_kept_strength"])) if fit["_kept_strength"].size else float("nan")
                            per_side = {}
                            for side, _, _ in SIDES:
                                st = fit["_kept_strength"][kept_sides == side]
                                ct = rims["_contrast"][all_sides == side]
                                med = float(np.nanmedian(st)) if st.size else None
                                per_side[side] = {"rays_with_point": int(st.size),
                                                  "median_step_dn_per_px": round(med, 2) if med is not None else None,
                                                  "relative_to_all_sides": round(med / overall, 2) if med and overall else None,
                                                  "median_floor_to_dark_contrast_dn": round(float(np.nanmedian(ct)), 1)
                                                  if np.isfinite(ct).any() else None}
                            fit["step_strength_by_side"] = {"all_sides_median_dn_per_px": round(overall, 2), **per_side}
                        fixed[name] = fit
                        shown = {k: v for k, v in fit.items() if not k.startswith("_")}
                        if fit.get("found"):
                            shown["radius_basis"] = (f"calipered {DISH_CALIPERED_DIAMETER_MM} +/- "
                                                     f"{DISH_CALIPERED_DIAMETER_SIGMA_MM} mm x {scale:.4f} px/mm "
                                                     f"(fish-plane scale at the {'floor corner (through water)' if name == 'floor_edge' else 'wall top'})")
                            shown["radius_sigma_from_diameter_px"] = DISH_CALIPERED_DIAMETER_SIGMA_MM / 2.0 * scale
                            shown["uncertainty_mm"] = {"centre_sigma_mm": fit["uncertainty"]["centre_sigma_px"] / scale}
                            shown["residual_mm_median_abs"] = fit["residual_px_median_abs"] / scale
                            if free.get("found"):
                                ddx, ddy = fit["x"] - free["x"], fit["y"] - free["y"]
                                shown["fixed_minus_free_centre_px"] = [ddx, ddy]
                                shown["fixed_vs_free_disagreement_mm"] = math.hypot(ddx, ddy) / scale
                                shown["free_radius_minus_fixed_px"] = free["radius"] - fit["radius_fixed_px"]
                        report_rims[f"{name}_fixed_radius"] = shown
                rims["fixed"] = fixed
                h_img, w_img = uniform_gray.shape
                principal = ((w_img - 1) / 2.0, (h_img - 1) / 2.0)
                if args.floor_diagnostic:
                    report_rims["parallax"] = parallax_check(rims["floor_edge"], rims["wall_top"], model, principal)
                outer = rims["wall_top"] if rims["wall_top"].get("found") else rims["floor_edge"]
                if outer.get("found"):
                    gaps = {"left": outer["x"] - outer["radius"], "top": outer["y"] - outer["radius"],
                            "right": (w_img - 1) - (outer["x"] + outer["radius"]),
                            "bottom": (h_img - 1) - (outer["y"] + outer["radius"])}
                    report_rims["frame_border_clearance_px"] = gaps
                    if s_high:
                        report_rims["frame_border_clearance_mm"] = {k: v / s_high for k, v in gaps.items()}
                    tight = [k for k, v in gaps.items() if v < 60]
                    if tight:
                        warnings.append(f"Cam{serial}: the dish rim comes within 60 px of the frame on the {', '.join(tight)}")
                dish["today"] = report_rims
                floor = rims["floor_edge"]
                if crosshair.get("found"):
                    ox, oy = crosshair["centre_camera_px"]
                    par = report_rims.get("parallax")

                    def offset(fit: dict[str, Any] | None, scale: float | None, height_scale: float | None) -> dict[str, Any] | None:
                        if not fit or not fit.get("found"):
                            return None
                        dx, dy = fit["x"] - ox, fit["y"] - oy
                        o: dict[str, Any] = {"offset_camera_px": [dx, dy], "distance_camera_px": math.hypot(dx, dy),
                                             "distance_sigma_camera_px": (fit.get("uncertainty") or {}).get("centre_sigma_px"),
                                             "arc_coverage_deg": (fit.get("arc_coverage") or {}).get("degrees_covered")}
                        if scale:
                            o.update({"offset_mm": [dx / scale, dy / scale], "distance_mm": math.hypot(dx, dy) / scale,
                                      "distance_sigma_mm": (o["distance_sigma_camera_px"] or float("nan")) / scale,
                                      "px_per_mm_used": scale})
                            if s0 and height_scale:
                                # the same physical centre moved to the film plane with the fish-plane model
                                p = np.array(principal)
                                film = p + s0 * (np.array([fit["x"], fit["y"]]) - p) / height_scale
                                o["film_corrected_distance_mm"] = math.hypot(film[0] - ox, film[1] - oy) / s0
                        return o

                    primary = offset((rims.get("fixed") or {}).get("wall_top"), s_high, s_high)
                    placement: dict[str, Any] = {}
                    if primary:
                        placement.update({"basis": "fixed_radius_wall_top_centre_vs_observed_crosshair", **primary,
                                          "pass": bool(primary.get("distance_mm") is not None and
                                                       primary["distance_mm"] <= args.max_dish_offset_mm),
                                          "note": "THE placement measurement (Jeremy 2026-11-15): wall top / inner rim centre from a fit "
                                                  "with the radius fixed by the calipered diameter, against the cross "
                                                  "Citrus drew; same camera image, no homography. mm at the wall-top "
                                                  "scale; film_corrected_distance_mm moves it to the film with the model"})
                    else:
                        placement["basis"] = "fixed_radius_wall_top_unavailable"
                    placement["cross_checks"] = {"free_wall_top": offset(rims["wall_top"], s_high, s_high)}
                    if args.floor_diagnostic:
                        placement["floor_diagnostic"] = {
                            "free_floor_edge": offset(floor, s_low, s_low),
                            "fixed_radius_floor_edge": offset((rims.get("fixed") or {}).get("floor_edge"), s_low, s_low),
                            "note": "diagnostic only; not in the placement or pass/fail"}
                    crosshair["dish_placement"] = placement
                    crosshair.pop("outline_vs_rim", None)
                    entry["crosshair"] = crosshair
                reg = dish.get("accepted_circle_camera_px")
                top = rims["wall_top"]
                if reg and top.get("found") and s_high:
                    dish["registration_vs_today"] = {
                        "compared_with": "wall_top fit (the registration's target is the inner rim at the top)",
                        "centre_offset_camera_px": [reg["x"] - top["x"], reg["y"] - top["y"]],
                        "centre_offset_mm": math.hypot(reg["x"] - top["x"], reg["y"] - top["y"]) / s_high,
                        "radius_difference_px": reg["radius"] - top["radius"]}
                else:
                    dish.pop("registration_vs_today", None)
            reg_circle = None
            reg = dish.get("accepted_circle_camera_px")
            if reg and all(math.isfinite(v) for v in reg.values()):
                state = (f"valid until {dish.get('valid_until_utc')}" if dish.get("fresh_now")
                         else f"EXPIRED {dish.get('valid_until_utc') or '?'}")
                reg_circle = {**reg, "label": f"registration {dish.get('artifact_id') or '?'}, {state}"}
            entry.setdefault("figures", []).extend(draw_dish_figures(
                out, serial, uniform_gray, rims, (crosshair.get("centre_camera_px") if crosshair.get("found") else None),
                crosshair.get("dish_placement"), reg_circle))

        if overlay is not None:
            from PIL import ImageDraw

            draw = ImageDraw.Draw(overlay)
            f = float(args.overlay_factor)
            for d in dots_out:
                p = d["predicted_camera_px"]
                draw.line([((p[0] - 40) / f, p[1] / f), ((p[0] + 40) / f, p[1] / f)], fill=(0, 255, 0))
                draw.line([(p[0] / f, (p[1] - 40) / f), (p[0] / f, (p[1] + 40) / f)], fill=(0, 255, 0))
                if d.get("found"):
                    m = d["camera_px"]
                    draw.ellipse([(m[0] - 24) / f, (m[1] - 24) / f, (m[0] + 24) / f, (m[1] + 24) / f], outline=(255, 0, 0))
            overlay.save(out / f"Cam{serial}_projection.png")

    projection_pass = [c["projection"].get("pass") for c in cameras.values() if "projection" in c]
    dish_checks = [c["dish"] for c in cameras.values() if c.get("dish", {}).get("status") == "checked"]
    report = {
        "schema_id": SCHEMA_ID, "schema_version": SCHEMA_VERSION,
        "created_utc": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "producer": {"tool": "scripts/registration_accuracy_check.py"},
        "inputs": {"session_dir": str(session), "citrus_config": str(args.citrus_config),
                   "citrus_config_sha256": "sha256:" + canvas_sha,
                   "fish_plane_scale": str(args.fish_plane_scale),
                   "fish_plane_scale_sha256": sha256_token(args.fish_plane_scale) if args.fish_plane_scale.is_file() else None},
        "limits": {"max_residual_mm": args.max_residual_mm, "max_dish_offset_mm": args.max_dish_offset_mm,
                   "edge_support": {"samples": EDGE_SAMPLES, "search_px": EDGE_SEARCH_PX, "min_step_dn": EDGE_MIN_STEP_DN,
                                    "min_fraction": EDGE_MIN_FRACTION, "gate": "orange_dish_rim_auto_accept_v1"},
                   "radius": {"inner_diameter_mm": DISH_INNER_DIAMETER_MM, "tolerance_fraction": RADIUS_TOLERANCE_FRACTION}},
        "summary": {
            "projection_residual_mm_median": float(np.median(all_residuals_mm)) if all_residuals_mm else None,
            "projection_residual_mm_max": float(np.max(all_residuals_mm)) if all_residuals_mm else None,
            "projection_pass": bool(projection_pass) and all(projection_pass),
            "crosshair_cameras_checked": sum(1 for c in cameras.values() if c.get("crosshair", {}).get("status") == "checked"),
            "dish_placement_mm_max": max((c["crosshair"]["dish_placement"].get("distance_mm", float("nan"))
                                          for c in cameras.values() if c.get("crosshair", {}).get("dish_placement")), default=None),
            "dish_cameras_checked": len(dish_checks),
            "dish_pass": bool(dish_checks) and all(
                d.get("rim_frame", {}).get("edge_support_pass", False) and d.get("radius_pass", False) and
                d.get("centre_in_canvas", {}).get("pass", False) for d in dish_checks),
        },
        "cameras": cameras, "warnings": warnings,
    }
    (out / "registration_accuracy.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {out / 'registration_accuracy.json'}")
    for serial, c in cameras.items():
        p = c.get("projection") or {}
        sim = p.get("similarity") or {}
        line = (f"Cam{serial}: dots {p.get('found')}/{p.get('drawn')} residual median "
                f"{p.get('residual_mm_median', float('nan')):.2f} max {p.get('residual_mm_max', float('nan')):.2f} mm")
        if sim.get("fitted"):
            line += (f"; similarity shift ({sim['translation_mm'][0]:.2f}, {sim['translation_mm'][1]:.2f}) mm "
                     f"rot {sim['rotation_deg']:.3f} deg scale {sim['scale']:.4f}")
        d = c.get("dish") or {}
        if d.get("status") == "checked":
            rf = d.get("rim_frame") or {}
            cc = d.get("centre_in_canvas") or {}
            line += (f" | dish: edge {rf.get('edge_support_fraction')} ({rf.get('source')}), radius "
                     f"{'ok' if d.get('radius_pass') else 'OUT'}, centre offset {cc.get('offset_mm_norm', float('nan')):.2f} mm"
                     f", fresh {d.get('fresh_now')}")
        x = c.get("crosshair") or {}
        if x.get("status") == "checked":
            hc = x.get("homography_at_centre") or {}
            dp = x.get("dish_placement") or {}
            line += (f" | cross: homography at centre {hc.get('residual_mm_norm', float('nan')):.2f} mm"
                     f", DISH (fixed-radius wall top) vs cross {dp.get('distance_mm', float('nan')):.2f}"
                     f" +/- {dp.get('distance_sigma_mm', float('nan')):.2f} mm (arc {dp.get('arc_coverage_deg')} deg,"
                     f" film-corrected {dp.get('film_corrected_distance_mm', float('nan')):.2f})"
                     + "".join(f"; {k} {v['distance_mm']:.2f}" for k, v in (dp.get("cross_checks") or {}).items()
                               if v and v.get("distance_mm") is not None))
        t = (c.get("dish") or {}).get("today") or {}
        for name in ("floor_edge", "wall_top"):
            fit = t.get(name) or {}
            if fit.get("found"):
                missing = [k for k, v in fit["per_side"].items() if not v["inliers"]]
                line += (f"\n    {name}: r {fit['radius']:.1f} px ({fit.get('radius_mm', float('nan')):.2f} mm), "
                         f"{fit['inliers']}/{fit['points']} inliers, resid median {fit['residual_px_median_abs_inliers']:.1f} "
                         f"max {fit['residual_px_max_abs_inliers']:.1f} px; per side "
                         + ", ".join(f"{k} {v['inliers']}" for k, v in fit["per_side"].items())
                         + (f"; NO POINTS {missing}" if missing else "")
                         + f"; arc {fit['arc_coverage']['degrees_covered']} deg, largest gap {fit['arc_coverage']['largest_gap_deg']} deg"
                         f" at {fit['arc_coverage']['largest_gap_centre_deg']} ({fit['arc_coverage']['largest_gap_side']})"
                         f"; sigma centre {fit['uncertainty']['centre_sigma_px']:.2f} px"
                         f" ({fit.get('uncertainty_mm', {}).get('centre_sigma_mm', float('nan')):.3f} mm),"
                         f" radius {fit['uncertainty']['radius_sigma_px']:.2f} px")
            elif name in t:
                line += f"\n    {name}: not found ({fit.get('reason')})"
        for name in ("floor_edge_fixed_radius", "wall_top_fixed_radius"):
            fx = t.get(name) or {}
            if fx.get("found"):
                line += (f"\n    {name}: r fixed {fx['radius_fixed_px']:.1f} px, {fx['points']} pts, resid median "
                         f"{fx['residual_px_median_abs']:.1f} px, arc {fx['arc_coverage']['degrees_covered']} deg, sigma centre "
                         f"{fx['uncertainty']['centre_sigma_px']:.2f} px; fixed vs free centre "
                         f"{fx.get('fixed_vs_free_disagreement_mm', float('nan')):.2f} mm, free r - fixed r "
                         f"{fx.get('free_radius_minus_fixed_px', float('nan')):.1f} px"
                         + "".join(f"\n      lost {k}: {v['degrees']} deg {v['angles_deg']}"
                                   for k, v in (fx.get("coverage_loss") or {}).items()
                                   if isinstance(v, dict) and v.get("degrees"))
                         + ("\n      step by side: " + ", ".join(
                             f"{k} {v['median_step_dn_per_px']} DN/px (x{v['relative_to_all_sides']}, contrast "
                             f"{v['median_floor_to_dark_contrast_dn']})"
                             for k, v in (fx.get("step_strength_by_side") or {}).items() if isinstance(v, dict))))
        par = t.get("parallax")
        if par:
            line += (f"\n    parallax: centre diff measured {np.round(par['centre_difference_measured_px'], 1).tolist()} px vs "
                     f"predicted {np.round(par['centre_difference_predicted_px'], 1).tolist()} px; radius diff measured "
                     f"{par['radius_difference_measured_px']:.1f} vs predicted {par['radius_difference_predicted_px']:.1f} px")
        if t.get("frame_border_clearance_px"):
            line += "\n    frame clearance px: " + ", ".join(f"{k} {v:.0f}" for k, v in t["frame_border_clearance_px"].items())
        rv = (c.get("dish") or {}).get("registration_vs_today")
        if rv:
            line += f"\n    registration vs today's wall top: {rv['centre_offset_mm']:.2f} mm, radius diff {rv['radius_difference_px']:.1f} px"
        print(line)
    for warning in warnings:
        print("warning: " + warning)
    return 0


if __name__ == "__main__":
    sys.exit(main())
