#!/usr/bin/env python3
"""Validate the complete Orange/Citrus observation-binding lifecycle."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
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
FINALIZED_COLLECTION = Path(
    "recording_observation_bindings/finalized_collection.json"
)
RECORDING_SESSION = Path("recording_session.json")
REQUEST_SCHEMA = "orange.citrus.recording_observation_binding_request"
ACCEPTANCE_SCHEMA = "citrus.recording_observation_binding_acceptance"
RECEIPT_SCHEMA = "citrus.recording_observation_finalized_receipt"
FINALIZATION_SCHEMA = "orange.recording.observation_binding_finalization"
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


def sha256_file(path: Path) -> tuple[str, int]:
    require(path.is_file() and not path.is_symlink(), f"not a regular file: {path}")
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
            size += len(block)
    require(size > 0, f"empty finalized artifact: {path}")
    return "sha256:" + digest.hexdigest(), size


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
    candidate = recording / path
    current = recording
    for part in path.parts:
        current = current / part
        require(not current.is_symlink(), f"artifact path traverses a symlink: {relative}")
    resolved = candidate.resolve(strict=False)
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


def schema_version_int(value: Any) -> int:
    """schema_version as a JSON integer (bool and float are not accepted)."""
    raw = value.get("schema_version") if isinstance(value, dict) else None
    return raw if isinstance(raw, int) and not isinstance(raw, bool) else 0


# Receipt v2 (agent-contracts citrus-recording-transfer-3.1): Citrus declares
# every per-session file besides the H5. Rules 1-5 and 8 of that README.
CITRUS_ROLE_ORDER = (
    "stimulus_video",
    "stimulus_video_container_finalization",
    "update_timing",
    "process_diagnostic",
)
PRE310_DIAGNOSTIC_PATH = re.compile(
    r"^citrus/\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}Z_threading_startup_\d+\.json$"
)
REVISION_REASON = "citrus_receipt_v2_upgrade"

# Acceptance v3 carries contract.subject (citrus.subject_declaration v1) and
# receipt v3 contract.subject_reference (citrus.subject_reference v1), both
# live since 2026-10-12 (agent-contracts citrus-orange/subject_declaration_v1).
# The rules mirror validate_subject_declaration / validate_subject_reference in
# src/session/recording_observation_binding.cpp.
UUID = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
LOWER_HEX_40 = re.compile(r"^[0-9a-f]{40}$")
LOWER_HEX_64 = re.compile(r"^[0-9a-f]{64}$")


def is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def null_or_string(value: Any, min_len: int = 1, max_len: int = 0) -> bool:
    if value is None:
        return True
    return isinstance(value, str) and len(value) >= min_len and (max_len == 0 or len(value) <= max_len)


def null_or_int(value: Any, minimum: int, maximum: int | None = None) -> bool:
    if value is None:
        return True
    return is_int(value) and value >= minimum and (maximum is None or value <= maximum)


def utc_timestamp(value: Any) -> bool:
    return isinstance(value, str) and 20 <= len(value) <= 64 and "T" in value and value.endswith("Z")


def uuid_or_null(value: Any) -> bool:
    return value is None or (isinstance(value, str) and UUID.match(value) is not None)


def validate_subject_declaration(subject: Any, context_id: str) -> None:
    """citrus.subject_declaration v1 (acceptance v3 contract.subject)."""
    require(
        isinstance(subject, dict)
        and set(subject) == {"schema_id", "schema_version", "dish_id", "fish_id", "subject_count",
                             "subject_type", "declared_at_utc", "source"}
        and subject["schema_id"] == "citrus.subject_declaration"
        and is_int(subject["schema_version"]) and subject["schema_version"] == 1
        and null_or_string(subject["dish_id"], 1, 128)
        and uuid_or_null(subject["fish_id"])
        and null_or_int(subject["subject_count"], 1)
        and subject["subject_type"] in (None, "individual", "dish_group")
        and utc_timestamp(subject["declared_at_utc"])
        and subject["source"] == "citrus_operator_declaration",
        f"acceptance subject declaration is invalid: {context_id}",
    )
    if subject["dish_id"] is None:
        nullability = subject["fish_id"] is None and subject["subject_count"] is None and subject["subject_type"] is None
    else:
        nullability = is_int(subject["subject_count"]) and isinstance(subject["subject_type"], str)
    require(nullability, f"acceptance subject declaration nullability is invalid: {context_id}")


def validate_subject_reference(reference: Any, context_id: str, reference_version: int = 1) -> None:
    """citrus.subject_reference v1 (receipt v3) or v2 (receipt v4, subject identity v4:
    subject_id null or a lowercase UUID; with a dish an individual has a UUID and count 1,
    a dish_group a null subject_id, count >= 2 and no fish)."""
    require(
        isinstance(reference, dict)
        and set(reference) == {"schema_id", "schema_version", "dish_id", "lookup_status", "lookup_reason",
                               "lookup_http_status", "queried_at_utc", "dish", "fish_status", "fish_reason",
                               "fish", "subject_id", "subject_type", "subject_count", "zebrobot_version",
                               "source"}
        and reference["schema_id"] == "citrus.subject_reference"
        and is_int(reference["schema_version"]) and reference["schema_version"] == reference_version
        and null_or_string(reference["dish_id"], 1, 128)
        and reference["lookup_status"] in ("collected", "not_collected")
        and null_or_string(reference["lookup_reason"])
        and null_or_int(reference["lookup_http_status"], 100, 599)
        and utc_timestamp(reference["queried_at_utc"])
        and reference["fish_status"] in ("collected", "not_collected")
        and null_or_string(reference["fish_reason"])
        and (uuid_or_null(reference["subject_id"]) if reference_version == 2 else null_or_string(reference["subject_id"]))
        and reference["subject_type"] in (None, "individual", "dish_group")
        and null_or_int(reference["subject_count"], 1)
        and reference["source"] == "citrus_mzb_lookup",
        f"receipt subject reference is invalid: {context_id}",
    )
    dish = reference["dish"]
    require(
        dish is None
        or (isinstance(dish, dict)
            and set(dish) == {"dish_id", "dish_uuid", "dish_revision", "dish_updated_at", "fish_count", "cross_id"}
            and isinstance(dish["dish_id"], str) and dish["dish_id"]
            and uuid_or_null(dish["dish_uuid"]) and dish["dish_uuid"] is not None
            and is_int(dish["dish_revision"]) and dish["dish_revision"] >= 0
            and isinstance(dish["dish_updated_at"], str) and dish["dish_updated_at"]
            and is_int(dish["fish_count"]) and dish["fish_count"] >= 0
            and (dish["cross_id"] is None or isinstance(dish["cross_id"], str))),
        f"receipt subject reference dish is invalid: {context_id}",
    )
    fish = reference["fish"]
    require(
        fish is None
        or (isinstance(fish, dict)
            and set(fish) == {"fish_id", "revision", "updated_at"}
            and uuid_or_null(fish["fish_id"]) and fish["fish_id"] is not None
            and is_int(fish["revision"]) and fish["revision"] >= 0
            and isinstance(fish["updated_at"], str) and fish["updated_at"]),
        f"receipt subject reference fish is invalid: {context_id}",
    )
    version = reference["zebrobot_version"]
    require(
        version is None
        or (isinstance(version, dict)
            and set(version) == {"base_url", "api_schema_version", "service_commit", "service_commit_dirty",
                                 "consumer_schema_sha256", "http_status", "error"}
            and null_or_string(version["base_url"])
            and null_or_int(version["api_schema_version"], 1)
            and (version["service_commit"] is None
                 or (isinstance(version["service_commit"], str) and LOWER_HEX_40.match(version["service_commit"])))
            and (version["service_commit_dirty"] is None or isinstance(version["service_commit_dirty"], bool))
            and (version["consumer_schema_sha256"] is None
                 or (isinstance(version["consumer_schema_sha256"], str)
                     and LOWER_HEX_64.match(version["consumer_schema_sha256"])))
            and null_or_int(version["http_status"], 100, 599)
            and (version["error"] is None or isinstance(version["error"], str))),
        f"receipt subject reference zebrobot_version is invalid: {context_id}",
    )
    if reference["lookup_status"] == "collected":
        lookup_ok = isinstance(dish, dict) and reference["lookup_reason"] is None and reference["lookup_http_status"] is None
    else:
        lookup_ok = dish is None and isinstance(reference["lookup_reason"], str)
    if reference["fish_status"] == "collected":
        fish_ok = isinstance(fish, dict) and reference["fish_reason"] is None
    else:
        fish_ok = fish is None and isinstance(reference["fish_reason"], str)
    if reference["dish_id"] is None:
        subject_ok = (reference["subject_id"] is None and reference["subject_type"] is None
                      and reference["subject_count"] is None)
    else:
        subject_ok = ((reference_version == 2 or isinstance(reference["subject_id"], str))
                      and isinstance(reference["subject_type"], str) and is_int(reference["subject_count"]))
    require(lookup_ok and fish_ok and subject_ok,
            f"receipt subject reference nullability is invalid: {context_id}")
    if reference_version == 2 and reference["subject_type"] == "individual":
        require(reference["subject_id"] is not None and reference["subject_count"] == 1,
                f"receipt subject reference v2 individual needs a UUID subject_id and count 1: {context_id}")
    if reference_version == 2 and reference["subject_type"] == "dish_group":
        require(reference["subject_id"] is None and is_int(reference["subject_count"])
                and reference["subject_count"] >= 2 and reference["fish_status"] == "not_collected"
                and fish is None,
                f"receipt subject reference v2 dish_group needs a null subject_id, count >= 2 and no fish: "
                f"{context_id}")


def subject_declaration_disagreement(acceptance_contract: dict[str, Any],
                                     receipt_contract: dict[str, Any]) -> str | None:
    """Subject identity v4, one context: an individual's receipt subject_id equals the
    acceptance v3 declaration's fish_id; a dish_group or no dish declares no fish. A
    disagreement is a Citrus bug; it is reported, never refused (the receipt is
    authoritative). Mirrors subject_declaration_receipt_disagreement in C++."""
    declared = acceptance_contract["subject"]["fish_id"]
    reference = receipt_contract["subject_reference"]
    if reference["subject_type"] == "individual":
        if declared != reference["subject_id"]:
            return (f"individual receipt subject_id {reference['subject_id']!r} differs from the "
                    f"declaration's fish_id {declared!r}")
    elif declared is not None:
        what = "no dish" if reference["dish_id"] is None else "a dish_group"
        return f"the declaration names fish_id {declared!r} but the receipt records {what}"
    return None


def validate_citrus_artifacts(
    recording: Path,
    artifacts: Any,
    h5_relative: str,
    context_id: str,
) -> list[dict[str, Any]]:
    require(isinstance(artifacts, list), f"citrus_artifacts is not a list: {context_id}")
    require(h5_relative.endswith(".h5"), f"receipt H5 path does not end in .h5: {context_id}")
    stem = h5_relative[: -len(".h5")]
    expected = {
        "stimulus_video": stem + ".mp4",
        "stimulus_video_container_finalization": stem + ".mp4.finalization.json",
        "update_timing": stem + "_update_timing.csv",
        "process_diagnostic": stem + "_threading_startup.json",
    }
    keys = {"role", "relative_path", "size_bytes", "sha256"}
    rows: list[dict[str, Any]] = []
    previous: tuple[int, str] | None = None
    counts: dict[str, int] = {}
    by_role: dict[str, dict[str, Any]] = {}
    for item in artifacts:
        require(isinstance(item, dict), f"citrus artifact is not an object: {context_id}")
        role = item.get("role")
        require(role in CITRUS_ROLE_ORDER, f"unknown citrus artifact role {role!r}: {context_id}")
        want = keys | ({"finalization"} if role == "stimulus_video_container_finalization" else set())
        require(set(item) == want, f"citrus artifact keys are not exact ({role}): {context_id}")
        path = item.get("relative_path")
        require(path == expected[role]
                or (role == "process_diagnostic" and isinstance(path, str)
                    and PRE310_DIAGNOSTIC_PATH.match(path) is not None),
                f"{role} is not at its required name: {path!r}")
        size = item.get("size_bytes")
        require(isinstance(size, int) and not isinstance(size, bool) and size > 0,
                f"citrus artifact size is invalid: {path}")
        key = (CITRUS_ROLE_ORDER.index(role), path)
        require(previous is None or previous < key,
                f"citrus_artifacts are not in role-then-path order: {context_id}")
        previous = key
        counts[role] = counts.get(role, 0) + 1
        by_role[role] = item
        # Proof: safe, regular, non-symlink, re-hash equal.
        file_path = resolve_recording_relative(recording, path)
        require(file_path.is_file() and not file_path.is_symlink(),
                f"declared Citrus artifact is missing or not a regular file: {path}")
        digest, actual_size = sha256_file(file_path)
        require(digest == item.get("sha256") and actual_size == size,
                f"declared Citrus artifact size or SHA-256 mismatch: {path}")
        rows.append({"role": role, "relative_path": path, "size_bytes": size, "sha256": digest})
    for role in CITRUS_ROLE_ORDER:
        require(counts.get(role, 0) <= 1, f"{role} appears more than once: {context_id}")
    require(("stimulus_video" in by_role) == ("stimulus_video_container_finalization" in by_role),
            f"stimulus video and its finalization must be declared together: {context_id}")
    if "stimulus_video" in by_role:
        declared = by_role["stimulus_video_container_finalization"]["finalization"]
        require(isinstance(declared, dict) and set(declared) == {
            "schema_id", "schema_version", "status", "terminal", "trailer_written",
            "output_closed", "container_file_size_bytes"},
            f"finalization facts are not exact: {context_id}")
        require(declared.get("schema_id") == "citrus.stimulus_video_container_finalization"
                and declared.get("schema_version") == 1
                and declared.get("status") == "complete"
                and declared.get("terminal") is True
                and declared.get("trailer_written") is True
                and declared.get("output_closed") is True
                and declared.get("container_file_size_bytes")
                == by_role["stimulus_video"]["size_bytes"],
                f"stimulus video finalization is not complete or not the video's size: {context_id}")
        on_disk, _ = load_json(resolve_recording_relative(
            recording, by_role["stimulus_video_container_finalization"]["relative_path"]))
        container = on_disk.get("container") or {}
        require(on_disk.get("schema_id") == declared["schema_id"]
                and on_disk.get("schema_version") == declared["schema_version"]
                and on_disk.get("status") == declared["status"]
                and on_disk.get("terminal") == declared["terminal"]
                and container.get("trailer_written") == declared["trailer_written"]
                and container.get("output_closed") == declared["output_closed"]
                and container.get("file_size_bytes") == declared["container_file_size_bytes"],
                f"stimulus video finalization file disagrees with the receipt: {context_id}")
    return rows


def load_collection_chain(recording: Path) -> list[tuple[int, str, dict[str, Any], bytes]]:
    """Revision 1 plus every finalized_collection.r<N>.json, linear and gap-free."""
    directory = recording / "recording_observation_bindings"
    revisions: set[int] = set()
    for entry in directory.glob("finalized_collection.r*.json"):
        match = re.fullmatch(r"finalized_collection\.r([1-9]\d{0,5})\.json", entry.name)
        require(match is not None, f"unexpected collection revision file: {entry.name}")
        revisions.add(int(match.group(1)))
    last = max(revisions, default=1)
    chain: list[tuple[int, str, dict[str, Any], bytes]] = []
    for revision in range(1, last + 1):
        relative = str(FINALIZED_COLLECTION) if revision == 1 else (
            f"recording_observation_bindings/finalized_collection.r{revision}.json")
        require(revision == 1 or revision in revisions, f"collection revision chain gap at r{revision}")
        collection, raw = load_json(recording / relative)
        if revision == 1:
            require(collection.get("schema_version") == 1 and "revision" not in collection,
                    "revision 1 must be the schema_version 1 collection")
        else:
            previous_relative, previous_raw = chain[-1][1], chain[-1][3]
            supersedes = collection.get("supersedes") or {}
            require(collection.get("schema_version") == 2
                    and collection.get("revision") == revision
                    and collection.get("revision_reason") == REVISION_REASON
                    and supersedes.get("relative_path") == previous_relative
                    and supersedes.get("sha256") == sha256_bytes(previous_raw),
                    f"collection r{revision} does not supersede r{revision - 1} exactly")
            before = chain[-1][2]
            for field in ("citrus_experiment_id", "recording_id", "binding_mode", "context_count"):
                require(collection.get(field) == before.get(field),
                        f"collection r{revision} changes {field}")
            for old, new in zip(before.get("observation_contexts") or [],
                                collection.get("observation_contexts") or []):
                for field in ("observation_context_id", "observation_identity_sha256",
                              "observation_identity", "request", "acceptance", "citrus_h5", "status"):
                    require(old.get(field) == new.get(field),
                            f"collection r{revision} changes {field} of {new.get('observation_context_id')}")
        chain.append((revision, relative, collection, raw))
    return chain


def validate_sealed_record(
    value: dict[str, Any],
    schema_id: str,
    id_field: str,
    allowed_versions: tuple[int, ...] = (1,),
) -> int:
    """Check a sealed envelope and return its schema_version.

    Request and acceptance envelopes are v1 or v2 with their contracts (the
    outer and inner schema_version must match); acceptances also v3, finalized
    receipts v1 to v4.
    """
    require(value.get("schema_id") == schema_id, f"wrong schema: {schema_id}")
    version = schema_version_int(value)
    require(
        version in allowed_versions,
        f"wrong schema version {value.get('schema_version')!r}: {schema_id}",
    )
    require(
        value.get("canonicalization")
        == "canonical_json_utf8_sort_keys_compact_v1",
        f"wrong canonicalization: {schema_id}",
    )
    contract = value.get("contract")
    require(isinstance(contract, dict), f"missing contract: {schema_id}")
    require(
        contract.get("schema_id") == schema_id
        and schema_version_int(contract) == version,
        f"envelope and contract schema versions differ: {schema_id}",
    )
    expected_sha = canonical_contract_sha256(contract)
    require(
        value.get("contract_sha256") == expected_sha,
        f"contract digest mismatch: {schema_id}",
    )
    identifier = value.get(id_field)
    prefixes = {
        "request_id": "obsbindreq_",
        "acceptance_id": "obsbindacc_",
        "receipt_id": "obsbindfin_",
    }
    expected_prefix = prefixes.get(id_field)
    require(expected_prefix is not None, f"unsupported sealed identifier: {id_field}")
    require(
        identifier == expected_prefix + expected_sha.removeprefix("sha256:"),
        f"derived identifier mismatch: {schema_id}",
    )
    return version


# Citrus unified H5 (9aa6d57): the sealed chain lives under
# /evidence/recording_binding and the session attributes under
# /metadata/session. The earlier development layout kept both at the root.
H5_LAYOUTS = (
    ("unified", "evidence/recording_binding", "metadata/session"),
    ("legacy_root", "recording_observation_binding", None),
)


def h5_binding_layout(h5: Any) -> tuple[str, Any, Any]:
    for name, group_path, session_path in H5_LAYOUTS:
        if group_path in h5:
            session = h5[session_path] if session_path else h5
            return name, h5[group_path], session
    raise ValidationError("H5 binding group missing (neither unified nor legacy layout)")


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

    request_versions: dict[str, int] = {}
    start_contexts: dict[str, Any] | None = None  # loaded on the first v2 request
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
        request_version = validate_sealed_record(request, REQUEST_SCHEMA, "request_id", (1, 2))
        request_versions[context_id] = request_version
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
    h5_paths: dict[str, Path] = {}
    for reference in acceptance_refs:
        require(isinstance(reference, dict), "acceptance reference is not an object")
        context_id = reference.get("observation_context_id")
        require(context_id in requests, f"acceptance has unknown context: {context_id}")
        require(context_id not in acceptances, f"duplicate acceptance context: {context_id}")
        path = resolve_recording_relative(recording, reference.get("relative_path"))
        acceptance, raw = load_json(path)
        require(reference.get("sha256") == sha256_bytes(raw), f"acceptance file digest mismatch: {context_id}")
        require(reference.get("byte_size") == len(raw), f"acceptance byte size mismatch: {context_id}")
        acceptance_version = validate_sealed_record(acceptance, ACCEPTANCE_SCHEMA, "acceptance_id", (1, 2, 3))
        # Acceptance v3 (contract.subject) answers a v2 request.
        require(
            acceptance_version == request_versions[context_id]
            or (acceptance_version == 3 and request_versions[context_id] == 2),
            f"acceptance version {acceptance_version} differs from request version {request_versions[context_id]}: {context_id}",
        )
        if acceptance_version == 3:
            validate_subject_declaration(acceptance["contract"].get("subject"), context_id)
        else:
            require("subject" not in acceptance["contract"],
                    f"acceptance v{acceptance_version} carries subject: {context_id}")
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
        if request_versions[context_id] == 2:
            if start_contexts is None:
                start_snapshot, _ = load_json(recording / "recording_snapshot_start.json")
                start_contexts = (start_snapshot.get("session") or {}).get("recording_contexts") or {}
            frozen = start_contexts.get(request_contract["target"]["camera_id"])
            require(
                frozen is not None
                and request_contract.get("recording_context") == frozen,
                f"v2 request context differs from the sealed start snapshot: {context_id}",
            )
        experiment_id = str(contract.get("citrus_experiment_id", ""))
        session_uuid = str(contract.get("citrus_session_uuid", ""))
        require(experiment_id and session_uuid, f"Citrus identities missing: {context_id}")
        experiment_ids.add(experiment_id)

        h5_relative = contract.get("planned_h5_relative_path")
        h5_path = resolve_recording_relative(recording, h5_relative)
        require(h5_path.is_file(), f"planned Citrus H5 is missing: {h5_path}")
        with h5py.File(h5_path, "r") as h5:
            h5_layout, group, session = h5_binding_layout(h5)
            require(decode_h5_string(session.attrs.get("session_status"), "session_status") == "COMPLETE", f"Citrus H5 is not COMPLETE: {h5_relative}")
            require(decode_h5_string(session.attrs.get("session_uuid"), "session_uuid") == session_uuid, f"Citrus session UUID mismatch: {h5_relative}")
            if request_versions[context_id] == 2:
                # Acceptance v2: the H5 session metadata is built from the
                # accepted context, so it must agree with the sealed request.
                frozen = request_contract["recording_context"]
                for key in ("recording_type", "recording_subtype", "behavior_mode"):
                    if key not in frozen:
                        # Context v2 with the subtype omitted: the H5 must
                        # preserve the omission, not substitute a value.
                        require(
                            key not in session.attrs,
                            f"H5 session {key} present although the accepted context omits it: {h5_relative}",
                        )
                        continue
                    require(
                        decode_h5_string(session.attrs.get(key), key) == frozen[key],
                        f"H5 session {key} differs from the accepted recording context: {h5_relative}",
                    )
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
        h5_paths[context_id] = h5_path
        h5_rows.append({
            "observation_context_id": context_id,
            "camera_id": contract["target"]["camera_id"],
            "arena_id": contract["target"]["arena_id"],
            "request_path": request_paths[context_id],
            "acceptance_path": str(path.relative_to(recording)),
            "h5_path": str(h5_path.relative_to(recording)),
            "h5_layout": h5_layout,
            "binding_schema_version": request_versions[context_id],
            "citrus_session_uuid": session_uuid,
        })

    require(len(experiment_ids) == 1, "acceptances do not share one Citrus experiment ID")
    experiment_id = next(iter(experiment_ids))

    chain = load_collection_chain(recording)
    head_revision, _head_relative, finalized, finalized_raw = chain[-1]
    require(
        finalized.get("schema_id") == FINALIZATION_SCHEMA,
        "finalized collection schema is invalid",
    )
    require(finalized.get("status") == "finalized", "collection is not finalized")
    require(finalized.get("binding_status") == "bound", "collection is not bound")
    require(
        finalized.get("recording_id") == collection.get("recording_id"),
        "finalized collection recording ID mismatch",
    )
    require(
        finalized.get("citrus_experiment_id") == experiment_id,
        "finalized collection Citrus experiment mismatch",
    )
    finalized_contexts = finalized.get("observation_contexts")
    require(isinstance(finalized_contexts, list), "finalized contexts are not an array")
    require(
        finalized.get("context_count") == len(finalized_contexts) == expected_count,
        "finalized context count is inconsistent",
    )

    receipt_rows: list[dict[str, Any]] = []
    finalized_ids: set[str] = set()
    declared_paths: set[str] = set()
    pre310_contexts: list[str] = []
    receipt_versions: set[int] = set()
    subject_disagreements: list[dict[str, str]] = []
    for context_row in finalized_contexts:
        require(isinstance(context_row, dict), "finalized context is not an object")
        context_id = context_row.get("observation_context_id")
        require(context_id in requests, f"finalized context is unknown: {context_id}")
        require(context_id not in finalized_ids, f"duplicate finalized context: {context_id}")
        finalized_ids.add(context_id)
        require(context_row.get("status") == "bound", f"context is not bound: {context_id}")

        request = requests[context_id]
        acceptance = acceptances[context_id]
        request_contract = request["contract"]
        acceptance_contract = acceptance["contract"]
        require(
            context_row.get("observation_identity_sha256")
            == request_contract.get("observation_identity_sha256"),
            f"finalized observation identity digest mismatch: {context_id}",
        )
        require(
            context_row.get("observation_identity")
            == request_contract.get("observation_identity"),
            f"finalized observation identity mismatch: {context_id}",
        )

        request_ref = context_row.get("request")
        acceptance_ref = context_row.get("acceptance")
        receipt_ref = context_row.get("finalized_receipt")
        require(isinstance(request_ref, dict), f"request reference missing: {context_id}")
        require(isinstance(acceptance_ref, dict), f"acceptance reference missing: {context_id}")
        require(isinstance(receipt_ref, dict), f"receipt reference missing: {context_id}")
        require(
            request_ref.get("request_id") == request.get("request_id")
            and request_ref.get("contract_sha256") == request.get("contract_sha256")
            and request_ref.get("relative_path") == request_paths[context_id],
            f"finalized request reference mismatch: {context_id}",
        )
        require(
            acceptance_ref.get("acceptance_id") == acceptance.get("acceptance_id")
            and acceptance_ref.get("contract_sha256") == acceptance.get("contract_sha256"),
            f"finalized acceptance reference mismatch: {context_id}",
        )

        receipt_path = resolve_recording_relative(
            recording, receipt_ref.get("relative_path")
        )
        receipt, receipt_raw = load_json(receipt_path)
        require(
            receipt_ref.get("sha256") == sha256_bytes(receipt_raw),
            f"receipt file digest mismatch: {context_id}",
        )
        receipt_version = validate_sealed_record(receipt, RECEIPT_SCHEMA, "receipt_id", (1, 2, 3, 4))
        require(
            receipt_ref.get("receipt_id") == receipt.get("receipt_id")
            and receipt_ref.get("contract_sha256") == receipt.get("contract_sha256"),
            f"finalized receipt reference mismatch: {context_id}",
        )
        receipt_contract = receipt["contract"]
        require(
            receipt_contract.get("request_id") == request.get("request_id")
            and receipt_contract.get("request_contract_sha256")
            == request.get("contract_sha256")
            and receipt_contract.get("acceptance_id") == acceptance.get("acceptance_id")
            and receipt_contract.get("acceptance_contract_sha256")
            == acceptance.get("contract_sha256"),
            f"receipt chain mismatch: {context_id}",
        )
        require(
            receipt_contract.get("observation_context_id") == context_id
            and receipt_contract.get("target") == acceptance_contract.get("target")
            and receipt_contract.get("citrus_experiment_id") == experiment_id
            and receipt_contract.get("citrus_session_uuid")
            == acceptance_contract.get("citrus_session_uuid")
            and receipt_contract.get("session_status") == "COMPLETE",
            f"receipt lifecycle or target mismatch: {context_id}",
        )
        require(
            isinstance(receipt_contract.get("runtime_geometry_contract_sha256"), str)
            and receipt_contract["runtime_geometry_contract_sha256"].startswith("sha256:"),
            f"receipt runtime geometry digest missing: {context_id}",
        )
        semantic = receipt_contract.get("protocol_semantic")
        require(
            isinstance(semantic, dict)
            and semantic.get("status") in ("available", "unsupported"),
            f"receipt protocol semantic evidence missing: {context_id}",
        )

        h5_artifact = receipt_contract.get("h5_artifact")
        require(isinstance(h5_artifact, dict), f"receipt H5 artifact missing: {context_id}")
        require(
            h5_artifact == context_row.get("citrus_h5"),
            f"collection H5 artifact differs from receipt: {context_id}",
        )
        require(
            h5_artifact.get("relative_path")
            == acceptance_contract.get("planned_h5_relative_path"),
            f"receipt H5 path differs from accepted path: {context_id}",
        )
        h5_sha, h5_size = sha256_file(h5_paths[context_id])
        require(
            h5_artifact.get("sha256") == h5_sha
            and h5_artifact.get("size_bytes") == h5_size,
            f"closed H5 size or SHA-256 mismatch: {context_id}",
        )
        require(h5_artifact.get("relative_path") not in declared_paths,
                f"H5 path declared twice in the collection: {context_id}")
        declared_paths.add(h5_artifact.get("relative_path"))
        citrus_rows: list[dict[str, Any]] = []
        receipt_versions.add(receipt_version)
        disagreement = None
        if receipt_version in (3, 4):
            validate_subject_reference(receipt_contract.get("subject_reference"), context_id,
                                       2 if receipt_version == 4 else 1)
        if receipt_version == 4:
            # Receipt v4 always answers an acceptance v3.
            require(schema_version_int(acceptance) == 3,
                    f"receipt v4 must answer an acceptance v3: {context_id}")
            disagreement = subject_declaration_disagreement(acceptance_contract, receipt_contract)
            if disagreement is not None:
                subject_disagreements.append({"observation_context_id": context_id,
                                              "disagreement": disagreement})
        if receipt_version not in (3, 4):
            require("subject_reference" not in receipt_contract,
                    f"receipt v{receipt_version} carries subject_reference: {context_id}")
        if receipt_version >= 2:
            # Receipt v3 = v2 + subject_reference.
            require(set(receipt_contract) >= {"citrus_artifacts"},
                    f"receipt v{receipt_version} lacks citrus_artifacts: {context_id}")
            require(receipt_contract.get("citrus_session_uuid")
                    == "citsess_" + context_id.removeprefix("obsctx_"),
                    f"receipt v2 citrus_session_uuid is not citsess_ + the context hex: {context_id}")
            citrus_rows = validate_citrus_artifacts(
                recording, receipt_contract.get("citrus_artifacts"),
                h5_artifact.get("relative_path"), context_id)
            for row in citrus_rows:
                require(row["relative_path"] not in declared_paths,
                        f"Citrus artifact declared twice in the collection: {row['relative_path']}")
                declared_paths.add(row["relative_path"])
                if (row["role"] == "process_diagnostic"
                        and PRE310_DIAGNOSTIC_PATH.match(row["relative_path"])):
                    pre310_contexts.append(context_id)
        else:
            require("citrus_artifacts" not in receipt_contract,
                    f"receipt v1 carries citrus_artifacts: {context_id}")
        receipt_rows.append({
            "receipt_schema_version": receipt_version,
            "citrus_artifacts": citrus_rows,
            "observation_context_id": context_id,
            "receipt_path": str(receipt_path.relative_to(recording)),
            "receipt_id": receipt["receipt_id"],
            "h5_sha256": h5_sha,
            "h5_size_bytes": h5_size,
        })

    require(finalized_ids == set(requests), "finalized collection does not cover every request")
    require(len(receipt_versions) == 1, "the head collection mixes receipt versions")
    # Sealer 3.3.0: a chain never mixes v3/v4 receipts with another version, so
    # every earlier revision of a v3 or v4 head carries that version too.
    head_version = next(iter(receipt_versions))
    for revision, _relative, collection, _raw in chain[:-1]:
        for row in collection.get("observation_contexts") or []:
            earlier, _ = load_json(resolve_recording_relative(
                recording, (row.get("finalized_receipt") or {}).get("relative_path")))
            earlier_version = schema_version_int(earlier)
            require(not ({earlier_version, head_version} & {3, 4}) or earlier_version == head_version,
                    f"collection r{revision} has receipt v{earlier_version} under a v{head_version} head")
    if pre310_contexts:
        require(len(pre310_contexts) == 1 and pre310_contexts[0] == min(finalized_ids),
                "a pre-3.1.0 process diagnostic must appear once, in the lowest observation_context_id")

    projection = dict(finalized)
    if len(chain) > 1:
        projection["revision_chain"] = [
            {"revision": revision, "relative_path": relative, "sha256": sha256_bytes(raw)}
            for revision, relative, _collection, raw in chain
        ]
    session, _ = load_json(recording / RECORDING_SESSION)
    require(
        session.get("recording_observation_bindings") == projection,
        "recording_session binding projection differs from the finalized collection head",
    )
    require(
        session.get("observation_contexts") == finalized_contexts,
        "recording_session observation contexts differ from finalized collection",
    )

    h5_rows.sort(key=lambda row: row["camera_id"])
    receipt_rows.sort(key=lambda row: row["observation_context_id"])
    return {
        "schema_id": "orange_citrus.recording_observation_binding_validation",
        "schema_version": 1,
        "status": "pass",
        "recording_folder": str(recording),
        "binding_mode": "required",
        "request_count": len(requests),
        "acceptance_count": len(acceptances),
        "h5_embedding_count": len(h5_rows),
        "finalized_receipt_count": len(receipt_rows),
        "finalized_collection_sha256": sha256_bytes(finalized_raw),
        "collection_revision": head_revision,
        "collection_revision_chain": [
            {"revision": revision, "relative_path": relative, "sha256": sha256_bytes(raw)}
            for revision, relative, _collection, raw in chain
        ],
        "recording_session_binding_status": "bound",
        "citrus_experiment_id": experiment_id,
        "cameras": sorted(cameras),
        "arenas": sorted(arenas),
        "edges": h5_rows,
        "receipts": receipt_rows,
        # Receipt v4 only: recorded, never a refusal.
        "subject_declaration_disagreements": subject_disagreements,
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
