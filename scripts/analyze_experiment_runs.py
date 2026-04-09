#!/usr/bin/env python3
import argparse
import copy
import json
import shlex
import sys
from collections import Counter
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Analyze a local experiment folder produced by orange_client "
            "--mode local --experiment-spec, and optionally emit exact rerun specs."
        )
    )
    parser.add_argument(
        "experiment_root",
        help="Path to the experiment root folder containing runs.json and summary.json.",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=10,
        help="Number of passing runs to show in the summary table (default: %(default)s).",
    )
    parser.add_argument(
        "--no-matrix",
        action="store_true",
        help="Do not print the codec/preset/tuning result matrix.",
    )
    parser.add_argument(
        "--filter",
        action="append",
        default=[],
        metavar="FIELD=VALUE",
        help=(
            "Filter runs by config/result field. Repeatable. "
            "Examples: codec=hevc, preset=p3, pass_fail=fail, status=failed."
        ),
    )
    parser.add_argument(
        "--rerun-mode",
        choices=["none", "failed", "nonpass", "all"],
        default="none",
        help=(
            "Emit exact rerun specs for matching runs. "
            "'failed' selects failed runs, 'nonpass' selects non-pass runs."
        ),
    )
    parser.add_argument(
        "--rerun-dir",
        default="",
        help=(
            "Directory for generated rerun specs. "
            "Default: <experiment_root>/rerun_specs when --rerun-mode is enabled."
        ),
    )
    parser.add_argument(
        "--rerun-limit",
        type=int,
        default=0,
        help="Maximum number of rerun specs to emit (0 means no limit).",
    )
    parser.add_argument(
        "--rerun-output-root",
        default="",
        help=(
            "Override fixed.output_root in generated rerun specs. "
            "Default: reuse the original experiment spec output_root."
        ),
    )
    return parser.parse_args()


def read_json(path: Path):
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def normalize(value):
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def parse_filters(filter_args):
    parsed = []
    for item in filter_args:
        if "=" not in item:
            raise ValueError(f"Invalid --filter value {item!r}; expected FIELD=VALUE.")
        field, value = item.split("=", 1)
        field = field.strip()
        value = value.strip()
        if not field:
            raise ValueError(f"Invalid --filter value {item!r}; field is empty.")
        parsed.append((field, value))
    return parsed


def flatten_run(run):
    rows = []
    camera_results = run.get("camera_results") or []
    if not camera_results:
        rows.append({
            "run": run,
            "camera_result": {},
            "camera_serial": "",
            "codec": run.get("config", {}).get("codec", ""),
            "preset": run.get("config", {}).get("preset", ""),
            "tuning": run.get("config", {}).get("tuning", ""),
            "rate_control_mode": run.get("config", {}).get("rate_control_mode", ""),
            "quality_value": run.get("config", {}).get("quality_value", ""),
            "gop_length": run.get("config", {}).get("gop_length", ""),
            "gpu_id": "",
            "gpu_name": "",
            "enc_fps_mean": 0.0,
            "enc_fps_p95": 0.0,
            "acq_free_entries_min": -1,
            "acq_free_events_min": -1,
            "yolo_events_min": -1,
            "pre_buffers_min": -1,
            "pre_events_min": -1,
            "pre_waits_final": 0,
            "pre_drops_final": 0,
            "enc_fail_final": 0,
            "enc_slow_final": 0,
            "dropped_frames_camera": -1,
            "pass_fail": run.get("pass_fail", ""),
            "status": run.get("status", ""),
            "reason": run.get("reason", ""),
        })
        return rows

    for camera_result in camera_results:
        rows.append({
            "run": run,
            "camera_result": camera_result,
            "camera_serial": camera_result.get("camera_serial", ""),
            "codec": camera_result.get("codec", run.get("config", {}).get("codec", "")),
            "preset": camera_result.get("preset", run.get("config", {}).get("preset", "")),
            "tuning": camera_result.get("tuning", run.get("config", {}).get("tuning", "")),
            "rate_control_mode": camera_result.get(
                "rate_control_mode", run.get("config", {}).get("rate_control_mode", "")
            ),
            "quality_value": camera_result.get(
                "quality_value", run.get("config", {}).get("quality_value", "")
            ),
            "gop_length": camera_result.get(
                "gop_length", run.get("config", {}).get("gop_length", "")
            ),
            "gpu_id": camera_result.get("gpu_id", ""),
            "gpu_name": camera_result.get("gpu_name", ""),
            "enc_fps_mean": camera_result.get("enc_fps_mean", 0.0),
            "enc_fps_p95": camera_result.get("enc_fps_p95", 0.0),
            "acq_free_entries_min": camera_result.get("acq_free_entries_min", -1),
            "acq_free_events_min": camera_result.get("acq_free_events_min", -1),
            "yolo_events_min": camera_result.get("yolo_events_min", -1),
            "pre_buffers_min": camera_result.get("pre_buffers_min", -1),
            "pre_events_min": camera_result.get("pre_events_min", -1),
            "pre_waits_final": camera_result.get("pre_waits_final", 0),
            "pre_drops_final": camera_result.get("pre_drops_final", 0),
            "enc_fail_final": camera_result.get("enc_fail_final", 0),
            "enc_slow_final": camera_result.get("enc_slow_final", 0),
            "dropped_frames_camera": camera_result.get("dropped_frames_camera", -1),
            "pass_fail": camera_result.get("pass_fail", run.get("pass_fail", "")),
            "status": camera_result.get("status", run.get("status", "")),
            "reason": camera_result.get("reason", run.get("reason", "")),
        })
    return rows


def row_field_candidates(row, field):
    run = row["run"]
    camera_result = row["camera_result"]
    config = run.get("config", {})
    candidates = []
    if field in row:
        candidates.append(row[field])
    if field in camera_result:
        candidates.append(camera_result[field])
    if field in run:
        candidates.append(run[field])
    if field in config:
        candidates.append(config[field])
    return candidates


def row_matches_filters(row, filters):
    for field, expected in filters:
        candidates = row_field_candidates(row, field)
        if not candidates:
            return False
        matched = False
        expected_norm = expected.lower()
        for candidate in candidates:
            if isinstance(candidate, list):
                values = [normalize(v).lower() for v in candidate]
                if expected_norm in values:
                    matched = True
                    break
            else:
                if normalize(candidate).lower() == expected_norm:
                    matched = True
                    break
        if not matched:
            return False
    return True


def first_reason(run):
    if run.get("reason"):
        return run["reason"]
    for camera_result in run.get("camera_results") or []:
        if camera_result.get("reason"):
            return camera_result["reason"]
    return ""


def first_pass_fail(run):
    if run.get("pass_fail"):
        return run["pass_fail"]
    for camera_result in run.get("camera_results") or []:
        if camera_result.get("pass_fail"):
            return camera_result["pass_fail"]
    return ""


def first_status(run):
    if run.get("status"):
        return run["status"]
    for camera_result in run.get("camera_results") or []:
        if camera_result.get("status"):
            return camera_result["status"]
    return ""


def collect_filtered_rows(runs_json, filters):
    rows = []
    for run in runs_json.get("runs", []):
        for row in flatten_run(run):
            if row_matches_filters(row, filters):
                rows.append(row)
    return rows


def format_table(headers, rows):
    if not rows:
        return ""
    widths = [len(header) for header in headers]
    for row in rows:
        for index, cell in enumerate(row):
            widths[index] = max(widths[index], len(cell))
    lines = []
    header_line = "  ".join(header.ljust(widths[i]) for i, header in enumerate(headers))
    lines.append(header_line)
    lines.append("  ".join("-" * widths[i] for i in range(len(headers))))
    for row in rows:
        lines.append("  ".join(cell.ljust(widths[i]) for i, cell in enumerate(row)))
    return "\n".join(lines)


def print_analysis(experiment_root: Path, summary_json, runs_json, rows, top_n):
    print(f"Experiment: {summary_json.get('experiment_id', experiment_root.name)}")
    print(f"Root: {experiment_root}")
    print(
        "Runs: "
        f"total={summary_json.get('total_runs', 0)} "
        f"completed={summary_json.get('completed_runs', 0)} "
        f"pass={summary_json.get('pass_runs', 0)} "
        f"marginal={summary_json.get('marginal_runs', 0)} "
        f"fail={summary_json.get('fail_runs', 0)}"
    )

    reason_counts = Counter()
    pass_fail_counts = Counter()
    for run in runs_json.get("runs", []):
        pass_fail_counts[first_pass_fail(run) or "unknown"] += 1
        reason_counts[first_reason(run) or ""] += 1

    print("Pass/Fail counts:")
    for key in ("pass", "marginal", "fail", "unknown"):
        if pass_fail_counts.get(key):
            print(f"  {key}: {pass_fail_counts[key]}")

    print("Top reasons:")
    for reason, count in reason_counts.most_common():
        if not reason:
            continue
        print(f"  {count:>2}  {reason}")


def compact_reason(reason):
    if not reason:
        return "fail"
    lowered = reason.lower()
    if "missing pipeline perf artifact" in lowered:
        return "no-art"
    if "nonzero preprocess drops" in lowered:
        return "pre-drop"
    if "encode fps below target tolerance" in lowered:
        return "fps-low"
    if "nonzero encode failures" in lowered:
        return "enc-fail"
    if "nonzero acquisition starvation" in lowered:
        return "acq-starve"
    if "meets current policy" in lowered:
        return "pass"
    return reason[:12]


def build_matrix_rows(rows):
    codecs = []
    presets = []
    tunings = []
    cells = {}

    for row in rows:
        codec = row.get("codec", "")
        preset = row.get("preset", "")
        tuning = row.get("tuning", "")
        if codec not in codecs:
            codecs.append(codec)
        if preset not in presets:
            presets.append(preset)
        if tuning not in tunings:
            tunings.append(tuning)

        pass_fail = str(row.get("pass_fail", "")).lower()
        if pass_fail == "pass":
            text = f"PASS {float(row.get('enc_fps_mean', 0.0)):.1f}"
        else:
            text = f"FAIL {compact_reason(row.get('reason', ''))}"
        cells[(codec, preset, tuning)] = text

    return codecs, presets, tunings, cells


def print_matrix(rows):
    codecs, presets, tunings, cells = build_matrix_rows(rows)
    if not codecs or not presets or not tunings:
        return

    print("")
    print("Result matrix:")
    for codec in codecs:
        print(f"  codec={codec}")
        headers = ["preset"] + tunings
        table_rows = []
        for preset in presets:
            row = [preset]
            for tuning in tunings:
                row.append(cells.get((codec, preset, tuning), ""))
            table_rows.append(row)
        print(format_table(headers, table_rows))
        print("")


def print_top_tables(rows, top_n):
    
    passing_rows = [
        row for row in rows
        if str(row.get("pass_fail", "")).lower() == "pass"
    ]
    passing_rows.sort(key=lambda row: float(row.get("enc_fps_mean", 0.0)), reverse=True)
    if passing_rows:
        print("")
        print(f"Top {min(top_n, len(passing_rows))} passing runs:")
        table_rows = []
        for row in passing_rows[:top_n]:
            table_rows.append([
                row["run"].get("run_id", ""),
                row.get("codec", ""),
                row.get("preset", ""),
                row.get("tuning", ""),
                row.get("rate_control_mode", ""),
                f"{float(row.get('enc_fps_mean', 0.0)):.3f}",
                f"{float(row.get('enc_fps_p95', 0.0)):.3f}",
                str(row.get("pre_buffers_min", -1)),
                str(row.get("pre_events_min", -1)),
                row.get("gpu_name", ""),
            ])
        print(format_table(
            ["run_id", "codec", "preset", "tuning", "rc", "enc_fps_mean", "enc_fps_p95", "pre_buf_min", "pre_evt_min", "gpu"],
            table_rows,
        ))

    failing_rows = [
        row for row in rows
        if str(row.get("pass_fail", "")).lower() != "pass"
    ]
    if failing_rows:
        print("")
        print(f"First {min(top_n, len(failing_rows))} non-pass runs:")
        table_rows = []
        for row in failing_rows[:top_n]:
            table_rows.append([
                row["run"].get("run_id", ""),
                row.get("codec", ""),
                row.get("preset", ""),
                row.get("tuning", ""),
                row.get("status", ""),
                row.get("pass_fail", ""),
                f"{float(row.get('enc_fps_mean', 0.0)):.1f}",
                str(row.get("acq_free_entries_min", -1)),
                str(row.get("acq_free_events_min", -1)),
                str(row.get("pre_buffers_min", -1)),
                str(row.get("pre_events_min", -1)),
                str(row.get("pre_waits_final", 0)),
                str(row.get("pre_drops_final", 0)),
                str(row.get("enc_fail_final", 0)),
                str(row.get("enc_slow_final", 0)),
                row.get("reason", "")[:56],
            ])
        print(format_table(
            ["run_id", "codec", "preset", "tuning", "status", "pass_fail", "enc_fps", "acq_ent_min", "acq_evt_min", "pre_buf_min", "pre_evt_min", "pre_waits", "pre_drops", "enc_fail", "enc_slow", "reason"],
            table_rows,
        ))


def run_selected_for_rerun(run, mode):
    pass_fail = (first_pass_fail(run) or "").lower()
    status = (first_status(run) or "").lower()
    if mode == "all":
        return True
    if mode == "failed":
        return status == "failed" or pass_fail == "fail"
    if mode == "nonpass":
        return pass_fail != "pass"
    return False


def sanitize_component(value):
    text = normalize(value)
    out = []
    for char in text:
        if char.isalnum() or char in "-_":
            out.append(char)
        else:
            out.append("_")
    sanitized = "".join(out).strip("_")
    return sanitized or "value"


def emit_rerun_specs(experiment_root: Path,
                     base_spec,
                     runs_json,
                     filters,
                     mode,
                     rerun_dir: Path,
                     rerun_limit,
                     rerun_output_root):
    rerun_dir.mkdir(parents=True, exist_ok=True)
    selected = []
    for run in runs_json.get("runs", []):
        if not run_selected_for_rerun(run, mode):
            continue
        flattened = flatten_run(run)
        if filters and not any(row_matches_filters(row, filters) for row in flattened):
            continue
        selected.append(run)

    if rerun_limit > 0:
        selected = selected[:rerun_limit]

    manifest = {
        "source_experiment_id": base_spec.get("experiment_id", experiment_root.name),
        "rerun_mode": mode,
        "rerun_count": len(selected),
        "runs": [],
    }

    repo_root = Path(__file__).resolve().parent.parent
    command_lines = [
        "#!/usr/bin/env bash",
        "set -euo pipefail",
        "",
    ]

    for run in selected:
        config = run.get("config", {})
        rerun_spec = copy.deepcopy(base_spec)
        rerun_spec["experiment_id"] = (
            f"{base_spec.get('experiment_id', experiment_root.name)}"
            f"__rerun__{sanitize_component(run.get('run_id', 'run'))}"
        )
        rerun_spec["notes"] = (
            f"{base_spec.get('notes', '').rstrip()} "
            f"[Rerun generated from {base_spec.get('experiment_id', experiment_root.name)} "
            f"{run.get('run_id', '')}]"
        ).strip()

        rerun_selection = rerun_spec.setdefault("selection", {})
        if "camera_serials" in config:
            rerun_selection["camera_serials"] = config["camera_serials"]
        if "gpu_ids" in config:
            rerun_selection["gpu_ids"] = config["gpu_ids"]

        rerun_fixed = rerun_spec.setdefault("fixed", {})
        rerun_fixed["duration_s"] = config.get("duration_s", rerun_fixed.get("duration_s", 0))
        rerun_fixed["warmup_s"] = config.get("warmup_s", rerun_fixed.get("warmup_s", 0))
        rerun_fixed["stream_start_delay_s"] = config.get(
            "stream_start_delay_s", rerun_fixed.get("stream_start_delay_s", 0)
        )
        if rerun_output_root:
            rerun_fixed["output_root"] = rerun_output_root

        rerun_spec["matrix"] = {
            "codec": [config.get("codec", "h264")],
            "preset": [config.get("preset", "p1")],
            "tuning": [config.get("tuning", "ll")],
            "rate_control_mode": [config.get("rate_control_mode", "vbr")],
            "quality_value": [config.get("quality_value", 20)],
            "gop_length": [config.get("gop_length", 0)],
        }

        spec_path = rerun_dir / f"{sanitize_component(run.get('run_id', 'run'))}.json"
        with spec_path.open("w", encoding="utf-8") as handle:
            json.dump(rerun_spec, handle, indent=2)
            handle.write("\n")

        manifest["runs"].append({
            "run_id": run.get("run_id", ""),
            "reason": first_reason(run),
            "pass_fail": first_pass_fail(run),
            "status": first_status(run),
            "spec_path": str(spec_path),
        })

        command_lines.append(
            "sudo "
            + shlex.quote(str(repo_root / "build" / "orange_client"))
            + " --mode local --experiment-spec "
            + shlex.quote(str(spec_path))
        )

    manifest_path = rerun_dir / "rerun_manifest.json"
    with manifest_path.open("w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2)
        handle.write("\n")

    shell_path = rerun_dir / "run_reruns.sh"
    with shell_path.open("w", encoding="utf-8") as handle:
        handle.write("\n".join(command_lines) + "\n")
    shell_path.chmod(0o755)

    print("")
    print(f"Generated {len(selected)} rerun spec(s) in {rerun_dir}")
    print(f"Manifest: {manifest_path}")
    print(f"Shell script: {shell_path}")


def main():
    args = parse_args()
    experiment_root = Path(args.experiment_root).expanduser().resolve()
    runs_path = experiment_root / "runs.json"
    summary_path = experiment_root / "summary.json"
    spec_path = experiment_root / "experiment_spec.json"

    if not runs_path.exists():
        print(f"Missing runs.json at {runs_path}", file=sys.stderr)
        return 2
    if not summary_path.exists():
        print(f"Missing summary.json at {summary_path}", file=sys.stderr)
        return 2

    try:
        filters = parse_filters(args.filter)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    runs_json = read_json(runs_path)
    summary_json = read_json(summary_path)
    rows = collect_filtered_rows(runs_json, filters)
    print_analysis(experiment_root, summary_json, runs_json, rows, args.top)
    if not args.no_matrix:
        print_matrix(rows)
    print_top_tables(rows, args.top)

    if args.rerun_mode != "none":
        if not spec_path.exists():
            print(f"Missing experiment_spec.json at {spec_path}", file=sys.stderr)
            return 2
        base_spec = read_json(spec_path)
        rerun_dir = Path(args.rerun_dir).expanduser().resolve() if args.rerun_dir else (experiment_root / "rerun_specs")
        emit_rerun_specs(
            experiment_root,
            base_spec,
            runs_json,
            filters,
            args.rerun_mode,
            rerun_dir,
            args.rerun_limit,
            args.rerun_output_root,
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
