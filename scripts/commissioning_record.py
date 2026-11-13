#!/usr/bin/env python3
"""One durable record per (re)commissioning session, written by the tools.

A record is a folder under ~/orange_data/calibrations/commissioning/:

    <rig>_<UTC open time>_<reason slug>/
      record.json      orange.commissioning_record v1 (docs/schemas/orange_commissioning_record_v1.schema.json)
      README.md        generated at close
      SHA256SUMS       generated at close (every file except itself)
      steps/<NN>_<name>/        copied launcher results, logs, reports
      steps/<NN>_<name>/review/ review evidence attached to a verdict
      analysis/                 analysis outputs attached with `attach`

Small files are copied in. Large products are referenced, not copied:
calibration sessions by their session_index.json sha256, report folders by a
per-file sha256 list. Nothing is left in /tmp or an agent scratchpad.

Commands (all print JSON on stdout):
  open     --rig omnifin0 --reason "..." [--operator jeremy] [--agent claude:<session>] [--root DIR]
  declare  --record DIR key=value [...] --source "..." [--by jeremy]
  step     --record DIR --name NAME --status passed|failed [--exit-code N]
           [--result-json P] [--log P ...] [--file ROLE=P ...] [--reference-dir ROLE=DIR ...]
           [--session ID_OR_DIR ...] [-- <command argv>]
  verdict  --record DIR --step N --reviewer claude|jeremy --verdict accepted|rejected|characterization|needs_operator
           [--note "..."] [--evidence P ...]
  attach   --record DIR --role ROLE P [...]          (into analysis/)
  close    --record DIR [--release-manifest P] [--allow-unreviewed]
  verify   --record DIR
  show     --record DIR

Launchers take --record DIR or the environment variable ORANGE_COMMISSIONING_RECORD.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterator

SCHEMA_ID = "orange.commissioning_record"
SCHEMA_VERSION = 1
RECORD_FILE = "record.json"
SUMS_FILE = "SHA256SUMS"
README_FILE = "README.md"
LOCK_FILE = ".record.lock"
DEFAULT_ROOT = Path.home() / "orange_data" / "calibrations" / "commissioning"
DEFAULT_SESSIONS_ROOT = Path.home() / "orange_data" / "calibrations" / "sessions"
VERDICTS = ("accepted", "rejected", "characterization", "needs_operator")
STEP_STATUSES = ("passed", "failed")
SESSION_ID = re.compile(r"^calsess_[A-Za-z0-9_]+$")
KEY = re.compile(r"^[a-z][a-z0-9_.]*$")
REPO = Path(__file__).resolve().parents[1]


class RecordError(RuntimeError):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def slug(value: str) -> str:
    text = re.sub(r"[^a-z0-9]+", "_", value.lower()).strip("_")
    return text[:48] or "session"


def git_commit() -> str | None:
    try:
        out = subprocess.run(
            ["git", "-C", str(REPO), "rev-parse", "HEAD"],
            check=True, capture_output=True, text=True, timeout=10,
        ).stdout.strip()
        dirty = subprocess.run(
            ["git", "-C", str(REPO), "status", "--porcelain", "--untracked-files=no"],
            check=True, capture_output=True, text=True, timeout=10,
        ).stdout.strip()
        return out + ("-dirty" if dirty else "")
    except (OSError, subprocess.SubprocessError):
        return None


def write_json_atomic(path: Path, value: Any) -> None:
    tmp = path.with_name(path.name + ".tmp")
    data = (json.dumps(value, indent=2, sort_keys=False) + "\n").encode("utf-8")
    with tmp.open("wb") as handle:
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(tmp, path)
    dir_fd = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(dir_fd)
    finally:
        os.close(dir_fd)


@contextmanager
def locked_record(record_dir: Path) -> Iterator[dict[str, Any]]:
    """Load record.json under an exclusive lock and write it back on success."""
    record_path = record_dir / RECORD_FILE
    if not record_path.is_file():
        raise RecordError(f"not a commissioning record: {record_dir}")
    status = json.loads(record_path.read_text(encoding="utf-8")).get("status")
    if status != "open":
        raise RecordError(f"record is {status}, not open: {record_dir}")
    with (record_dir / LOCK_FILE).open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        record = json.loads(record_path.read_text(encoding="utf-8"))
        if record.get("schema_id") != SCHEMA_ID or record.get("schema_version") != SCHEMA_VERSION:
            raise RecordError(f"unsupported record schema in {record_path}")
        if record.get("status") != "open":
            raise RecordError(f"record is {record.get('status')}, not open: {record_dir}")
        yield record
        write_json_atomic(record_path, record)


def current_state(record: dict[str, Any]) -> dict[str, Any]:
    state: dict[str, Any] = {}
    for declaration in record.get("declarations", []):
        state[declaration["key"]] = {
            "value": declaration["value"],
            "source": declaration["source"],
            "declared_at_utc": declaration["at_utc"],
        }
    return state


def copy_into(src: Path, dest_dir: Path) -> Path:
    if not src.is_file():
        raise RecordError(f"file does not exist: {src}")
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / src.name
    counter = 1
    while dest.exists():
        dest = dest_dir / f"{src.stem}.{counter}{src.suffix}"
        counter += 1
    shutil.copy2(src, dest)
    return dest


def file_row(record_dir: Path, path: Path, role: str, source: Path | None = None) -> dict[str, Any]:
    row = {
        "role": role,
        "path": path.relative_to(record_dir).as_posix(),
        "sha256": sha256_file(path),
        "bytes": path.stat().st_size,
    }
    if source is not None:
        row["copied_from"] = str(source.resolve())
    return row


def resolve_session(value: str, sessions_root: Path) -> dict[str, Any]:
    path = Path(value)
    if not path.is_dir():
        if not SESSION_ID.match(value):
            raise RecordError(f"not a calibration session id or directory: {value}")
        path = sessions_root / value
    index = path / "session_index.json"
    row: dict[str, Any] = {"session_id": path.name, "session_dir": str(path.resolve())}
    if index.is_file():
        row["session_index_sha256"] = sha256_file(index)
    else:
        row["session_index_sha256"] = None
        row["note"] = "session_index.json missing when recorded"
    return row


def sessions_in_result(value: Any) -> list[str]:
    found: list[str] = []

    def walk(item: Any) -> None:
        if isinstance(item, dict):
            for key, child in item.items():
                if key == "session_id" and isinstance(child, str) and SESSION_ID.match(child):
                    if child not in found:
                        found.append(child)
                else:
                    walk(child)
        elif isinstance(item, list):
            for child in item:
                walk(child)

    walk(value)
    return found


def reference_dir(path: Path, role: str) -> dict[str, Any]:
    if not path.is_dir():
        raise RecordError(f"directory does not exist: {path}")
    files = []
    for item in sorted(p for p in path.rglob("*") if p.is_file()):
        files.append({
            "path": item.relative_to(path).as_posix(),
            "sha256": sha256_file(item),
            "bytes": item.stat().st_size,
        })
    return {"role": role, "dir": str(path.resolve()), "file_count": len(files), "files": files}


def parse_role_path(value: str, option: str) -> tuple[str, Path]:
    role, sep, path = value.partition("=")
    if not sep or not KEY.match(role) or not path:
        raise RecordError(f"{option} expects ROLE=PATH, got {value!r}")
    return role, Path(path)


# ---------------------------------------------------------------- commands


def cmd_open(args: argparse.Namespace) -> dict[str, Any]:
    opened = datetime.now(timezone.utc)
    record_id = f"{slug(args.rig)}_{opened.strftime('%Y%m%dT%H%M%SZ')}_{slug(args.reason)}"
    record_dir = args.root / record_id
    if record_dir.exists():
        raise RecordError(f"record already exists: {record_dir}")
    (record_dir / "steps").mkdir(parents=True)
    (record_dir / "analysis").mkdir()
    record = {
        "schema_id": SCHEMA_ID,
        "schema_version": SCHEMA_VERSION,
        "record_id": record_id,
        "rig_id": args.rig,
        "reason": args.reason,
        "status": "open",
        "opened_at_utc": opened.strftime("%Y-%m-%dT%H:%M:%S.%fZ"),
        "closed_at_utc": None,
        "operator": args.operator,
        "agents": list(args.agent or []),
        "tool_git_commit": git_commit(),
        "declarations": [],
        "steps": [],
        "attachments": [],
        "release": None,
    }
    write_json_atomic(record_dir / RECORD_FILE, record)
    return {"record_dir": str(record_dir), "record_id": record_id}


def cmd_declare(args: argparse.Namespace) -> dict[str, Any]:
    if not args.source:
        raise RecordError("declare requires --source (who or what says so)")
    pairs = []
    for item in args.pairs:
        key, sep, value = item.partition("=")
        if not sep or not KEY.match(key):
            raise RecordError(f"declare expects key=value with a lowercase key, got {item!r}")
        pairs.append((key, value))
    with locked_record(args.record) as record:
        now = utc_now()
        for key, value in pairs:
            record["declarations"].append({
                "at_utc": now, "key": key, "value": value,
                "source": args.source, "declared_by": args.by,
            })
        state = current_state(record)
    return {"declared": [k for k, _ in pairs], "state": state}


def cmd_step(args: argparse.Namespace) -> dict[str, Any]:
    if args.status not in STEP_STATUSES:
        raise RecordError(f"--status must be one of {STEP_STATUSES}")
    if not KEY.match(args.name.replace("-", "_")):
        raise RecordError(f"step name must be lowercase words: {args.name!r}")
    with locked_record(args.record) as record:
        index = len(record["steps"]) + 1
        step_dir = args.record / "steps" / f"{index:02d}_{slug(args.name)}"
        step_dir.mkdir(parents=True)
        files: list[dict[str, Any]] = []
        session_values = list(args.session or [])
        result_status = None
        if args.result_json:
            if args.result_json.is_file():
                dest = copy_into(args.result_json, step_dir)
                files.append(file_row(args.record, dest, "launcher_result", args.result_json))
                try:
                    result = json.loads(args.result_json.read_text(encoding="utf-8"))
                    result_status = result.get("status") if isinstance(result, dict) else None
                    session_values += [s for s in sessions_in_result(result) if s not in session_values]
                except json.JSONDecodeError:
                    pass
            else:
                files.append({"role": "launcher_result", "missing": str(args.result_json)})
        for log in args.log or []:
            if log.is_file():
                dest = copy_into(log, step_dir / "logs")
                files.append(file_row(args.record, dest, "log", log))
            else:
                files.append({"role": "log", "missing": str(log)})
        for item in args.file or []:
            role, path = parse_role_path(item, "--file")
            dest = copy_into(path, step_dir)
            files.append(file_row(args.record, dest, role, path))
        references = [reference_dir(path, role)
                      for role, path in (parse_role_path(v, "--reference-dir") for v in args.reference_dir or [])]
        sessions = [resolve_session(v, args.sessions_root) for v in session_values]
        record["steps"].append({
            "step_index": index,
            "name": args.name,
            "dir": step_dir.relative_to(args.record).as_posix(),
            "recorded_at_utc": utc_now(),
            "status": args.status,
            "exit_code": args.exit_code,
            "launcher_result_status": result_status,
            "command": list(args.command or []),
            "tool_git_commit": git_commit(),
            "sessions": sessions,
            "files": files,
            "referenced_dirs": references,
            "state_at_step": current_state(record),
            "verdicts": [],
        })
    return {"step_index": index, "step_dir": str(step_dir), "sessions": [s["session_id"] for s in sessions]}


def cmd_verdict(args: argparse.Namespace) -> dict[str, Any]:
    if args.verdict not in VERDICTS:
        raise RecordError(f"--verdict must be one of {VERDICTS}")
    with locked_record(args.record) as record:
        steps = record["steps"]
        if not 1 <= args.step <= len(steps):
            raise RecordError(f"no step {args.step} (record has {len(steps)})")
        step = steps[args.step - 1]
        if args.verdict == "accepted" and step["status"] != "passed":
            raise RecordError("a failed step cannot be accepted")
        review_dir = args.record / step["dir"] / "review"
        evidence = []
        for path in args.evidence or []:
            dest = copy_into(path, review_dir)
            evidence.append(file_row(args.record, dest, "review_evidence", path))
        if args.verdict == "accepted" and args.reviewer == "claude" and not evidence:
            raise RecordError("a Claude acceptance needs --evidence (the images or reports reviewed)")
        step["verdicts"].append({
            "at_utc": utc_now(),
            "reviewer": args.reviewer,
            "verdict": args.verdict,
            "note": args.note or "",
            "evidence": evidence,
        })
    return {"step_index": args.step, "verdict": args.verdict}


def cmd_attach(args: argparse.Namespace) -> dict[str, Any]:
    if not KEY.match(args.role):
        raise RecordError(f"--role must be lowercase words: {args.role!r}")
    with locked_record(args.record) as record:
        rows = []
        for path in args.paths:
            dest = copy_into(path, args.record / "analysis")
            rows.append(file_row(args.record, dest, args.role, path))
        record["attachments"].extend(rows)
    return {"attached": [r["path"] for r in rows]}


def render_readme(record: dict[str, Any]) -> str:
    lines = [
        f"# Commissioning record {record['record_id']}",
        "",
        f"Rig `{record['rig_id']}`, reason: {record['reason']}.",
        f"Opened {record['opened_at_utc']}, closed {record['closed_at_utc']}. "
        f"Operator: {record.get('operator') or 'unstated'}; agents: {', '.join(record.get('agents') or []) or 'none'}.",
        "Generated by `scripts/commissioning_record.py`; `record.json` is the authority, "
        "`SHA256SUMS` covers every file.",
        "",
        "## Declared state (latest value per key)",
        "",
        "| Key | Value | Source | Declared |",
        "|---|---|---|---|",
    ]
    for key, entry in sorted(current_state(record).items()):
        lines.append(f"| {key} | {entry['value']} | {entry['source']} | {entry['declared_at_utc']} |")
    lines += ["", "## Steps", "", "| # | Step | Status | Sessions | Verdicts |", "|---|---|---|---|---|"]
    for step in record["steps"]:
        sessions = ", ".join(s["session_id"] for s in step["sessions"]) or "-"
        verdicts = "; ".join(f"{v['reviewer']}: {v['verdict']}" + (f" ({v['note']})" if v["note"] else "")
                             for v in step["verdicts"]) or "none"
        lines.append(f"| {step['step_index']} | {step['name']} | {step['status']} | {sessions} | {verdicts} |")
    if record.get("release"):
        release = record["release"]
        lines += ["", "## Release", "", f"`{release['path']}` {release['sha256']}"]
    if record.get("attachments"):
        lines += ["", "## Analysis", ""]
        lines += [f"- `{row['path']}` ({row['role']})" for row in record["attachments"]]
    return "\n".join(lines) + "\n"


def write_sums(record_dir: Path) -> int:
    rows = []
    for path in sorted(p for p in record_dir.rglob("*") if p.is_file()):
        if path.name in (SUMS_FILE, LOCK_FILE):
            continue
        rows.append(f"{sha256_file(path).removeprefix('sha256:')}  {path.relative_to(record_dir).as_posix()}")
    (record_dir / SUMS_FILE).write_text("\n".join(rows) + "\n", encoding="utf-8")
    return len(rows)


def make_read_only(record_dir: Path) -> None:
    for path in sorted(record_dir.rglob("*"), reverse=True):
        mode = path.stat().st_mode
        path.chmod(mode & ~(stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH))
    record_dir.chmod(record_dir.stat().st_mode & ~(stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH))


def cmd_close(args: argparse.Namespace) -> dict[str, Any]:
    with locked_record(args.record) as record:
        unreviewed = [s["step_index"] for s in record["steps"] if s["status"] == "passed" and not s["verdicts"]]
        if unreviewed and not args.allow_unreviewed:
            raise RecordError(f"passed steps without a verdict: {unreviewed} (or pass --allow-unreviewed)")
        if args.release_manifest:
            dest = copy_into(args.release_manifest, args.record)
            record["release"] = file_row(args.record, dest, "citrus_commissioning_release", args.release_manifest)
        record["status"] = "closed"
        record["closed_at_utc"] = utc_now()
        closed = dict(record)
    (args.record / README_FILE).write_text(render_readme(closed), encoding="utf-8")
    lock = args.record / LOCK_FILE
    if lock.exists():
        lock.unlink()
    count = write_sums(args.record)
    make_read_only(args.record)
    return {"record_dir": str(args.record), "status": "closed", "files": count}


def cmd_verify(args: argparse.Namespace) -> dict[str, Any]:
    sums = args.record / SUMS_FILE
    if not sums.is_file():
        raise RecordError("record has no SHA256SUMS (not closed)")
    bad = []
    listed = set()
    for line in sums.read_text(encoding="utf-8").splitlines():
        digest, _, rel = line.partition("  ")
        listed.add(rel)
        path = args.record / rel
        if not path.is_file() or sha256_file(path).removeprefix("sha256:") != digest:
            bad.append(rel)
    extra = [p.relative_to(args.record).as_posix() for p in args.record.rglob("*")
             if p.is_file() and p.name != SUMS_FILE and p.relative_to(args.record).as_posix() not in listed]
    if bad or extra:
        raise RecordError(f"record changed after close: mismatched {bad}, unlisted {extra}")
    return {"record_dir": str(args.record), "verified_files": len(listed)}


def cmd_show(args: argparse.Namespace) -> dict[str, Any]:
    record = json.loads((args.record / RECORD_FILE).read_text(encoding="utf-8"))
    return {
        "record_id": record["record_id"],
        "status": record["status"],
        "state": {k: v["value"] for k, v in current_state(record).items()},
        "steps": [{"step_index": s["step_index"], "name": s["name"], "status": s["status"],
                   "sessions": [x["session_id"] for x in s["sessions"]],
                   "verdicts": [f"{v['reviewer']}:{v['verdict']}" for v in s["verdicts"]]}
                  for s in record["steps"]],
    }


def append_launcher_step(
    record_dir: str | Path,
    name: str,
    exit_code: int,
    files: list[tuple[str, Path]] = (),  # type: ignore[assignment]
    reference_dirs: list[tuple[str, Path]] = (),  # type: ignore[assignment]
    command: list[str] = (),  # type: ignore[assignment]
) -> int:
    """For Python launchers: append one step; missing files/dirs are skipped."""
    argv = ["step", "--record", str(record_dir), "--name", name,
            "--status", "passed" if exit_code == 0 else "failed", "--exit-code", str(exit_code)]
    for role, path in files:
        if Path(path).is_file():
            argv += ["--file", f"{role}={path}"]
    for role, path in reference_dirs:
        if Path(path).is_dir():
            argv += ["--reference-dir", f"{role}={path}"]
    argv += ["--", *command]
    return main(argv)


def record_path(value: str | None) -> Path:
    value = value or os.environ.get("ORANGE_COMMISSIONING_RECORD", "")
    if not value:
        raise RecordError("no record: pass --record or set ORANGE_COMMISSIONING_RECORD")
    return Path(value)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command_name", required=True)

    p = sub.add_parser("open")
    p.add_argument("--rig", required=True)
    p.add_argument("--reason", required=True)
    p.add_argument("--operator")
    p.add_argument("--agent", action="append")
    p.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    p.set_defaults(func=cmd_open, needs_record=False)

    p = sub.add_parser("declare")
    p.add_argument("--record")
    p.add_argument("pairs", nargs="+")
    p.add_argument("--source", required=True)
    p.add_argument("--by")
    p.set_defaults(func=cmd_declare)

    p = sub.add_parser("step")
    p.add_argument("--record")
    p.add_argument("--name", required=True)
    p.add_argument("--status", required=True)
    p.add_argument("--exit-code", type=int)
    p.add_argument("--result-json", type=Path)
    p.add_argument("--log", type=Path, action="append")
    p.add_argument("--file", action="append")
    p.add_argument("--reference-dir", action="append")
    p.add_argument("--session", action="append")
    p.add_argument("--sessions-root", type=Path, default=DEFAULT_SESSIONS_ROOT)
    p.add_argument("command", nargs="*")
    p.set_defaults(func=cmd_step)

    p = sub.add_parser("verdict")
    p.add_argument("--record")
    p.add_argument("--step", type=int, required=True)
    p.add_argument("--reviewer", required=True, choices=["claude", "jeremy"])
    p.add_argument("--verdict", required=True)
    p.add_argument("--note")
    p.add_argument("--evidence", type=Path, action="append")
    p.set_defaults(func=cmd_verdict)

    p = sub.add_parser("attach")
    p.add_argument("--record")
    p.add_argument("--role", required=True)
    p.add_argument("paths", type=Path, nargs="+")
    p.set_defaults(func=cmd_attach)

    p = sub.add_parser("close")
    p.add_argument("--record")
    p.add_argument("--release-manifest", type=Path)
    p.add_argument("--allow-unreviewed", action="store_true")
    p.set_defaults(func=cmd_close)

    for name, func in (("verify", cmd_verify), ("show", cmd_show)):
        p = sub.add_parser(name)
        p.add_argument("--record")
        p.set_defaults(func=func)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        if getattr(args, "needs_record", True):
            args.record = record_path(args.record)
        print(json.dumps(args.func(args), indent=2))
    except RecordError as error:
        print(f"commissioning_record: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
