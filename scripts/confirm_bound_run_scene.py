#!/usr/bin/env python3
"""The operator's confirmation in a bound run with an empty background
(scripts/run_orange_citrus_bound.sh --empty-background capture_at_setup).

The runner captured the empty, filled dishes and now waits. Load the fish
without moving the dishes or changing anything else, then run

    scripts/confirm_bound_run_scene.py --run-dir <out dir> --confirmed-by <identity>

or, if the scene is not as captured (a dish moved, a light changed, the
session is called off),

    scripts/confirm_bound_run_scene.py --run-dir <out dir> --confirmed-by <identity> --abort

It writes <run dir>/scene_confirmation.json once (never replaced), naming the
capture it answers (id and sha256 from <run dir>/empty_background_capture.json),
the decision, its fixed statement, who confirmed and when. The runner then
starts the recording with scene_unchanged_confirmed true, or refuses on
--abort. Nothing else sets that flag.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from orange_citrus_orchestrator import (  # noqa: E402
    CAPTURE_RECORD_NAME,
    CAPTURE_RECORD_SCHEMA_ID,
    IDENTITY_PATTERN,
    SCENE_CONFIRMATION_NAME,
    SCENE_DECISION_ABORT,
    SCENE_DECISION_FISH_LOADED,
    build_scene_confirmation,
    write_json_exclusive,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--run-dir", type=Path, required=True, help="The bound run's out dir.")
    parser.add_argument("--confirmed-by", required=True, help="Your identity, e.g. delahantyj.")
    parser.add_argument("--abort", action="store_true", help="Refuse the start instead.")
    parser.add_argument("--note", default="", help="Optional free text recorded with the confirmation.")
    args = parser.parse_args(argv)

    if not IDENTITY_PATTERN.fullmatch(args.confirmed_by):
        print("--confirmed-by must match ^[a-z][a-z0-9_-]{0,31}$", file=sys.stderr)
        return 2
    record_path = args.run_dir / CAPTURE_RECORD_NAME
    try:
        record = json.loads(record_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"no capture record at {record_path} ({exc}); has the runner captured yet?", file=sys.stderr)
        return 2
    if record.get("schema_id") != CAPTURE_RECORD_SCHEMA_ID:
        print(f"{record_path} is not a {CAPTURE_RECORD_SCHEMA_ID}", file=sys.stderr)
        return 2
    decision = SCENE_DECISION_ABORT if args.abort else SCENE_DECISION_FISH_LOADED
    confirmation = build_scene_confirmation(record, args.confirmed_by, decision,
                                            tool="scripts/confirm_bound_run_scene.py", note=args.note)
    target = args.run_dir / SCENE_CONFIRMATION_NAME
    try:
        write_json_exclusive(target, confirmation)
    except FileExistsError:
        print(f"{target} already exists; a confirmation is never replaced", file=sys.stderr)
        return 2
    print(f"{decision} for capture {record['capture_id']} ({record['descriptor_sha256']}) by "
          f"{args.confirmed_by}: {confirmation['statement']}\nwritten {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
