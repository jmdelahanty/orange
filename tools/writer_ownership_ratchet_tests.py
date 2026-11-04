#!/usr/bin/env python3
"""Tests for scripts/check_writer_ownership.py on scratch trees."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CHECK = REPO / "scripts" / "check_writer_ownership.py"


def run(root: Path) -> subprocess.CompletedProcess:
    env = dict(os.environ, ORANGE_RATCHET_ROOT=str(root))
    return subprocess.run([sys.executable, str(CHECK)], env=env,
                          capture_output=True, text=True, check=False)


def make_tree(root: Path, source: str, allowed: dict | None = None, helpers: dict | None = None) -> None:
    (root / "contracts").mkdir(parents=True, exist_ok=True)
    (root / "src").mkdir(exist_ok=True)
    (root / "contracts" / "writer_ownership_allowlist.json").write_text(
        json.dumps({"allowed": allowed or {}, "helpers": helpers or {}}), encoding="utf-8")
    (root / "src" / "writer.cpp").write_text(source, encoding="utf-8")


def expect(name: str, source: str, passes: bool, allowed: dict | None = None,
           helpers: dict | None = None, contains: str = "") -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        make_tree(root, source, allowed, helpers)
        result = run(root)
        ok = (result.returncode == 0) == passes and contains in result.stdout
        if not ok:
            raise AssertionError(f"{name}: exit {result.returncode}\n{result.stdout}{result.stderr}")


UNGUARDED = '''
void write_it(const std::string& path)
{
    std::ofstream out(path);
    out << "x";
}
'''

GUARDED = '''
void write_it(const std::string& path)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(path);
    // std::ofstream in_a_comment(path);
}
'''

GUARD_BLOCK_CLOSED = '''
void write_it(const std::string& path)
{
    {
        orange::ScopedFsuid fsuid_guard;
        make_folder(path);
    }
    writer->meta->open(path.c_str());
}
'''

THREAD_LAMBDA = '''
void start(const std::string& path)
{
    orange::ScopedFsuid fsuid_guard;
    worker_ = std::thread([path]() {
        std::ofstream out(path);
    });
}
'''

SYNC_LAMBDA = '''
void start(const std::string& path)
{
    orange::ScopedFsuid fsuid_guard;
    const auto write = [&]() {
        std::ofstream out(path);
    };
    write();
}
'''

DECLARATIONS = '''
struct Writer {
    std::ofstream file_;
    bool write_file_atomic(const std::string& path, const std::string& bytes, const Options& options = {});
};
'''

HELPER = '''
void open_meta(Writer* writer)
{
    writer->meta.open(writer->path);
}

void guarded_caller(Writer* writer)
{
    orange::ScopedFsuid fsuid_guard;
    open_meta(writer);
}

void unguarded_caller(Writer* writer)
{
    open_meta(writer);
}
'''

KEY = "src/writer.cpp|write_it|ofstream"


def main() -> int:
    expect("unguarded write fails", UNGUARDED, False, contains="write_it")
    expect("guarded write passes", GUARDED, True)
    expect("write after the guard's block closed fails", GUARD_BLOCK_CLOSED, False, contains="stream_open")
    expect("thread lambda does not inherit the guard", THREAD_LAMBDA, False, contains="<thread lambda>")
    expect("synchronous lambda inherits the guard", SYNC_LAMBDA, True)
    expect("declarations write nothing", DECLARATIONS, True)
    expect("listed site passes", UNGUARDED, True, allowed={KEY: {"count": 1, "reason": "root on purpose"}})
    expect("empty reason fails", UNGUARDED, False, allowed={KEY: {"count": 1, "reason": " "}},
           contains="empty reason")
    expect("count above the tree fails (list only shrinks)", UNGUARDED, False,
           allowed={KEY: {"count": 2, "reason": "root on purpose"}}, contains="lower it")
    expect("count below the tree fails", UNGUARDED + UNGUARDED.replace("write_it", "write_it"), False,
           allowed={KEY: {"count": 1, "reason": "root on purpose"}}, contains="2 unguarded")
    expect("unguarded helper call fails", HELPER, False,
           allowed={"src/writer.cpp|open_meta|stream_open": {"count": 1, "reason": "helper"}},
           helpers={"open_meta": "opens the meta CSV"}, contains="unguarded_caller")
    print("writer_ownership_ratchet_tests: PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL: {error}")
        sys.exit(1)
