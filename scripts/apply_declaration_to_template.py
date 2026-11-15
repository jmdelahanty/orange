#!/usr/bin/env python3
"""Apply a plan's generated declaration to a committed Orange run-spec template.

    scripts/apply_declaration_to_template.py TEMPLATE DECLARATION_DIR --out MERGED_SPEC

DECLARATION_DIR is orchard's `tools/declare.py ... --out DIR`: one
`<plan>.r<rev>.<session>.orange_declaration.json` (orange.recording_start_declaration
v2, source.kind orchard_run_plan) and its `run_plan/` files. TEMPLATE is a run
spec committed, unedited, in an Orange checkout (cameras, encoders, modes; no
subjects). The merged spec is the template plus:

  fixed.declaration           the generated declaration, unchanged
  fixed.declaration_template  {repo, commit, path, sha256} of the template
  fixed.declaration_files_root  DECLARATION_DIR, from which orange_client seals
                              the run_plan/ files after checking their hashes

The merged spec is generated, not committed: both inputs are named (the
template here, the plan by the declaration's source) and orange_client seals
its bytes as run_spec/<sha256>.json. orange_client's parser is the full check;
this step refuses what it can see early (Jeremy, 2026-11-15:
docs/recording_start_declaration_v2_scope.md).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

FORBIDDEN_TEMPLATE_KEYS = ("declaration", "declaration_template", "declaration_files_root", "subject_references")
GITHUB_REMOTE = re.compile(r"^(?:https://github\.com/|git@github\.com:|ssh://git@github\.com/)"
                           r"([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(?:\.git)?/?$")


class MergeError(RuntimeError):
    pass


def git(cwd: Path, *args: str) -> str:
    result = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise MergeError(f"git {' '.join(args)} failed in {cwd}: {result.stderr.strip()}")
    return result.stdout.strip()


def template_identity(template: Path) -> dict[str, str]:
    """{repo, commit, path, sha256}; refuses a template that is not committed and clean."""
    template = template.resolve()
    root = Path(git(template.parent, "rev-parse", "--show-toplevel"))
    relative = template.relative_to(root).as_posix()
    git(root, "ls-files", "--error-unmatch", "--", relative)
    if git(root, "status", "--porcelain", "--", relative):
        raise MergeError(f"template {relative} has uncommitted changes; commit it first")
    commit = git(root, "rev-parse", "HEAD")
    remote = git(root, "remote", "get-url", "origin")
    match = GITHUB_REMOTE.match(remote)
    if not match:
        raise MergeError(f"origin {remote!r} is not a GitHub owner/name remote")
    committed = subprocess.run(["git", "show", f"HEAD:{relative}"], cwd=root, capture_output=True, check=True).stdout
    if committed != template.read_bytes():
        raise MergeError(f"template {relative} differs from HEAD")
    return {"repo": match.group(1), "commit": commit, "path": relative,
            "sha256": hashlib.sha256(committed).hexdigest()}


def load_declaration(declaration_dir: Path) -> tuple[dict[str, Any], Path]:
    found = sorted(declaration_dir.glob("*.orange_declaration.json"))
    if len(found) != 1:
        raise MergeError(f"{declaration_dir} must hold exactly one *.orange_declaration.json (found {len(found)})")
    declaration = json.loads(found[0].read_text())
    if declaration.get("schema_version") != 2 or declaration.get("source", {}).get("kind") != "orchard_run_plan":
        raise MergeError("the declaration must be orange.recording_start_declaration v2 from an orchard_run_plan")
    return declaration, found[0]


def check_run_plan_files(declaration: dict[str, Any], declaration_dir: Path) -> None:
    """Every run_plan/ file is named by the source and hashes to its name."""
    source = declaration["source"]
    named = {source[k] for k in ("plan_copy_relative_path", "plan_json_copy_relative_path",
                                 "sign_off_copy_relative_path")}
    present = {p.relative_to(declaration_dir).as_posix()
               for p in (declaration_dir / "run_plan").rglob("*") if p.is_file()}
    if present != named:
        raise MergeError(f"run_plan/ files {sorted(present)} are not exactly the declared {sorted(named)}")
    for relative in sorted(named):
        digest = hashlib.sha256((declaration_dir / relative).read_bytes()).hexdigest()
        if Path(relative).stem != digest:
            raise MergeError(f"{relative} does not hash to its name ({digest})")


def merge(template: Path, declaration_dir: Path) -> dict[str, Any]:
    spec = json.loads(template.read_text())
    fixed = spec.get("fixed")
    if not isinstance(fixed, dict):
        raise MergeError("the template has no fixed block")
    for key in FORBIDDEN_TEMPLATE_KEYS:
        if key in fixed:
            raise MergeError(f"a template must not set fixed.{key}; the declaration supplies it")
    if "recording_contexts" not in fixed:
        raise MergeError("the template needs fixed.recording_contexts for the declared intent")
    identity = template_identity(template)
    declaration, _ = load_declaration(declaration_dir)
    check_run_plan_files(declaration, declaration_dir)
    fixed["declaration"] = declaration
    fixed["declaration_template"] = identity
    fixed["declaration_files_root"] = str(declaration_dir.resolve())
    return spec


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("template", type=Path)
    parser.add_argument("declaration_dir", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.out.exists():
        print(f"refusing to overwrite {args.out}", file=sys.stderr)
        return 2
    try:
        merged = merge(args.template, args.declaration_dir)
    except (MergeError, OSError, json.JSONDecodeError, subprocess.CalledProcessError) as exc:
        print(f"apply_declaration_to_template: {exc}", file=sys.stderr)
        return 1
    args.out.write_text(json.dumps(merged, indent=2) + "\n")
    print(f"merged {args.out} (template {merged['fixed']['declaration_template']['path']} "
          f"@ {merged['fixed']['declaration_template']['commit'][:12]})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
