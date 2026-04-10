#!/usr/bin/env python3

import argparse
import json
import os
import sys
from typing import Any, Dict, List, Tuple


def load_snapshot(path: str) -> Dict[str, Any]:
    snapshot_path = path
    if os.path.isdir(path):
        snapshot_path = os.path.join(path, "recording_snapshot.json")
    with open(snapshot_path, "r", encoding="utf-8") as f:
        return json.load(f)


def pick_camera(encoders: Dict[str, Any], requested: str) -> str:
    if requested:
        if requested not in encoders:
            raise KeyError(f"camera '{requested}' not found; available: {', '.join(sorted(encoders.keys()))}")
        return requested
    if not encoders:
        raise KeyError("no encoders found in snapshot")
    if len(encoders) == 1:
        return next(iter(encoders.keys()))
    raise KeyError(
        "multiple encoder entries found; pass --camera. available: "
        + ", ".join(sorted(encoders.keys()))
    )


def get_nested(obj: Dict[str, Any], key: str) -> Any:
    cur: Any = obj
    for part in key.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


def summarize_entry(name: str, encoder: Dict[str, Any]) -> Dict[str, Any]:
    fields = [
        "preset",
        "tuning",
        "path",
        "fps",
        "gop_length",
        "frame_interval_p",
        "lookahead.enable",
        "lookahead.depth",
        "low_delay_keyframe_scale",
        "rc.mode",
        "rc.average_bitrate",
        "rc.max_bitrate",
        "rc.vbv_buffer_size",
        "rc.multi_pass.name",
        "resolved_config.initialize.tuning_info.name",
        "resolved_config.initialize.enable_weighted_prediction",
        "resolved_config.initialize.enable_output_in_vidmem",
        "resolved_config.common.mono_chrome_encoding",
        "resolved_config.rc.enable_aq",
        "resolved_config.rc.enable_temporal_aq",
        "resolved_config.rc.enable_lookahead",
        "resolved_config.rc.lookahead_depth",
        "resolved_config.rc.low_delay_keyframe_scale",
        "resolved_config.rc.multi_pass.name",
        "resolved_config.codec.name",
        "resolved_config.codec.idr_period",
        "resolved_config.codec.max_num_ref_frames",
        "resolved_config.codec.max_num_ref_frames_in_dpb",
        "resolved_config.codec.repeat_sps_pps",
    ]
    return {"name": name, "fields": {field: get_nested(encoder, field) for field in fields}}


def print_summary(summaries: List[Dict[str, Any]]) -> None:
    for summary in summaries:
        print(f"=== {summary['name']} ===")
        for key, value in summary["fields"].items():
            print(f"{key}: {value}")
        print()


def print_diff(summaries: List[Dict[str, Any]]) -> None:
    if len(summaries) < 2:
        return
    all_keys = sorted({key for summary in summaries for key in summary["fields"].keys()})
    changed: List[Tuple[str, List[Any]]] = []
    for key in all_keys:
        values = [summary["fields"].get(key) for summary in summaries]
        if any(value != values[0] for value in values[1:]):
            changed.append((key, values))

    if not changed:
        print("No differences across selected fields.")
        return

    print("=== Differences ===")
    names = [summary["name"] for summary in summaries]
    header = "field".ljust(48) + "  " + "  ".join(name.ljust(24) for name in names)
    print(header)
    print("-" * len(header))
    for key, values in changed:
        row = key.ljust(48) + "  " + "  ".join(str(value).ljust(24) for value in values)
        print(row)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare resolved NVENC encoder config snapshots across recording folders or recording_snapshot.json files."
    )
    parser.add_argument("paths", nargs="+", help="Recording folder(s) or recording_snapshot.json path(s)")
    parser.add_argument("--camera", default="", help="Camera serial to inspect when snapshot has multiple encoders")
    args = parser.parse_args()

    summaries: List[Dict[str, Any]] = []
    for path in args.paths:
        snapshot = load_snapshot(path)
        encoders = snapshot.get("encoders", {})
        camera_key = pick_camera(encoders, args.camera)
        encoder = encoders[camera_key]
        name = os.path.basename(path.rstrip("/")) or path
        summaries.append(summarize_entry(name, encoder))

    print_summary(summaries)
    print_diff(summaries)
    return 0


if __name__ == "__main__":
    sys.exit(main())
