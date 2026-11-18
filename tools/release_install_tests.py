#!/usr/bin/env python3
"""Tests for scripts/install_orange_release.sh (test mode: --no-root --prefix)
and the wrappers' installed-release check (refusals only; acceptance needs a
root-owned /opt release and is checked on the rig).

A fake staged release lives in a scratch git repository at
targets/releases/<tag>, tagged like build_orange_release.sh tags it.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
INSTALLER = REPO_ROOT / "scripts" / "install_orange_release.sh"
WRAPPERS = {
    "orange_gui_validation_wrapper.sh": ("--orange-bin", "orange"),
    "orange_local_benchmark_wrapper.sh": ("--orange-client", "orange_client"),
}


def run(cmd, cwd=None, check=True):
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if check and result.returncode != 0:
        raise AssertionError(f"{cmd} failed ({result.returncode}):\n{result.stdout}\n{result.stderr}")
    return result


def make_repo(root: Path) -> Path:
    repo = root / "repo"
    repo.mkdir()
    run(["git", "init", "-q"], cwd=repo)
    run(["git", "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-q", "--allow-empty", "-m", "c"], cwd=repo)
    return repo


def stage_release(repo: Path, tag: str, on_fork: bool = True, payload: str = "v1") -> Path:
    commit = run(["git", "rev-parse", "HEAD"], cwd=repo).stdout.strip()
    stage = repo / "targets" / "releases" / tag
    (stage / "bin").mkdir(parents=True)
    (stage / "share" / "orange" / "scripts").mkdir(parents=True)
    for name in ("orange", "orange_client", "external_recorder_ipc_probe_native"):
        path = stage / "bin" / name
        path.write_text(f"#!/bin/sh\necho {name} {payload}\n")
        path.chmod(0o755)
    (stage / "share" / "orange" / "scripts" / "ptp_stack.sh").write_text("#!/bin/sh\n")
    (stage / "INSTALLED.json").write_text(json.dumps({
        "schema_id": "orange.installed_release", "schema_version": 1,
        "release": tag, "tag": tag,
        "source": {"commit": commit, "on_fork": on_fork, "tracked_tree_clean": True},
        "installed_at": None, "installed_by": None,
    }))
    lines = []
    for path in sorted(p for p in stage.rglob("*") if p.is_file() and p.name not in ("INSTALLED.json", "SHA256SUMS")):
        rel = path.relative_to(stage).as_posix()
        lines.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {rel}\n")
    (stage / "SHA256SUMS").write_text("".join(lines))
    run(["git", "tag", "-f", tag, commit], cwd=repo)
    return stage


def install(prefix: Path, stage: Path, *extra, check=True):
    return run(["bash", str(INSTALLER), "--no-root", "--prefix", str(prefix),
                "--release-dir", str(stage), *extra], check=check)


def make_writable(path: Path) -> None:
    for p in [path, *path.rglob("*")]:
        if not p.is_symlink():
            p.chmod(p.stat().st_mode | stat.S_IWUSR)


def test_install_flip_rollback_and_refusals() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        repo = make_repo(root)
        prefix = root / "opt_orange"
        try:
            first = stage_release(repo, "orange-v2026.11.17")
            install(prefix, first)
            dest = prefix / "releases" / "orange-v2026.11.17"
            assert os.readlink(prefix / "current") == "releases/orange-v2026.11.17"
            assert not (prefix / "previous").exists()
            doc = json.loads((dest / "INSTALLED.json").read_text())
            assert doc["installed_at"] and doc["installed_by"], doc
            assert not os.access(dest / "bin" / "orange", os.W_OK), "installed binary is writable"
            assert not (dest.stat().st_mode & 0o222), "release folder is writable"
            run(["bash", str(INSTALLER), "--prefix", str(prefix), "--verify"])

            # A file written into the release after the install (a root-run
            # Python's __pycache__) fails --verify.
            scripts_dir = dest / "share" / "orange" / "scripts"
            scripts_dir.chmod(0o755)
            (scripts_dir / "__pycache__").mkdir()
            (scripts_dir / "__pycache__" / "x.cpython-310.pyc").write_bytes(b"pyc")
            added = run(["bash", str(INSTALLER), "--prefix", str(prefix), "--verify"], check=False)
            assert added.returncode != 0 and "not in SHA256SUMS: share/orange/scripts/__pycache__/x.cpython-310.pyc" in added.stderr, added.stderr
            shutil.rmtree(scripts_dir / "__pycache__")
            scripts_dir.chmod(0o555)
            run(["bash", str(INSTALLER), "--prefix", str(prefix), "--verify"])

            # Same tag again: refused (releases are immutable).
            again = install(prefix, first, check=False)
            assert again.returncode != 0 and "already installed" in again.stderr, again.stderr

            # A second release flips current; previous names the first.
            second = stage_release(repo, "orange-v2026.11.17.2", payload="v2")
            install(prefix, second, "--link-recorder")
            assert os.readlink(prefix / "current") == "releases/orange-v2026.11.17.2"
            assert os.readlink(prefix / "previous") == "releases/orange-v2026.11.17"
            rec = prefix / "bin" / "external_recorder_ipc_probe_native"
            assert os.readlink(rec) == "../current/bin/external_recorder_ipc_probe_native"
            assert "v2" in rec.read_text()
            listing = run(["bash", str(INSTALLER), "--prefix", str(prefix), "--list"]).stdout
            assert "* orange-v2026.11.17.2" in listing and "- orange-v2026.11.17 " in listing, listing

            run(["bash", str(INSTALLER), "--no-root", "--prefix", str(prefix), "--rollback"])
            assert os.readlink(prefix / "current") == "releases/orange-v2026.11.17"
            assert os.readlink(prefix / "previous") == "releases/orange-v2026.11.17.2"
            assert "v1" in rec.read_text(), "recorder link must follow current"

            # Tampered staged file: refused before anything is copied.
            third = stage_release(repo, "orange-v2026.11.17.3")
            (third / "bin" / "orange").write_text("#!/bin/sh\necho tampered\n")
            bad = install(prefix, third, check=False)
            assert bad.returncode != 0 and "SHA256SUMS" in bad.stderr, bad.stderr
            assert not (prefix / "releases" / "orange-v2026.11.17.3").exists()

            # Tag naming another commit: refused.
            fourth = stage_release(repo, "orange-v2026.11.17.4")
            run(["git", "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-q", "--allow-empty", "-m", "d"], cwd=repo)
            run(["git", "tag", "-f", "orange-v2026.11.17.4", "HEAD"], cwd=repo)
            moved = install(prefix, fourth, check=False)
            assert moved.returncode != 0 and "names" in moved.stderr, moved.stderr

            # Without --no-root, a non-root install is refused outright.
            if os.geteuid() != 0:
                plain = run(["bash", str(INSTALLER), "--prefix", str(prefix), "--release-dir", str(second)], check=False)
                assert plain.returncode != 0 and "sudo" in plain.stderr, plain.stderr
        finally:
            if prefix.exists():
                make_writable(prefix)
            shutil.rmtree(root / "repo" / "targets", ignore_errors=True)


def test_unpushed_release_refused_outside_test_mode() -> None:
    if os.geteuid() == 0:
        return
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        repo = make_repo(root)
        stage = stage_release(repo, "orange-v2026.11.18", on_fork=False)
        prefix = root / "p"
        install(prefix, stage)  # test mode allows it
        make_writable(prefix)
        text = INSTALLER.read_text()
        assert 'built from a commit not on the fork' in text


def test_wrappers_refuse_user_owned_release() -> None:
    """A release folder the user owns (anything outside root-owned /opt) is refused."""
    with tempfile.TemporaryDirectory() as tmp:
        fake = Path(tmp) / "orange-v2026.11.17" / "bin"
        fake.mkdir(parents=True)
        for wrapper, (flag, binary) in WRAPPERS.items():
            path = fake / binary
            path.write_text("#!/bin/sh\n")
            path.chmod(0o755)
            result = run(["bash", str(REPO_ROOT / "scripts" / wrapper), flag, str(path), "--dry-run"], check=False)
            assert result.returncode != 0, f"{wrapper} accepted {path}"
            assert "Refusing" in result.stderr, result.stderr
        # The release check itself: a path under /opt/orange/releases that is not
        # a real, root-owned release is refused.
        script = REPO_ROOT / "scripts" / "orange_gui_validation_wrapper.sh"
        fn = script.read_text().split("installed_release_dir_for_bin() {", 1)[1].split("\n}\n", 1)[0]
        probe = (
            'ORANGE_RELEASES_ROOT="/opt/orange/releases"\n'
            "installed_release_dir_for_bin() {" + fn + "\n}\n"
            'installed_release_dir_for_bin /opt/orange/releases/orange-v2099.01.01/bin/orange orange && exit 9 || exit 0\n'
        )
        result = run(["bash", "-c", probe], check=False)
        assert result.returncode == 0, "nonexistent release accepted"


def test_launcher_bin_dir_helper() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        rel = Path(tmp) / "orange-v2026.11.17"
        scripts = rel / "share" / "orange" / "scripts"
        scripts.mkdir(parents=True)
        (rel / "bin").mkdir()
        (rel / "INSTALLED.json").write_text("{}")
        shutil.copy(REPO_ROOT / "scripts" / "orange_bin_dir.sh", scripts / "orange_bin_dir.sh")
        out = run(["bash", "-c", f'source "{scripts}/orange_bin_dir.sh"; echo "$ORANGE_BIN_DIR|$ORANGE_RELEASE_DIR"']).stdout.strip()
        assert out == f"{rel}/bin|{rel}", out
        out = run(["bash", "-c", f'source "{REPO_ROOT}/scripts/orange_bin_dir.sh"; echo "$ORANGE_BIN_DIR|$ORANGE_RELEASE_DIR"']).stdout.strip()
        assert out == f"{REPO_ROOT}/targets/release|", out


def test_release_library_check() -> None:
    """check_release_libraries.py: refuses a /home runpath, records outside libs."""
    checker = REPO_ROOT / "scripts" / "check_release_libraries.py"
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / "orange-v2026.11.17"
        (stage / "bin").mkdir(parents=True)
        (stage / "lib").mkdir()
        good = stage / "bin" / "true_copy"
        shutil.copy("/bin/true", good)
        record = Path(tmp) / "libs.json"
        run([sys.executable, str(checker), str(stage), "--record", str(record)])
        doc = json.loads(record.read_text())
        libc = [e for e in doc["binaries"]["bin/true_copy"] if e["soname"].startswith("libc.so")]
        assert libc and libc[0]["sha256"].startswith("sha256:") and libc[0]["path"].startswith("/"), doc
        if shutil.which("patchelf"):
            bad = stage / "bin" / "home_runpath"
            shutil.copy("/bin/true", bad)
            run(["patchelf", "--set-rpath", "/home/someone/.local/lib", str(bad)])
            result = run([sys.executable, str(checker), str(stage)], check=False)
            assert result.returncode == 1 and "runpath names /home/someone" in result.stderr, result.stderr
            bad.unlink()
        # Missing library: a binary that needs a soname nothing provides.
        if shutil.which("patchelf"):
            missing = stage / "bin" / "needs_missing"
            shutil.copy("/bin/true", missing)
            run(["patchelf", "--add-needed", "liborange_does_not_exist.so.1", str(missing)])
            result = run([sys.executable, str(checker), str(stage)], check=False)
            assert result.returncode == 1 and "not found" in result.stderr, result.stderr


def test_root_wrappers_never_write_python_bytecode() -> None:
    for wrapper in WRAPPERS:
        text = (REPO_ROOT / "scripts" / wrapper).read_text()
        assert "\nexport PYTHONDONTWRITEBYTECODE=1\n" in text[:600], wrapper


def test_gui_window_layout_is_kept_in_the_data_root_as_the_invoker() -> None:
    """ImGui wrote imgui.ini to the current folder as root (a release run left a
    root-owned ~/orange_data/imgui.ini). Orange turns ImGui's own save off,
    loads and saves <data root>/imgui.ini, and writes it as the sudo invoker."""
    text = (REPO_ROOT / "src" / "gx_helper.h").read_text()
    assert "io.IniFilename = nullptr;" in text
    assert 'orange::data_root::resolve(&error)' in text and '/ "imgui.ini"' in text
    assert "options.owner = std::make_pair(uid, gid);" in text
    assert "gx_imgui_save_ini();" in text and "gx_imgui_save_ini(true);" in text
    for path in (REPO_ROOT / "src").rglob("*"):
        if path.suffix in (".cpp", ".h") and path.name != "gx_helper.h":
            assert "IniFilename" not in path.read_text(errors="replace"), path


def test_release_script_ships_cudart() -> None:
    text = (REPO_ROOT / "scripts" / "build_orange_release.sh").read_text()
    assert "NATIVE_RUNPATH='$ORIGIN/../lib;/opt/orange/lib/ffmpeg-nvidia/lib'" in text
    assert '"$STAGE.partial/lib/libcudart.so.13"' in text
    assert "check_release_libraries.py" in text


def test_release_ships_every_helper_and_finds_support_files_from_the_binary() -> None:
    """A release run found no scripts/ptp_stack.sh (two lookups bypassed
    src/runtime_paths.h) and had no nic_thermal_monitor (a helper orange starts
    from its own folder). Every add_dependencies(orange|orange_client <helper>)
    must ship, and no source builds a support path from the current folder."""
    import re
    cmake = (REPO_ROOT / "CMakeLists.txt").read_text()
    helpers = set()
    for m in re.finditer(r"add_dependencies\((orange|orange_client)\s+([^)]+)\)", cmake):
        helpers.update(m.group(2).split())
    script = (REPO_ROOT / "scripts" / "build_orange_release.sh").read_text()
    shipped = re.search(r"^SHIPPED=\(([^)]*)\)", script, re.M).group(1).split()
    assert helpers and helpers <= set(shipped), f"helpers not shipped: {sorted(helpers - set(shipped))}"
    offenders = []
    for path in sorted((REPO_ROOT / "src").rglob("*")):
        if path.suffix not in (".cpp", ".h") or path.name == "runtime_paths.h":
            continue
        text = path.read_text(errors="replace")
        if re.search(r'current_path\(\)\s*/\s*"(scripts|fonts|contracts|docs)"', text):
            offenders.append(path.relative_to(REPO_ROOT).as_posix())
    assert not offenders, f"support paths from the current folder (use runtime_paths): {offenders}"


def main() -> int:
    tests = [
        test_install_flip_rollback_and_refusals,
        test_unpushed_release_refused_outside_test_mode,
        test_wrappers_refuse_user_owned_release,
        test_launcher_bin_dir_helper,
        test_release_library_check,
        test_root_wrappers_never_write_python_bytecode,
        test_release_script_ships_cudart,
        test_gui_window_layout_is_kept_in_the_data_root_as_the_invoker,
        test_release_ships_every_helper_and_finds_support_files_from_the_binary,
    ]
    for test in tests:
        try:
            test()
        except Exception as exc:  # noqa: BLE001 - standalone test runner
            print(f"[FAIL] {test.__name__}: {exc}", file=sys.stderr)
            return 1
        print(f"[PASS] {test.__name__}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
