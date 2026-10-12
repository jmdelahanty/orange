"""Which schema_id / schema_version pairs Orange publishes in docs/schemas, and where.

A schema file declares the identity of the documents it describes with a "schema_id" const and a
"schema_version" const in the same "properties" object, either at the root, in a root
oneOf/anyOf/allOf branch (one file per document with several line kinds, e.g. event logs), or in
a root $defs entry. Declarations deeper down describe embedded documents of other schemas and
do not count. Shared by validate_output_schemas.py and check_output_surface.py.
"""

from __future__ import annotations

import json
from pathlib import Path


def _identity(node) -> tuple[str, int] | None:
    props = node.get("properties") if isinstance(node, dict) else None
    if not isinstance(props, dict):
        return None
    sid = props.get("schema_id", {}).get("const") if isinstance(props.get("schema_id"), dict) else None
    ver = (props.get("schema_version", {}).get("const")
           if isinstance(props.get("schema_version"), dict) else None)
    if isinstance(sid, str) and isinstance(ver, int):
        return sid, ver
    return None


def published(schema_dir: Path) -> dict[tuple[str, int], tuple[str, str]]:
    """(schema_id, version) -> (file name, JSON pointer of the subschema to validate with)."""
    found: dict[tuple[str, int], tuple[str, str]] = {}
    for path in sorted(schema_dir.glob("*.schema.json")):
        try:
            schema = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            continue
        if not isinstance(schema, dict):
            continue
        candidates = [("", schema)]
        for keyword in ("oneOf", "anyOf", "allOf"):
            for branch in schema.get(keyword, []) if isinstance(schema.get(keyword), list) else []:
                candidates.append(("", branch))
        for name, sub in (schema.get("$defs") or {}).items():
            candidates.append((f"#/$defs/{name}", sub))
        for pointer, node in candidates:
            identity = _identity(node)
            if identity and identity not in found:
                found[identity] = (path.name, pointer)
    return found
