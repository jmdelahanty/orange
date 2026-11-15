#!/usr/bin/env python3
"""Empty-background v1 fixtures against the published schemas, in jsonschema.

Everything under tools/fixtures/empty_background_v1/valid must validate and everything under
invalid/ must fail: capture_* against orange_empty_background_capture_v1 (which $refs
orange.image_levels.v1 by $id), snapshot_* against orange_recording_empty_background_v1. The
schema examples are checked as valid documents too. The generated C++ readers are checked on the
same files by empty_background_schema_fixture_tests (C++).

  empty_background_schema_fixture_tests.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

try:
    from jsonschema import Draft202012Validator
    from referencing import Registry, Resource
except ImportError:
    sys.exit("empty_background_schema_fixture_tests: python3 needs the jsonschema package")

REPO = Path(__file__).resolve().parents[1]
SCHEMAS = REPO / "docs" / "schemas"
FIXTURES = REPO / "tools" / "fixtures" / "empty_background_v1"
CAPTURE = "orange_empty_background_capture_v1.schema.json"
SNAPSHOT = "orange_recording_empty_background_v1.schema.json"


def registry() -> Registry:
    resources = []
    for path in sorted(SCHEMAS.glob("*.schema.json")):
        schema = json.loads(path.read_text(encoding="utf-8"))
        resource = Resource.from_contents(schema)
        resources.append((path.name, resource))
        if isinstance(schema.get("$id"), str):
            resources.append((schema["$id"], resource))
    return Registry().with_resources(resources)


def validator_for(name: str, reg: Registry) -> Draft202012Validator:
    schema = json.loads((SCHEMAS / name).read_text(encoding="utf-8"))
    Draft202012Validator.check_schema(schema)
    return Draft202012Validator(schema, registry=reg)


def main() -> int:
    reg = registry()
    validators = {"capture_": validator_for(CAPTURE, reg), "snapshot_": validator_for(SNAPSHOT, reg)}
    examples = {
        SCHEMAS / "examples" / "orange_empty_background_capture_v1_example.json": "capture_",
        SCHEMAS / "examples" / "orange_recording_empty_background_v1_example.json": "snapshot_",
    }
    failures = 0
    checked = {"valid": 0, "invalid": 0}

    def kind_of(path: Path) -> str:
        if path in examples:
            return examples[path]
        for prefix in validators:
            if path.name.startswith(prefix):
                return prefix
        raise SystemExit(f"{path}: fixture name must start with capture_ or snapshot_")

    valid = sorted((FIXTURES / "valid").glob("*.json")) + list(examples)
    invalid = sorted((FIXTURES / "invalid").glob("*.json"))
    for path in valid:
        errors = list(validators[kind_of(path)].iter_errors(json.loads(path.read_text(encoding="utf-8"))))
        checked["valid"] += 1
        if errors:
            failures += 1
            print(f"[FAIL] valid {path.name}: {errors[0].message}")
        else:
            print(f"[PASS] valid {path.name}")
    for path in invalid:
        errors = list(validators[kind_of(path)].iter_errors(json.loads(path.read_text(encoding="utf-8"))))
        checked["invalid"] += 1
        if errors:
            print(f"[PASS] invalid {path.name} refused ({errors[0].message[:120]})")
        else:
            failures += 1
            print(f"[FAIL] invalid {path.name} validated")
    if checked["valid"] < 5 or checked["invalid"] < 15:
        print(f"[FAIL] fixture sets incomplete: {checked}")
        failures += 1
    print("empty_background_schema_fixture_tests.py: " + ("PASS" if failures == 0 else "FAIL") +
          f" ({checked['valid']} valid, {checked['invalid']} invalid)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
