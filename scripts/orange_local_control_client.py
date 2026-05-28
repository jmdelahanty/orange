#!/usr/bin/env python3
"""Send requests to Orange's local Unix-domain JSON control socket."""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


REQUEST_SCHEMA_ID = "orange.local_control.request"
REQUEST_SCHEMA_VERSION = 1


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def default_socket_path() -> str:
    for name in ("ORANGE_GUI_LOCAL_CONTROL_SOCKET", "ORANGE_LOCAL_CONTROL_SOCKET"):
        value = os.environ.get(name)
        if value:
            return value
    return "/tmp/orange_local_control.sock"


def positive_float(value: str) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a number") from exc
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def nonnegative_float(value: str) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a number") from exc
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return parsed


def add_request_id_arg(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--request-id",
        default=None,
        help="Idempotency key for this request. Default: generated UUID.",
    )


def add_operation_id_arg(parser: argparse.ArgumentParser, *, help_text: str) -> None:
    parser.add_argument(
        "--operation-id",
        default=None,
        help=help_text,
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--socket",
        default=default_socket_path(),
        help="Orange local-control socket path. Default: env value or %(default)s",
    )
    parser.add_argument(
        "--source",
        default="orange_local_control_client",
        help="Request source label.",
    )
    parser.add_argument(
        "--timeout",
        type=positive_float,
        default=2.0,
        help="Socket connect/read timeout in seconds.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the request JSON without connecting.",
    )
    parser.add_argument(
        "--compact",
        action="store_true",
        help="Print compact JSON instead of pretty JSON.",
    )
    parser.add_argument(
        "--allow-error",
        action="store_true",
        help="Exit 0 even when Orange returns ok=false.",
    )

    subparsers = parser.add_subparsers(dest="command", required=True)

    status = subparsers.add_parser("status", help="Request Orange status.")
    add_request_id_arg(status)

    citrus = subparsers.add_parser(
        "citrus-completion",
        help="Diagnostic Citrus experiment terminal-state notification.",
    )
    add_request_id_arg(citrus)
    add_operation_id_arg(
        citrus,
        help_text="Operation id. Default: --experiment-id.",
    )
    citrus.add_argument("--experiment-id", required=True, help="Citrus experiment id.")
    citrus.add_argument(
        "--terminal-state",
        default="completed",
        help="Citrus terminal state label. Default: %(default)s",
    )
    citrus.add_argument(
        "--reason",
        default="protocol_finished",
        help="Completion reason. Default: %(default)s",
    )
    citrus.add_argument(
        "--grace-seconds",
        type=nonnegative_float,
        default=10.0,
        help="Requested Orange stop grace period when Citrus completion stop is enabled.",
    )

    start = subparsers.add_parser(
        "start-recording",
        help="Send a start_recording request. Diagnostic endpoint currently rejects this.",
    )
    add_request_id_arg(start)
    add_operation_id_arg(
        start,
        help_text="Operation id. Default: generated from request id.",
    )
    start.add_argument("--reason", default="orchestrator_start", help="Start reason.")

    stop = subparsers.add_parser(
        "stop-recording",
        help="Send a stop_recording request. Requires Orange recording-stop control to be enabled.",
    )
    add_request_id_arg(stop)
    add_operation_id_arg(
        stop,
        help_text="Operation id. Default: generated from request id.",
    )
    stop.add_argument("--reason", default="orchestrator_stop", help="Stop reason.")
    stop.add_argument(
        "--grace-seconds",
        type=nonnegative_float,
        default=0.0,
        help="Requested stop grace period.",
    )

    return parser.parse_args(argv)


def build_request(args: argparse.Namespace) -> dict[str, Any]:
    request_id = args.request_id or str(uuid.uuid4())
    method_by_command = {
        "status": "status",
        "citrus-completion": "citrus_completion",
        "start-recording": "start_recording",
        "stop-recording": "stop_recording",
    }
    method = method_by_command[args.command]
    payload: dict[str, Any] = {
        "schema_id": REQUEST_SCHEMA_ID,
        "schema_version": REQUEST_SCHEMA_VERSION,
        "method": method,
        "request_id": request_id,
        "source": args.source,
        "sent_at_utc": utc_now(),
        "params": {},
    }

    if args.command == "citrus-completion":
        payload["operation_id"] = args.operation_id or args.experiment_id
        payload["params"] = {
            "experiment_id": args.experiment_id,
            "terminal_state": args.terminal_state,
            "reason": args.reason,
            "grace_seconds": args.grace_seconds,
        }
    elif args.command == "start-recording":
        payload["operation_id"] = args.operation_id or f"start-recording-{request_id}"
        payload["params"] = {
            "reason": args.reason,
        }
    elif args.command == "stop-recording":
        payload["operation_id"] = args.operation_id or f"stop-recording-{request_id}"
        payload["params"] = {
            "reason": args.reason,
            "grace_seconds": args.grace_seconds,
        }

    return payload


def send_request(socket_path: str, payload: dict[str, Any], timeout: float) -> dict[str, Any]:
    rendered = json.dumps(payload, separators=(",", ":")) + "\n"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(socket_path)
        client.sendall(rendered.encode("utf-8"))
        client.shutdown(socket.SHUT_WR)

        chunks: list[bytes] = []
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)

    response_text = b"".join(chunks).decode("utf-8")
    return json.loads(response_text)


def print_json(payload: dict[str, Any], compact: bool) -> None:
    if compact:
        print(json.dumps(payload, separators=(",", ":")))
    else:
        print(json.dumps(payload, indent=2, sort_keys=True))


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    request = build_request(args)
    if args.dry_run:
        print_json(request, args.compact)
        return 0

    socket_path = Path(args.socket)
    if not socket_path.exists():
        print(f"local-control socket does not exist: {socket_path}", file=sys.stderr)
        return 2

    try:
        response = send_request(str(socket_path), request, args.timeout)
    except (OSError, TimeoutError, json.JSONDecodeError) as exc:
        print(f"local-control request failed: {exc}", file=sys.stderr)
        return 1

    print_json(response, args.compact)
    if not response.get("ok", False) and not args.allow_error:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
