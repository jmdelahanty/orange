#!/usr/bin/env python3
"""Digest of an Orange camera preset folder (rule orange_camera_config_dir_sha256_v1).

A preset folder (e.g. ~/orange_data/config/local/30_cam4_ptp_fourcam) holds one
JSON per camera; Orange loads every *.json directly in it. A run plan pins a
preset by folder name and this digest, and the orchestrator refuses a start
whose folder no longer matches.

Rule orange_camera_config_dir_sha256_v1:
  files  = every regular file directly in the folder whose name ends in
           ".json" (no subfolders; README and other files are not included)
  entry  = file name (basename, exact bytes as UTF-8) -> "sha256:<hex>" of the
           file's raw bytes (no parsing, no normalisation)
  digest = "sha256:<hex>" of the UTF-8 bytes of
           json.dumps({"files": {name: entry, ...}}, sort_keys=True,
                      separators=(",", ":"), ensure_ascii=False)
A folder with no *.json file has no digest (error).

    scripts/camera_config_dir_digest.py <folder>          # prints the digest
    scripts/camera_config_dir_digest.py <folder> --json   # rule, files, digest
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

HASH_RULE = "orange_camera_config_dir_sha256_v1"


def camera_config_dir_files(folder: Path) -> dict[str, str]:
    files = {}
    for path in sorted(folder.iterdir()):
        if path.name.endswith(".json") and path.is_file():
            files[path.name] = "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
    if not files:
        raise ValueError(f"no camera JSON files in {folder}")
    return files


def camera_config_dir_digest(folder: Path) -> dict:
    files = camera_config_dir_files(folder)
    canonical = json.dumps({"files": files}, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return {
        "hash_rule": HASH_RULE,
        "name": folder.name,
        "files": files,
        "sha256": "sha256:" + hashlib.sha256(canonical.encode("utf-8")).hexdigest(),
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--json", action="store_true", help="print rule, files and digest as JSON")
    args = parser.parse_args(argv)
    folder = args.folder.expanduser()
    if not folder.is_dir():
        print(f"not a folder: {folder}", file=sys.stderr)
        return 2
    try:
        result = camera_config_dir_digest(folder)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, sort_keys=True) if args.json else result["sha256"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
