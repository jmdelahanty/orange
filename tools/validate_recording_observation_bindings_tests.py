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
            subject_reference: dict[str, Any] | None = None,
            receipt_version: int = 3) -> None:
    """version 2 carries the frozen context in each request (and a start
    snapshot that freezes it); unified_h5 writes the Citrus 9aa6d57 layout
    (/evidence/recording_binding + /metadata/session) instead of the root one.
    subject_declaration makes the acceptances v3 (version must be 2);
    subject_reference makes the receipts v3, or receipt_version (4: reference v2),
    with a citsess_ session and citrus_artifacts."""
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
            "schema_version": receipt_version if subject_reference is not None else 1,
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


SUBJECT_V4 = REPO_ROOT / "tools" / "fixtures" / "citrus_subject_v4"
PINS = {  # agent-contracts main 2aaa7bc; the digests Citrus, the sealer and Palette pin
    "citrus_subject_reference_v2.schema.json":
        "30f42c9d49d1ba040b509d6743144aefacda63365c5ca4261524506b35931eb9",
    "citrus_recording_observation_finalized_receipt_v4.schema.json":
        "47d2251244ed6bf18cf5e65c360da2b4a809afbdee1864a3cef75b0b44a76b83",
}
GROUP_REFERENCE_V2 = dict(SUBJECT_REFERENCE, schema_version=2, subject_id=None, subject_type="dish_group",
                          subject_count=4, fish_reason="dish_group_subject")
FISH = "0f3a6c2e-1b4d-4e8f-9a7b-2c5d6e7f8a9b"
INDIVIDUAL_REFERENCE_V2 = dict(SUBJECT_REFERENCE, schema_version=2, subject_id=FISH,
                               fish_reason="create_transport_failure")


def subject_v4_tests(module: Any, expected: set[str]) -> None:
    """Receipt v4 / reference v2 (agent-contracts #105, witnesses #115)."""
    import jsonschema

    contracts = REPO_ROOT / "contracts"
    for name, sha in PINS.items():
        assert hashlib.sha256((contracts / name).read_bytes()).hexdigest() == sha, f"pin changed: {name}"
    reference_schema = json.loads((contracts / "citrus_subject_reference_v2.schema.json").read_text())
    receipt_schema = json.loads(
        (contracts / "citrus_recording_observation_finalized_receipt_v4.schema.json").read_text())
    reference_validator = jsonschema.Draft202012Validator(reference_schema)
    receipt_validator = jsonschema.Draft202012Validator(receipt_schema)

    # The 11 reference v2 fixtures: the pinned schema and Orange's rules agree.
    for verdict in ("accepted", "refused"):
        for path in sorted((SUBJECT_V4 / "reference_v2" / verdict).glob("*.json")):
            document = json.loads(path.read_text())
            assert reference_validator.is_valid(document) == (verdict == "accepted"), path.name
            try:
                module.validate_subject_reference(document, path.stem, 2)
                ok = True
            except module.ValidationError:
                ok = False
            assert ok == (verdict == "accepted"), f"Orange's reference v2 rules disagree: {path.name}"
    # Citrus's sealed receipt v4 witnesses: schema-valid, rules pass, declaration agrees.
    receipts = sorted((SUBJECT_V4 / "witnesses").glob("*.receipt.json"))
    assert len(receipts) == 9
    for path in receipts + [SUBJECT_V4 / "example_receipt_v4.json"]:
        receipt = json.loads(path.read_text())
        assert receipt_validator.is_valid(receipt), path.name
        module.validate_subject_reference(receipt["contract"]["subject_reference"], path.name, 2)
        if path.name.endswith(".receipt.json"):
            acceptance_path = path.with_name(path.name.replace(".receipt.json", ".acceptance.json"))
            acceptance = json.loads(acceptance_path.read_text())
            assert module.subject_declaration_disagreement(acceptance["contract"], receipt["contract"]) is None

    # Whole recordings with receipt v4.
    declared = dict(SUBJECT_DECLARATION, fish_id=FISH)
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        fixture(root, version=2, unified_h5=True, subject_declaration=declared,
                subject_reference=INDIVIDUAL_REFERENCE_V2, receipt_version=4)
        result = module.validate(root, expected, 4)
        assert {row["receipt_schema_version"] for row in result["receipts"]} == {4}
        assert result["subject_declaration_disagreements"] == []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        group_declared = dict(SUBJECT_DECLARATION, subject_type="dish_group", subject_count=4)
        fixture(root, version=2, unified_h5=True, subject_declaration=group_declared,
                subject_reference=GROUP_REFERENCE_V2, receipt_version=4)
        assert module.validate(root, expected, 4)["status"] == "pass"
    with tempfile.TemporaryDirectory() as tmp:
        # The declaration names no fish, the receipt an individual: recorded, not refused.
        root = Path(tmp)
        fixture(root, version=2, unified_h5=True, subject_declaration=SUBJECT_DECLARATION,
                subject_reference=INDIVIDUAL_REFERENCE_V2, receipt_version=4)
        result = module.validate(root, expected, 4)
        assert result["status"] == "pass" and len(result["subject_declaration_disagreements"]) == 4
    refused = [
        (dict(SUBJECT_DECLARATION), dict(GROUP_REFERENCE_V2, schema_version=1), 4, "subject reference is invalid"),
        (dict(SUBJECT_DECLARATION), GROUP_REFERENCE_V2, 3, "subject reference is invalid"),
        (dict(SUBJECT_DECLARATION), dict(GROUP_REFERENCE_V2, subject_count=1), 4, "dish_group"),
        (dict(SUBJECT_DECLARATION), dict(INDIVIDUAL_REFERENCE_V2, subject_id="not-a-uuid"), 4,
         "subject reference is invalid"),
        (None, INDIVIDUAL_REFERENCE_V2, 4, "acceptance v3"),
    ]
    for declaration, reference, version, message in refused:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture(root, version=2, unified_h5=True, subject_declaration=declaration,
                    subject_reference=reference, receipt_version=version)
            try:
                module.validate(root, expected, 4)
            except module.ValidationError as error:
                assert message in str(error), str(error)
            else:
                raise AssertionError(f"{message} passed validation")


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

    subject_v4_tests(module, expected)
    print("validate_recording_observation_bindings_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
