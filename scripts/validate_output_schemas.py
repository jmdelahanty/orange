#!/usr/bin/env python3
"""Validate Orange output files against the schemas Orange publishes in docs/schemas.

Walks the given files or folders. Every root document (a JSON file, a JSONL line) that carries a
schema_id and an integer schema_version is validated against the published schema with that
schema_id and version. A root document of a published family whose version is newer than every
published version fails (the code writes a version nobody published); an older one is counted.
Schema ids without any published schema are only counted.

Objects nested inside documents that also carry a schema_id are validated too but reported
separately: many are references to another document (a digest and a path) that copy its
schema_id without being it. --strict-nested makes their failures count.

  validate_output_schemas.py <recording folder or files>...  [--json report.json]

Needs the jsonschema package (pip install jsonschema).
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path
from urllib.parse import urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
from published_schemas import published  # noqa: E402

try:
    from jsonschema import Draft202012Validator
    from referencing import Registry, Resource
except ImportError:  # pragma: no cover - reported to the caller
    sys.exit("validate_output_schemas: needs the jsonschema package (pip install jsonschema)")

REPO = Path(__file__).resolve().parents[1]
SCHEMAS = REPO / "docs" / "schemas"
MAX_ERRORS_PER_DOCUMENT = 3


def load_schemas() -> tuple[dict[tuple[str, int], dict], set[str], Registry]:
    by_name = {p.name: json.loads(p.read_text(encoding="utf-8")) for p in sorted(SCHEMAS.glob("*.schema.json"))}

    def retrieve(uri: str) -> Resource:
        name = Path(urlparse(uri).path).name or uri
        if name not in by_name:
            raise LookupError(f"schema {uri} is not in docs/schemas")
        return Resource.from_contents(by_name[name])

    registry = Registry(retrieve=retrieve).with_resources(
        (name, Resource.from_contents(schema)) for name, schema in by_name.items())
    # Validate with a $ref into the publishing file so relative $refs and $defs resolve there.
    by_identity = {identity: {"$ref": f"{name}{pointer}"}
                   for identity, (name, pointer) in published(SCHEMAS).items()}
    families = {sid for sid, _ in by_identity}
    return by_identity, families, registry


def documents(value, location: str):
    """Yield (location, object) for value and every nested object that declares a schema."""
    if isinstance(value, dict):
        if isinstance(value.get("schema_id"), str) and isinstance(value.get("schema_version"), int):
            yield location, value
        for key, child in value.items():
            yield from documents(child, f"{location}/{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from documents(child, f"{location}[{index}]")


def files(targets: list[Path]):
    for target in targets:
        if target.is_dir():
            yield from sorted(p for p in target.rglob("*") if p.suffix in (".json", ".jsonl") and p.is_file())
        else:
            yield target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("targets", nargs="+", type=Path)
    parser.add_argument("--json", type=Path, help="write a machine-readable report")
    parser.add_argument("--strict-nested", action="store_true",
                        help="fail on nested objects that do not match their schema_id's schema")
    args = parser.parse_args()

    by_identity, families, registry = load_schemas()
    validators: dict[tuple[str, int], Draft202012Validator] = {}
    validated: Counter = Counter()
    unpublished: Counter = Counter()
    failures: list[str] = []
    nested_mismatch: Counter = Counter()
    nested_lines: list[str] = []
    older_unpublished: Counter = Counter()

    for path in files(args.targets):
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            failures.append(f"{path}: unreadable ({exc})")
            continue
        if path.suffix == ".jsonl":
            parsed = []
            for number, line in enumerate(text.splitlines(), start=1):
                if line.strip():
                    try:
                        parsed.append((f"{path}:{number}", json.loads(line)))
                    except json.JSONDecodeError as exc:
                        failures.append(f"{path}:{number}: invalid JSON ({exc})")
        else:
            try:
                parsed = [(str(path), json.loads(text))]
            except json.JSONDecodeError as exc:
                failures.append(f"{path}: invalid JSON ({exc})")
                continue
        for where, document in parsed:
            for location, obj in documents(document, where):
                identity = (obj["schema_id"], obj["schema_version"])
                nested = location != where
                if identity not in by_identity:
                    newest = max((v for s, v in by_identity if s == identity[0]), default=None)
                    if newest is None:
                        unpublished[identity[0]] += 1
                    elif identity[1] < newest or nested:
                        older_unpublished[f"{identity[0]} v{identity[1]}"] += 1
                    else:
                        failures.append(f"{location}: {identity[0]} v{identity[1]} has no schema "
                                        "file in docs/schemas")
                    continue
                if identity not in validators:
                    validators[identity] = Draft202012Validator(by_identity[identity], registry=registry)
                errors = sorted(validators[identity].iter_errors(obj), key=lambda e: list(e.path))
                if nested and errors and not args.strict_nested:
                    nested_mismatch[f"{identity[0]} v{identity[1]}"] += 1
                    nested_lines.append(f"{location}: {errors[0].message[:200]}")
                    continue
                validated[f"{identity[0]} v{identity[1]}"] += 1
                for error in errors[:MAX_ERRORS_PER_DOCUMENT]:
                    failures.append(f"{location}: {identity[0]} v{identity[1]} "
                                    f"({by_identity[identity]['$ref']}): {error.message[:300]} at "
                                    f"/{'/'.join(str(p) for p in error.path)}")

    for name, count in sorted(validated.items()):
        print(f"validated {count:6d}  {name}")
    for name, count in sorted(unpublished.items()):
        print(f"no schema {count:6d}  {name}")
    for name, count in sorted(older_unpublished.items()):
        print(f"older     {count:6d}  {name} (predates the published versions)")
    for name, count in sorted(nested_mismatch.items()):
        print(f"nested    {count:6d}  {name} objects that are not full documents (references?)")
    for line in failures:
        print(f"FAIL {line}")
    if args.json:
        args.json.write_text(json.dumps({"validated": validated, "unpublished": unpublished,
                                         "older_unpublished": older_unpublished,
                                         "nested_mismatch": nested_mismatch,
                                         "nested_examples": nested_lines[:50],
                                         "failures": failures}, indent=2) + "\n")
    print(f"validate_output_schemas: {sum(validated.values())} documents validated, "
          f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
