#!/usr/bin/env python3
"""Focused tests for the Phase-A Orange/Citrus binding validator."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = REPO_ROOT / "scripts" / "validate_recording_observation_bindings.py"


def ensure_h5py_interpreter() -> None:
    try:
        import h5py  # noqa: F401
        return
    except ImportError:
        pass
    if "--h5py-child" in sys.argv:
        raise RuntimeError("h5py unavailable in designated test interpreter")
    candidate = Path("/home/jeremy/miniforge3/envs/juicebox/bin/python")
    if not candidate.is_file():
        print("validate_recording_observation_bindings_tests: SKIP (h5py unavailable)")
        raise SystemExit(0)
    completed = subprocess.run(
        [str(candidate), str(Path(__file__).resolve()), "--h5py-child"],
        check=False,
    )
    raise SystemExit(completed.returncode)


def load_validator() -> Any:
    spec = importlib.util.spec_from_file_location("binding_validator", VALIDATOR)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not import binding validator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(value: dict[str, Any]) -> str:
    raw = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return "sha256:" + hashlib.sha256(raw).hexdigest()


# Live since 2026-10-12: acceptance v3 contract.subject and receipt v3
# contract.subject_reference (agent-contracts citrus-orange/subject_declaration_v1).
SUBJECT_DECLARATION = {
    "schema_id": "citrus.subject_declaration",
    "schema_version": 1,
    "dish_id": "19234_2",
    "fish_id": None,
    "subject_count": 1,
    "subject_type": "individual",
    "declared_at_utc": "2026-10-12T15:00:00Z",
    "source": "citrus_operator_declaration",
}
SUBJECT_REFERENCE = {
    "schema_id": "citrus.subject_reference",
    "schema_version": 1,
    "dish_id": "19234_2",
    "lookup_status": "collected",
    "lookup_reason": None,
    "lookup_http_status": None,
    "queried_at_utc": "2026-10-12T15:00:01Z",
    "dish": {
        "dish_id": "19234_2",
        "dish_uuid": "061346be-666a-49f3-8b95-9f0eb352bf3a",
        "dish_revision": 2,
        "dish_updated_at": "2026-10-12 09:00:00",
        "fish_count": 1,
        "cross_id": None,
    },
    "fish_status": "not_collected",
    "fish_reason": "operator_did_not_select",
    "fish": None,
    "subject_id": "c1a0f3d2-5b7e-4c1a-9d2e-3f4a5b6c7d8e",
    "subject_type": "individual",
    "subject_count": 1,
    "zebrobot_version": None,
    "source": "citrus_mzb_lookup",
}


def seal(schema: str, id_field: str, contract: dict[str, Any]) -> dict[str, Any]:
    """Envelope version follows the contract (request 1|2, acceptance 1|2|3, receipt 1|3)."""
    sha = digest(contract)
    prefix = {
        "request_id": "obsbindreq_",
        "acceptance_id": "obsbindacc_",
        "receipt_id": "obsbindfin_",
    }[id_field]
    return {
        "schema_id": schema,
        "schema_version": contract.get("schema_version", 1),
        "canonicalization": "canonical_json_utf8_sort_keys_compact_v1",
        id_field: prefix + sha.removeprefix("sha256:"),
        "contract_sha256": sha,
        "contract": contract,
    }


def write_json(path: Path, value: dict[str, Any]) -> bytes:
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = (json.dumps(value, indent=2) + "\n").encode()
    path.write_bytes(raw)
    return raw


CONTEXT = {
    "schema_id": "citrus.parent_recording_context",
    "schema_version": 1,
    "recording_type": "behavior",
    "recording_subtype": "dish_stimulus",
    "behavior_mode": "embedded",
    "recording_intent": "stimulus_experiment",
    "data_origin": "acquired",
}


CONTEXT_NO_SUBTYPE = {k: v for k, v in CONTEXT.items() if k != "recording_subtype"}
CONTEXT_NO_SUBTYPE["schema_version"] = 2


def fixture(root: Path, version: int = 1, unified_h5: bool = False,
            parent_context: dict[str, Any] = CONTEXT,
            subject_declaration: dict[str, Any] | None = None,
            subject_reference: dict[str, Any] | None = None) -> None:
    """version 2 carries the frozen context in each request (and a start
    snapshot that freezes it); unified_h5 writes the Citrus 9aa6d57 layout
    (/evidence/recording_binding + /metadata/session) instead of the root one.
    subject_declaration makes the acceptances v3 (version must be 2);
    subject_reference makes the receipts v3 (citsess_ session, citrus_artifacts)."""
    import h5py

    if version == 2:
        write_json(root / "recording_snapshot_start.json", {
            "schema_version": 2,
            "session": {"recording_contexts": {
                camera: dict(parent_context) for camera in ("2010093", "2010094", "2010095", "2010096")}},
        })
    request_refs = []
    acceptance_refs = []
    finalized_contexts = []
    experiment_id = "citexp_fixture"
    for index, camera in enumerate(("2010093", "2010094", "2010095", "2010096"), 1):
        context = "obsctx_" + hashlib.sha256(camera.encode()).hexdigest()
        session_uuid = (
            "citsess_" + context.removeprefix("obsctx_")
            if subject_reference is not None else f"session_{index}")
        target = {
            "rig_id": "omnifin0",
            "canvas_name": "shadow",
            "arena_id": f"arena_{index}",
            "camera_id": camera,
            "source_camera_stream_id": camera,
        }
        request_contract = {
            "schema_id": "orange.citrus.recording_observation_binding_request",
            "schema_version": version,
            "observation_context_id": context,
            "observation_identity_sha256": "sha256:" + "1" * 64,
            "observation_identity": {"fixture": camera},
            "binding_mode": "required",
            "requested_at_utc": "2026-08-13T20:00:00Z",
            "recording": {"recording_id": "fixture"},
            "target": target,
            "recording_geometry_contract": {"status": "available"},
        }
        if version == 2:
            request_contract["recording_context"] = dict(parent_context)

        request = seal(
            "orange.citrus.recording_observation_binding_request",
            "request_id",
            request_contract,
        )
        request_path = Path("recording_observation_bindings/requests") / f"{context}.json"
        request_raw = write_json(root / request_path, request)
        request_refs.append({
            "observation_context_id": context,
            "request_id": request["request_id"],
            "request_contract_sha256": request["contract_sha256"],
            "relative_path": str(request_path),
            "sha256": "sha256:" + hashlib.sha256(request_raw).hexdigest(),
            "byte_size": len(request_raw),
        })

        h5_relative = Path("citrus") / f"arena_{index}.h5"
        acceptance_contract = {
            "schema_id": "citrus.recording_observation_binding_acceptance",
            "schema_version": 3 if subject_declaration is not None else version,
            "status": "accepted",
            "request_id": request["request_id"],
            "request_contract_sha256": request["contract_sha256"],
            "observation_context_id": context,
            "decided_at_utc": "2026-08-13T20:00:01Z",
            "citrus_experiment_id": experiment_id,
            "citrus_session_uuid": session_uuid,
            "target": target,
            "planned_h5_relative_path": str(h5_relative),
        }
        if subject_declaration is not None:
            acceptance_contract["subject"] = dict(subject_declaration)
        acceptance = seal(
            "citrus.recording_observation_binding_acceptance",
            "acceptance_id",
            acceptance_contract,
        )
        acceptance_path = Path("recording_observation_bindings/acceptances") / f"{context}.json"
        acceptance_raw = write_json(root / acceptance_path, acceptance)
        acceptance_refs.append({
            "observation_context_id": context,
            "acceptance_id": acceptance["acceptance_id"],
            "acceptance_contract_sha256": acceptance["contract_sha256"],
            "relative_path": str(acceptance_path),
            "sha256": "sha256:" + hashlib.sha256(acceptance_raw).hexdigest(),
            "byte_size": len(acceptance_raw),
        })

        h5_path = root / h5_relative
        h5_path.parent.mkdir(parents=True, exist_ok=True)
        with h5py.File(h5_path, "w") as h5:
            session = h5.create_group("metadata/session") if unified_h5 else h5
            session.attrs["session_status"] = "COMPLETE"
            session.attrs["session_uuid"] = session_uuid
            if version == 2:
                for key in ("recording_type", "recording_subtype", "behavior_mode"):
                    if key in parent_context:
                        session.attrs[key] = parent_context[key]
            group = h5.create_group(
                "evidence/recording_binding" if unified_h5 else "recording_observation_binding")
            group.attrs["schema_id"] = "citrus.recording_observation_binding_h5"
            group.attrs["schema_version"] = 1
            group.attrs["status"] = "accepted_pending_finalization"
            group.attrs["citrus_experiment_id"] = experiment_id
            dtype = h5py.string_dtype(encoding="utf-8")
            group.create_dataset("request_json", data=json.dumps(request), dtype=dtype)
            group.create_dataset("acceptance_json", data=json.dumps(acceptance), dtype=dtype)

        h5_raw = h5_path.read_bytes()
        h5_artifact = {
            "relative_path": str(h5_relative),
            "size_bytes": len(h5_raw),
            "sha256": "sha256:" + hashlib.sha256(h5_raw).hexdigest(),
        }
        receipt_contract = {
            "schema_id": "citrus.recording_observation_finalized_receipt",
            "schema_version": 3 if subject_reference is not None else 1,
            "request_id": request["request_id"],
            "request_contract_sha256": request["contract_sha256"],
            "acceptance_id": acceptance["acceptance_id"],
            "acceptance_contract_sha256": acceptance["contract_sha256"],
            "observation_context_id": context,
            "finalized_at_utc": "2026-08-13T20:01:00Z",
            "citrus_experiment_id": experiment_id,
            "citrus_session_uuid": session_uuid,
            "target": target,
            "h5_artifact": h5_artifact,
            "session_status": "COMPLETE",
            "runtime_geometry_contract_sha256": "sha256:" + "2" * 64,
            "protocol_semantic": {
                "status": "available",
                "semantic_sha256": "sha256:" + "3" * 64,
            },
        }
        if subject_reference is not None:
            receipt_contract["citrus_artifacts"] = []
            receipt_contract["subject_reference"] = dict(subject_reference)
        receipt = seal(
            "citrus.recording_observation_finalized_receipt",
            "receipt_id",
            receipt_contract,
        )
        receipt_path = Path("recording_observation_bindings/receipts") / f"{context}.json"
        receipt_raw = write_json(root / receipt_path, receipt)
        finalized_contexts.append({
            "observation_context_id": context,
            "observation_identity_sha256": request_contract["observation_identity_sha256"],
            "observation_identity": request_contract["observation_identity"],
            "status": "bound",
            "request": {
                "request_id": request["request_id"],
                "contract_sha256": request["contract_sha256"],
                "relative_path": str(request_path),
            },
            "acceptance": {
                "acceptance_id": acceptance["acceptance_id"],
                "contract_sha256": acceptance["contract_sha256"],
                "relative_path": str(acceptance_path),
            },
            "finalized_receipt": {
                "receipt_id": receipt["receipt_id"],
                "contract_sha256": receipt["contract_sha256"],
                "relative_path": str(receipt_path),
                "sha256": "sha256:" + hashlib.sha256(receipt_raw).hexdigest(),
            },
            "citrus_h5": h5_artifact,
        })

    write_json(root / "recording_observation_bindings/request_collection.json", {
        "schema_id": "orange.recording.observation_binding_request_collection",
        "schema_version": 1,
        "status": "materialized",
        "binding_mode": "required",
        "recording_id": "fixture",
        "request_count": 4,
        "requests": request_refs,
    })
    write_json(root / "recording_observation_bindings/pre_arm_decision.json", {
        "schema_id": "orange.recording.observation_binding_pre_arm_decision",
        "schema_version": 1,
        "binding_mode": "required",
        "lifecycle_status": "accepted_pending_finalization",
        "arm_allowed": True,
        "transport_attempted": True,
        "acceptance_count": 4,
        "acceptances": acceptance_refs,
    })
    finalized_contexts.sort(key=lambda row: row["observation_context_id"])
    finalized = {
        "schema_id": "orange.recording.observation_binding_finalization",
        "schema_version": 1,
        "status": "finalized",
        "binding_status": "bound",
        "binding_mode": "required",
        "recording_id": "fixture",
        "citrus_experiment_id": experiment_id,
        "finalized_at_utc": "2026-08-13T20:01:00Z",
        "context_count": 4,
        "observation_contexts": finalized_contexts,
    }
    write_json(
        root / "recording_observation_bindings/finalized_collection.json",
        finalized,
    )
    write_json(root / "recording_session.json", {
        "schema_id": "orange.recording_session",
        "recording_observation_bindings": finalized,
        "observation_contexts": finalized_contexts,
    })


def main() -> int:
    ensure_h5py_interpreter()
    module = load_validator()
    expected = {"2010093", "2010094", "2010095", "2010096"}
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        fixture(root)
        result = module.validate(root, expected, 4)
        assert result["status"] == "pass"
        assert result["h5_embedding_count"] == 4
        assert result["finalized_receipt_count"] == 4
        assert result["recording_session_binding_status"] == "bound"

        import h5py

        with h5py.File(root / "citrus/arena_4.h5", "r+") as h5:
            del h5["recording_observation_binding/acceptance_json"]
        try:
            module.validate(root, expected, 4)
        except module.ValidationError as error:
            assert "payloads missing" in str(error)
        else:
            raise AssertionError("missing H5 acceptance payload passed validation")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        fixture(root)
        receipt = next(
            (root / "recording_observation_bindings/receipts").glob("*.json")
        )
        value = json.loads(receipt.read_text())
        value["contract"]["h5_artifact"]["sha256"] = "sha256:" + "f" * 64
        write_json(receipt, value)
        try:
            module.validate(root, expected, 4)
        except module.ValidationError as error:
            assert "receipt file digest mismatch" in str(error)
        else:
            raise AssertionError("tampered finalized receipt passed validation")
    # Binding v2 with the Citrus unified H5 layout: passes, reports the
    # layout and version, and rejects a v1 acceptance answering a v2 request
    # or an H5 session built from a different context.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        fixture(root, version=2, unified_h5=True)
        result = module.validate(root, expected, 4)
        assert result["status"] == "pass"
        assert {row["h5_layout"] for row in result["edges"]} == {"unified"}
        assert {row["binding_schema_version"] for row in result["edges"]} == {2}

        acceptance_path = next(
            (root / "recording_observation_bindings/acceptances").glob("*.json")
        )
        value = json.loads(acceptance_path.read_text())
        downgraded = dict(value["contract"], schema_version=1)
        write_json(acceptance_path, seal(
            "citrus.recording_observation_binding_acceptance", "acceptance_id", downgraded))
        try:
            module.validate(root, expected, 4)
        except module.ValidationError as error:
            assert "acceptance" in str(error), str(error)
        else:
            raise AssertionError("v1 acceptance of a v2 request passed validation")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        fixture(root, version=2, unified_h5=True)
        import h5py

        with h5py.File(root / "citrus/arena_2.h5", "r+") as h5:
            h5["metadata/session"].attrs["recording_subtype"] = "dish_freeswim"
        try:
            module.validate(root, expected, 4)
        except module.ValidationError as error:
            assert "recording_subtype differs" in str(error), str(error)
        else:
            raise AssertionError("H5 session context drift passed validation")

    # Context v2 without a subtype: the omission passes and must be preserved in
    # the H5 session metadata; a substituted subtype is refused.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        fixture(root, version=2, unified_h5=True, parent_context=CONTEXT_NO_SUBTYPE)
        assert module.validate(root, expected, 4)["status"] == "pass"
        import h5py

        with h5py.File(root / "citrus/arena_3.h5", "r+") as h5:
            h5["metadata/session"].attrs["recording_subtype"] = "dish_stimulus"
        try:
            module.validate(root, expected, 4)
        except module.ValidationError as error:
            assert "present although the accepted context omits it" in str(error), str(error)
        else:
            raise AssertionError("substituted subtype passed validation")

    # Acceptance v3 + receipt v3 (live since 2026-10-12): pass with the subject
    # declaration and reference; malformed subject blocks are refused.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        fixture(root, version=2, unified_h5=True,
                subject_declaration=SUBJECT_DECLARATION, subject_reference=SUBJECT_REFERENCE)
        result = module.validate(root, expected, 4)
        assert result["status"] == "pass"
        assert {row["receipt_schema_version"] for row in result["receipts"]} == {3}

    refused = [
        # A subject identity v4 dish group: dish set, subject_id null. Valid only
        # in reference v2 (receipt v4), so receipt v3 refuses it.
        ({}, dict(SUBJECT_REFERENCE, subject_id=None, subject_type="dish_group", subject_count=4,
                  fish_reason="dish_group_subject"),
         "subject reference nullability"),
        ({}, dict(SUBJECT_REFERENCE, fish_status="collected"), "subject reference nullability"),
        ({}, dict(SUBJECT_REFERENCE, schema_version=2), "subject reference is invalid"),
        ({"fish_id": "C1A0F3D2-5B7E-4C1A-9D2E-3F4A5B6C7D8E"}, SUBJECT_REFERENCE,
         "subject declaration is invalid"),
        ({"dish_id": None}, SUBJECT_REFERENCE, "subject declaration nullability"),
    ]
    for declaration_change, reference, message in refused:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture(root, version=2, unified_h5=True,
                    subject_declaration=dict(SUBJECT_DECLARATION, **declaration_change),
                    subject_reference=reference)
            try:
                module.validate(root, expected, 4)
            except module.ValidationError as error:
                assert message in str(error), str(error)
            else:
                raise AssertionError(f"{message} passed validation")

    print("validate_recording_observation_bindings_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
