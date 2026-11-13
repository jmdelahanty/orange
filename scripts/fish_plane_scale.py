#!/usr/bin/env python3
"""Fish-plane camera scale from two physical-target fits per camera.

Inputs are fit reports written by physical_target_fit_cli: one with the 3 mm target on the
gel (target bottom at z = 0) and one with it on the dish floor (target bottom at z = P_floor).
Pinhole model (docs/omnifin0_calibration_procedure.md section 5): 1/s is linear in height, so

    k      = P_floor / (1/s_gel - 1/s_floor)
    1/s(z) = 1/s_floor - (z - P_floor - e) / k
    s_fish = s(P_fish + w * (1 - 1/n))

with e the effective depth inside the disk that the hole centres represent (0..3 mm, the same
in both captures, so k does not depend on it), P_fish the fish height (default P_floor + 1.5)
and w the water over the fish (apparent-height shift, n = 1.333).

Uncertainty:
  * scale sigma per fit: the larger of the fit's statistical scale error
    (fit RMS / sqrt(2) / sqrt(sum r^2) over the fit points) and the spread between the two fit
    definitions in the report (Citrus refit vs. Orange analysis); --scale-sigma-px-per-mm
    overrides it;
  * k: statistical part from the two scale sigmas, common-mode part from P_floor +/- unc;
  * s_fish: envelope over e in [0, 3] and P_floor +/- unc, plus the statistical part.

Acceptance: both fits quality_pass, s_floor > s_gel, and every k within --k-agreement-sigmas
of the weighted mean using the per-camera statistical sigma (P_floor is common to all cameras,
so it cannot make them disagree). Needs at least two cameras.

Exit: 0 accepted, 2 input error, 3 acceptance failed or incomplete.
"""

import argparse
import hashlib
import json
import math
import os
import sys
from datetime import datetime, timezone

SCHEMA_ID = "orange.calibration.fish_plane_scale"
DISK_THICKNESS_MM = 3.0


def sha256_token(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return "sha256:" + digest.hexdigest()


def load_report(path):
    with open(path, "r", encoding="utf-8") as handle:
        report = json.load(handle)
    if not isinstance(report, dict) or report.get("schema_id") != (
        "orange.calibration.physical_target_fit_report"
    ):
        raise ValueError(f"{path}: not a physical_target_fit_cli fit_report.json")
    return report


def report_scale(report, field):
    value = report.get(field)
    if not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
        raise ValueError(f"report has no usable {field}")
    return float(value)


def statistical_scale_sigma(report, scale):
    """Fit RMS (2-D) -> per-axis sigma, over the radial lever arm of the fit points."""
    refit = report.get("citrus_equivalent_refit") or {}
    fit_rms = refit.get("fit_rms_mm", report.get("fit_rms_mm"))
    points = [
        point["target_mm"]
        for point in (report.get("analysis_report") or {}).get("correspondences", [])
        if point.get("used_for_fit")
    ]
    if not isinstance(fit_rms, (int, float)) or len(points) < 4:
        return None
    cx = sum(p[0] for p in points) / len(points)
    cy = sum(p[1] for p in points) / len(points)
    lever = math.sqrt(sum((p[0] - cx) ** 2 + (p[1] - cy) ** 2 for p in points))
    if lever <= 0:
        return None
    return scale * (fit_rms / math.sqrt(2.0)) / lever


def scale_sigma(report, scale, override):
    if override is not None:
        return override, {"method": "command_line", "value": override}
    statistical = statistical_scale_sigma(report, scale)
    a = report.get("camera_px_per_mm")
    b = report.get("camera_px_per_mm_orange_analysis")
    definition_spread = (
        abs(a - b) if isinstance(a, (int, float)) and isinstance(b, (int, float)) else None
    )
    candidates = [v for v in (statistical, definition_spread) if v is not None]
    if not candidates:
        raise ValueError("cannot estimate a scale sigma; pass --scale-sigma-px-per-mm")
    return max(candidates), {
        "method": "max(statistical_fit_scale_error, citrus_refit_vs_orange_analysis_spread)",
        "statistical_fit_scale_error_px_per_mm": statistical,
        "definition_spread_px_per_mm": definition_spread,
    }


def k_from(s_gel, s_floor, p_floor):
    difference = 1.0 / s_gel - 1.0 / s_floor
    if difference <= 0:
        return None
    return p_floor / difference


def s_at(z, s_floor, k, p_floor, e):
    inverse = 1.0 / s_floor - (z - p_floor - e) / k
    return 1.0 / inverse if inverse > 0 else float("nan")


def parse_pair(text):
    parts = text.split(",")
    if len(parts) != 3 or not all(parts):
        raise argparse.ArgumentTypeError("--pair wants SERIAL,GEL_REPORT,FLOOR_REPORT")
    return parts[0], parts[1], parts[2]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--pair", action="append", type=parse_pair, required=True,
        metavar="SERIAL,GEL_REPORT,FLOOR_REPORT",
        help="one per camera: serial, gel fit_report.json, dish-floor fit_report.json",
    )
    parser.add_argument("--p-floor-mm", type=float, required=True,
                        help="dish floor top above the gel top (target bottom on the floor)")
    parser.add_argument("--p-floor-unc-mm", type=float, required=True,
                        help="uncertainty of P_floor (+/-)")
    parser.add_argument("--p-floor-source", required=True,
                        help="where P_floor comes from (recorded verbatim)")
    parser.add_argument("--e-mm", type=float, default=1.5,
                        help="effective depth inside the 3 mm disk (default 1.5)")
    parser.add_argument("--p-fish-mm", type=float, default=None,
                        help="fish height above the gel (default P_floor + 1.5)")
    parser.add_argument("--water-over-fish-mm", type=float, default=0.0,
                        help="water above the fish; raises its apparent height by w(1-1/n)")
    parser.add_argument("--refractive-index", type=float, default=1.333)
    parser.add_argument("--scale-field", default="camera_px_per_mm",
                        choices=["camera_px_per_mm", "camera_px_per_mm_orange_analysis"],
                        help="which scale in the fit reports (default: the Citrus refit value)")
    parser.add_argument("--scale-sigma-px-per-mm", type=float, default=None,
                        help="override the per-fit scale sigma")
    parser.add_argument("--k-agreement-sigmas", type=float, default=2.0)
    parser.add_argument("--out", default=None, help="write the JSON here (never overwrites)")
    args = parser.parse_args(argv)

    if args.p_floor_mm <= 0 or args.p_floor_unc_mm < 0:
        parser.error("P_floor must be > 0 and its uncertainty >= 0")
    if not 0.0 <= args.e_mm <= DISK_THICKNESS_MM:
        parser.error("--e-mm must lie in [0, 3]")
    if args.refractive_index < 1.0 or args.water_over_fish_mm < 0:
        parser.error("refractive index must be >= 1 and water depth >= 0")
    if args.out and os.path.exists(args.out):
        print(f"refusing to overwrite {args.out}", file=sys.stderr)
        return 2
    serials = [pair[0] for pair in args.pair]
    if len(set(serials)) != len(serials):
        parser.error("a camera serial appears twice")

    p_floor = args.p_floor_mm
    p_unc = args.p_floor_unc_mm
    p_fish_default = args.p_fish_mm is None
    water_shift = args.water_over_fish_mm * (1.0 - 1.0 / args.refractive_index)

    def p_fish_for(floor):
        return (floor + DISK_THICKNESS_MM / 2.0) if p_fish_default else args.p_fish_mm

    cameras = {}
    try:
        for serial, gel_path, floor_path in args.pair:
            gel = load_report(gel_path)
            floor = load_report(floor_path)
            notes = []
            for name, report in (("gel", gel), ("floor", floor)):
                reported = report.get("camera_id")
                if reported not in (None, serial):
                    raise ValueError(f"{serial}: {name} report is for camera {reported}")
            gel_target = (gel.get("target") or {}).get("target_id")
            floor_target = (floor.get("target") or {}).get("target_id")
            if gel_target != floor_target:
                notes.append(f"target ids differ: gel {gel_target!r}, floor {floor_target!r}")
            s_gel = report_scale(gel, args.scale_field)
            s_floor = report_scale(floor, args.scale_field)
            sigma_gel, sigma_gel_basis = scale_sigma(gel, s_gel, args.scale_sigma_px_per_mm)
            sigma_floor, sigma_floor_basis = scale_sigma(
                floor, s_floor, args.scale_sigma_px_per_mm)

            k = k_from(s_gel, s_floor, p_floor)
            entry = {
                "inputs": {
                    "gel_report": {"path": os.path.abspath(gel_path),
                                   "sha256": sha256_token(gel_path),
                                   "target_placement": gel.get("target_placement")},
                    "floor_report": {"path": os.path.abspath(floor_path),
                                     "sha256": sha256_token(floor_path),
                                     "target_placement": floor.get("target_placement")},
                },
                "s_gel_px_per_mm": s_gel,
                "s_floor_px_per_mm": s_floor,
                "s_gel_sigma_px_per_mm": sigma_gel,
                "s_floor_sigma_px_per_mm": sigma_floor,
                "s_sigma_basis": {"gel": sigma_gel_basis, "floor": sigma_floor_basis},
                "notes": notes,
            }
            checks = {
                "gel_fit_quality_pass": gel.get("quality_pass") is True,
                "floor_fit_quality_pass": floor.get("quality_pass") is True,
                "s_floor_greater_than_s_gel": s_floor > s_gel,
            }
            if k is None:
                entry.update({"k_px": None, "s_fish_px_per_mm": None})
                entry["checks"] = checks
                cameras[serial] = entry
                continue

            difference = 1.0 / s_gel - 1.0 / s_floor
            sigma_difference = math.hypot(sigma_gel / s_gel ** 2, sigma_floor / s_floor ** 2)
            k_sigma_stat = k * sigma_difference / difference
            k_sigma_p_floor = k * p_unc / p_floor
            p_fish = p_fish_for(p_floor)
            z_eff = p_fish + water_shift
            s_fish = s_at(z_eff, s_floor, k, p_floor, args.e_mm)

            # Envelope over e in [0, 3] and P_floor +/- unc (k and a defaulted P_fish follow).
            envelope = []
            for floor_value in (p_floor - p_unc, p_floor, p_floor + p_unc):
                k_value = k_from(s_gel, s_floor, floor_value)
                for e_value in (0.0, args.e_mm, DISK_THICKNESS_MM):
                    z_value = p_fish_for(floor_value) + water_shift
                    envelope.append(s_at(z_value, s_floor, k_value, floor_value, e_value))
            e_only = [s_at(z_eff, s_floor, k, p_floor, e) for e in (0.0, DISK_THICKNESS_MM)]

            # Statistical part: numerical propagation of the two scale sigmas.
            def s_fish_with(gel_value, floor_value):
                k_value = k_from(gel_value, floor_value, p_floor)
                if k_value is None:
                    return float("nan")
                return s_at(z_eff, floor_value, k_value, p_floor, args.e_mm)

            d_gel = (s_fish_with(s_gel + sigma_gel, s_floor) - s_fish) if sigma_gel else 0.0
            d_floor = (s_fish_with(s_gel, s_floor + sigma_floor) - s_fish) if sigma_floor else 0.0
            s_fish_sigma_stat = math.hypot(d_gel, d_floor)

            entry.update({
                "k_px": k,
                "k_sigma_statistical_px": k_sigma_stat,
                "k_sigma_p_floor_px": k_sigma_p_floor,
                "k_sigma_total_px": math.hypot(k_sigma_stat, k_sigma_p_floor),
                "implied_camera_height_above_gel_mm": k / s_gel + args.e_mm,
                "p_fish_mm": p_fish,
                "apparent_height_shift_mm": water_shift,
                "z_effective_mm": z_eff,
                "e_mm": args.e_mm,
                "s_fish_px_per_mm": s_fish,
                "s_fish_minus_s_floor_fraction": s_fish / s_floor - 1.0,
                "s_fish_range_e_0_to_3_px_per_mm": [min(e_only), max(e_only)],
                "s_fish_range_e_and_p_floor_px_per_mm": [min(envelope), max(envelope)],
                "s_fish_sigma_statistical_px_per_mm": s_fish_sigma_stat,
                "s_fish_half_range_plus_statistical_px_per_mm": math.hypot(
                    0.5 * (max(envelope) - min(envelope)), s_fish_sigma_stat),
            })
            entry["checks"] = checks
            cameras[serial] = entry
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    # k agreement across cameras (statistical sigma only; P_floor is common-mode).
    with_k = {s: c for s, c in cameras.items() if c.get("k_px") is not None}
    agreement = {"evaluated": False, "n_sigmas": args.k_agreement_sigmas}
    if len(with_k) >= 2:
        weights = {s: 1.0 / c["k_sigma_statistical_px"] ** 2 for s, c in with_k.items()}
        mean = sum(weights[s] * c["k_px"] for s, c in with_k.items()) / sum(weights.values())
        chi2 = 0.0
        per_camera = {}
        for serial, camera in with_k.items():
            pull = (camera["k_px"] - mean) / camera["k_sigma_statistical_px"]
            chi2 += pull ** 2
            per_camera[serial] = {"k_px": camera["k_px"], "pull_sigmas": pull,
                                  "within": abs(pull) <= args.k_agreement_sigmas}
            camera["checks"]["k_agrees_with_other_cameras"] = (
                abs(pull) <= args.k_agreement_sigmas)
        agreement.update({
            "evaluated": True,
            "weighted_mean_k_px": mean,
            "chi2": chi2,
            "degrees_of_freedom": len(with_k) - 1,
            "per_camera": per_camera,
            "all_within": all(item["within"] for item in per_camera.values()),
        })

    camera_pass = all(all(c["checks"].values()) and c.get("k_px") is not None
                      for c in cameras.values())
    accepted = camera_pass and agreement.get("all_within", False)
    if not agreement["evaluated"]:
        status = "incomplete_k_agreement_needs_two_cameras"
    else:
        status = "accepted" if accepted else "failed_acceptance"

    result = {
        "schema_id": SCHEMA_ID,
        "schema_version": 1,
        "created_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "status": status,
        "accepted": accepted,
        "model": {
            "reference": "docs/omnifin0_calibration_procedure.md section 5",
            "k": "P_floor / (1/s_gel - 1/s_floor)",
            "s_of_z": "1/s(z) = 1/s_floor - (z - P_floor - e)/k",
            "apparent_height_shift": "w * (1 - 1/n)",
            "scale_field": args.scale_field,
        },
        "parameters": {
            "p_floor_mm": p_floor,
            "p_floor_unc_mm": p_unc,
            "p_floor_source": args.p_floor_source,
            "e_mm": args.e_mm,
            "e_range_mm": [0.0, DISK_THICKNESS_MM],
            "p_fish_mm": p_fish_for(p_floor),
            "p_fish_defaulted_to_p_floor_plus_1_5": p_fish_default,
            "water_over_fish_mm": args.water_over_fish_mm,
            "refractive_index": args.refractive_index,
            "k_agreement_sigmas": args.k_agreement_sigmas,
        },
        "cameras": cameras,
        "k_agreement": agreement,
    }
    text = json.dumps(result, indent=2) + "\n"
    if args.out:
        try:
            with open(args.out, "x", encoding="utf-8") as handle:
                handle.write(text)
        except FileExistsError:
            print(f"refusing to overwrite {args.out}", file=sys.stderr)
            return 2
    sys.stdout.write(text)
    return 0 if accepted else 3


if __name__ == "__main__":
    sys.exit(main())
