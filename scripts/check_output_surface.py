#!/usr/bin/env python3
"""Ratchet: what Orange writes may change only on purpose, together with a schema version.

Extracts the output surface of Orange's producer sources (src/ and the recorder tool):
  - csv_headers: string literals (adjacent literals joined) that look like a CSV header;
  - json_keys:   keys written through nlohmann initializer lists ({"key", ...}),
                 assignments (["key"] = ...) and hand-built JSON text (\\"key\\":);
  - schemas:     schema_id / schema_version pairs, with k-constants resolved. A declared
                 version of a schema family published in docs/schemas must have its file.

A file that writes through generated code (#include "orange_outputs/<schema>.h" and a call of
the generated to_json, see tools/schema_codegen) is credited with that schema's property names
and schema_id / version, since the key literals live in the generated header. A file that only
reads through the generated parser is not credited: it writes nothing.

Published blocks without a schema_id (published_schemas.EMBEDDED, e.g. the snapshot's
models.<serial>.detect) are recorded under "embedded <path>" with the path's keys, which their
writers pass as arguments.

  check_output_surface.py                 compare with tools/output_surface_snapshot.json
  check_output_surface.py --update        rewrite the snapshot (commit it with the change)
  check_output_surface.py --base <ref>    also require that, if the csv_headers or json_keys
                                          over all files differ from the snapshot at <ref>,
                                          the schemas differ too (a version was bumped or
                                          added); a name moving between files is no change

Static extraction: it sees names in the source, not files on disk, so it complements the
writers' own tests rather than replacing them. No dependencies beyond the standard library.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_output_names import producer_files, strip_comments  # noqa: E402
from published_schemas import EMBEDDED, published  # noqa: E402

# ORANGE_RATCHET_ROOT points the checks at another tree (used by their tests).
REPO = Path(os.environ.get("ORANGE_RATCHET_ROOT") or Path(__file__).resolve().parents[1])
SNAPSHOT = REPO / "tools" / "output_surface_snapshot.json"
SNAPSHOT_REL = "tools/output_surface_snapshot.json"

LITERAL_RUN = re.compile(r'(?:"(?:[^"\\\n]|\\.)*"\s*)+')
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
CSV_HEADER = re.compile(r"^[a-z][a-z0-9_]*(,[a-z][a-z0-9_]*){2,},?$")
INIT_KEY = re.compile(r'\{\s*"([a-z][a-z0-9_]*)"\s*,')
ASSIGN_KEY = re.compile(r'\[\s*"([a-z][a-z0-9_]*)"\s*\]\s*=[^=]')
TEXT_KEY = re.compile(r'\\"([a-z][a-z0-9_]*)\\"\s*:')
SCHEMA_PAIR = re.compile(
    r'"schema_id"\s*,\s*("[a-z][a-z0-9_.]*"|[A-Za-z_:][A-Za-z0-9_:]*)\s*\}\s*,\s*'
    r'\{\s*"schema_version"\s*,\s*([0-9]+|[A-Za-z_:][A-Za-z0-9_:]*)')
TEXT_SCHEMA_PAIR = re.compile(
    r'\\"schema_id\\":\s*\\"([a-z][a-z0-9_.]*)\\",(?:\\n)?[\s"<>\w\\]*?\\"schema_version\\":\s*([0-9]+)')
INT_CONSTANT = re.compile(r"\b(k[A-Za-z0-9_]+)\s*=\s*([0-9]+)\s*;")
STR_CONSTANT = re.compile(r'\b(k[A-Za-z0-9_]+)\s*=\s*"([a-z][a-z0-9_.]*)"\s*;')
GENERATED_INCLUDE = re.compile(r'#include\s+"orange_outputs/([a-z0-9_]+)\.h"')


def schema_surface(name: str) -> tuple[set[str], set[tuple[str, str]]]:
    """Property names and schema_id / version consts declared by docs/schemas/<name>.schema.json."""
    keys: set[str] = set()
    schemas: set[tuple[str, str]] = set()

    def walk(node) -> None:
        if isinstance(node, dict):
            props = node.get("properties")
            if isinstance(props, dict):
                keys.update(props)
                sid = props.get("schema_id", {})
                ver = props.get("schema_version", {})
                if isinstance(sid, dict) and isinstance(ver, dict) and "const" in sid and "const" in ver:
                    schemas.add((sid["const"], str(ver["const"])))
            for child in node.values():
                walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)

    walk(json.loads((REPO / "docs" / "schemas" / f"{name}.schema.json").read_text(encoding="utf-8")))
    return keys, schemas


def constants(texts: dict[str, str]) -> tuple[dict[str, str], dict[str, str]]:
    ints: dict[str, str] = {}
    strs: dict[str, str] = {}
    for text in texts.values():
        ints.update(dict(INT_CONSTANT.findall(text)))
        strs.update(dict(STR_CONSTANT.findall(text)))
    return ints, strs


def resolve(token: str, table: dict[str, str]) -> str:
    if token.startswith('"'):
        return token.strip('"')
    return table.get(token.split("::")[-1], token)


def extract() -> dict[str, dict[str, list]]:
    texts = {
        str(p.relative_to(REPO)): strip_comments(p.read_text(encoding="utf-8", errors="replace"))
        for p in producer_files()
    }
    ints, strs = constants(texts)
    surface: dict[str, dict[str, list]] = {}
    for rel, text in texts.items():
        headers: set[str] = set()
        for run in LITERAL_RUN.findall(text):
            joined = "".join(LITERAL.findall(run)).replace("\\n", "")
            if CSV_HEADER.match(joined):
                headers.add(joined)
        keys = set(INIT_KEY.findall(text)) | set(ASSIGN_KEY.findall(text))
        for literal in LITERAL.findall(text):
            keys.update(TEXT_KEY.findall(literal))
        schemas = {
            (resolve(sid, strs), resolve(ver, ints)) for sid, ver in SCHEMA_PAIR.findall(text)
        }
        schemas.update(TEXT_SCHEMA_PAIR.findall(text))
        for name in GENERATED_INCLUDE.findall(text) if "to_json(" in text else ():
            generated_keys, generated_schemas = schema_surface(name)
            keys |= generated_keys
            schemas |= generated_schemas
        if headers or keys or schemas:
            surface[rel] = {
                "csv_headers": sorted(headers),
                "json_keys": sorted(keys),
                "schemas": sorted([sid, str(ver)] for sid, ver in schemas),
            }
    for _pattern, path, schema_file in EMBEDDED:
        keys = sorted(k for k in path.strip("/").split("/") if not k.startswith("<"))
        surface[f"embedded {path}"] = {"csv_headers": [], "json_keys": keys, "schemas": [[schema_file, "path"]]}
    return dict(sorted(surface.items()))


def describe(old: dict, new: dict) -> list[str]:
    lines: list[str] = []
    for rel in sorted(set(old) | set(new)):
        for kind in ("csv_headers", "json_keys", "schemas"):
            before = {json.dumps(v) for v in old.get(rel, {}).get(kind, [])}
            after = {json.dumps(v) for v in new.get(rel, {}).get(kind, [])}
            for value in sorted(after - before):
                lines.append(f"+ {rel} {kind}: {value}")
            for value in sorted(before - after):
                lines.append(f"- {rel} {kind}: {value}")
    return lines


def published_versions() -> dict[str, set[str]]:
    """schema_id -> versions that have a schema in docs/schemas (see published_schemas.py)."""
    versions: dict[str, set[str]] = {}
    for sid, ver in published(REPO / "docs" / "schemas"):
        versions.setdefault(sid, set()).add(str(ver))
    return versions


def unpublished(surface: dict) -> list[str]:
    """Declared versions of a published schema family that have no schema file."""
    published = published_versions()
    missing = []
    for rel, entry in surface.items():
        for sid, ver in entry["schemas"]:
            if sid in published and ver.isdigit() and ver not in published[sid]:
                missing.append(f"{rel}: {sid} v{ver} is written but docs/schemas has only "
                               f"v{', v'.join(sorted(published[sid]))}")
    return missing


def all_schemas(surface: dict) -> set[str]:
    return {json.dumps(s) for entry in surface.values() for s in entry["schemas"]}


def all_names(surface: dict) -> dict[str, set[str]]:
    """The output names over every producer file: moving a name between files changes nothing."""
    return {kind: {name for entry in surface.values() for name in entry[kind]}
            for kind in ("csv_headers", "json_keys")}


def names_changed(old: dict, new: dict) -> list[str]:
    before, after = all_names(old), all_names(new)
    changed = []
    for kind in ("csv_headers", "json_keys"):
        changed += [f"+{name}" for name in sorted(after[kind] - before[kind])]
        changed += [f"-{name}" for name in sorted(before[kind] - after[kind])]
    return changed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--update", action="store_true", help="rewrite the snapshot")
    parser.add_argument("--base", help="git ref whose snapshot the version-bump rule compares to")
    args = parser.parse_args()

    current = extract()
    if args.update:
        SNAPSHOT.write_text(json.dumps(current, indent=1, sort_keys=True) + "\n", encoding="utf-8")
        print(f"check_output_surface: wrote {SNAPSHOT_REL} ({len(current)} files)")
        return 0

    snapshot = json.loads(SNAPSHOT.read_text(encoding="utf-8"))
    status = 0
    diff = describe(snapshot, current)
    if diff:
        print("\n".join(diff))
        print("check_output_surface: Orange's output names differ from the snapshot. If this is "
              "intended, bump the schema version of each changed output, run "
              "scripts/check_output_surface.py --update and commit the snapshot with the change.")
        status = 1

    missing = unpublished(current)
    if missing:
        print("\n".join(missing))
        print("check_output_surface: publish the schema file for each version Orange writes "
              "(Palette pins docs/schemas byte for byte)")
        status = 1

    if args.base:
        try:
            base_text = subprocess.run(
                ["git", "-C", str(REPO), "show", f"{args.base}:{SNAPSHOT_REL}"],
                check=True, capture_output=True, text=True).stdout
        except subprocess.CalledProcessError:
            print(f"check_output_surface: no snapshot at {args.base}; version-bump rule skipped")
            base_text = ""
        if base_text:
            base = json.loads(base_text)
            changed = names_changed(base, snapshot)
            if changed and all_schemas(base) == all_schemas(snapshot):
                print("check_output_surface: output names changed since "
                      f"{args.base} ({', '.join(changed)}) but no schema version changed")
                status = 1
    if status == 0:
        print(f"check_output_surface: passed ({len(current)} producer files)")
    return status


if __name__ == "__main__":
    sys.exit(main())
