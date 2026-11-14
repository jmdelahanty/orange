#!/usr/bin/env python3
"""source_version v1 and v2 in the GUI recording validator (2026-11-14).

v2 records the binary's build stamp at the top level; the run-time worktree is
optional (an /opt release has none) and reported in worktree_state.
"""

from __future__ import annotations

import copy
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import validate_gui_ptp_recording as v  # noqa: E402

COMMIT = "4a25512f0f1d2c3b4a5968778899aabbccddeeff"
SHORT = "4a25512"
GIT_USER = {"mode": "sudo_invoking_user", "uid": 1000, "gid": 1000}

V1 = {
    "schema_version": 1, "vcs": "git", "available": True,
    "worktree": "/home/jeremy/orange", "commit": COMMIT, "commit_short": SHORT,
    "git_command_available": True, "git_command_user": GIT_USER,
    "dirty_tracked_available": True, "dirty_tracked": False,
}
BUILD = {"available": True, "commit": COMMIT, "commit_short": SHORT, "branch": "main",
         "describe": SHORT, "dirty_tracked_known": True, "dirty_tracked": False}
V2_TREE = {
    "schema_version": 2, "vcs": "git", "available": True, "provenance": "build_stamp",
    "commit": COMMIT, "commit_short": SHORT, "dirty_tracked_available": True,
    "dirty_tracked": False, "build": BUILD, "install": None,
    "worktree": "/home/jeremy/orange", "git_command_available": True, "git_command_user": GIT_USER,
    "worktree_state": {"available": True, "path": "/home/jeremy/orange", "commit": COMMIT,
                       "describe": SHORT, "dirty_tracked": False, "matches_build": True},
}
V2_INSTALLED = {
    "schema_version": 2, "vcs": "git", "available": True, "provenance": "build_stamp",
    "commit": COMMIT, "commit_short": SHORT, "dirty_tracked_available": True,
    "dirty_tracked": False, "build": BUILD,
    "install": {"release_dir": "/opt/orange/releases/orange-v2026.11.14", "tag": "orange-v2026.11.14"},
    "worktree_state": {"available": False, "error": "git worktree not found from cwd or executable path"},
}


def run(source: dict, **expect) -> v.Reporter:
    reporter = v.Reporter(verbose=False)
    v.check_source_version(
        reporter, {"source_version": source, "producer_version": SHORT},
        require_source_version=True,
        expected_git_command_user_mode=expect.get("mode"),
        expected_dirty_tracked=expect.get("dirty"),
    )
    return reporter


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    r = run(V1, mode="sudo_invoking_user", dirty=0)
    require(not r.failures, f"v1 still validates: {r.failures}")

    r = run(V2_TREE, mode="sudo_invoking_user", dirty=0)
    require(not r.failures and not r.warnings, f"v2 from a tree validates: {r.failures} {r.warnings}")

    r = run(V2_INSTALLED, mode="sudo_invoking_user", dirty=0)
    require(not r.failures, f"v2 from an install needs no worktree or git user: {r.failures}")

    stale = copy.deepcopy(V2_TREE)
    stale["worktree_state"].update({"commit": "f" * 40, "describe": "ffffff", "matches_build": False})
    r = run(stale)
    require(not r.failures and r.warnings, "a binary older than its tree warns, not fails")

    built_dirty_tree_clean = copy.deepcopy(V2_TREE)
    built_dirty_tree_clean.update({"dirty_tracked": True})
    built_dirty_tree_clean["build"] = dict(BUILD, dirty_tracked=True)
    r = run(built_dirty_tree_clean, dirty=0)
    require(not r.failures, "dirty 'auto' expectation compares the tree at launch in v2")

    no_stamp = copy.deepcopy(V2_INSTALLED)
    no_stamp["build"] = dict(BUILD, available=False, commit="")
    r = run(no_stamp)
    require(r.failures, "v2 without a build stamp fails when provenance is required")

    r = run(dict(V1, schema_version=3))
    require(r.failures, "unknown schema version fails")

    print("source_version_tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
