#!/usr/bin/env python3
"""Camera-free admission of fixed.declaration in headless experiment specs.

The spec's declaration (orange.recording_start_declaration v1) is the one
source of the recording's subjects and intent; see
src/session/recording_start_declaration.h.
"""

from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE_SPEC = ROOT / "experiment_specs" / "2010096_headless_timed_recording_control_smoke_a16_gpu5.json"

CONTEXTS = {
    "schema_version": 1,
    "default": {"behavior_mode": "free", "data_origin": "acquired",
                "recording_intent": "stimulus_experiment", "recording_type": "behavior"},
}
DECLARATION = {
    "schema_id": "orange.recording_start_declaration",
    "schema_version": 1,
    "declared_by": "delahantyj",
    "recording_intent": "recording_only",
    "subject_references": {"schema_version": 1},
}


def main() -> int:
    client = Path(sys.argv[1]).resolve()
    base = json.loads(BASE_SPEC.read_text())
    cases: list[tuple[str, dict, bool, str]] = []

    def case(name: str, accepted: bool, *, declaration=DECLARATION, contexts=CONTEXTS, subjects=None,
             zebrobot=None, refusal="declaration") -> None:
        spec = copy.deepcopy(base)
        fixed = spec["fixed"]
        if zebrobot is not None:
            fixed["zebrobot"] = zebrobot
        if contexts is not None:
            fixed["recording_contexts"] = copy.deepcopy(contexts)
        if declaration is not None:
            fixed["declaration"] = copy.deepcopy(declaration)
        if subjects is not None:
            fixed["subject_references"] = subjects
        cases.append((name, spec, accepted, refusal))

    case("no_declaration", True, declaration=None)
    case("blank_declaration", True)
    dish_declaration = {**DECLARATION, "subject_references": {
        "schema_version": 1, "cameras": {"2010096": {"dish_id": "dish-1", "subject_count": 1}}}}
    # A declared dish needs MetaZebrobot (2026-11-16): refused without a base_url.
    case("declared_dish", True, declaration=dish_declaration, zebrobot={"base_url": "http://zebrobot.invalid"})
    case("declared_dish_without_zebrobot", False, declaration=dish_declaration, refusal="zebrobot_not_configured")
    case("subject_references_dish_without_zebrobot", False, declaration=None,
         subjects={"schema_version": 1, "default": {"dish_id": "dish-1"}}, refusal="zebrobot_not_configured")
    case("count_only_without_zebrobot", True, declaration={**DECLARATION, "subject_references": {
        "schema_version": 1, "default": {"subject_count": 3}}})
    case("declaration_and_subject_references", False, subjects={"schema_version": 1})
    case("declaration_without_contexts", False, contexts=None)
    case("missing_declared_by", False, declaration={k: v for k, v in DECLARATION.items() if k != "declared_by"})
    case("bad_identity", False, declaration={**DECLARATION, "declared_by": "Jeremy Delahanty"})
    case("bad_intent", False, declaration={**DECLARATION, "recording_intent": "whatever"})
    case("unknown_field", False, declaration={**DECLARATION, "confirmed_by": "run_spec"})

    with tempfile.TemporaryDirectory(prefix="orange_declaration_spec_") as tmp:
        directory = Path(tmp)
        for name, spec, accepted, refusal in cases:
            spec["fixed"]["output_root"] = str(directory / "must_not_exist")
            path = directory / f"{name}.json"
            path.write_text(json.dumps(spec))
            result = subprocess.run(
                [str(client), "--mode", "local", "--experiment-spec", str(path), "--validate-experiment-spec"],
                capture_output=True, text=True, timeout=20, check=False)
            if (result.returncode == 0) != accepted:
                raise AssertionError(f"{name}: exit {result.returncode}\n{result.stdout}\n{result.stderr}")
            if not accepted and refusal not in (result.stdout + result.stderr):
                raise AssertionError(f"{name}: the refusal should name {refusal}\n{result.stderr}")
            if (directory / "must_not_exist").exists():
                raise AssertionError("admission entered recording startup")
            if accepted and "declaration" in spec["fixed"]:
                # The sealed copy is the exact file bytes, named by their sha256.
                digest = hashlib.sha256(path.read_bytes()).hexdigest()
                expected = (f"spec_sha256={digest} spec_copy=run_spec/{digest}.json "
                            f"spec_copy_bytes={len(path.read_bytes())}")
                if expected not in result.stdout:
                    raise AssertionError(f"{name}: declaration line should read {expected!r}\n{result.stdout}")
    print(f"{len(cases)} headless declaration spec cases passed (no camera launch)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
