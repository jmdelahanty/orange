#!/usr/bin/env python3
"""Correct realtime_products in a sealed recording whose YOLO logs were digested
before their writers flushed (Orange bug fixed in 8bb9099; Palette refused the
2026-10-09 snow sessions, 2026-10-11).

Read-only on --source (a staged folder or the verified backup). Writes only:
  --output-manifest PATH      the corrected recording_session.json, or
  --build-source DIR          a NEW directory: hard links (or copies with --copy)
                              of every file in --source, plus a newly written
                              recording_session.json (a new inode; the source
                              manifest is never written through a link).

For every file realtime_products declares whose size/sha256 differ from the file:
  - the declared bytes must be an exact prefix of the file;
  - the file must end with "\n";
  - the row the prefix ends inside, completed by the tail, must parse, and every
    row from there on must be well formed (jsonl: a yolo_result/pose_result
    object; csv: the header's column count);
  - recording_frame_id must continue consecutively from the last complete
    declared row, and end on --last-frame-id when given.
Any other mismatch refuses. The product block (row counts, last frame id,
rows_by_kind/status, parse errors) is recomputed exactly as Orange's
build_realtime_products_json does; a top-level manifest_corrections entry
records old and new size/sha for every changed file.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any

FIX_COMMIT = "8bb9099"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def kind_for_status(product: str, status: str) -> str:
    if product == "detections":
        if status in ("detections", "zero_detections"):
            return "result"
        if status in ("failed", "timeout", "error"):
            return "failed"
        return "other"
    if status == "poses":
        return "result"
    if status == "no_result":
        return "no_result"
    if status == "failed":
        return "failed"
    return "other"


def scan_event_log(data: bytes, product: str) -> dict[str, Any]:
    """Mirror of scan_event_log() in src/session/realtime_products.cpp."""
    log: dict[str, Any] = {
        "size_bytes": len(data), "sha256": sha256_bytes(data), "schema_id": "", "schema_version": 0,
        "header_rows": 0, "row_count": 0, "parse_errors": 0,
        "first_recording_frame_id": 0, "last_recording_frame_id": 0,
        "rows_by_status": {}, "rows_by_kind": {k: 0 for k in ("result", "no_result", "failed", "other")},
    }
    status_block = "yolo" if product == "detections" else "pose"
    for raw in data.split(b"\n"):
        if not raw:
            continue
        try:
            event = json.loads(raw)
        except ValueError:
            event = None
        if not isinstance(event, dict):
            log["parse_errors"] += 1
            continue
        if not log["schema_id"]:
            log["schema_id"] = event.get("schema_id", "") or ""
            log["schema_version"] = event.get("schema_version", 0) or 0
        kind = event.get("event_kind", "")
        if kind not in ("yolo_result", "pose_result"):
            log["header_rows"] += 1
            continue
        log["row_count"] += 1
        frame = event.get("frame") if isinstance(event.get("frame"), dict) else {}
        fid = int(frame.get("recording_frame_id", 0) or 0)
        if fid > 0:
            if log["first_recording_frame_id"] == 0 or fid < log["first_recording_frame_id"]:
                log["first_recording_frame_id"] = fid
            log["last_recording_frame_id"] = max(log["last_recording_frame_id"], fid)
        block = event.get(status_block) if isinstance(event.get(status_block), dict) else {}
        status = block.get("status", "") or ""
        key = status or "missing"
        log["rows_by_status"][key] = log["rows_by_status"].get(key, 0) + 1
        log["rows_by_kind"][kind_for_status(product, status)] += 1
    log["rows_by_status"] = dict(sorted(log["rows_by_status"].items()))
    log["rows_by_kind"] = dict(sorted(log["rows_by_kind"].items()))
    return log


def row_frame_id(line: bytes, path: str, header: list[str] | None) -> int:
    if path.endswith(".jsonl"):
        event = json.loads(line)
        if not isinstance(event, dict) or event.get("event_kind") not in ("yolo_result", "pose_result"):
            raise ValueError("not a frame row")
        return int(event["frame"]["recording_frame_id"])
    fields = line.decode().split(",")
    if header is None or len(fields) != len(header):
        raise ValueError(f"{len(fields)} columns, header has {len(header or [])}")
    return int(fields[header.index("recording_frame_id")])


def check_prefix_tail(data: bytes, declared_size: int, declared_sha: str, path: str,
                      last_frame_id: int | None) -> dict[str, Any]:
    if declared_size > len(data):
        raise ValueError("declared size is larger than the file")
    if sha256_bytes(data[:declared_size]) != declared_sha:
        raise ValueError("declared bytes are not a prefix of the file")
    if not data.endswith(b"\n"):
        raise ValueError("file does not end with a newline")
    header = None
    if path.endswith(".csv"):
        header = data[: data.index(b"\n")].decode().split(",")
        if "recording_frame_id" not in header:
            raise ValueError("csv has no recording_frame_id column")
    # The row the declared prefix ends inside starts after the last "\n" in it.
    row_start = data.rfind(b"\n", 0, declared_size) + 1
    complete_before = data[:row_start].split(b"\n")[:-1]
    previous_id = None
    for line in reversed(complete_before):
        try:
            previous_id = row_frame_id(line, path, header)
            break
        except (ValueError, KeyError, TypeError, IndexError):
            continue
    tail_lines = data[row_start:].split(b"\n")[:-1]
    if not tail_lines:
        raise ValueError("no tail rows")
    ids = []
    for line in tail_lines:
        ids.append(row_frame_id(line, path, header))  # raises on a malformed row
    expected_first = (previous_id + 1) if previous_id is not None else ids[0]
    if ids[0] != expected_first or any(b != a + 1 for a, b in zip(ids, ids[1:])):
        raise ValueError(f"tail recording_frame_ids not consecutive from {expected_first}: {ids[:5]}...")
    if last_frame_id is not None and ids[-1] != last_frame_id:
        raise ValueError(f"tail ends on recording_frame_id {ids[-1]}, expected {last_frame_id}")
    return {"split_row_completed": row_start < declared_size, "tail_rows": len(tail_lines),
            "tail_first_recording_frame_id": ids[0], "tail_last_recording_frame_id": ids[-1],
            "tail_bytes": len(data) - declared_size}


def correct(source: Path, last_frame_id: int | None) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    manifest_bytes = (source / "recording_session.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    block = manifest.get("realtime_products")
    if not isinstance(block, dict):
        raise SystemExit("recording_session.json has no realtime_products block")
    changes: list[dict[str, Any]] = []
    for serial, camera in block.get("cameras", {}).items():
        for product_key in ("detections", "pose"):
            product = camera.get(product_key, {})
            for entry in product.get("files", []):
                changes += correct_entry(source, entry, last_frame_id)
            events = next((e for e in product.get("files", []) if e.get("role") == "events"), None)
            if events and any(c["path"] == events["path"] for c in changes):
                log = scan_event_log((source / events["path"]).read_bytes(), product_key)
                for key in ("row_count", "header_rows", "parse_errors", "first_recording_frame_id",
                            "last_recording_frame_id", "rows_by_kind", "rows_by_status"):
                    product[key] = log[key]
                product["line_schema"] = {"schema_id": log["schema_id"], "schema_version": log["schema_version"]}
        for list_key in ("crop_files", "acquisition_files"):
            for entry in camera.get(list_key, []):
                changes += correct_entry(source, entry, last_frame_id)
    return manifest, changes


def correct_entry(source: Path, entry: dict[str, Any], last_frame_id: int | None) -> list[dict[str, Any]]:
    path = entry["path"]
    data = (source / path).read_bytes()
    sha = sha256_bytes(data)
    if len(data) == entry["size_bytes"] and sha == entry["sha256"]:
        return []
    detail = check_prefix_tail(data, int(entry["size_bytes"]), entry["sha256"], path, last_frame_id)
    change = {"path": path,
              "declared": {"size_bytes": entry["size_bytes"], "sha256": entry["sha256"]},
              "corrected": {"size_bytes": len(data), "sha256": sha}, **detail}
    entry["size_bytes"] = len(data)
    entry["sha256"] = sha
    return [change]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", type=Path, required=True)
    ap.add_argument("--last-frame-id", type=int, help="the session's last recording_frame_id")
    out = ap.add_mutually_exclusive_group()
    out.add_argument("--output-manifest", type=Path)
    out.add_argument("--build-source", type=Path)
    ap.add_argument("--copy", action="store_true", help="with --build-source: copy instead of hard-linking")
    args = ap.parse_args()
    source = args.source.resolve()

    original = json.loads((source / "recording_session.json").read_bytes())
    manifest, changes = correct(source, args.last_frame_id)
    if not changes:
        print("realtime_products already matches every declared file; nothing to correct")
        return 0
    manifest["manifest_corrections"] = list(original.get("manifest_corrections", [])) + [{
        "kind": "realtime_products_digest_before_flush",
        "corrected_at_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "reason": ("realtime_products was digested while the YOLO log writers still held buffered rows; "
                   "each corrected file's declared bytes are an exact prefix and the remaining bytes are "
                   "the recording's own rows (consecutive recording_frame_ids), no new data"),
        "orange_fix_commit": FIX_COMMIT,
        "files": changes,
    }]
    # Nothing else may change.
    probe = json.loads(json.dumps(manifest))
    probe.pop("manifest_corrections")
    if "manifest_corrections" in original:
        probe["manifest_corrections"] = original["manifest_corrections"]
    for key in set(original) | set(probe):
        if key != "realtime_products" and original.get(key) != probe.get(key):
            raise SystemExit(f"internal error: top-level key {key} changed")
    text = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    for c in changes:
        print(f"{c['path']}: {c['declared']['size_bytes']} -> {c['corrected']['size_bytes']} B, "
              f"{c['tail_rows']} tail rows, frames {c['tail_first_recording_frame_id']}.."
              f"{c['tail_last_recording_frame_id']}, split row {'yes' if c['split_row_completed'] else 'no'}")

    if args.output_manifest:
        target = args.output_manifest.resolve()
        if target.parent == source or str(target).startswith(str(source) + os.sep):
            raise SystemExit("refusing to write inside --source")
        target.write_text(text)
        print(f"wrote {target}")
    elif args.build_source:
        dest = args.build_source.resolve()
        if dest.exists():
            raise SystemExit(f"{dest} exists; --build-source needs a new directory")
        if str(dest).startswith(str(source) + os.sep):
            raise SystemExit("refusing to build inside --source")
        for root, dirs, files in os.walk(source):
            rel = Path(root).relative_to(source)
            (dest / rel).mkdir(parents=True, exist_ok=True)
            for name in files:
                if rel == Path(".") and name == "recording_session.json":
                    continue
                src_file = Path(root) / name
                if args.copy:
                    shutil.copy2(src_file, dest / rel / name)
                else:
                    os.link(src_file, dest / rel / name)
        (dest / "recording_session.json").write_text(text)  # a new file, never a link
        print(f"built {dest} ({'copies' if args.copy else 'hard links'} + new recording_session.json)")
    else:
        print("dry run: pass --output-manifest or --build-source to write")
    return 0


if __name__ == "__main__":
    sys.exit(main())
