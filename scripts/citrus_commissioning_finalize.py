#!/usr/bin/env python3
"""Ask a running Citrus GUI for rig-canvas commissioning status, or finalize the release.

  citrus_commissioning_finalize.py status
  citrus_commissioning_finalize.py finalize --transaction-id <id> [--session ID ...] [--record DIR] --accept

finalize sends `finalize_rig_canvas_commissioning` over Citrus local control with
the canvas checksum computed now and `orange_session_dirs` set to the sessions
given, or, with --record and no --session, every session of the record's steps
that a reviewer accepted. Citrus merges these with the sessions it infers; the
commissioning-reference session must be among them (2026-11-13: the GUI path
sent an empty list). With --record the request, the response and the release
manifest go into the record as a step. Mutating: needs --accept, and the
commissioning rule (Claude reviews, every gate passed, evidence kept) or
Jeremy's word.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import socket
import sys
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any

SOCKET = Path("/tmp/citrus_local_control.sock")
DEFAULT_CANVAS = Path("/home/jeremy/citrus/targets/rigs/omnifin0/shadow/shadow.json")
SESSIONS_ROOT = Path.home() / "orange_data" / "calibrations" / "sessions"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def send(sock: Path, method: str, params: dict[str, Any] | None = None,
         operation_id: str | None = None, timeout_s: float = 60.0) -> tuple[dict[str, Any], dict[str, Any]]:
    request: dict[str, Any] = {
        "schema_id": "citrus.local_control.request",
        "schema_version": 1,
        "method": method,
        "request_id": f"orange-commissioning-{method}-{uuid.uuid4().hex}",
        "source": "orange_citrus_commissioning_finalize_cli",
    }
    if operation_id:
        request["operation_id"] = operation_id
    if params:
        request["params"] = params
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout_s)
        client.connect(str(sock))
        client.sendall((json.dumps(request, separators=(",", ":")) + "\n").encode())
        client.shutdown(socket.SHUT_WR)
        data = b""
        while chunk := client.recv(65536):
            data += chunk
    return request, json.loads(data)


def accepted_sessions(record_dir: Path) -> list[str]:
    record = json.loads((record_dir / "record.json").read_text(encoding="utf-8"))
    sessions: list[str] = []
    for step in record.get("steps", []):
        verdicts = step.get("verdicts", [])
        if step.get("status") == "passed" and verdicts and verdicts[-1].get("verdict") == "accepted":
            for session in step.get("sessions", []):
                if session["session_dir"] not in sessions:
                    sessions.append(session["session_dir"])
    return sessions


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=["status", "finalize"])
    parser.add_argument("--socket", type=Path, default=SOCKET)
    parser.add_argument("--canvas", type=Path, default=DEFAULT_CANVAS)
    parser.add_argument("--transaction-id")
    parser.add_argument("--session", action="append", help="session id or directory (repeatable)")
    parser.add_argument("--record", default=os.environ.get("ORANGE_COMMISSIONING_RECORD", ""))
    parser.add_argument("--accept", action="store_true", help="arm the persistent finalize")
    args = parser.parse_args(argv)

    if args.mode == "status":
        request, response = send(args.socket, "rig_canvas_commissioning_status")
        print(json.dumps({"request": request, "response": response}, indent=1))
        return 0 if response.get("ok") else 1

    if not args.transaction_id:
        parser.error("finalize needs --transaction-id")
    if not args.accept:
        parser.error("finalize is persistent; pass --accept")
    if args.session:
        sessions = [str(Path(s) if Path(s).is_dir() else SESSIONS_ROOT / s) for s in args.session]
    elif args.record:
        sessions = accepted_sessions(Path(args.record))
    else:
        parser.error("finalize needs --session or --record")
    if not sessions:
        parser.error("no sessions to send (no accepted steps in the record?)")
    missing = [s for s in sessions if not Path(s).is_dir()]
    if missing:
        parser.error(f"session directories do not exist: {missing}")

    params = {
        "transaction_id": args.transaction_id,
        "canvas_path": str(args.canvas),
        "expected_canvas_checksum": sha256_file(args.canvas),
        "orange_session_dirs": sessions,
        "accept_commissioning_armed": True,
    }
    request, response = send(args.socket, "finalize_rig_canvas_commissioning", params,
                             f"orange-commissioning-finalize-{time.time_ns()}")
    exchange = {"request": request, "response": response}
    print(json.dumps(exchange, indent=1))
    ok = bool(response.get("ok") and response.get("accepted"))
    manifest = (args.canvas.parent / "calibration_artifacts" / "commissioning" /
                f"commissioning_{args.transaction_id}" / "commissioning.json")
    if args.record:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from commissioning_record import append_launcher_step

        with tempfile.TemporaryDirectory() as tmp:
            exchange_path = Path(tmp) / "finalize_exchange.json"
            exchange_path.write_text(json.dumps(exchange, indent=1) + "\n", encoding="utf-8")
            files = [("finalize_exchange", exchange_path)]
            if ok and manifest.is_file():
                files.append(("citrus_commissioning_release", manifest))
            append_launcher_step(args.record, "finalize_rig_canvas_commissioning", 0 if ok else 1,
                                 files=files, command=[sys.argv[0], *(argv or sys.argv[1:])])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
