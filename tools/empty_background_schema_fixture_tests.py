#!/usr/bin/env python3
"""Empty-background fixtures against the published schemas, in jsonschema.

Per version N (1, and the version 2 draft): everything under tools/fixtures/empty_background_vN/valid
must validate and everything under invalid/ must fail: capture_* against
orange_empty_background_capture_vN (which $refs orange.image_levels.v1, and from version 2
orange.requester.v1, by $id), snapshot_* against orange_recording_empty_background_vN. The v1
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
# (fixture dir, capture schema, snapshot schema, examples, minimum valid, minimum invalid)
SETS = [
    ("empty_background_v1", "orange_empty_background_capture_v1.schema.json",
     "orange_recording_empty_background_v1.schema.json",
     ["orange_empty_background_capture_v1_example.json", "orange_recording_empty_background_v1_example.json"], 5, 15),
    ("empty_background_v2", "orange_empty_background_capture_v2.schema.json",
     "orange_recording_empty_background_v2.schema.json", [], 6, 21),
]


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


def check_set(reg: Registry, fixture_dir: str, capture: str, snapshot: str, example_names: list,
              min_valid: int, min_invalid: int) -> int:
    fixtures = REPO / "tools" / "fixtures" / fixture_dir
    validators = {"capture_": validator_for(capture, reg), "snapshot_": validator_for(snapshot, reg)}
    examples = {SCHEMAS / "examples" / name: ("capture_" if "capture" in name else "snapshot_")
                for name in example_names}
    failures = 0
    checked = {"valid": 0, "invalid": 0}

    def kind_of(path: Path) -> str:
        if path in examples:
            return examples[path]
        for prefix in validators:
            if path.name.startswith(prefix):
                return prefix
        raise SystemExit(f"{path}: fixture name must start with capture_ or snapshot_")

    valid = sorted((fixtures / "valid").glob("*.json")) + list(examples)
    invalid = sorted((fixtures / "invalid").glob("*.json"))
    for path in valid:
        errors = list(validators[kind_of(path)].iter_errors(json.loads(path.read_text(encoding="utf-8"))))
        checked["valid"] += 1
        if errors:
            failures += 1
            print(f"[FAIL] {fixture_dir} valid {path.name}: {errors[0].message}")
        else:
            print(f"[PASS] {fixture_dir} valid {path.name}")
    for path in invalid:
        errors = list(validators[kind_of(path)].iter_errors(json.loads(path.read_text(encoding="utf-8"))))
        checked["invalid"] += 1
        if errors:
            print(f"[PASS] {fixture_dir} invalid {path.name} refused ({errors[0].message[:120]})")
        else:
            failures += 1
            print(f"[FAIL] {fixture_dir} invalid {path.name} validated")
    if checked["valid"] < min_valid or checked["invalid"] < min_invalid:
        print(f"[FAIL] {fixture_dir} fixture sets incomplete: {checked}")
        failures += 1
    print(f"{fixture_dir}: {checked['valid']} valid, {checked['invalid']} invalid, {failures} failures")
    return failures


def main() -> int:
    reg = registry()
    failures = sum(check_set(reg, *entry) for entry in SETS)
    print("empty_background_schema_fixture_tests.py: " + ("PASS" if failures == 0 else "FAIL"))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
