#!/usr/bin/env python3
"""Projector brightness map from Orange calibration sessions.

A brightness map is, per camera, the black-subtracted uniform-gray image
(`uniform_gray_reference - black_reference`) inside the lit arena aperture,
at one or more projector gray levels (one calibration session per level).

Per camera and level the tool reports the median signal above black, the
saturated fraction, a block grid (block mean / camera median) and its min,
max and max/min, and per camera the largest block difference of each level's
relative map from the highest level's map (the "same shape at every gray"
check). These follow the 2026-11-13 ad hoc analysis (flatfield_levels.py)
exactly, so that its saved result reproduces.

Facing-edge check: neighbouring arenas are seen by different cameras, and the
projected field is smooth across the gap between them. Using each camera's
active Citrus homography (camera native px -> final display canvas px) and
the arena rectangles of the Citrus canvas, the tool takes, in each camera of
a neighbouring pair, a strip of lit pixels along the arena side that faces
the neighbour and reports the ratio of the strips' mean signals. A ratio near
1 means the cameras agree on brightness and the in-arena non-uniformity is
the projector's.

Strip definition (defaults; see --edge-*): the canvas-space band from
`--edge-inset-mm` to `--edge-inset-mm + --edge-width-mm` inside the facing
side of the configured arena rectangle (3-8 mm: past the edge roll-off, which
ends within about 4 mm in every camera on omnifin0), over the same canvas
interval along the side for both cameras: +-`--edge-half-length-mm` about the
middle of the two sides' common extent. mm are canvas mm (the arena's
`pixels_per_mm_projector`). Only strip pixels inside the camera's lit mask are
averaged; the lit fraction of the strip is reported, and a strip below 0.95
lit or with differing exposure/gain between the two cameras is marked
invalid. Inward signal profiles (0.5 mm bins) and the ratio for other bands
are recorded as a sensitivity check.

Outputs (in --out, never overwritten): brightness_map.json
(schema_id orange.commissioning.brightness_map, schema_version 1),
brightness_map_relative.png and brightness_map_facing_edges.png.

Exit codes: 0 written, 2 input missing or inconsistent, 3 output exists.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np

SCHEMA_ID = "orange.commissioning.brightness_map"
SCHEMA_VERSION = 1
HOMOGRAPHY_DIRECTION = "camera_native_px_to_final_display_canvas_px"
OUTPUT_JSON = "brightness_map.json"
OUTPUT_RELATIVE_PNG = "brightness_map_relative.png"
OUTPUT_EDGES_PNG = "brightness_map_facing_edges.png"
SATURATION_THRESHOLD_U8 = 250
BLOCK_VALID_MASK_FRACTION = 0.9
STRIP_MIN_LIT_FRACTION = 0.95
STRIP_MIN_PIXELS = 1000
SATURATION_WARN_FRACTION = 1e-3
PROFILE_BIN_MM = 0.5
PROFILE_OUTSIDE_MM = 1.0
PROFILE_INSIDE_MM = 13.0
SENSITIVITY_BANDS_MM = ((1.0, 3.0), (3.0, 8.0), (8.0, 13.0))

EXIT_OK = 0
EXIT_INPUT = 2
EXIT_OUTPUT_EXISTS = 3


class InputError(Exception):
    """Missing or inconsistent input; exit 2."""


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise InputError(f"missing file: {path}") from exc
    except json.JSONDecodeError as exc:
        raise InputError(f"not JSON: {path}: {exc}") from exc


def arena_number(arena_id: str) -> int:
    match = re.search(r"(\d+)$", arena_id)
    return int(match.group(1)) if match else 0


def finite_or_none(value: float, digits: int | None = 6) -> float | None:
    if value is None or not math.isfinite(value):
        return None
    return float(value) if digits is None else round(float(value), digits)


# --------------------------------------------------------------------------
# Inputs


@dataclass
class ImageRef:
    role: str
    path: Path
    exposure_us: float | None
    gain: float | None
    sha256: str = ""


@dataclass
class CameraCapture:
    serial: str
    arena_id: str
    artifact_dir: Path
    image_set_path: Path
    image_set_sha256: str
    black: ImageRef
    uniform: ImageRef
    gray_u8: int | None
    gray_source: str
    resolved_arena: dict[str, Any] | None


@dataclass
class SessionInput:
    session_id: str
    session_dir: Path
    session_index_sha256: str
    cameras: list[CameraCapture]
    gray_u8: int = 0
    gray_source: str = ""
    exposure_us: float = 0.0
    exposure_source: str = ""
    gain: float | None = None


@dataclass
class ArenaLayout:
    arena_id: str
    camera_id: str
    center: tuple[float, float]
    size: tuple[int, int]
    origin: tuple[int, int]
    pixels_per_mm_projector: float

    def rect_edges(self) -> tuple[float, float, float, float]:
        """Continuous canvas coordinates (pixel centres at integers) of the
        rectangle covering pixels origin .. origin + size - 1."""
        x0 = self.origin[0] - 0.5
        y0 = self.origin[1] - 0.5
        return x0, y0, x0 + self.size[0], y0 + self.size[1]


@dataclass
class Homography:
    arena_id: str
    camera_id: str
    matrix: np.ndarray
    provenance: dict[str, Any] = field(default_factory=dict)


def resolve_session_dir(arg: str, sessions_root: Path) -> Path:
    candidate = Path(arg).expanduser()
    if candidate.is_dir() and (candidate / "session_index.json").is_file():
        return candidate.resolve()
    direct = sessions_root / arg
    if direct.is_dir():
        return direct.resolve()
    matches = sorted(p for p in sessions_root.glob(f"{arg}*") if p.is_dir()) if sessions_root.is_dir() else []
    if len(matches) == 1:
        return matches[0].resolve()
    if len(matches) > 1:
        raise InputError(f"session {arg!r} is ambiguous under {sessions_root}: {[m.name for m in matches]}")
    raise InputError(f"session not found: {arg} (sessions root {sessions_root})")


def gray_from_uniform_entry(entry: dict[str, Any], arena_id: str) -> tuple[int | None, str, dict[str, Any] | None]:
    post = entry.get("citrus_calibration_scene_post_capture") or {}
    for target in post.get("resolved_targets") or []:
        if target.get("arena_id") != arena_id:
            continue
        illumination = target.get("uniform_illumination") or {}
        gray = illumination.get("gray_u8")
        if gray is not None:
            return int(gray), "citrus_calibration_scene_post_capture.resolved_targets.uniform_illumination.gray_u8", target.get("arena")
        return None, "", target.get("arena")
    options = (entry.get("capture_group_membership") or {}).get("scene_options") or {}
    if options.get("foreground_gray_u8") is not None:
        return int(options["foreground_gray_u8"]), "capture_group_membership.scene_options.foreground_gray_u8", None
    return None, "", None


def load_camera_capture(artifact_dir: Path) -> CameraCapture:
    image_set_path = artifact_dir / "image_set.json"
    image_set = read_json(image_set_path)
    rig = image_set.get("rig_context") or {}
    serial = str(rig.get("camera_id") or (image_set.get("camera") or {}).get("serial") or "")
    arena_id = str(rig.get("arena_id") or "")
    if not serial or not arena_id:
        raise InputError(f"{image_set_path}: no rig_context camera_id/arena_id")
    by_role: dict[str, dict[str, Any]] = {}
    for entry in image_set.get("images") or []:
        role = entry.get("role")
        if role not in ("black_reference", "uniform_gray_reference"):
            continue
        if role in by_role:
            raise InputError(f"{image_set_path}: more than one {role} image")
        by_role[role] = entry
    refs: dict[str, ImageRef] = {}
    for role in ("black_reference", "uniform_gray_reference"):
        entry = by_role.get(role)
        if entry is None or not entry.get("path"):
            raise InputError(f"{image_set_path}: no {role} image")
        capture = entry.get("capture") or {}
        refs[role] = ImageRef(
            role=role,
            path=artifact_dir / entry["path"],
            exposure_us=capture.get("exposure_us"),
            gain=capture.get("gain"),
        )
    gray, gray_source, resolved_arena = gray_from_uniform_entry(by_role["uniform_gray_reference"], arena_id)
    return CameraCapture(
        serial=serial,
        arena_id=arena_id,
        artifact_dir=artifact_dir,
        image_set_path=image_set_path,
        image_set_sha256=sha256_file(image_set_path),
        black=refs["black_reference"],
        uniform=refs["uniform_gray_reference"],
        gray_u8=gray,
        gray_source=gray_source,
        resolved_arena=resolved_arena,
    )


def load_session(arg: str, sessions_root: Path, cli_gray: int | None, cli_exposure_us: float | None) -> SessionInput:
    session_dir = resolve_session_dir(arg, sessions_root)
    index_path = session_dir / "session_index.json"
    index = read_json(index_path)
    order = index.get("artifact_order") or sorted((index.get("artifacts_by_id") or {}).keys())
    if not order:
        raise InputError(f"{index_path}: no artifacts")
    cameras = []
    for artifact_id in order:
        artifact = (index.get("artifacts_by_id") or {}).get(artifact_id) or {}
        if artifact.get("artifact_schema_id") not in (None, "orange.calibration.image_set"):
            continue
        cameras.append(load_camera_capture(session_dir / "artifacts" / artifact_id))
    session = SessionInput(
        session_id=session_dir.name,
        session_dir=session_dir,
        session_index_sha256=sha256_file(index_path),
        cameras=cameras,
    )

    grays = {c.gray_u8 for c in cameras if c.gray_u8 is not None}
    if len(grays) > 1:
        raise InputError(f"{session.session_id}: cameras disagree on gray level {sorted(grays)}")
    if grays:
        gray = grays.pop()
        if cli_gray is not None and cli_gray != gray:
            raise InputError(f"{session.session_id}: --gray {cli_gray} but the session metadata says {gray}")
        session.gray_u8, session.gray_source = gray, "image_set:" + cameras[0].gray_source
    elif cli_gray is not None:
        session.gray_u8, session.gray_source = cli_gray, "cli"
    else:
        raise InputError(f"{session.session_id}: no gray level in the metadata; pass --gray")

    exposures: set[float] = set()
    gains: set[float] = set()
    for camera in cameras:
        black, uniform = camera.black, camera.uniform
        if black.exposure_us is not None and uniform.exposure_us is not None and black.exposure_us != uniform.exposure_us:
            raise InputError(
                f"{session.session_id} Cam{camera.serial}: black exposure {black.exposure_us} us != uniform {uniform.exposure_us} us"
            )
        if black.gain is not None and uniform.gain is not None and black.gain != uniform.gain:
            raise InputError(f"{session.session_id} Cam{camera.serial}: black gain {black.gain} != uniform {uniform.gain}")
        if uniform.exposure_us is not None:
            exposures.add(float(uniform.exposure_us))
        if uniform.gain is not None:
            gains.add(float(uniform.gain))
    if len(exposures) == 1:
        exposure = exposures.pop()
        if cli_exposure_us is not None and cli_exposure_us != exposure:
            raise InputError(f"{session.session_id}: --exposure-us {cli_exposure_us} but the session metadata says {exposure}")
        session.exposure_us, session.exposure_source = exposure, "image_set:images[].capture.exposure_us"
    elif len(exposures) > 1:
        # Different exposures per camera are recorded per camera; the level
        # exposure is then undefined.
        session.exposure_us, session.exposure_source = float("nan"), "image_set:per_camera_differs"
    elif cli_exposure_us is not None:
        session.exposure_us, session.exposure_source = cli_exposure_us, "cli"
        for camera in cameras:
            camera.black.exposure_us = camera.uniform.exposure_us = cli_exposure_us
    else:
        raise InputError(f"{session.session_id}: no exposure in the metadata; pass --exposure-us")
    session.gain = gains.pop() if len(gains) == 1 else None
    return session


def load_layout(citrus_config: Path) -> dict[str, ArenaLayout]:
    config = read_json(citrus_config)
    layout: dict[str, ArenaLayout] = {}
    for arena_id, arena in (config.get("arenas") or {}).items():
        camera_id = str(arena.get("active_camera_id") or "")
        entry = None
        for calibration in arena.get("camera_calibrations") or []:
            if str(calibration.get("camera_id")) == camera_id:
                entry = calibration
        if entry is None:
            continue
        cx, cy = int(entry["arena_center_x_px"]), int(entry["arena_center_y_px"])
        w, h = int(entry["arena_width_px"]), int(entry["arena_height_px"])
        layout[arena_id] = ArenaLayout(
            arena_id=arena_id,
            camera_id=camera_id,
            center=(float(cx), float(cy)),
            size=(w, h),
            origin=(cx - w // 2, cy - h // 2),
            pixels_per_mm_projector=float(entry["pixels_per_mm_projector"]),
        )
    if not layout:
        raise InputError(f"{citrus_config}: no arenas with an active camera calibration")
    return layout


def load_homography(calibration_dir: Path, arena_id: str, camera_id: str, canvas_sha256: str) -> Homography:
    pointer_path = calibration_dir / f"homography_active_{arena_id}_{camera_id}.json"
    pointer = read_json(pointer_path)
    contract = pointer.get("coordinate_contract") or {}
    source = contract.get("source") or {}
    destination = contract.get("destination") or {}
    problems = []
    if pointer.get("arena_id") != arena_id or str(pointer.get("camera_id")) != camera_id:
        problems.append("arena/camera identity")
    if pointer.get("homography_direction") != HOMOGRAPHY_DIRECTION or contract.get("homography_direction") != HOMOGRAPHY_DIRECTION:
        problems.append("direction")
    for name, side, space in (("source", source, "camera_native_px"), ("destination", destination, "final_display_canvas_px")):
        if side.get("space") != space or side.get("origin") != "top_left" or side.get("first_pixel_center") != [0.0, 0.0] \
                or side.get("positive_x") != "right" or side.get("positive_y") != "down":
            problems.append(f"{name} convention")
    if pointer.get("status") != "accepted":
        problems.append(f"status {pointer.get('status')!r}")
    matrix = np.asarray(pointer.get("homography_matrix"), dtype=np.float64)
    if matrix.shape != (3, 3) or not np.isfinite(matrix).all() or abs(np.linalg.det(matrix)) < 1e-12:
        problems.append("matrix")
    if problems:
        raise InputError(f"{pointer_path}: unexpected homography pointer ({', '.join(problems)})")
    canvas_at_acceptance = str(pointer.get("canvas_checksum_at_acceptance") or "")
    yaml_path = Path(str(pointer.get("homography_yaml_path") or ""))
    yaml_stated = str(pointer.get("homography_yaml_checksum") or "")
    yaml_actual = "sha256:" + sha256_file(yaml_path) if yaml_path.is_file() else None
    provenance = {
        "arena_id": arena_id,
        "camera_id": camera_id,
        "pointer_path": str(pointer_path),
        "pointer_sha256": sha256_file(pointer_path),
        "schema_id": pointer.get("schema_id"),
        "status": pointer.get("status"),
        "homography_role": pointer.get("homography_role"),
        "authority_role": pointer.get("authority_role"),
        "candidate_id": pointer.get("candidate_id"),
        "accepted_at_utc": pointer.get("accepted_at_utc"),
        "homography_direction": HOMOGRAPHY_DIRECTION,
        "pixel_convention": "source and destination: origin top_left, pixel centres at integer indices, +x right, +y down",
        "homography_yaml_path": str(yaml_path),
        "homography_yaml_checksum_stated": yaml_stated,
        "homography_yaml_checksum_matches": (yaml_actual == yaml_stated) if yaml_actual else None,
        "canvas_checksum_at_acceptance": canvas_at_acceptance,
        "canvas_checksum_matches_citrus_config": canvas_at_acceptance == "sha256:" + canvas_sha256,
        "matrix": matrix.tolist(),
    }
    return Homography(arena_id=arena_id, camera_id=camera_id, matrix=matrix, provenance=provenance)


# --------------------------------------------------------------------------
# Analysis


def load_u8(path: Path) -> np.ndarray:
    from PIL import Image

    with Image.open(path) as image:
        if image.mode != "L":
            raise InputError(f"{path}: expected 8-bit grayscale (mode L), got {image.mode}")
        return np.asarray(image)


def lit_mask(signal: np.ndarray) -> tuple[np.ndarray, float]:
    """The 2026-11-13 rule: lit where the signal exceeds half the median of
    the brightest 40 % of pixels."""
    threshold = 0.5 * float(np.median(signal[signal > np.percentile(signal, 60)]))
    return signal > threshold, threshold


def block_grid(signal: np.ndarray, mask: np.ndarray, blocks: int) -> np.ndarray:
    height, width = signal.shape
    bh, bw = height // blocks, width // blocks
    grid = np.full((blocks, blocks), np.nan)
    for i in range(blocks):
        for j in range(blocks):
            m = mask[i * bh:(i + 1) * bh, j * bw:(j + 1) * bw]
            if m.mean() > BLOCK_VALID_MASK_FRACTION:
                grid[i, j] = signal[i * bh:(i + 1) * bh, j * bw:(j + 1) * bw][m].mean()
    return grid


def level_statistics(black: np.ndarray, uniform: np.ndarray, blocks: int) -> tuple[dict[str, Any], np.ndarray, np.ndarray, np.ndarray]:
    if black.shape != uniform.shape:
        raise InputError(f"black {black.shape} and uniform {uniform.shape} differ in size")
    signal = uniform.astype(np.int16) - black.astype(np.int16)
    mask, threshold = lit_mask(signal)
    if not mask.any():
        raise InputError("no lit pixels")
    grid = block_grid(signal, mask, blocks)
    median_grid = np.nanmedian(grid) if np.isfinite(grid).any() else float("nan")
    relative = grid / median_grid
    rel_min, rel_max = float(np.nanmin(relative)), float(np.nanmax(relative))
    stats = {
        "median_above_black": float(np.median(signal[mask])),
        "saturated_fraction": float((uniform[mask] >= SATURATION_THRESHOLD_U8).mean()),
        "lit_area_fraction": float(mask.mean()),
        "lit_threshold": threshold,
        "black_mean": float(black.mean()),
        "block_median_above_black": float(median_grid),
        "rel_min": rel_min,
        "rel_max": rel_max,
        "max_over_min": rel_max / rel_min,
        "valid_blocks": int(np.isfinite(relative).sum()),
    }
    return stats, relative, signal, mask


@dataclass
class FacingSide:
    arena_id: str
    side: str  # left, right, top, bottom (canvas)

    def inward_and_tangent(self, layout: ArenaLayout, x: np.ndarray, y: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        x0, y0, x1, y1 = layout.rect_edges()
        if self.side == "right":
            return x1 - x, y
        if self.side == "left":
            return x - x0, y
        if self.side == "bottom":
            return y1 - y, x
        return y - y0, x

    def edge_coordinate(self, layout: ArenaLayout) -> float:
        x0, y0, x1, y1 = layout.rect_edges()
        return {"right": x1, "left": x0, "bottom": y1, "top": y0}[self.side]

    def tangent_range(self, layout: ArenaLayout) -> tuple[float, float]:
        x0, y0, x1, y1 = layout.rect_edges()
        return (y0, y1) if self.side in ("left", "right") else (x0, x1)

    def canvas_region(self, layout: ArenaLayout, d_lo: float, d_hi: float, t_lo: float, t_hi: float) -> np.ndarray:
        """Corners (canvas px) of the region with inward distance d in
        [d_lo, d_hi] and tangent coordinate in [t_lo, t_hi]."""
        e = self.edge_coordinate(layout)
        sign = -1.0 if self.side in ("right", "bottom") else 1.0
        n_lo, n_hi = e + sign * d_lo, e + sign * d_hi
        if self.side in ("left", "right"):
            return np.array([[n_lo, t_lo], [n_hi, t_lo], [n_hi, t_hi], [n_lo, t_hi]])
        return np.array([[t_lo, n_lo], [t_lo, n_hi], [t_hi, n_hi], [t_hi, n_lo]])


def neighbour_pairs(layout: dict[str, ArenaLayout]) -> list[tuple[str, str, str, str]]:
    """(arena_a, side_a, arena_b, side_b) for arenas whose rectangles face
    each other across a gap with no third arena between them."""
    ids = sorted(layout, key=lambda a: (arena_number(a), a))
    pairs = []

    def overlap(a: tuple[float, float], b: tuple[float, float]) -> float:
        return min(a[1], b[1]) - max(a[0], b[0])

    for i, a in enumerate(ids):
        for b in ids[i + 1:]:
            la, lb = layout[a], layout[b]
            dx, dy = lb.center[0] - la.center[0], lb.center[1] - la.center[1]
            horizontal = abs(dx) >= abs(dy)
            ax0, ay0, ax1, ay1 = la.rect_edges()
            bx0, by0, bx1, by1 = lb.rect_edges()
            if horizontal:
                side_a, side_b = ("right", "left") if dx > 0 else ("left", "right")
                if overlap((ay0, ay1), (by0, by1)) <= 0:
                    continue
                lo, hi = sorted((la.center[0], lb.center[0]))
            else:
                side_a, side_b = ("bottom", "top") if dy > 0 else ("top", "bottom")
                if overlap((ax0, ax1), (bx0, bx1)) <= 0:
                    continue
                lo, hi = sorted((la.center[1], lb.center[1]))
            blocked = False
            for c in ids:
                if c in (a, b):
                    continue
                lc = layout[c]
                cx0, cy0, cx1, cy1 = lc.rect_edges()
                along = lc.center[0] if horizontal else lc.center[1]
                if lo < along < hi:
                    across_c = (cy0, cy1) if horizontal else (cx0, cx1)
                    if overlap(across_c, (ay0, ay1) if horizontal else (ax0, ax1)) > 0 and \
                            overlap(across_c, (by0, by1) if horizontal else (bx0, bx1)) > 0:
                        blocked = True
            if not blocked:
                pairs.append((a, side_a, b, side_b))
    return pairs


def map_points(matrix: np.ndarray, x: np.ndarray, y: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    w = matrix[2, 0] * x + matrix[2, 1] * y + matrix[2, 2]
    return (matrix[0, 0] * x + matrix[0, 1] * y + matrix[0, 2]) / w, (matrix[1, 0] * x + matrix[1, 1] * y + matrix[1, 2]) / w


def sample_side(
    signal: np.ndarray,
    mask: np.ndarray,
    homography: Homography,
    layout: ArenaLayout,
    side: FacingSide,
    t_window: tuple[float, float],
    bands_mm: list[tuple[float, float]],
) -> dict[str, Any]:
    """Pixels of the camera that map into the canvas region in front of the
    facing side: inward profile and band means (lit pixels only)."""
    ppmm = layout.pixels_per_mm_projector
    corners = side.canvas_region(layout, -PROFILE_OUTSIDE_MM * ppmm, PROFILE_INSIDE_MM * ppmm, *t_window)
    inverse = np.linalg.inv(homography.matrix)
    cam_x, cam_y = map_points(inverse, corners[:, 0], corners[:, 1])
    height, width = signal.shape
    c0 = max(int(math.floor(cam_x.min())) - 2, 0)
    c1 = min(int(math.ceil(cam_x.max())) + 3, width)
    r0 = max(int(math.floor(cam_y.min())) - 2, 0)
    r1 = min(int(math.ceil(cam_y.max())) + 3, height)
    result: dict[str, Any] = {"camera_bbox_px": [c0, r0, c1, r1]}
    if c1 <= c0 or r1 <= r0:
        result["profile"] = []
        result["bands"] = [{"band_mm": list(b), "pixels": 0, "lit_fraction": 0.0, "mean": None, "median": None} for b in bands_mm]
        return result
    rows, cols = np.mgrid[r0:r1, c0:c1]
    canvas_x, canvas_y = map_points(homography.matrix, cols.astype(np.float64), rows.astype(np.float64))
    inward, tangent = side.inward_and_tangent(layout, canvas_x, canvas_y)
    inward_mm = inward / ppmm
    in_window = (tangent >= t_window[0]) & (tangent <= t_window[1])
    sub_signal = signal[r0:r1, c0:c1]
    sub_mask = mask[r0:r1, c0:c1]

    profile = []
    edges = np.arange(-PROFILE_OUTSIDE_MM, PROFILE_INSIDE_MM + 1e-9, PROFILE_BIN_MM)
    for lo, hi in zip(edges[:-1], edges[1:]):
        sel = in_window & (inward_mm >= lo) & (inward_mm < hi)
        n = int(sel.sum())
        profile.append({
            "inward_mm": round(float((lo + hi) / 2), 3),
            "pixels": n,
            "mean_all": finite_or_none(float(sub_signal[sel].mean())) if n else None,
            "lit_fraction": finite_or_none(float(sub_mask[sel].mean())) if n else None,
        })
    bands = []
    for lo, hi in bands_mm:
        sel = in_window & (inward_mm >= lo) & (inward_mm < hi)
        n = int(sel.sum())
        lit = sel & sub_mask
        n_lit = int(lit.sum())
        bands.append({
            "band_mm": [lo, hi],
            "pixels": n,
            "lit_fraction": finite_or_none(n_lit / n) if n else 0.0,
            "mean": finite_or_none(float(sub_signal[lit].mean())) if n_lit else None,
            "median": finite_or_none(float(np.median(sub_signal[lit]))) if n_lit else None,
        })
    result["profile"] = profile
    result["bands"] = bands
    return result


def facing_edge_geometry(layout: dict[str, ArenaLayout], pair: tuple[str, str, str, str], half_length_mm: float) -> dict[str, Any]:
    a, side_a, b, side_b = pair
    la, lb = layout[a], layout[b]
    fa, fb = FacingSide(a, side_a), FacingSide(b, side_b)
    ra, rb = fa.tangent_range(la), fb.tangent_range(lb)
    common = (max(ra[0], rb[0]), min(ra[1], rb[1]))
    mid = 0.5 * (common[0] + common[1])
    ppmm = 0.5 * (la.pixels_per_mm_projector + lb.pixels_per_mm_projector)
    half = min(half_length_mm * ppmm, 0.5 * (common[1] - common[0]))
    gap_px = abs(fb.edge_coordinate(lb) - fa.edge_coordinate(la))
    return {
        "arena_a": a,
        "camera_a": la.camera_id,
        "side_a": side_a,
        "arena_b": b,
        "camera_b": lb.camera_id,
        "side_b": side_b,
        "tangent_axis": "canvas_y" if side_a in ("left", "right") else "canvas_x",
        "tangent_window_canvas_px": [round(mid - half, 3), round(mid + half, 3)],
        "tangent_window_mm": round(2 * half / ppmm, 3),
        "edge_a_canvas_px": fa.edge_coordinate(la),
        "edge_b_canvas_px": fb.edge_coordinate(lb),
        "gap_canvas_px": round(gap_px, 3),
        "gap_mm": round(gap_px / ppmm, 3),
        "_sides": (fa, fb),
        "_window": (mid - half, mid + half),
    }


# --------------------------------------------------------------------------
# Report


def build_report(args: argparse.Namespace) -> tuple[dict[str, Any], dict[str, Any]]:
    sessions_root = Path(args.sessions_root).expanduser()
    citrus_config = Path(args.citrus_config).expanduser().resolve()
    if not citrus_config.is_file():
        raise InputError(f"missing Citrus canvas config: {citrus_config}")
    grays = args.gray or []
    exposures = args.exposure_us or []
    if grays and len(grays) != len(args.session):
        raise InputError("--gray must be given once per --session (in the same order) or not at all")
    if exposures and len(exposures) != len(args.session):
        raise InputError("--exposure-us must be given once per --session (in the same order) or not at all")
    sessions = [
        load_session(s, sessions_root, grays[i] if grays else None, exposures[i] if exposures else None)
        for i, s in enumerate(args.session)
    ]

    # Fail fast on missing images, all at once.
    missing = [str(ref.path) for s in sessions for c in s.cameras for ref in (c.black, c.uniform) if not ref.path.is_file()]
    if missing:
        raise InputError("missing images:\n  " + "\n  ".join(missing))
    level_keys = [str(s.gray_u8) for s in sessions]
    if len(set(level_keys)) != len(level_keys):
        raise InputError(f"two sessions at the same gray level: {level_keys}")
    camera_sets = {tuple(sorted((c.serial, c.arena_id) for c in s.cameras)) for s in sessions}
    if len(camera_sets) != 1:
        raise InputError("sessions do not cover the same cameras/arenas")
    sessions.sort(key=lambda s: s.gray_u8)
    cameras = sorted(next(iter(camera_sets)), key=lambda c: (arena_number(c[1]), c[1]))

    canvas_sha256 = sha256_file(citrus_config)
    layout = load_layout(citrus_config)
    calibration_dir = citrus_config.parent / "calibration_artifacts"
    homographies: dict[str, Homography] = {}
    for serial, arena_id in cameras:
        if arena_id not in layout or layout[arena_id].camera_id != serial:
            raise InputError(f"Cam{serial} {arena_id}: not the active camera of that arena in {citrus_config}")
        homographies[serial] = load_homography(calibration_dir, arena_id, serial, canvas_sha256)

    warnings: list[str] = []
    for h in homographies.values():
        if not h.provenance["canvas_checksum_matches_citrus_config"]:
            warnings.append(f"Cam{h.camera_id}: homography accepted on a different canvas checksum than {citrus_config.name} now has")
        if h.provenance["homography_yaml_checksum_matches"] is False:
            warnings.append(f"Cam{h.camera_id}: homography yaml checksum differs from the pointer")

    session_arena_ids = {a for _, a in cameras}
    pairs = [p for p in neighbour_pairs(layout) if p[0] in session_arena_ids and p[2] in session_arena_ids]
    geometries = [facing_edge_geometry(layout, p, args.edge_half_length_mm) for p in pairs]
    strip_band = (args.edge_inset_mm, args.edge_inset_mm + args.edge_width_mm)
    bands = [strip_band] + [b for b in SENSITIVITY_BANDS_MM if b != strip_band]

    level_results: dict[str, dict[str, Any]] = {}
    relative_maps: dict[tuple[str, str], np.ndarray] = {}
    side_samples: dict[tuple[str, str, str], dict[str, Any]] = {}
    input_sessions = []
    for session in sessions:
        level = str(session.gray_u8)
        level_results[level] = {}
        session_record = {
            "session_id": session.session_id,
            "session_dir": str(session.session_dir),
            "session_index_sha256": session.session_index_sha256,
            "gray_u8": session.gray_u8,
            "gray_source": session.gray_source,
            "exposure_us": finite_or_none(session.exposure_us),
            "exposure_source": session.exposure_source,
            "gain": session.gain,
            "cameras": [],
        }
        for camera in sorted(session.cameras, key=lambda c: (arena_number(c.arena_id), c.arena_id)):
            for ref in (camera.black, camera.uniform):
                ref.sha256 = sha256_file(ref.path)
            black = load_u8(camera.black.path)
            uniform = load_u8(camera.uniform.path)
            try:
                stats, relative, signal, mask = level_statistics(black, uniform, args.blocks)
            except InputError as exc:
                raise InputError(f"{session.session_id} Cam{camera.serial}: {exc}") from exc
            del black
            relative_maps[(level, camera.serial)] = relative
            if stats["saturated_fraction"] > SATURATION_WARN_FRACTION:
                warnings.append(f"gray {level} Cam{camera.serial}: saturated fraction {stats['saturated_fraction']:.4f}")
            layout_check = None
            if camera.resolved_arena:
                origin = camera.resolved_arena.get("origin_final_display_canvas_px") or {}
                size = camera.resolved_arena.get("size_px") or {}
                la = layout[camera.arena_id]
                layout_check = (origin.get("x"), origin.get("y"), size.get("width"), size.get("height")) == \
                    (la.origin[0], la.origin[1], la.size[0], la.size[1])
                if not layout_check:
                    warnings.append(f"{session.session_id} {camera.arena_id}: presented arena rectangle differs from {citrus_config.name}")
            record = {
                "arena_id": camera.arena_id,
                "median_above_black": stats["median_above_black"],
                "saturated_fraction": stats["saturated_fraction"],
                "lit_area_fraction": round(stats["lit_area_fraction"], 6),
                "lit_threshold": stats["lit_threshold"],
                "black_mean": round(stats["black_mean"], 4),
                "block_median_above_black": round(stats["block_median_above_black"], 4),
                "rel_min": stats["rel_min"],
                "rel_max": stats["rel_max"],
                "max_over_min": stats["max_over_min"],
                "valid_blocks": stats["valid_blocks"],
                "block_grid_relative": [[finite_or_none(v, 5) for v in row] for row in relative],
                "exposure_us": camera.uniform.exposure_us,
                "gain": camera.uniform.gain,
                "presented_arena_matches_citrus_config": layout_check,
            }
            level_results[level][camera.serial] = record
            for geometry in geometries:
                for which in ("a", "b"):
                    if geometry[f"camera_{which}"] != camera.serial:
                        continue
                    side = geometry["_sides"][0 if which == "a" else 1]
                    side_samples[(level, geometry["arena_a"] + "|" + geometry["arena_b"], which)] = sample_side(
                        signal, mask, homographies[camera.serial], layout[camera.arena_id], side, geometry["_window"], bands
                    )
            del signal, mask, uniform
            session_record["cameras"].append({
                "serial": camera.serial,
                "arena_id": camera.arena_id,
                "image_set_path": str(camera.image_set_path),
                "image_set_sha256": camera.image_set_sha256,
                "black_reference": {"path": str(camera.black.path), "sha256": camera.black.sha256,
                                    "exposure_us": camera.black.exposure_us, "gain": camera.black.gain},
                "uniform_gray_reference": {"path": str(camera.uniform.path), "sha256": camera.uniform.sha256,
                                           "exposure_us": camera.uniform.exposure_us, "gain": camera.uniform.gain},
            })
        input_sessions.append(session_record)

    # Shape vs the highest level.
    reference_level = str(sessions[-1].gray_u8)
    shape = {}
    for serial, _ in cameras:
        reference = relative_maps[(reference_level, serial)]
        shape[serial] = {}
        for session in sessions:
            level = str(session.gray_u8)
            diff = np.abs(relative_maps[(level, serial)] - reference)
            shape[serial][level] = finite_or_none(float(np.nanmax(diff)) if np.isfinite(diff).any() else float("nan"), None)

    # Facing edges.
    facing = []
    for geometry in geometries:
        key = geometry["arena_a"] + "|" + geometry["arena_b"]
        per_level = {}
        for session in sessions:
            level = str(session.gray_u8)
            sa, sb = side_samples[(level, key, "a")], side_samples[(level, key, "b")]
            ra = level_results[level][geometry["camera_a"]]
            rb = level_results[level][geometry["camera_b"]]
            reasons = []
            if ra["exposure_us"] != rb["exposure_us"] or ra["gain"] != rb["gain"]:
                reasons.append("exposure or gain differs between the two cameras")
            band_ratios = []
            for band_a, band_b in zip(sa["bands"], sb["bands"]):
                ok = band_a["mean"] is not None and band_b["mean"] is not None and band_b["mean"] > 0
                band_ratios.append({
                    "band_mm": band_a["band_mm"],
                    "mean_a": band_a["mean"],
                    "mean_b": band_b["mean"],
                    "pixels_a": band_a["pixels"],
                    "pixels_b": band_b["pixels"],
                    "lit_fraction_a": band_a["lit_fraction"],
                    "lit_fraction_b": band_b["lit_fraction"],
                    "ratio_a_over_b": finite_or_none(band_a["mean"] / band_b["mean"]) if ok else None,
                })
            strip = band_ratios[0]
            for which in ("a", "b"):
                if strip[f"pixels_{which}"] < STRIP_MIN_PIXELS:
                    reasons.append(f"strip {which} has {strip[f'pixels_{which}']} pixels (< {STRIP_MIN_PIXELS})")
                if (strip[f"lit_fraction_{which}"] or 0.0) < STRIP_MIN_LIT_FRACTION:
                    reasons.append(f"strip {which} lit fraction {strip[f'lit_fraction_{which}']} (< {STRIP_MIN_LIT_FRACTION})")
            if strip["ratio_a_over_b"] is None:
                reasons.append("no strip mean")
            per_level[level] = {
                "status": "ok" if not reasons else "invalid",
                "invalid_reasons": reasons,
                "ratio_a_over_b": strip["ratio_a_over_b"],
                "mean_a": strip["mean_a"],
                "mean_b": strip["mean_b"],
                "pixels_a": strip["pixels_a"],
                "pixels_b": strip["pixels_b"],
                "lit_fraction_a": strip["lit_fraction_a"],
                "lit_fraction_b": strip["lit_fraction_b"],
                "band_sensitivity": band_ratios[1:],
                "profile_a": sa["profile"],
                "profile_b": sb["profile"],
                "camera_bbox_px_a": sa["camera_bbox_px"],
                "camera_bbox_px_b": sb["camera_bbox_px"],
            }
            if reasons:
                warnings.append(f"facing edge {key} gray {level}: invalid ({'; '.join(reasons)})")
        facing.append({k: v for k, v in geometry.items() if not k.startswith("_")} | {"levels": per_level})

    # Summary.
    all_mom = [r["max_over_min"] for lvl in level_results.values() for r in lvl.values()]
    relative_brightness: dict[str, dict[str, float]] = {}
    for level, per_camera in level_results.items():
        mean_median = float(np.mean([r["median_above_black"] for r in per_camera.values()]))
        relative_brightness[level] = {s: round(r["median_above_black"] / mean_median, 4) for s, r in per_camera.items()}
    mean_relative = {serial: round(float(np.mean([relative_brightness[l][serial] for l in relative_brightness])), 4) for serial, _ in cameras}
    shape_values = [v for per in shape.values() for lvl, v in per.items() if lvl != reference_level and v is not None]
    facing_summary = {}
    ok_ratios = []
    for entry in facing:
        ratios = {lvl: d["ratio_a_over_b"] for lvl, d in entry["levels"].items() if d["status"] == "ok"}
        ok_ratios += list(ratios.values())
        facing_summary[f"{entry['arena_a']}|{entry['arena_b']}"] = {
            "cameras": f"{entry['camera_a']}/{entry['camera_b']}",
            "ratio_by_level": ratios,
            "ratio_mean": finite_or_none(float(np.mean(list(ratios.values()))), 4) if ratios else None,
            "ratio_range": [round(min(ratios.values()), 4), round(max(ratios.values()), 4)] if ratios else None,
        }
    summary = {
        "levels_gray_u8": [s.gray_u8 for s in sessions],
        "within_arena_max_over_min_range": [round(min(all_mom), 4), round(max(all_mom), 4)],
        "shape_reference_level": reference_level,
        "shape_max_abs_diff_vs_reference": round(max(shape_values), 4) if shape_values else None,
        "camera_median_relative_to_mean_by_level": relative_brightness,
        "camera_median_relative_to_mean": mean_relative,
        "facing_edge": facing_summary,
        "facing_edge_max_abs_deviation_from_1": round(max(abs(r - 1.0) for r in ok_ratios), 4) if ok_ratios else None,
        "warnings": warnings,
    }

    report = {
        "schema_id": SCHEMA_ID,
        "schema_version": SCHEMA_VERSION,
        "created_utc": utc_now(),
        "producer": {
            "tool": "scripts/brightness_map.py",
            "tool_sha256": sha256_file(Path(__file__).resolve()),
        },
        "inputs": {
            "sessions": input_sessions,
            "citrus_config": {"path": str(citrus_config), "sha256": canvas_sha256},
            "homographies": [homographies[s].provenance for s, _ in cameras],
        },
        "parameters": {
            "blocks": args.blocks,
            "signal": "uniform_gray_reference - black_reference (8-bit counts above black)",
            "lit_mask": "signal > 0.5 * median(signal[signal > percentile(signal, 60)])",
            "block_rule": f"block mean of lit pixels where the block is > {BLOCK_VALID_MASK_FRACTION:.0%} lit; relative = block / median of blocks",
            "saturation_threshold_u8": SATURATION_THRESHOLD_U8,
            "saturated_fraction_domain": "uniform image pixels inside the lit mask",
            "facing_edge_strip": {
                "inward_band_mm": list(strip_band),
                "half_length_mm": args.edge_half_length_mm,
                "definition": (
                    "canvas-space band from inset to inset+width mm inside the facing side of the configured arena "
                    "rectangle (shadow.json camera_calibrations arena_center/size; edges at origin-0.5 and "
                    "origin+size-0.5 with pixel centres at integers), restricted to the same canvas interval "
                    "along the side for both cameras (+-half_length_mm about the middle of the two sides' common "
                    "extent, clipped to it); mm via each arena's pixels_per_mm_projector; camera pixels mapped "
                    "with the active homography; mean over strip pixels inside the camera's lit mask"
                ),
                "ratio": "mean(strip of arena_a's camera) / mean(strip of arena_b's camera); arena_a is the lower-numbered arena",
                "valid_if": f"lit fraction >= {STRIP_MIN_LIT_FRACTION} and >= {STRIP_MIN_PIXELS} pixels in both strips, same exposure and gain",
                "profile_bin_mm": PROFILE_BIN_MM,
                "profile_range_mm": [-PROFILE_OUTSIDE_MM, PROFILE_INSIDE_MM],
            },
        },
        "levels": level_results,
        "shape_max_abs_diff_vs_highest_level": shape,
        "facing_edges": facing,
        "summary": summary,
    }
    plot_data = {"relative_maps": relative_maps, "sessions": sessions, "cameras": cameras}
    return report, plot_data


# --------------------------------------------------------------------------
# Plots


def plot_relative(path: Path, report: dict[str, Any], plot_data: dict[str, Any]) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    sessions, cameras = plot_data["sessions"], plot_data["cameras"]
    rows, cols = len(sessions), len(cameras)
    fig, axes = plt.subplots(rows, cols, figsize=(4.5 * cols, 4.5 * rows), squeeze=False)
    image = None
    for ri, session in enumerate(sessions):
        level = str(session.gray_u8)
        for ci, (serial, arena_id) in enumerate(cameras):
            record = report["levels"][level][serial]
            ax = axes[ri, ci]
            image = ax.imshow(plot_data["relative_maps"][(level, serial)], vmin=0.6, vmax=1.5, cmap="RdBu_r")
            ax.set_xticks([])
            ax.set_yticks([])
            exposure = record["exposure_us"]
            exposure_text = f"{exposure / 1000:g} ms" if exposure else "?"
            ax.set_title(
                f"gray {level} ({exposure_text}) Cam{serial} {arena_id}\n"
                f"median {record['median_above_black']:.0f} u8, max/min {record['max_over_min']:.2f}",
                fontsize=9,
            )
    if image is not None:
        fig.colorbar(image, ax=axes, fraction=0.02, label="block mean / camera median")
    fig.suptitle("Relative illumination per arena (block mean / camera median, camera-relative)", fontsize=12)
    fig.savefig(path, format="png", dpi=80, bbox_inches="tight")
    plt.close(fig)


def plot_facing_edges(path: Path, report: dict[str, Any], plot_data: dict[str, Any]) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    facing = report["facing_edges"]
    sessions = plot_data["sessions"]
    rows, cols = max(len(facing), 1), len(sessions)
    fig, axes = plt.subplots(rows, cols, figsize=(4.2 * cols, 3.2 * rows), squeeze=False)
    band = report["parameters"]["facing_edge_strip"]["inward_band_mm"]
    for ri, entry in enumerate(facing):
        half_gap = entry["gap_mm"] / 2
        for ci, session in enumerate(sessions):
            level = str(session.gray_u8)
            data = entry["levels"][level]
            ax = axes[ri, ci]
            for which, sign, color in (("a", -1.0, "#3366aa"), ("b", 1.0, "#cc6633")):
                points = [(p["inward_mm"], p["mean_all"]) for p in data[f"profile_{which}"] if p["mean_all"] is not None]
                if points:
                    d, v = zip(*points)
                    ax.plot([sign * (half_gap + x) for x in d], v, color=color,
                            label=f"Cam{entry['camera_' + which]} ({entry['arena_' + which]})")
                ax.axvspan(sign * (half_gap + band[0]), sign * (half_gap + band[1]), color=color, alpha=0.12)
            ax.axvspan(-half_gap, half_gap, color="0.85", alpha=0.6)
            ratio = data["ratio_a_over_b"]
            ratio_text = f"{ratio:.3f}" if ratio is not None else "n/a"
            status = "" if data["status"] == "ok" else " INVALID"
            ax.set_title(f"{entry['arena_a']}|{entry['arena_b']} gray {level}: a/b {ratio_text}{status}", fontsize=9)
            ax.set_xlabel("canvas mm from gap centre", fontsize=8)
            if ci == 0:
                ax.set_ylabel("u8 above black", fontsize=8)
            ax.tick_params(labelsize=7)
            ax.legend(fontsize=7, loc="lower center")
    fig.suptitle(
        f"Facing-edge profiles (shaded: strips {band[0]:g}-{band[1]:g} mm in from each arena edge; grey: gap)",
        fontsize=11,
    )
    fig.tight_layout()
    fig.savefig(path, format="png", dpi=80, bbox_inches="tight")
    plt.close(fig)


# --------------------------------------------------------------------------
# CLI


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--session", action="append", required=True,
                        help="calibration session id, unique id prefix or directory; once per gray level")
    parser.add_argument("--sessions-root", default=str(Path.home() / "orange_data/calibrations/sessions"))
    parser.add_argument("--citrus-config", default="/home/jeremy/citrus/targets/rigs/omnifin0/shadow/shadow.json",
                        help="Citrus canvas JSON; homography pointers are read from its calibration_artifacts/")
    parser.add_argument("--blocks", type=int, default=12)
    parser.add_argument("--gray", type=int, action="append",
                        help="gray level per --session (same order), used only if the session metadata has none")
    parser.add_argument("--exposure-us", type=float, action="append",
                        help="exposure per --session (same order), used only if the session metadata has none")
    parser.add_argument("--edge-inset-mm", type=float, default=3.0)
    parser.add_argument("--edge-width-mm", type=float, default=5.0)
    parser.add_argument("--edge-half-length-mm", type=float, default=15.0)
    parser.add_argument("--out", required=True, help="output directory (created; existing outputs are never overwritten)")
    args = parser.parse_args(argv)
    if args.blocks < 1:
        parser.error("--blocks must be >= 1")
    if args.edge_inset_mm < 0 or args.edge_width_mm <= 0 or args.edge_half_length_mm <= 0:
        parser.error("edge strip sizes must be positive")
    if args.edge_inset_mm + args.edge_width_mm > PROFILE_INSIDE_MM:
        parser.error(f"edge strip must end within {PROFILE_INSIDE_MM} mm of the edge")
    return args


def write_atomic_bytes_from(path: Path, writer) -> None:
    temporary = path.with_name("." + path.name + ".tmp")
    writer(temporary)
    temporary.replace(path)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    out = Path(args.out).expanduser()
    outputs = [out / OUTPUT_JSON, out / OUTPUT_RELATIVE_PNG, out / OUTPUT_EDGES_PNG]
    existing = [str(p) for p in outputs if p.exists()]
    if existing:
        print("refusing to overwrite: " + ", ".join(existing), file=sys.stderr)
        return EXIT_OUTPUT_EXISTS
    try:
        report, plot_data = build_report(args)
    except InputError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_INPUT
    out.mkdir(parents=True, exist_ok=True)
    existing = [str(p) for p in outputs if p.exists()]
    if existing:
        print("refusing to overwrite: " + ", ".join(existing), file=sys.stderr)
        return EXIT_OUTPUT_EXISTS
    report["outputs"] = {"relative_maps_png": OUTPUT_RELATIVE_PNG, "facing_edges_png": OUTPUT_EDGES_PNG}
    write_atomic_bytes_from(outputs[1], lambda p: plot_relative(p, report, plot_data))
    write_atomic_bytes_from(outputs[2], lambda p: plot_facing_edges(p, report, plot_data))
    write_atomic_bytes_from(outputs[0], lambda p: p.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8"))

    summary = report["summary"]
    print(f"wrote {outputs[0]}")
    print(f"levels {summary['levels_gray_u8']}; within-arena max/min {summary['within_arena_max_over_min_range']}; "
          f"shape max abs diff vs gray {summary['shape_reference_level']}: {summary['shape_max_abs_diff_vs_reference']}")
    print("camera median / mean of cameras: " + ", ".join(f"Cam{s} {v}" for s, v in summary["camera_median_relative_to_mean"].items()))
    for pair, data in summary["facing_edge"].items():
        print(f"facing edge {pair} ({data['cameras']}): " + ", ".join(f"gray {l} {r}" for l, r in data["ratio_by_level"].items()))
    for warning in summary["warnings"]:
        print(f"warning: {warning}")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
