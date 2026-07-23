#!/usr/bin/env python3
"""Recover, inspect, and explicitly promote persisted Citrus scale candidates.

All subcommands are dry-run by default. ``load`` reconstructs Citrus's in-memory
review transaction without changing active pointers. ``promote`` first requires
a passed Citrus revalidation and then requires both ``--execute`` and
``--accept-scales`` before it can replace active scale pointers.
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
import uuid
from pathlib import Path
from typing import Any


REQUEST_SCHEMA = "citrus.local_control.request"
SCALE_SET_SCHEMA = "citrus.calibration.projected_surface_scale_candidate_set"
SCALE_CANDIDATE_SCHEMA = "citrus.calibration.projected_surface_scale_candidate"
ORANGE_SET_SCHEMA = "orange.calibration.projected_surface_scale_observation_set"


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"could not read JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"expected a JSON object: {path}")
    return value


def load_candidate_set(
    manifest_path: Path,
    expected_candidate_set_id: str,
    expected_canvas_sha256: str,
) -> tuple[
    dict[str, Any], list[dict[str, str]], list[dict[str, str]]
]:
    manifest_path = manifest_path.resolve()
    manifest = read_json(manifest_path)
    if manifest_path.name != "manifest.json":
        raise ValueError("candidate-set path must end in manifest.json")
    if (
        manifest.get("schema_id") != SCALE_SET_SCHEMA
        or manifest.get("schema_version") != 1
        or manifest.get("status") != "ready_for_review"
    ):
        raise ValueError("candidate-set manifest is not ready for review")
    if manifest.get("candidate_set_id") != expected_candidate_set_id:
        raise ValueError("expected candidate-set ID does not match the manifest")
    if manifest_path.parent.name != expected_candidate_set_id:
        raise ValueError("candidate-set directory name does not match its ID")
    if manifest.get("canvas_checksum") != expected_canvas_sha256:
        raise ValueError("expected canvas checksum does not match the manifest")
    if (manifest_path.parent / "acceptance_receipt.json").exists() or (
        manifest_path.parent / "rejection_receipt.json"
    ).exists():
        raise ValueError("candidate set is already finalized")

    targets: list[dict[str, str]] = []
    sources: list[dict[str, str]] = []
    identities: set[tuple[str, str]] = set()
    candidates = sorted(manifest_path.parent.glob("*/candidate.json"))
    if len(candidates) != manifest.get("candidate_count"):
        raise ValueError("candidate file count does not match the manifest")
    for path in candidates:
        candidate = read_json(path)
        arena_id = candidate.get("arena_id")
        camera_id = candidate.get("camera_id")
        identity = (arena_id, camera_id)
        if (
            candidate.get("schema_id") != SCALE_CANDIDATE_SCHEMA
            or candidate.get("schema_version") != 1
            or candidate.get("status") != "ready_for_review"
            or candidate.get("candidate_set_id") != expected_candidate_set_id
            or not all(isinstance(item, str) and item for item in identity)
            or identity in identities
        ):
            raise ValueError(f"invalid candidate identity or state: {path}")
        identities.add(identity)
        targets.append({"arena_id": arena_id, "camera_id": camera_id})
        source = candidate.get("source_observation")
        if (
            not isinstance(source, dict)
            or not isinstance(source.get("path"), str)
            or not source["path"]
            or not isinstance(source.get("sha256"), str)
            or not source["sha256"].startswith("sha256:")
        ):
            raise ValueError(f"invalid source-observation provenance: {path}")
        sources.append(
            {
                "arena_id": arena_id,
                "camera_id": camera_id,
                "observation_path": source["path"],
                "observation_sha256": source["sha256"],
            }
        )
    if not targets:
        raise ValueError("candidate set contains no targets")
    return manifest, targets, sources


def request(method: str, operation_id: str | None, params: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {
        "schema_id": REQUEST_SCHEMA,
        "schema_version": 1,
        "method": method,
        "request_id": f"orange-scale-review-cli-{method}-{uuid.uuid4().hex}",
        "source": "orange_scale_review_cli",
    }
    if operation_id:
        result["operation_id"] = operation_id
    if params:
        result["params"] = params
    return result


def send(socket_path: Path, payload: dict[str, Any], timeout_seconds: float) -> dict[str, Any]:
    data = (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")
    chunks: list[bytes] = []
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(timeout_seconds)
            client.connect(str(socket_path))
            client.sendall(data)
            client.shutdown(socket.SHUT_WR)
            while True:
                chunk = client.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
    except OSError as exc:
        raise ValueError(f"Citrus local-control request failed: {exc}") from exc
    try:
        response = json.loads(b"".join(chunks).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"Citrus returned invalid JSON: {exc}") from exc
    if not isinstance(response, dict):
        raise ValueError("Citrus returned a non-object response")
    return response


def require_accepted(response: dict[str, Any]) -> None:
    if not response.get("ok") or not response.get("accepted"):
        raise ValueError(f"Citrus rejected the request: {response.get('error', 'unknown_error')}")


def scale_status(response: dict[str, Any]) -> dict[str, Any]:
    for container_name in ("effect", "status"):
        container = response.get(container_name)
        if isinstance(container, dict):
            value = container.get("projected_surface_scale_candidate")
            if isinstance(value, dict):
                return value
    return {}


def operation_id(action: str) -> str:
    return f"orange-scale-review-cli-{action}-{time.time_ns()}"


def print_json(value: dict[str, Any]) -> None:
    print(json.dumps(value, indent=2, sort_keys=True))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", type=Path, default=Path("/tmp/citrus_local_control.sock"))
    parser.add_argument("--timeout-seconds", type=float, default=5.0)
    parser.add_argument("--execute", action="store_true", help="Send the request; otherwise print a dry run")
    subparsers = parser.add_subparsers(dest="command", required=True)

    load = subparsers.add_parser("load", help="Load and revalidate a persisted set without promotion")
    load.add_argument("--candidate-set-manifest", type=Path, required=True)
    load.add_argument("--expected-candidate-set-id", required=True)
    load.add_argument("--expected-canvas-sha256", required=True)

    status = subparsers.add_parser("status", help="Read the current scale review status")
    status.add_argument("--transaction-id", required=True)

    promote = subparsers.add_parser("promote", help="Explicitly promote a revalidated set")
    promote.add_argument("--candidate-set-manifest", type=Path, required=True)
    promote.add_argument("--expected-candidate-set-id", required=True)
    promote.add_argument("--expected-canvas-sha256", required=True)
    promote.add_argument("--verification-manifest", type=Path, required=True)
    promote.add_argument("--accept-scales", action="store_true")

    args = parser.parse_args()
    try:
        if args.command == "status":
            payload = request(
                "projected_surface_scale_candidate_status",
                None,
                {},
            )
            if not args.execute:
                print_json({"dry_run": True, "request": payload})
                return 0
            response = send(args.socket, payload, args.timeout_seconds)
            require_accepted(response)
            current = scale_status(response)
            if current.get("transaction_id") != args.transaction_id:
                raise ValueError("Citrus scale status does not match the requested transaction")
            print_json(response)
            return 0

        manifest, targets, candidate_sources = load_candidate_set(
            args.candidate_set_manifest,
            args.expected_candidate_set_id,
            args.expected_canvas_sha256,
        )
        if args.command == "load":
            payload = request(
                "load_projected_surface_scale_candidate_set_for_review",
                operation_id("load"),
                {
                    "candidate_set_dir": str(args.candidate_set_manifest.resolve().parent),
                    "expected_candidate_set_id": args.expected_candidate_set_id,
                    "expected_canvas_checksum": args.expected_canvas_sha256,
                    "targets": targets,
                },
            )
            if not args.execute:
                print_json({"dry_run": True, "request": payload})
                return 0
            response = send(args.socket, payload, args.timeout_seconds)
            require_accepted(response)
            print_json(response)
            return 0

        if not args.execute or not args.accept_scales:
            raise ValueError(
                "promotion requires both --execute and --accept-scales; "
                "run load and review its status first"
            )
        verification_manifest = read_json(args.verification_manifest.resolve())
        if (
            verification_manifest.get("schema_id") != ORANGE_SET_SCHEMA
            or verification_manifest.get("status") != "passed"
            or verification_manifest.get("citrus_canvas_sha256")
            != args.expected_canvas_sha256
        ):
            raise ValueError("Orange verification manifest is not a passed matching observation set")
        verification = verification_manifest.get("verification")
        if not isinstance(verification, dict) or verification.get("status") != "passed":
            raise ValueError("Orange verification payload is not passed")
        normalized_manifest_sources = sorted(
            (
                row.get("arena_id"),
                row.get("camera_id"),
                row.get("observation_path"),
                row.get("observation_sha256"),
            )
            for row in verification_manifest.get("observations", [])
            if isinstance(row, dict)
        )
        normalized_candidate_sources = sorted(
            (
                row["arena_id"],
                row["camera_id"],
                row["observation_path"],
                row["observation_sha256"],
            )
            for row in candidate_sources
        )
        if normalized_manifest_sources != normalized_candidate_sources:
            raise ValueError("Orange verification observations do not exactly match the candidates")

        status_request = request("projected_surface_scale_candidate_status", None, {})
        status_response = send(args.socket, status_request, args.timeout_seconds)
        require_accepted(status_response)
        current = scale_status(status_response)
        revalidation = current.get("revalidation")
        if (
            not current.get("active")
            or current.get("state") != "ready_for_review"
            or not current.get("loaded_from_persisted_candidate_set")
            or current.get("candidate_set_id") != args.expected_candidate_set_id
            or current.get("canvas_checksum") != args.expected_canvas_sha256
            or not isinstance(revalidation, dict)
            or revalidation.get("status") != "passed"
        ):
            raise ValueError("Citrus does not hold the matching passed persisted-set revalidation")
        verification = dict(verification)
        verification["citrus_revalidation"] = revalidation
        verification["candidate_set_manifest_path"] = str(
            args.candidate_set_manifest.resolve()
        )
        verification["operator_review"] = {
            "status": "passed",
            "explicit_cli_accept_scales": True,
            "all_camera_overlays_reviewed": True,
            "orientation_markers_reviewed": True,
            "pitch_fit_and_holdout_reviewed": True,
            "independent_dimensions_reviewed": True,
            "three_mm_plane_contract_reviewed": True,
        }
        payload = request(
            "promote_projected_surface_scale_candidates",
            operation_id("promote"),
            {
                "transaction_id": manifest["transaction_id"],
                "expected_canvas_checksum": args.expected_canvas_sha256,
                "accept_scales_armed": True,
                "verification": verification,
            },
        )
        response = send(args.socket, payload, args.timeout_seconds)
        require_accepted(response)
        print_json(response)
        return 0
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
