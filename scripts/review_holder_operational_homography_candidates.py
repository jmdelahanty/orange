#!/usr/bin/env python3
"""Review or explicitly promote a persisted holder-plane homography set.

The default is a read-only dry run. Promotion requires a running Citrus local
control socket, a passed checksummed holder evidence package, ``--execute``,
and ``--accept-operational-homographies``. The command reloads and revalidates
the immutable candidate set before asking Citrus to promote it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import socket
import sys
import time
import uuid
from pathlib import Path
from typing import Any


REQUEST_SCHEMA = "citrus.local_control.request"
CANDIDATE_SET_SCHEMA = "citrus.homography_candidate.status"
CANDIDATE_SCHEMA = "citrus.calibration.homography_candidate"
EVIDENCE_SCHEMA = "orange.calibration.holder_fixture_evidence_package"


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"could not read JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"expected a JSON object: {path}")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def load_candidate_set(
    candidate_set_dir: Path,
    expected_candidate_set_id: str,
    expected_canvas_sha256: str,
) -> tuple[dict[str, Any], list[dict[str, str]]]:
    candidate_set_dir = candidate_set_dir.resolve()
    manifest_path = candidate_set_dir / "candidate_set.json"
    manifest = read_json(manifest_path)
    if (
        manifest.get("schema_id") != CANDIDATE_SET_SCHEMA
        or manifest.get("schema_version") != 1
        or manifest.get("state") != "ready_for_review"
        or manifest.get("candidate_set_id") != expected_candidate_set_id
        or candidate_set_dir.name != expected_candidate_set_id
        or manifest.get("canvas_checksum") != expected_canvas_sha256
    ):
        raise ValueError("homography candidate set is not a matching reviewable set")
    targets: list[dict[str, str]] = []
    identities: set[tuple[str, str]] = set()
    rows = manifest.get("candidates")
    if not isinstance(rows, list) or not rows:
        raise ValueError("candidate-set manifest contains no candidates")
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("candidate-set manifest row is not an object")
        arena_id = row.get("arena_id")
        camera_id = row.get("camera_id")
        identity = (arena_id, camera_id)
        candidate_path = candidate_set_dir / f"{arena_id}_{camera_id}" / "candidate.json"
        candidate = read_json(candidate_path)
        if (
            candidate != row
            or candidate.get("schema_id") != CANDIDATE_SCHEMA
            or candidate.get("schema_version") != 1
            or candidate.get("status") != "ready_for_review"
            or candidate.get("candidate_set_id") != expected_candidate_set_id
            or candidate.get("homography_role") != "operational_candidate"
            or not all(isinstance(value, str) and value for value in identity)
            or identity in identities
        ):
            raise ValueError(f"invalid operational candidate: {candidate_path}")
        identities.add(identity)
        targets.append({"arena_id": arena_id, "camera_id": camera_id})
    return manifest, targets


def validate_evidence(
    path: Path, targets: list[dict[str, str]]
) -> tuple[dict[str, Any], str]:
    path = path.resolve()
    evidence = read_json(path)
    if (
        evidence.get("schema_id") != EVIDENCE_SCHEMA
        or evidence.get("schema_version") != 1
        or evidence.get("source_image_sets_modified") is not False
        or evidence.get("physical_state", {}).get("state_id")
        != "holder_installed_dish_absent"
        or evidence.get("operational_candidate_assessment", {}).get("status")
        != "passed"
    ):
        raise ValueError("holder evidence package is not operational-candidate-passed")
    observed = {
        (str(row.get("arena_id", "")), str(row.get("camera_serial", "")))
        for row in evidence.get("camera_observations", [])
        if isinstance(row, dict)
    }
    expected = {(row["arena_id"], row["camera_id"]) for row in targets}
    if observed != expected:
        raise ValueError("holder evidence target set does not match candidates")
    return evidence, sha256_file(path)


def request(method: str, operation_id: str | None, params: dict[str, Any]) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "schema_id": REQUEST_SCHEMA,
        "schema_version": 1,
        "method": method,
        "request_id": f"orange-holder-h-review-{method}-{uuid.uuid4().hex}",
        "source": "orange_holder_operational_homography_review_cli",
    }
    if operation_id:
        payload["operation_id"] = operation_id
    if params:
        payload["params"] = params
    return payload


def send(socket_path: Path, payload: dict[str, Any], timeout_seconds: float) -> dict[str, Any]:
    encoded = (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")
    chunks: list[bytes] = []
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(timeout_seconds)
            client.connect(str(socket_path))
            client.sendall(encoded)
            client.shutdown(socket.SHUT_WR)
            while True:
                chunk = client.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
    except OSError as error:
        raise ValueError(f"Citrus local-control request failed: {error}") from error
    try:
        response = json.loads(b"".join(chunks).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"Citrus returned invalid JSON: {error}") from error
    if not isinstance(response, dict) or not response.get("ok") or not response.get("accepted"):
        raise ValueError(
            f"Citrus rejected request: "
            f"{response.get('error', 'invalid_response') if isinstance(response, dict) else 'invalid_response'}"
        )
    return response


def candidate_status(response: dict[str, Any]) -> dict[str, Any]:
    for container_name in ("effect", "status"):
        container = response.get(container_name)
        if isinstance(container, dict):
            value = container.get("homography_candidate")
            if isinstance(value, dict):
                return value
    return {}


def operation_id(action: str) -> str:
    return f"orange-holder-h-review-{action}-{time.time_ns()}"


def matching_review_status(
    status: dict[str, Any],
    candidate_set_id: str,
    canvas_sha256: str,
) -> bool:
    return (
        status.get("state") == "ready_for_review"
        and status.get("candidate_set_id") == candidate_set_id
        and status.get("homography_role") == "operational_candidate"
        and status.get("canvas_checksum") == canvas_sha256
        and isinstance(status.get("revalidation"), dict)
        and status["revalidation"].get("status") == "passed"
    )


def wait_for_review_status(
    socket_path: Path,
    timeout_seconds: float,
    candidate_set_id: str,
    canvas_sha256: str,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_seconds
    last_status: dict[str, Any] = {}
    while time.monotonic() < deadline:
        response = send(
            socket_path,
            request("homography_candidate_status", None, {}),
            min(2.0, max(0.1, deadline - time.monotonic())),
        )
        last_status = candidate_status(response)
        if matching_review_status(
            last_status, candidate_set_id, canvas_sha256
        ):
            return last_status
        if last_status.get("state") in {
            "fit_failed",
            "quality_gate_failed",
            "rejected",
            "promotion_failed",
        }:
            break
        time.sleep(0.05)
    raise ValueError(
        "Citrus persisted-set revalidation did not pass: "
        + json.dumps(
            {
                "state": last_status.get("state"),
                "candidate_set_id": last_status.get("candidate_set_id"),
                "homography_role": last_status.get("homography_role"),
                "canvas_checksum": last_status.get("canvas_checksum"),
                "revalidation": last_status.get("revalidation"),
                "error": last_status.get("error"),
            },
            sort_keys=True,
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate-set-dir", type=Path, required=True)
    parser.add_argument("--expected-candidate-set-id", required=True)
    parser.add_argument("--expected-canvas-sha256", required=True)
    parser.add_argument("--holder-evidence-manifest", type=Path, required=True)
    parser.add_argument("--socket", type=Path, default=Path("/tmp/citrus_local_control.sock"))
    parser.add_argument("--timeout-seconds", type=float, default=10.0)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--accept-operational-homographies", action="store_true")
    args = parser.parse_args()

    try:
        manifest, targets = load_candidate_set(
            args.candidate_set_dir,
            args.expected_candidate_set_id,
            args.expected_canvas_sha256,
        )
        evidence, evidence_sha256 = validate_evidence(
            args.holder_evidence_manifest, targets
        )
        load_payload = request(
            "load_homography_candidate_set_for_review",
            operation_id("load"),
            {
                "candidate_set_dir": str(args.candidate_set_dir.resolve()),
                "expected_candidate_set_id": args.expected_candidate_set_id,
                "expected_canvas_checksum": args.expected_canvas_sha256,
                "targets": targets,
            },
        )
        verification = {
            "status": "passed",
            "holder_fixture_evidence": {
                "manifest_path": str(args.holder_evidence_manifest.resolve()),
                "manifest_sha256": evidence_sha256,
                "package_id": evidence.get("package_id"),
            },
            "operator_review": {
                "status": "passed",
                "all_camera_holder_overlays_reviewed": True,
                "all_citrus_detection_overlays_reviewed": True,
                "all_citrus_reprojection_overlays_reviewed": True,
                "coordinate_frame_evidence_reviewed": True,
                "independent_verification_dots_reviewed": True,
                "flattened_operational_gels_confirmed": True,
                "explicit_cli_accept_operational_homographies": True,
            },
        }
        if not args.execute:
            print(json.dumps({
                "dry_run": True,
                "candidate_set": {
                    "candidate_set_id": args.expected_candidate_set_id,
                    "transaction_id": manifest.get("transaction_id"),
                    "targets": targets,
                },
                "holder_evidence": {
                    "manifest_path": str(args.holder_evidence_manifest.resolve()),
                    "manifest_sha256": evidence_sha256,
                },
                "load_request": load_payload,
                "promotion_requires": [
                    "--execute",
                    "--accept-operational-homographies",
                ],
            }, indent=2, sort_keys=True))
            return 0
        if not args.accept_operational_homographies:
            raise ValueError(
                "promotion requires --accept-operational-homographies after visual review"
            )
        current_response = send(
            args.socket,
            request("homography_candidate_status", None, {}),
            args.timeout_seconds,
        )
        loaded = candidate_status(current_response)
        if not matching_review_status(
            loaded,
            args.expected_candidate_set_id,
            args.expected_canvas_sha256,
        ):
            send(args.socket, load_payload, args.timeout_seconds)
            loaded = wait_for_review_status(
                args.socket,
                args.timeout_seconds,
                args.expected_candidate_set_id,
                args.expected_canvas_sha256,
            )
        revalidation = loaded["revalidation"]
        verification["citrus_revalidation"] = revalidation
        promote_payload = request(
            "promote_homography_candidates",
            operation_id("promote"),
            {
                "transaction_id": manifest["transaction_id"],
                "expected_canvas_checksum": args.expected_canvas_sha256,
                "accept_homographies_armed": True,
                "verification": verification,
            },
        )
        response = send(args.socket, promote_payload, args.timeout_seconds)
        print(json.dumps(response, indent=2, sort_keys=True))
        return 0
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
