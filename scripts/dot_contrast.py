#!/usr/bin/env python3
"""Stimulus-dot Weber contrast from an Orange static_dots calibration session.

    dot_contrast.py --session <calsess id|dir> --dark-session <calsess id|dir> --out <dir>

Inputs: a guided-capture session made with `--static-dots-scenes` (images with role
stimulus_dot_contrast, one per Citrus static_dots scene, each carrying Citrus's scene status with
the drawn dot centres and radii in final display canvas px) and a light-blocked dark frame session
(role dark_frame_projector_light_blocked) at the same exposure and gain.

Per camera the dot geometry is taken to the camera with the inverse of the active Citrus
homography (camera native px -> final display canvas px), the same pointer files the brightness
map uses. For every drawn dot:

  core     canvas distance from the centre <= --core-fraction x radius (default 0.6, away from the
           blurred edge),
  annulus  canvas distance in [--annulus-inner x radius, --annulus-outer x radius] (default
           1.3-1.8: local background, clear of the edge and of the neighbouring corner dots),

  I_core, I_annulus = mean(image - dark) over those camera pixels,
  weber_local = (I_core - I_annulus) / I_annulus                (what the procedure asks for)
  weber_vs_background_frame = (I_core - B_core) / B_core         (B = the background-only scene at
                                                                 the same pixels; removes the
                                                                 projector's own non-uniformity)

and the design contrast from the codes is given beside them. Camera photometry: the contrast is in
the camera's spectral response, not the fish's. Saturated core or annulus pixels are counted and a
dot with any is flagged.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import brightness_map as bm  # noqa: E402

SCHEMA_ID = "orange.commissioning.dot_contrast"
SCHEMA_VERSION = 1
DOT_ROLE = "stimulus_dot_contrast"
DARK_ROLE = "dark_frame_projector_light_blocked"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def camera_dirs(session: Path) -> dict[str, Path]:
    out: dict[str, Path] = {}
    for artifact in sorted((session / "artifacts").glob("Cam*_arena_*")):
        serial = artifact.name[3:].split("_", 1)[0]
        out[serial] = artifact
    return out


def images_with_role(artifact: Path, role: str) -> list[dict[str, Any]]:
    image_set = json.loads((artifact / "image_set.json").read_text(encoding="utf-8"))
    return [image for image in image_set.get("images", []) if image.get("role") == role]


def scene_label(image: dict[str, Any]) -> str:
    pattern_id = str((image.get("projected_pattern") or {}).get("pattern_id") or "")
    return pattern_id.split(":", 1)[1] if pattern_id.startswith("citrus_static_dots:") else pattern_id


def drawn_dots(image: dict[str, Any]) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    pre = image.get("citrus_calibration_scene_pre_capture") or {}
    post = image.get("citrus_calibration_scene_post_capture") or {}
    dots = ((pre.get("static_dots") or {}).get("dots")) or []
    background = (pre.get("static_dots") or {}).get("background")
    same = (pre.get("content_fingerprint") == post.get("content_fingerprint") and
            pre.get("scene_revision") == post.get("scene_revision"))
    return dots, {"background": background, "same_scene_pre_post": same,
                  "presented": bool(pre.get("presented"))}


def gray_of(color: Any) -> int | None:
    if isinstance(color, dict):
        if "gray_u8" in color:
            return int(color["gray_u8"])
        rgb = color.get("rgb_u8")
        if isinstance(rgb, list) and len(rgb) == 3 and len(set(rgb)) == 1:
            return int(rgb[0])
    return None


def region_masks(matrix: np.ndarray, shape: tuple[int, int], center: tuple[float, float], radius: float,
                 core_fraction: float, annulus: tuple[float, float]) -> tuple[np.ndarray, np.ndarray, tuple[slice, slice]]:
    """Camera-pixel masks for the core and annulus of one canvas-space dot."""
    inverse = np.linalg.inv(matrix)
    outer = radius * annulus[1]
    ring_x = center[0] + outer * np.cos(np.linspace(0, 2 * np.pi, 64))
    ring_y = center[1] + outer * np.sin(np.linspace(0, 2 * np.pi, 64))
    cam_x, cam_y = bm.map_points(inverse, ring_x, ring_y)
    x0, x1 = int(max(0, np.floor(cam_x.min()) - 2)), int(min(shape[1], np.ceil(cam_x.max()) + 3))
    y0, y1 = int(max(0, np.floor(cam_y.min()) - 2)), int(min(shape[0], np.ceil(cam_y.max()) + 3))
    if x1 <= x0 or y1 <= y0:
        return np.zeros((0, 0), bool), np.zeros((0, 0), bool), (slice(0, 0), slice(0, 0))
    rows, cols = np.mgrid[y0:y1, x0:x1]
    canvas_x, canvas_y = bm.map_points(matrix, cols.astype(np.float64), rows.astype(np.float64))
    distance = np.hypot(canvas_x - center[0], canvas_y - center[1])
    core = distance <= radius * core_fraction
    ring = (distance >= radius * annulus[0]) & (distance <= radius * annulus[1])
    return core, ring, (slice(y0, y1), slice(x0, x1))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--session", required=True)
    parser.add_argument("--dark-session", required=True)
    parser.add_argument("--sessions-root", type=Path, default=Path.home() / "orange_data" / "calibrations" / "sessions")
    parser.add_argument("--citrus-config", type=Path,
                        default=Path("/home/jeremy/citrus/targets/rigs/omnifin0/shadow/shadow.json"))
    parser.add_argument("--core-fraction", type=float, default=0.6)
    parser.add_argument("--annulus-inner", type=float, default=1.3)
    parser.add_argument("--annulus-outer", type=float, default=1.8)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)

    session = bm.resolve_session_dir(args.session, args.sessions_root)
    dark_session = bm.resolve_session_dir(args.dark_session, args.sessions_root)
    canvas_sha = sha256_file(args.citrus_config).split(":", 1)[1]
    calibration_dir = args.citrus_config.parent / "calibration_artifacts"
    layout = bm.load_layout(args.citrus_config)
    dark_dirs = camera_dirs(dark_session)
    warnings: list[str] = []
    cameras: dict[str, Any] = {}
    rows_out: list[dict[str, Any]] = []

    for serial, artifact in camera_dirs(session).items():
        arena_id = artifact.name.split("_", 1)[1]
        if arena_id not in layout:
            warnings.append(f"Cam{serial}: {arena_id} not in the Citrus canvas")
            continue
        homography = bm.load_homography(calibration_dir, arena_id, serial, canvas_sha)
        dot_images = images_with_role(artifact, DOT_ROLE)
        dark_images = images_with_role(dark_dirs.get(serial, Path("/nonexistent")), DARK_ROLE) \
            if serial in dark_dirs else []
        if not dot_images or not dark_images:
            warnings.append(f"Cam{serial}: missing dot images or dark frame")
            continue
        dark_entry = dark_images[-1]
        exposure = float((dot_images[0].get("capture") or {}).get("exposure_us") or 0)
        gain = (dot_images[0].get("capture") or {}).get("gain")
        dark_exposure = float((dark_entry.get("capture") or {}).get("exposure_us") or 0)
        dark_gain = (dark_entry.get("capture") or {}).get("gain")
        if abs(dark_exposure - exposure) > 1 or dark_gain != gain:
            warnings.append(f"Cam{serial}: dark frame exposure/gain {dark_exposure}/{dark_gain} != {exposure}/{gain}")
        dark = bm.load_u8(dark_dirs[serial] / dark_entry["path"]).astype(np.float64)

        background_image = None
        scenes: list[tuple[dict[str, Any], np.ndarray, list[dict[str, Any]], dict[str, Any]]] = []
        for image in dot_images:
            pixels = bm.load_u8(artifact / image["path"])
            dots, info = drawn_dots(image)
            if not info["presented"] or not info["same_scene_pre_post"]:
                warnings.append(f"Cam{serial} {scene_label(image)}: scene not presented or changed during capture")
            if not dots:
                background_image = pixels
            scenes.append((image, pixels, dots, info))

        camera_rows = []
        for image, pixels, dots, info in scenes:
            signal = pixels.astype(np.float64) - dark
            background_gray = gray_of(info["background"])
            for dot in dots:
                if dot.get("arena_id") != arena_id:
                    continue
                center = tuple(float(v) for v in dot["center_canvas_px"])
                radius = float(dot["radius_px"])
                core, ring, window = region_masks(homography.matrix, pixels.shape, center, radius,
                                                  args.core_fraction, (args.annulus_inner, args.annulus_outer))
                if core.sum() == 0 or ring.sum() == 0:
                    warnings.append(f"Cam{serial} {dot['dot_id']}: dot outside the camera image")
                    continue
                win_signal, win_raw = signal[window], pixels[window]
                i_core, i_ring = float(win_signal[core].mean()), float(win_signal[ring].mean())
                saturated = int((win_raw[core] >= 255).sum() + (win_raw[ring] >= 255).sum())
                row = {
                    "camera_id": serial, "arena_id": arena_id, "scene": scene_label(image),
                    "dot_id": dot["dot_id"], "position_preset": dot.get("position_preset"),
                    "dot_gray_u8": gray_of(dot.get("color")), "background_gray_u8": background_gray,
                    "center_canvas_px": list(center), "radius_canvas_px": radius,
                    "core_camera_px": int(core.sum()), "annulus_camera_px": int(ring.sum()),
                    "i_core_above_dark": round(i_core, 4), "i_annulus_above_dark": round(i_ring, 4),
                    "core_std": round(float(win_signal[core].std()), 4),
                    "weber_local": round((i_core - i_ring) / i_ring, 5) if i_ring > 0 else None,
                    "saturated_pixels": saturated,
                }
                if background_image is not None:
                    b_core = float((background_image[window].astype(np.float64) - dark[window])[core].mean())
                    row["background_frame_core_above_dark"] = round(b_core, 4)
                    row["weber_vs_background_frame"] = round((i_core - b_core) / b_core, 5) if b_core > 0 else None
                if saturated:
                    warnings.append(f"Cam{serial} {scene_label(image)} {dot['dot_id']}: {saturated} saturated pixels")
                camera_rows.append(row)
        rows_out.extend(camera_rows)
        cameras[serial] = {
            "arena_id": arena_id, "exposure_us": exposure, "gain": gain,
            "dark_frame": {"session_dir": str(dark_dirs[serial].parent.parent), "path": dark_entry["path"],
                           "exposure_us": dark_exposure, "gain": dark_gain},
            "homography": homography.provenance,
        }

    # Summary: per arena and dot code, mean over the corners (Latin square) of each contrast.
    summary: dict[str, Any] = {}
    for arena_id in sorted({r["arena_id"] for r in rows_out}):
        per_code: dict[str, Any] = {}
        for code in sorted({r["dot_gray_u8"] for r in rows_out if r["arena_id"] == arena_id}, key=lambda c: (c is None, c)):
            group = [r for r in rows_out if r["arena_id"] == arena_id and r["dot_gray_u8"] == code]
            local = [r["weber_local"] for r in group if r["weber_local"] is not None]
            vs_bg = [r.get("weber_vs_background_frame") for r in group if r.get("weber_vs_background_frame") is not None]
            bg = group[0]["background_gray_u8"] if group else None
            per_code[str(code)] = {
                "n": len(group),
                "weber_local_mean": round(float(np.mean(local)), 4) if local else None,
                "weber_local_min": round(float(np.min(local)), 4) if local else None,
                "weber_local_max": round(float(np.max(local)), 4) if local else None,
                "weber_vs_background_frame_mean": round(float(np.mean(vs_bg)), 4) if vs_bg else None,
                "design_code_ratio_minus_1": round(code / bg - 1, 4) if (code is not None and bg) else None,
            }
        summary[arena_id] = per_code

    args.out.mkdir(parents=True, exist_ok=True)
    report = {
        "schema_id": SCHEMA_ID, "schema_version": SCHEMA_VERSION,
        "created_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "producer": {"tool": "scripts/dot_contrast.py"},
        "inputs": {"session_dir": str(session), "dark_session_dir": str(dark_session),
                   "citrus_config": str(args.citrus_config), "citrus_config_sha256": "sha256:" + canvas_sha},
        "parameters": {"core_fraction": args.core_fraction,
                       "annulus_fraction": [args.annulus_inner, args.annulus_outer],
                       "intensity": "image - light-blocked dark frame, camera DN (u8 average of the capture)",
                       "spectral_note": "camera spectral response, not the fish's"},
        "cameras": cameras, "dots": rows_out, "summary": summary, "warnings": warnings,
    }
    (args.out / "dot_contrast.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {args.out / 'dot_contrast.json'} ({len(rows_out)} dot measurements)")
    for arena_id, per_code in summary.items():
        print(arena_id + ": " + "; ".join(
            f"code {code}: local {v['weber_local_mean']} [{v['weber_local_min']}, {v['weber_local_max']}]"
            f" vs bg-frame {v['weber_vs_background_frame_mean']} (design {v['design_code_ratio_minus_1']})"
            for code, v in per_code.items()))
    for warning in warnings:
        print("warning: " + warning)
    return 0


if __name__ == "__main__":
    sys.exit(main())
