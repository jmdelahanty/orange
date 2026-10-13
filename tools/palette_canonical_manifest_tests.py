#!/usr/bin/env python3
"""scripts/palette_canonical_manifest.py: the engine build's check of Palette canonical manifests.

Mirrors the record-start check in tools/model_identity_tests.cpp: the production pose manifest (v1)
and a valid v2 detect manifest pass; a v1 shape declaring v2, a tampered payload, an unpinned
version and a detect manifest carrying a pose input contract fail.

  palette_canonical_manifest_tests.py <real v1 canonical manifest>
"""

from __future__ import annotations

import copy
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from palette_canonical_manifest import payload_digest, problems  # noqa: E402


def check(doc, expect: str | None, what: str, folder: Path) -> bool:
    path = folder / "m.json"
    path.write_text(json.dumps(doc, indent=2))
    found = problems(path)
    ok = (not found) if expect is None else any(expect in line for line in found)
    print(("[PASS] " if ok else "[FAIL] ") + what + ("" if ok else f": {found}"))
    return ok


def main() -> int:
    v1 = json.loads(Path(sys.argv[1]).read_text())
    token = "sha256:" + "a" * 64
    payload = {"status": "complete", "run_id": "run", "set_id": "set", "task": "detect",
               "weights": {"relative_path": "weights/best.pt", "sha256": token},
               "onnx": {"relative_path": "exports/onnx/model.onnx", "sha256": token},
               "source_export_manifest": {"relative_path": "exports/onnx/model.onnx.manifest.json", "sha256": token}}
    v2 = {"schema_id": "palette.canonical_onnx_model_manifest", "schema_version": 2,
          "canonicalization": "canonical_json_utf8_sort_keys_compact_v1",
          "payload_sha256": "sha256:" + payload_digest(payload), "payload": payload}
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        ok &= check(v1, None, "the production pose canonical manifest (v1) passes", folder)
        ok &= check(v2, None, "a valid v2 detect manifest passes", folder)
        ok &= check(dict(v1, schema_version=2), "v2", "a v1 shape declaring v2 fails", folder)
        tampered = copy.deepcopy(v1)
        tampered["payload"]["run_id"] = "another_run"
        ok &= check(tampered, "is not the payload's digest", "a tampered payload fails", folder)
        ok &= check(dict(v1, schema_version=3), "no pinned Palette schema", "an unpinned version fails", folder)
        contract = copy.deepcopy(v2)
        contract["payload"]["pose_model_input_contract"] = {"relative_path": "c.json", "sha256": token, "payload_sha256": token}
        contract["payload_sha256"] = "sha256:" + payload_digest(contract["payload"])
        ok &= check(contract, "pose_model_input_contract", "a detect manifest with a pose input contract fails", folder)
    print("palette_canonical_manifest_tests " + ("passed" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
