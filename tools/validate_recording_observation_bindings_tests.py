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


def seal(schema: str, id_field: str, contract: dict[str, Any]) -> dict[str, Any]:
    sha = digest(contract)
    prefix = "obsbindreq_" if id_field == "request_id" else "obsbindacc_"
    return {
        "schema_id": schema,
        "schema_version": 1,
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


def fixture(root: Path) -> None:
    import h5py

    request_refs = []
    acceptance_refs = []
    experiment_id = "citexp_fixture"
    for index, camera in enumerate(("2010093", "2010094", "2010095", "2010096"), 1):
        context = "obsctx_" + hashlib.sha256(camera.encode()).hexdigest()
        target = {
            "rig_id": "omnifin0",
            "canvas_name": "shadow",
            "arena_id": f"arena_{index}",
            "camera_id": camera,
            "source_camera_stream_id": camera,
        }
        request_contract = {
            "schema_id": "orange.citrus.recording_observation_binding_request",
            "schema_version": 1,
            "observation_context_id": context,
            "observation_identity_sha256": "sha256:" + "1" * 64,
            "observation_identity": {"fixture": camera},
            "binding_mode": "required",
            "requested_at_utc": "2026-08-13T20:00:00Z",
            "recording": {"recording_id": "fixture"},
            "target": target,
            "recording_geometry_contract": {"status": "available"},
        }
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
            "schema_version": 1,
            "status": "accepted",
            "request_id": request["request_id"],
            "request_contract_sha256": request["contract_sha256"],
            "observation_context_id": context,
            "decided_at_utc": "2026-08-13T20:00:01Z",
            "citrus_experiment_id": experiment_id,
            "citrus_session_uuid": f"session_{index}",
            "target": target,
            "planned_h5_relative_path": str(h5_relative),
        }
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
            h5.attrs["session_status"] = "COMPLETE"
            h5.attrs["session_uuid"] = f"session_{index}"
            group = h5.create_group("recording_observation_binding")
            group.attrs["schema_id"] = "citrus.recording_observation_binding_h5"
            group.attrs["schema_version"] = 1
            group.attrs["status"] = "accepted_pending_finalization"
            group.attrs["citrus_experiment_id"] = experiment_id
            dtype = h5py.string_dtype(encoding="utf-8")
            group.create_dataset("request_json", data=json.dumps(request), dtype=dtype)
            group.create_dataset("acceptance_json", data=json.dumps(acceptance), dtype=dtype)

    write_json(root / "recording_observation_bindings/request_collection.json", {
        "schema_id": "orange.recording.observation_binding_request_collection",
        "schema_version": 1,
        "status": "materialized",
        "binding_mode": "required",
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

        import h5py

        with h5py.File(root / "citrus/arena_4.h5", "r+") as h5:
            del h5["recording_observation_binding/acceptance_json"]
        try:
            module.validate(root, expected, 4)
        except module.ValidationError as error:
            assert "payloads missing" in str(error)
        else:
            raise AssertionError("missing H5 acceptance payload passed validation")
    print("validate_recording_observation_bindings_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
