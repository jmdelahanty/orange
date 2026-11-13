#!/usr/bin/env python3
"""Tests for scripts/commissioning_record.py (stdlib only; jsonschema used when installed)."""

from __future__ import annotations

import contextlib
import io
import json
import os
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import commissioning_record as cr  # noqa: E402

SCHEMA = REPO / "docs" / "schemas" / "orange_commissioning_record_v1.schema.json"


def run(*argv: str) -> tuple[int, dict | None, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = cr.main(list(argv))
    text = out.getvalue()
    return code, (json.loads(text) if text.strip() else None), err.getvalue()


def ok(*argv: str) -> dict:
    code, value, err = run(*argv)
    assert code == 0, f"{argv[0]} failed: {err}"
    assert value is not None
    return value


def refused(expect: str, *argv: str) -> None:
    code, _, err = run(*argv)
    assert code == 2, f"{argv[0]} should be refused ({expect}), got {code}"
    assert expect in err, f"expected {expect!r} in {err!r}"


def validate_schema(record_dir: Path) -> None:
    try:
        import jsonschema
    except ImportError:
        print("  (jsonschema not installed; schema check skipped)")
        return
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    record = json.loads((record_dir / "record.json").read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator(schema).validate(record)


def make_session(root: Path, session_id: str) -> Path:
    session = root / session_id
    session.mkdir(parents=True)
    (session / "session_index.json").write_text('{"session": "' + session_id + '"}\n')
    return session


def test_full_lifecycle(tmp: Path) -> None:
    sessions_root = tmp / "sessions"
    make_session(sessions_root, "calsess_2026_11_13T00_46_39Z_shadow")
    make_session(sessions_root, "calsess_2026_11_13T00_05_56Z_shadow")
    result = tmp / "orange_gui_guided_capture_x.json"
    result.write_text(json.dumps({
        "status": "pass",
        "persistence": {"session_id": "calsess_2026_11_13T00_46_39Z_shadow"},
        "stage_records": [{"persistence": {"session_id": "calsess_2026_11_13T00_46_39Z_shadow"}}],
    }))
    log = tmp / "orange.log"
    log.write_text("log line\n")
    report_dir = tmp / "holder_fixture_x"
    (report_dir / "overlays").mkdir(parents=True)
    (report_dir / "overlays" / "Cam1.png").write_bytes(b"png")
    (report_dir / "validation_report.json").write_text("{}")
    overlay = tmp / "scale_overlays_4cams.png"
    overlay.write_bytes(b"overlay")

    opened = ok("open", "--rig", "omnifin0", "--reason", "Recommission after projector move",
                "--operator", "jeremy", "--agent", "claude:test", "--root", str(tmp / "commissioning"))
    record = opened["record_dir"]
    assert Path(record).name.startswith("omnifin0_") and Path(record).name.endswith("_recommission_after_projector_move")

    state = ok("declare", "--record", record, "ir_illuminators=unplugged", "nir_filters=off",
               "--source", "Jeremy at the rig", "--by", "jeremy")["state"]
    assert state["ir_illuminators"]["value"] == "unplugged"
    refused("key=value", "declare", "--record", record, "Bad Key=1", "--source", "x")

    step = ok("step", "--record", record, "--name", "projected_surface_scale", "--status", "passed",
              "--exit-code", "0", "--result-json", str(result), "--log", str(log),
              "--log", str(tmp / "missing.log"), "--reference-dir", f"holder_report={report_dir}",
              "--session", "calsess_2026_11_13T00_05_56Z_shadow", "--sessions-root", str(sessions_root),
              "--", "scripts/run_gui_guided_capture_smoke.sh", "--execute")
    assert step["step_index"] == 1
    assert step["sessions"] == ["calsess_2026_11_13T00_05_56Z_shadow", "calsess_2026_11_13T00_46_39Z_shadow"]
    data = json.loads((Path(record) / "record.json").read_text())
    s1 = data["steps"][0]
    assert s1["launcher_result_status"] == "pass"
    assert s1["command"] == ["scripts/run_gui_guided_capture_smoke.sh", "--execute"]
    assert s1["state_at_step"]["nir_filters"]["value"] == "off"
    assert all(sess["session_index_sha256"].startswith("sha256:") for sess in s1["sessions"])
    assert {f["role"] for f in s1["files"]} == {"launcher_result", "log"}
    assert any("missing" in f for f in s1["files"])
    assert s1["referenced_dirs"][0]["file_count"] == 2
    assert (Path(record) / s1["dir"] / "logs" / "orange.log").is_file()

    failed = ok("step", "--record", record, "--name", "arena_centering", "--status", "failed", "--exit-code", "1")
    assert failed["step_index"] == 2
    refused("cannot be accepted", "verdict", "--record", record, "--step", "2", "--reviewer", "claude",
            "--verdict", "accepted", "--evidence", str(overlay))
    refused("needs --evidence", "verdict", "--record", record, "--step", "1", "--reviewer", "claude",
            "--verdict", "accepted")
    refused("no step 3", "verdict", "--record", record, "--step", "3", "--reviewer", "jeremy", "--verdict", "rejected")
    ok("verdict", "--record", record, "--step", "1", "--reviewer", "claude", "--verdict", "accepted",
       "--note", "all gates pass", "--evidence", str(overlay))
    ok("attach", "--record", record, "--role", "brightness_map", str(log))

    third = ok("step", "--record", record, "--name", "wet_map", "--status", "passed")
    refused("without a verdict: [3]", "close", "--record", record)
    ok("verdict", "--record", record, "--step", str(third["step_index"]), "--reviewer", "jeremy",
       "--verdict", "characterization")
    validate_schema(Path(record))

    release = tmp / "commissioning.json"
    release.write_text('{"release_id": "r"}')
    closed = ok("close", "--record", record, "--release-manifest", str(release))
    assert closed["status"] == "closed"
    validate_schema(Path(record))
    readme = (Path(record) / "README.md").read_text()
    assert "projected_surface_scale" in readme and "claude: accepted (all gates pass)" in readme
    assert "ir_illuminators | unplugged" in readme
    ok("verify", "--record", record)
    refused("not open", "declare", "--record", record, "x=1", "--source", "late")
    assert not os.access(Path(record) / "record.json", os.W_OK) or os.geteuid() == 0

    # Tampering after close is detected.
    target = Path(record) / "record.json"
    target.chmod(0o644)
    target.write_text(target.read_text() + " ")
    refused("record changed after close", "verify", "--record", record)


def test_record_from_environment(tmp: Path) -> None:
    opened = ok("open", "--rig", "omnifin0", "--reason", "env test", "--root", str(tmp / "c2"))
    os.environ["ORANGE_COMMISSIONING_RECORD"] = opened["record_dir"]
    try:
        assert ok("show")["status"] == "open"
    finally:
        del os.environ["ORANGE_COMMISSIONING_RECORD"]
    refused("no record", "show")


def launcher_function(script: str, name: str) -> str:
    text = (REPO / "scripts" / script).read_text(encoding="utf-8")
    start = text.index(f"{name}() {{")
    end = text.index("\n}\n", start) + 3
    return text[start:end]


def test_guided_launcher_record_step(tmp: Path) -> None:
    """The launcher's EXIT-trap function appends exactly one step, passed or failed."""
    import subprocess
    record = ok("open", "--rig", "omnifin0", "--reason", "launcher test", "--root", str(tmp / "c3"))["record_dir"]
    result = tmp / "result.json"
    result.write_text('{"status": "fail"}')
    log = tmp / "orange.log"
    log.write_text("x\n")
    authority = tmp / "authority.json"
    authority.write_text('{"foreground_gray_u8": 24}')
    function = launcher_function("run_gui_guided_capture_smoke.sh", "record_step")
    script = f"""
set -euo pipefail
REPO_ROOT={REPO}
RECORD_DIR={record}
RECORD_STEP_NAME=guided_capture_test
RECORD_DONE=0
EXECUTE=1
RESULT_JSON={result}
ORANGE_LOG={log}
CITRUS_LOG={tmp / "missing_citrus.log"}
INTENSITY_AUTHORITY_JSON={authority}
ORIGINAL_ARGS=(--execute --save)
{function}
record_step 3
record_step 0
"""
    subprocess.run(["bash", "-c", script], check=True)
    data = json.loads((Path(record) / "record.json").read_text())
    assert len(data["steps"]) == 1, "the trap function records once"
    step = data["steps"][0]
    assert step["status"] == "failed" and step["exit_code"] == 3
    assert step["launcher_result_status"] == "fail"
    assert step["command"][-2:] == ["--execute", "--save"]
    roles = sorted(f["role"] for f in step["files"])
    assert roles == ["launcher_result", "log", "log", "projector_intensity_authority"], roles


def test_arena_launcher_record_step(tmp: Path) -> None:
    import subprocess
    record = ok("open", "--rig", "omnifin0", "--reason", "arena test", "--root", str(tmp / "c4"))["record_dir"]
    result = tmp / "result.json"
    result.write_text('{"status": "pass", "persistence": {"session_id": "calsess_x"}}')
    function = launcher_function("run_gui_arena_centering_commissioning.sh", "record_step")
    script = f"""
set -euo pipefail
REPO_ROOT={REPO}
RECORD_DIR={record}
RECORD_STEP_NAME=arena_centering_bootstrap_centers
RECORD_DONE=0
EXECUTE=1
RESULT_JSON={result}
ORANGE_LOG={tmp / "o.log"}
CITRUS_LOG={tmp / "c.log"}
ORIGINAL_ARGS=(--execute --bootstrap-centers-only)
{function}
record_step 0
"""
    subprocess.run(["bash", "-c", script], check=True)
    step = json.loads((Path(record) / "record.json").read_text())["steps"][0]
    assert step["status"] == "passed" and step["name"] == "arena_centering_bootstrap_centers"
    assert [s["session_id"] for s in step["sessions"]] == ["calsess_x"]
    assert step["sessions"][0]["session_index_sha256"] is None


def test_python_launcher_step(tmp: Path) -> None:
    record = ok("open", "--rig", "omnifin0", "--reason", "py test", "--root", str(tmp / "c5"))["record_dir"]
    out = tmp / "holder_fixture_x"
    (out / "overlays").mkdir(parents=True)
    (out / "run_manifest.json").write_text("{}")
    (out / "overlays" / "a.png").write_bytes(b"x")
    with contextlib.redirect_stdout(io.StringIO()):
        code = cr.append_launcher_step(
            record, "holder_fixture_validation", 4,
            files=[("run_manifest", out / "run_manifest.json"), ("validation_report", out / "missing.json")],
            reference_dirs=[("report_dir", out)], command=["run_holder_fixture_validation.py", "--execute"])
    assert code == 0
    step = json.loads((Path(record) / "record.json").read_text())["steps"][0]
    assert step["status"] == "failed" and step["exit_code"] == 4
    assert [f["role"] for f in step["files"]] == ["run_manifest"]
    assert step["referenced_dirs"][0]["file_count"] == 2


def main() -> int:
    tests = [test_full_lifecycle, test_record_from_environment, test_guided_launcher_record_step,
             test_arena_launcher_record_step, test_python_launcher_step]
    failures = 0
    for test in tests:
        with tempfile.TemporaryDirectory() as tmp:
            try:
                test(Path(tmp))
                print(f"[PASS] {test.__name__}")
            except Exception as error:  # noqa: BLE001
                failures += 1
                print(f"[FAIL] {test.__name__}: {error!r}")
            finally:
                for path in Path(tmp).rglob("*"):
                    try:
                        path.chmod(0o755 if path.is_dir() else 0o644)
                    except OSError:
                        pass
                Path(tmp).chmod(0o755)
    print("commissioning_record_tests " + ("passed" if not failures else f"FAILED ({failures})"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
