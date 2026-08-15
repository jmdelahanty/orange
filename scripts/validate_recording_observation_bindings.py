#!/usr/bin/env python3
"""Validate a finalized Phase-A Orange/Citrus observation-binding run."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Any

try:
    import h5py  # type: ignore
except ImportError:  # pragma: no cover - reported by main with an actionable message.
    h5py = None


REQUEST_COLLECTION = Path(
    "recording_observation_bindings/request_collection.json"
)
PRE_ARM_DECISION = Path("recording_observation_bindings/pre_arm_decision.json")
REQUEST_SCHEMA = "orange.citrus.recording_observation_binding_request"
ACCEPTANCE_SCHEMA = "citrus.recording_observation_binding_acceptance"
H5_BINDING_SCHEMA = "citrus.recording_observation_binding_h5"


class ValidationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def load_json(path: Path) -> tuple[dict[str, Any], bytes]:
    require(path.is_file(), f"missing JSON artifact: {path}")
    raw = path.read_bytes()
    try:
        value = json.loads(raw)
    except json.JSONDecodeError as error:
        raise ValidationError(f"invalid JSON artifact {path}: {error}") from error
    require(isinstance(value, dict), f"JSON artifact is not an object: {path}")
    return value, raw


def sha256_bytes(raw: bytes) -> str:
    return "sha256:" + hashlib.sha256(raw).hexdigest()


def canonical_contract_sha256(contract: dict[str, Any]) -> str:
    raw = json.dumps(
        contract,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return sha256_bytes(raw)


def resolve_recording_relative(recording: Path, relative: Any) -> Path:
    require(isinstance(relative, str) and relative, "artifact path is missing")
    path = Path(relative)
    require(not path.is_absolute(), f"artifact path is absolute: {relative}")
    require(
        all(part not in ("", ".", "..") for part in path.parts),
        f"artifact path is not normalized: {relative}",
    )
    resolved = (recording / path).resolve(strict=False)
    try:
        resolved.relative_to(recording)
    except ValueError as error:
        raise ValidationError(
            f"artifact path escapes recording folder: {relative}"
        ) from error
    return resolved


def decode_h5_string(value: Any, label: str) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8")
    if isinstance(value, str):
        return value
    # h5py scalar string datasets can return a zero-dimensional array scalar.
    if hasattr(value, "item"):
        return decode_h5_string(value.item(), label)
    raise ValidationError(f"{label} is not a scalar UTF-8 string")


def parse_embedded_json(value: Any, label: str) -> dict[str, Any]:
    try:
        parsed = json.loads(decode_h5_string(value, label))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"{label} is invalid JSON: {error}") from error
    require(isinstance(parsed, dict), f"{label} is not a JSON object")
    return parsed


def validate_sealed_record(
    value: dict[str, Any], schema_id: str, id_field: str
) -> None:
    require(value.get("schema_id") == schema_id, f"wrong schema: {schema_id}")
    require(value.get("schema_version") == 1, f"wrong schema version: {schema_id}")
    require(
        value.get("canonicalization")
        == "canonical_json_utf8_sort_keys_compact_v1",
        f"wrong canonicalization: {schema_id}",
    )
    contract = value.get("contract")
    require(isinstance(contract, dict), f"missing contract: {schema_id}")
    expected_sha = canonical_contract_sha256(contract)
    require(
        value.get("contract_sha256") == expected_sha,
        f"contract digest mismatch: {schema_id}",
    )
    identifier = value.get(id_field)
    expected_prefix = "obsbindreq_" if id_field == "request_id" else "obsbindacc_"
    require(
        identifier == expected_prefix + expected_sha.removeprefix("sha256:"),
        f"derived identifier mismatch: {schema_id}",
    )


def validate(
    recording_folder: Path,
    expected_cameras: set[str],
    expected_count: int,
) -> dict[str, Any]:
    require(h5py is not None, "h5py is required (the juicebox environment provides it)")
    recording = recording_folder.resolve()
    require(recording.is_dir(), f"recording folder does not exist: {recording}")

    collection, _ = load_json(recording / REQUEST_COLLECTION)
    decision, _ = load_json(recording / PRE_ARM_DECISION)
    require(
        collection.get("schema_id")
        == "orange.recording.observation_binding_request_collection",
        "request collection schema is invalid",
    )
    require(collection.get("schema_version") == 1, "request collection version is invalid")
    require(collection.get("status") == "materialized", "requests were not materialized")
    require(collection.get("binding_mode") == "required", "binding mode is not required")
    request_refs = collection.get("requests")
    require(isinstance(request_refs, list), "request collection requests is not an array")
    require(collection.get("request_count") == len(request_refs), "request count is inconsistent")
    require(len(request_refs) == expected_count, f"expected {expected_count} requests, found {len(request_refs)}")

    require(
        decision.get("schema_id")
        == "orange.recording.observation_binding_pre_arm_decision",
        "pre-arm decision schema is invalid",
    )
    require(decision.get("schema_version") == 1, "pre-arm decision version is invalid")
    require(decision.get("binding_mode") == "required", "pre-arm mode is not required")
    require(decision.get("lifecycle_status") == "accepted_pending_finalization", "pre-arm lifecycle was not accepted")
    require(decision.get("arm_allowed") is True, "pre-arm decision did not allow arm")
    require(decision.get("transport_attempted") is True, "Citrus transport was not attempted")
    acceptance_refs = decision.get("acceptances")
    require(isinstance(acceptance_refs, list), "pre-arm acceptances is not an array")
    require(decision.get("acceptance_count") == len(acceptance_refs), "acceptance count is inconsistent")
    require(len(acceptance_refs) == expected_count, f"expected {expected_count} acceptances, found {len(acceptance_refs)}")

    requests: dict[str, dict[str, Any]] = {}
    request_paths: dict[str, str] = {}
    cameras: set[str] = set()
    arenas: set[str] = set()
    for reference in request_refs:
        require(isinstance(reference, dict), "request reference is not an object")
        context_id = reference.get("observation_context_id")
        require(isinstance(context_id, str) and context_id, "request context ID is missing")
        require(context_id not in requests, f"duplicate request context: {context_id}")
        path = resolve_recording_relative(recording, reference.get("relative_path"))
        request, raw = load_json(path)
        require(reference.get("sha256") == sha256_bytes(raw), f"request file digest mismatch: {context_id}")
        require(reference.get("byte_size") == len(raw), f"request byte size mismatch: {context_id}")
        validate_sealed_record(request, REQUEST_SCHEMA, "request_id")
        contract = request["contract"]
        require(contract.get("observation_context_id") == context_id, f"request context mismatch: {context_id}")
        require(contract.get("binding_mode") == "required", f"request is not required: {context_id}")
        require(reference.get("request_id") == request.get("request_id"), f"request ID mismatch: {context_id}")
        require(reference.get("request_contract_sha256") == request.get("contract_sha256"), f"request contract digest mismatch: {context_id}")
        target = contract.get("target")
        require(isinstance(target, dict), f"request target missing: {context_id}")
        camera = str(target.get("camera_id", ""))
        arena = str(target.get("arena_id", ""))
        require(camera and target.get("source_camera_stream_id") == camera, f"source stream mismatch: {context_id}")
        require(camera not in cameras, f"multiple source edges for camera: {camera}")
        require(arena and arena not in arenas, f"duplicate or missing arena: {arena}")
        cameras.add(camera)
        arenas.add(arena)
        requests[context_id] = request
        request_paths[context_id] = str(path.relative_to(recording))

    require(cameras == expected_cameras, f"camera set mismatch: {sorted(cameras)}")

    acceptances: dict[str, dict[str, Any]] = {}
    experiment_ids: set[str] = set()
    h5_rows: list[dict[str, Any]] = []
    for reference in acceptance_refs:
        require(isinstance(reference, dict), "acceptance reference is not an object")
        context_id = reference.get("observation_context_id")
        require(context_id in requests, f"acceptance has unknown context: {context_id}")
        require(context_id not in acceptances, f"duplicate acceptance context: {context_id}")
        path = resolve_recording_relative(recording, reference.get("relative_path"))
        acceptance, raw = load_json(path)
        require(reference.get("sha256") == sha256_bytes(raw), f"acceptance file digest mismatch: {context_id}")
        require(reference.get("byte_size") == len(raw), f"acceptance byte size mismatch: {context_id}")
        validate_sealed_record(acceptance, ACCEPTANCE_SCHEMA, "acceptance_id")
        require(reference.get("acceptance_id") == acceptance.get("acceptance_id"), f"acceptance ID mismatch: {context_id}")
        require(reference.get("acceptance_contract_sha256") == acceptance.get("contract_sha256"), f"acceptance contract digest mismatch: {context_id}")
        contract = acceptance["contract"]
        request = requests[context_id]
        request_contract = request["contract"]
        require(contract.get("status") == "accepted", f"Citrus rejected context: {context_id}")
        require(contract.get("observation_context_id") == context_id, f"acceptance context mismatch: {context_id}")
        require(contract.get("request_id") == request.get("request_id"), f"acceptance request ID mismatch: {context_id}")
        require(contract.get("request_contract_sha256") == request.get("contract_sha256"), f"acceptance request digest mismatch: {context_id}")
        require(contract.get("target") == request_contract.get("target"), f"acceptance target mismatch: {context_id}")
        experiment_id = str(contract.get("citrus_experiment_id", ""))
        session_uuid = str(contract.get("citrus_session_uuid", ""))
        require(experiment_id and session_uuid, f"Citrus identities missing: {context_id}")
        experiment_ids.add(experiment_id)

        h5_relative = contract.get("planned_h5_relative_path")
        h5_path = resolve_recording_relative(recording, h5_relative)
        require(h5_path.is_file(), f"planned Citrus H5 is missing: {h5_path}")
        with h5py.File(h5_path, "r") as h5:
            require(decode_h5_string(h5.attrs.get("session_status"), "session_status") == "COMPLETE", f"Citrus H5 is not COMPLETE: {h5_relative}")
            require(decode_h5_string(h5.attrs.get("session_uuid"), "session_uuid") == session_uuid, f"Citrus session UUID mismatch: {h5_relative}")
            require("recording_observation_binding" in h5, f"H5 binding group missing: {h5_relative}")
            group = h5["recording_observation_binding"]
            require(decode_h5_string(group.attrs.get("schema_id"), "binding schema_id") == H5_BINDING_SCHEMA, f"H5 binding schema mismatch: {h5_relative}")
            require(int(group.attrs.get("schema_version", 0)) == 1, f"H5 binding version mismatch: {h5_relative}")
            require(decode_h5_string(group.attrs.get("status"), "binding status") == "accepted_pending_finalization", f"H5 binding status mismatch: {h5_relative}")
            require(decode_h5_string(group.attrs.get("citrus_experiment_id"), "citrus_experiment_id") == experiment_id, f"H5 experiment ID mismatch: {h5_relative}")
            require("request_json" in group and "acceptance_json" in group, f"H5 binding payloads missing: {h5_relative}")
            embedded_request = parse_embedded_json(group["request_json"][()], "request_json")
            embedded_acceptance = parse_embedded_json(group["acceptance_json"][()], "acceptance_json")
            require(embedded_request == request, f"H5 request differs from Orange request: {h5_relative}")
            require(embedded_acceptance == acceptance, f"H5 acceptance differs from Orange acceptance: {h5_relative}")

        acceptances[context_id] = acceptance
        h5_rows.append({
            "observation_context_id": context_id,
            "camera_id": contract["target"]["camera_id"],
            "arena_id": contract["target"]["arena_id"],
            "request_path": request_paths[context_id],
            "acceptance_path": str(path.relative_to(recording)),
            "h5_path": str(h5_path.relative_to(recording)),
            "citrus_session_uuid": session_uuid,
        })

    require(len(experiment_ids) == 1, "acceptances do not share one Citrus experiment ID")
    h5_rows.sort(key=lambda row: row["camera_id"])
    return {
        "schema_id": "orange_citrus.recording_observation_binding_validation",
        "schema_version": 1,
        "status": "pass",
        "recording_folder": str(recording),
        "binding_mode": "required",
        "request_count": len(requests),
        "acceptance_count": len(acceptances),
        "h5_embedding_count": len(h5_rows),
        "citrus_experiment_id": next(iter(experiment_ids)),
        "cameras": sorted(cameras),
        "arenas": sorted(arenas),
        "edges": h5_rows,
    }


def write_json_once(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    rendered = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if path.exists():
        require(path.read_text(encoding="utf-8") == rendered, f"existing output differs: {path}")
        return
    temporary = path.with_name(path.name + f".tmp.{os.getpid()}")
    temporary.write_text(rendered, encoding="utf-8")
    os.replace(temporary, path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording_folder", type=Path)
    parser.add_argument(
        "--expected-cameras",
        default="2010093,2010094,2010095,2010096",
    )
    parser.add_argument("--expected-count", type=int, default=4)
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()
    expected_cameras = {item for item in args.expected_cameras.split(",") if item}
    try:
        result = validate(args.recording_folder, expected_cameras, args.expected_count)
        if args.json_out:
            write_json_once(args.json_out, result)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (OSError, ValidationError, ValueError, TypeError) as error:
        failure = {
            "schema_id": "orange_citrus.recording_observation_binding_validation",
            "schema_version": 1,
            "status": "fail",
            "recording_folder": str(args.recording_folder),
            "error": str(error),
        }
        print(json.dumps(failure, indent=2, sort_keys=True), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
