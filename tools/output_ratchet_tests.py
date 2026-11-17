#!/usr/bin/env python3
"""Tests for scripts/check_output_names.py and scripts/check_output_surface.py on a scratch tree."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
NAMES = REPO / "scripts" / "check_output_names.py"
SURFACE = REPO / "scripts" / "check_output_surface.py"


def run(script: Path, root: Path, *args: str) -> subprocess.CompletedProcess:
    env = dict(os.environ, ORANGE_RATCHET_ROOT=str(root))
    return subprocess.run([sys.executable, str(script), *args], env=env,
                          capture_output=True, text=True, check=False)


def require(ok: bool, message: str) -> None:
    if not ok:
        raise AssertionError(message)


def make_tree(root: Path, source: str, allowed: dict[str, str] | None = None) -> None:
    (root / "contracts").mkdir(parents=True, exist_ok=True)
    (root / "src").mkdir(exist_ok=True)
    (root / "tools").mkdir(exist_ok=True)
    shutil.copy(REPO / "contracts" / "frame_identity_v1.json", root / "contracts")
    (root / "contracts" / "output_names_allowlist.json").write_text(
        json.dumps({"allowed": allowed or {}}), encoding="utf-8")
    (root / "src" / "writer.cpp").write_text(source, encoding="utf-8")


GOOD = '''
void write() {
    out << "recording_frame_id,timestamp,timestamp_sys,stream_frame_id\\n";
    json j = {{"schema_id", "orange.test"}, {"schema_version", 1}, {"camera_frame_id", 3},
              {"keyframe_video_frame_indices", v}, {"noncontiguous_recording_frame_ids", 0},
              {"frame_rate_num", 100}};
    // a comment may say frame_id
}
'''


def test_names() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        make_tree(root, GOOD)
        result = run(NAMES, root)
        require(result.returncode == 0, f"conforming names must pass: {result.stdout}")

        for bad, expected in (('{"frame_id", 1}', "forbidden name 'frame_id'"),
                              ('"recording_frame_id,local_frame_id,timestamp\\n"', "forbidden"),
                              ('{"source_frame_index", 0}', "without a frame_identity_v1 suffix"),
                              ('{"first_frame_num", 0}', "without a frame_identity_v1 suffix")):
            make_tree(root, GOOD + f"\nauto x = {bad};\n")
            result = run(NAMES, root)
            require(result.returncode == 1 and expected in result.stdout,
                    f"{bad} must fail with {expected!r}: {result.stdout}")

        make_tree(root, GOOD + '\nauto x = {"source_frame_index", 0};\n',
                  {"source_frame_index": "owned by another schema"})
        require(run(NAMES, root).returncode == 0, "an allowlisted name with a reason passes")
        make_tree(root, GOOD, {"source_frame_index": "owned by another schema"})
        result = run(NAMES, root)
        require(result.returncode == 1 and "no longer used" in result.stdout,
                f"an unused allowlist entry must fail: {result.stdout}")
    print("[PASS] names")


def git(root: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t", *args],
                   check=True, capture_output=True)


def test_surface() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        make_tree(root, GOOD)
        require(run(SURFACE, root, "--update").returncode == 0, "snapshot update")
        require(run(SURFACE, root).returncode == 0, "unchanged surface must pass")
        git(root, "init", "-q")
        git(root, "add", ".")
        git(root, "commit", "-q", "-m", "base")

        renamed = GOOD.replace("stream_frame_id\\n", "stream_frame_id,bytes\\n")
        make_tree(root, renamed)
        result = run(SURFACE, root)
        require(result.returncode == 1 and "+ src/writer.cpp csv_headers" in result.stdout,
                f"a header change must fail until the snapshot is updated: {result.stdout}")

        require(run(SURFACE, root, "--update").returncode == 0, "snapshot update")
        result = run(SURFACE, root, "--base", "HEAD")
        require(result.returncode == 1 and "no schema version changed" in result.stdout,
                f"a name change without a version bump must fail: {result.stdout}")

        make_tree(root, renamed.replace('{"schema_version", 1}', '{"schema_version", 2}'))
        require(run(SURFACE, root, "--update").returncode == 0, "snapshot update")
        result = run(SURFACE, root, "--base", "HEAD")
        require(result.returncode == 0, f"a name change with a version bump passes: {result.stdout}")

        git(root, "add", ".")
        git(root, "commit", "-q", "-m", "bumped")
        (root / "src" / "writer.cpp").write_text(renamed.replace('{"schema_version", 1}', '{"schema_version", 2}')
                                                 .replace('{"frame_rate_num", 100}', '{}'))
        (root / "src" / "moved.cpp").write_text('auto y = {"frame_rate_num", 100};\n')
        require(run(SURFACE, root, "--update").returncode == 0, "snapshot update")
        result = run(SURFACE, root, "--base", "HEAD")
        require(result.returncode == 0, f"a name moving between files is no change: {result.stdout}")

        (root / "docs" / "schemas").mkdir(parents=True, exist_ok=True)
        (root / "docs" / "schemas" / "orange_gen_v1.schema.json").write_text(json.dumps({
            "properties": {"schema_id": {"const": "orange.gen"}, "schema_version": {"const": 1},
                           "generated_key": {"type": "integer"}}}))
        (root / "src" / "gen_writer.cpp").write_text(
            '#include "orange_outputs/orange_gen_v1.h"\nauto j = gen::to_json(value);\n')
        (root / "src" / "gen_reader.cpp").write_text(
            '#include "orange_outputs/orange_gen_v1.h"\nbool ok = gen::parse(j, &value, &error);\n')
        require(run(SURFACE, root, "--update").returncode == 0, "snapshot update")
        snapshot = json.loads((root / "tools" / "output_surface_snapshot.json").read_text())
        require("generated_key" in snapshot["src/gen_writer.cpp"]["json_keys"]
                and ["orange.gen", "1"] in snapshot["src/gen_writer.cpp"]["schemas"],
                f"a generated include is credited with its schema: {snapshot.get('src/gen_writer.cpp')}")
        require("src/gen_reader.cpp" not in snapshot,
                f"a file that only parses is not a writer: {snapshot.get('src/gen_reader.cpp')}")

        # Several generated headers: only the schema whose to_json the file calls by namespace.
        (root / "CMakeLists.txt").write_text('set(ORANGE_GENERATED_SCHEMAS\n    "orange_gen_v1:gen_v1"\n'
                                             '    "orange_read_v1:read_v1"\n)\n')
        (root / "docs" / "schemas" / "orange_read_v1.schema.json").write_text(json.dumps({
            "properties": {"schema_id": {"const": "orange.read"}, "schema_version": {"const": 1},
                           "read_only_key": {"type": "integer"}}}))
        (root / "src" / "mixed.cpp").write_text(
            '#include "orange_outputs/orange_gen_v1.h"\n#include "orange_outputs/orange_read_v1.h"\n'
            'namespace out = orange::outputs::gen::gen_v1;\nnamespace in = orange::outputs::gen::read_v1;\n'
            'bool ok = in::parse(j, &source, &error);\nauto k = out::to_json(value);\n')
        require(run(SURFACE, root, "--update").returncode == 0, "snapshot update")
        snapshot = json.loads((root / "tools" / "output_surface_snapshot.json").read_text())
        mixed = snapshot.get("src/mixed.cpp", {})
        require("generated_key" in mixed.get("json_keys", []) and "read_only_key" not in mixed.get("json_keys", [])
                and ["orange.read", "1"] not in mixed.get("schemas", []),
                f"a file with two generated includes is credited only with the one it writes: {mixed}")
    print("[PASS] surface")


VALIDATE = REPO / "scripts" / "validate_output_schemas.py"
TOKEN = "sha256:" + "a" * 64


def test_digest_form() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        (folder / "ok.json").write_text(json.dumps({"artifact": {"sha256": TOKEN}, "file_sha256": TOKEN}))
        result = subprocess.run([sys.executable, str(VALIDATE), str(folder)], capture_output=True, text=True)
        require(result.returncode == 0, f"tokens under digest names pass: {result.stdout}")
        for bad, expected in (({"payload_sha256": "a" * 64}, "is not a sha256: token"),
                              ({"blob": {"digest": TOKEN}}, "not sha256 / *_sha256"),
                              ({"files": [{"sha256": "not-a-digest"}]}, "is not a sha256: token")):
            (folder / "bad.json").write_text(json.dumps(bad))
            result = subprocess.run([sys.executable, str(VALIDATE), str(folder)], capture_output=True, text=True)
            require(result.returncode == 1 and expected in result.stdout,
                    f"{bad} must fail with {expected!r}: {result.stdout}")
        (folder / "bad.json").unlink()
        # Family A is allowlisted by schema_id: engine manifests are named <engine stem>.manifest.json.
        (folder / "detect_a16_int8.manifest.json").write_text(
            json.dumps({"schema_id": "orange.tensorrt_engine_manifest", "engine": {"sha256": "a" * 64}}))
        result = subprocess.run([sys.executable, str(VALIDATE), str(folder)], capture_output=True, text=True)
        require(result.returncode == 0, f"allowlisted family passes: {result.stdout}")
        (folder / "other.manifest.json").write_text(
            json.dumps({"schema_id": "palette.other_manifest", "engine": {"sha256": "a" * 64}}))
        result = subprocess.run([sys.executable, str(VALIDATE), str(folder)], capture_output=True, text=True)
        require(result.returncode == 1 and "other.manifest.json" in result.stdout,
                f"the schema_id entry must not cover another schema in a like-named file: {result.stdout}")
    print("[PASS] digest form")


def test_embedded_blocks() -> None:
    """Published blocks without a schema_id (snapshot models.<serial>.detect/.pose) are validated by path."""
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        (folder / "recording_snapshot_start.json").write_text(
            json.dumps({"models": {"2010093": {"detect": {"enabled": True}}}}))
        result = subprocess.run([sys.executable, str(VALIDATE), str(folder)], capture_output=True, text=True)
        require(result.returncode == 1 and "orange_recording_detect_model_v1.schema.json" in result.stdout
                and "'runtime' is a required property" in result.stdout,
                f"an incomplete snapshot detect block must fail: {result.stdout}")
        (folder / "recording_snapshot_start.json").unlink()
        (folder / "other.json").write_text(json.dumps({"models": {"2010093": {"detect": {"enabled": True}}}}))
        result = subprocess.run([sys.executable, str(VALIDATE), str(folder)], capture_output=True, text=True)
        require(result.returncode == 0, f"the same path in another file is not a snapshot: {result.stdout}")
    print("[PASS] embedded blocks")


def main() -> int:
    test_names()
    test_surface()
    test_digest_form()
    test_embedded_blocks()
    print("output_ratchet_tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
