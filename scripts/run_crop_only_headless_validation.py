#!/usr/bin/env python3
"""Prepare/run the existing headless runner's single-camera crop-only profiles.

Default is a side-effect-free JSON plan. Context declarations are supplied by the
operator, never fabricated. --execute is a real hardware recording and requires
--confirm-scene. Neither option installs a wrapper or changes production config.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]


def make_spec(profile, context, cpus, run_id, output_root=None):
    if not isinstance(context, dict) or context.get("enabled") is not True:
        raise ValueError("context-config must be the enabled registered_scene_context object")
    if not isinstance(cpus, list) or not cpus or any(type(cpu) is not int or cpu < 0 for cpu in cpus):
        raise ValueError("explicit nonnegative master writer CPUs are required")
    spec = json.loads((ROOT / "experiment_specs" / f"crop_only_2010093_{profile}_v1.json").read_text())
    spec["experiment_id"] += "_" + run_id
    spec["fixed"]["registered_scene_context"] = context
    spec["fixed"]["master_frame_journal"] = {"schema_version": 1, "enabled": True, "writer_cpu_ids": cpus}
    if output_root is not None:
        spec["fixed"]["output_root"] = str(output_root.resolve())
    return spec


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("single", "rolling"), required=True)
    parser.add_argument("--context-config", type=Path, required=True, help="Actual native-capture or daily-reuse registered_scene_context JSON")
    parser.add_argument("--writer-cpu", type=int, action="append", required=True)
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--orange-client", type=Path, default=Path("/tmp/orange-timing-build-20260906/orange_client"))
    parser.add_argument("--wrapper", type=Path, default=Path("/usr/local/bin/orange-local-benchmark"))
    parser.add_argument("--write-spec", type=Path, help="Publish a new spec file; never overwrite an existing file")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--confirm-scene", action="store_true", help="Confirm the supplied scene/registration declarations for this run")
    args = parser.parse_args()
    if args.execute and not args.confirm_scene:
        parser.error("--execute requires --confirm-scene for this recording")
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "_" + uuid.uuid4().hex[:8]
    spec = make_spec(args.profile, json.loads(args.context_config.read_text()), args.writer_cpu, run_id, args.output_root)
    path = args.write_spec
    if args.execute and path is None:
        path = Path(tempfile.mkdtemp(prefix="orange_crop_only_acceptance_")) / "experiment.json"
    if path is not None:
        with path.open("x", encoding="utf-8") as handle:
            handle.write(json.dumps(spec, indent=2) + "\n")
        path = path.resolve()
    command = ["sudo", "-n", str(args.wrapper), "--orange-client", str(args.orange_client), str(path or "<new-spec.json>")]
    print(json.dumps({"execute": args.execute, "spec": spec, "spec_path": str(path) if path else None, "command": command}, indent=2), flush=True)
    if not args.execute:
        return 0
    # Parsing opens no camera. The installed wrapper is checked without starting
    # PTP or acquisition, then the same command is executed explicitly.
    subprocess.run([str(args.orange_client), "--mode", "local", "--experiment-spec", str(path),
                    "--validate-experiment-spec"], cwd=ROOT, check=True)
    subprocess.run([str(args.wrapper), "--dry-run", "--orange-client", str(args.orange_client), str(path)], cwd=ROOT, check=True)
    return subprocess.run(command, cwd=ROOT, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
