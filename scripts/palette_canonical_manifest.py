#!/usr/bin/env python3
"""Check a Palette canonical ONNX model manifest before an engine names it.

The manifest must declare version 2 and be valid against the pinned Palette schema
(contracts/palette_canonical_onnx_model_manifest_v2.schema.json, contracts/README.md; version 1,
with a bare-hex payload_digest, was retired on 2026-10-13), and its payload_sha256 must be the
SHA-256 token of the payload's canonical JSON (canonical_json_utf8_sort_keys_compact_v1: sorted keys, compact, UTF-8). Orange's
record start checks the same in C++ (src/model_identity.cpp) and refuses on failure.

  palette_canonical_manifest.py <manifest.json>...     exit 1 when any is not valid

Needs the jsonschema package.
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

CONTRACTS = Path(__file__).resolve().parents[1] / "contracts"
SCHEMA_ID = "palette.canonical_onnx_model_manifest"


def payload_digest(payload) -> str:
    text = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def problems(path: Path) -> list[str]:
    """Why the manifest at `path` may not be named by an engine; empty when it may."""
    try:
        from jsonschema import Draft202012Validator
    except ImportError:
        return ["the jsonschema package is needed to check canonical manifests"]
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        return [f"unreadable: {exc}"]
    if not isinstance(manifest, dict) or manifest.get("schema_id") != SCHEMA_ID:
        return [f"schema_id is not {SCHEMA_ID}"]
    version = manifest.get("schema_version")
    if version != 2 or isinstance(version, bool):
        return [f"schema_version {version!r} is not accepted: Orange accepts {SCHEMA_ID} v2 only"]
    schema = json.loads((CONTRACTS / "palette_canonical_onnx_model_manifest_v2.schema.json").read_text(encoding="utf-8"))
    found = [f"not valid against {SCHEMA_ID} v2: {e.message} at /{'/'.join(map(str, e.path))}"
             for e in Draft202012Validator(schema).iter_errors(manifest)]
    if found:
        return found
    declared = manifest["payload_sha256"]
    expected = "sha256:" + payload_digest(manifest["payload"])
    if declared != expected:
        return [f"payload digest {declared} is not the payload's digest {expected}"]
    return []


def require_valid(path: Path) -> None:
    """Exit with the reasons when `path` is not a valid canonical manifest (engine build scripts)."""
    found = problems(path)
    if found:
        sys.exit(f"Palette canonical manifest {path} cannot be named by an engine:\n  " + "\n  ".join(found))


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    status = 0
    for name in sys.argv[1:]:
        found = problems(Path(name))
        print(("OK   " if not found else "FAIL ") + name)
        for line in found:
            print("     " + line)
        status |= bool(found)
    return status


if __name__ == "__main__":
    sys.exit(main())
