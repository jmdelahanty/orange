#!/usr/bin/env python3
"""Declaration v2 end to end without cameras: merge a plan's generated
declaration into a committed template, then orange_client --validate-experiment-spec.

Uses tools/fixtures/declaration_v2/orchard_v2 (orchard declare.py output at
ccbdd67, run plan v2, the committed example plan revision 5, session S1).
"""

from __future__ import annotations

import copy
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MERGE = ROOT / "scripts" / "apply_declaration_to_template.py"
FIXTURE = ROOT / "tools" / "fixtures" / "declaration_v2" / "orchard_v2"
BASE_SPEC = ROOT / "experiment_specs" / "2010096_headless_timed_recording_control_smoke_a16_gpu5.json"
CONTEXTS = {"schema_version": 1, "default": {"behavior_mode": "free", "data_origin": "acquired",
                                             "recording_intent": "recording_only", "recording_type": "behavior"}}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(list(args), cwd=cwd, capture_output=True, text=True, check=False)


def make_template_repo(base: Path, must_not_exist: Path) -> Path:
    repo = base / "orange_repo"
    repo.mkdir()
    for args in (("init", "-q"), ("remote", "add", "origin", "https://github.com/jmdelahanty/orange.git"),
                 ("config", "user.email", "t@example.invalid"), ("config", "user.name", "t")):
        require(run("git", *args, cwd=repo).returncode == 0, f"git {args}")
    spec = json.loads(BASE_SPEC.read_text())
    spec["fixed"]["recording_contexts"] = CONTEXTS
    spec["fixed"]["output_root"] = str(must_not_exist)
    template = repo / "experiment_specs" / "templates" / "fourcam_plan_template.json"
    template.parent.mkdir(parents=True)
    template.write_text(json.dumps(spec, indent=2) + "\n")
    require(run("git", "add", ".", cwd=repo).returncode == 0, "git add")
    require(run("git", "commit", "-q", "-m", "template", cwd=repo).returncode == 0, "git commit")
    return template


def validate(client: Path, spec: Path) -> subprocess.CompletedProcess[str]:
    return run(str(client), "--mode", "local", "--experiment-spec", str(spec), "--validate-experiment-spec")


def main() -> int:
    client = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="orange_declaration_v2_") as tmp:
        base = Path(tmp)
        must_not_exist = base / "must_not_exist"
        template = make_template_repo(base, must_not_exist)
        declared = base / "declared"
        shutil.copytree(FIXTURE, declared)

        merged = base / "merged.json"
        result = run(sys.executable, str(MERGE), str(template), str(declared), "--out", str(merged))
        require(result.returncode == 0, f"merge: {result.stdout}{result.stderr}")
        spec = json.loads(merged.read_text())
        identity = spec["fixed"]["declaration_template"]
        require(identity["repo"] == "jmdelahanty/orange" and len(identity["commit"]) == 40
                and identity["path"] == "experiment_specs/templates/fourcam_plan_template.json",
                f"template identity recorded: {identity}")
        require(spec["fixed"]["declaration_files_root"] == str(declared.resolve()), "files root recorded")
        check = validate(client, merged)
        require(check.returncode == 0, f"a merged v2 spec validates:\n{check.stdout}\n{check.stderr}")
        require("schema_version=2 source_kind=orchard_run_plan declaration_files=3" in check.stdout, check.stdout)
        require(not must_not_exist.exists(), "validation never starts a recording")

        # The merge step refuses an edited template, and a stray run_plan file.
        template.write_text(template.read_text().replace("\n}", ',\n  "notes": "edited"\n}', 1))
        result = run(sys.executable, str(MERGE), str(template), str(declared), "--out", str(base / "m2.json"))
        require(result.returncode != 0 and "uncommitted" in result.stderr, f"edited template refused: {result.stderr}")
        run("git", "checkout", "--", ".", cwd=template.parent.parent.parent)
        stray = declared / "run_plan" / "extra.json"
        stray.write_text("{}")
        result = run(sys.executable, str(MERGE), str(template), str(declared), "--out", str(base / "m3.json"))
        require(result.returncode != 0 and "not exactly the declared" in result.stderr, f"stray file: {result.stderr}")
        stray.unlink()

        # orange_client's own checks, on hand-made merged specs.
        def variant(name: str, mutate) -> subprocess.CompletedProcess[str]:
            v = copy.deepcopy(spec)
            mutate(v)
            path = base / f"{name}.json"
            path.write_text(json.dumps(v))
            return validate(client, path)

        cases = {
            "tree_dirty": lambda v: v["fixed"]["declaration"]["source"].update(tree_dirty=True),
            "unknown_rule": lambda v: v["fixed"]["declaration"]["source"].update(
                plan_hash_rule="canonical_json_utf8_sort_keys_compact_v1"),
            "repo_url": lambda v: v["fixed"]["declaration"]["source"].update(
                repo="https://github.com/jmdelahanty/orchard.git"),
            "sign_off_by": lambda v: v["fixed"]["declaration"]["source"]["sign_off"].update(by="someone"),
            "fractional_time": lambda v: v["fixed"]["declaration"]["source"]["sign_off"].update(
                at_utc="2026-11-18T14:55:00.5Z"),
            "bool_revision": lambda v: v["fixed"]["declaration"]["source"].update(plan_revision=True),
            "old_session_key": lambda v: v["fixed"]["declaration"]["source"].update(
                session_id=v["fixed"]["declaration"]["source"].pop("plan_session_id")),
            "no_template": lambda v: v["fixed"].pop("declaration_template"),
            "no_files_root": lambda v: v["fixed"].pop("declaration_files_root"),
            "blank_with_count": lambda v: v["fixed"]["declaration"]["subject_references"]["cameras"].update(
                {"2010096": {"blank": True, "subject_count": 1}}),
            "v1_with_blank": lambda v: v["fixed"]["declaration"].update(schema_version=1) or
                v["fixed"]["declaration"].pop("source"),
        }
        for name, mutate in cases.items():
            result = variant(name, mutate)
            require(result.returncode != 0 and "declaration" in (result.stdout + result.stderr),
                    f"{name} must be refused:\n{result.stdout}\n{result.stderr}")

        # A run_plan file whose bytes no longer match its name is refused at load.
        plan_copy = declared / spec["fixed"]["declaration"]["source"]["plan_copy_relative_path"]
        plan_copy.write_bytes(plan_copy.read_bytes() + b"# edited\n")
        result = validate(client, merged)
        require(result.returncode != 0 and "does not hash" in (result.stdout + result.stderr),
                f"a tampered plan copy is refused:\n{result.stderr}")
    print(f"declaration_v2_merge_tests passed ({len(cases) + 4} refusals checked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
