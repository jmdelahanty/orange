#!/usr/bin/env python3
"""Static guards for Orange GUI local-control lifecycle wiring."""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(path: str) -> str:
    return (REPO_ROOT / path).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    needle = f"{name}("
    start = source.find(needle)
    require(start >= 0, f"missing function {name}")
    brace = source.find("{", start)
    require(brace >= 0, f"missing function body for {name}")
    depth = 0
    for index in range(brace, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function body for {name}")


def test_drain_timeout_requests_forced_stream_shutdown() -> None:
    orange = read("src/orange.cpp")
    timeout_body = function_body(orange, "gui_poll_local_control_drain_timeout")
    require(
        "stop_scheduler->forced_finalize_requested = true" in timeout_body,
        "drain-timeout path must arm forced finalize",
    )
    require(
        'recording_run->stop_control["forced_finalize_requested"] = true' in timeout_body,
        "drain-timeout path must preserve forced-finalize provenance in stop control",
    )
    force_body = function_body(orange, "gui_request_local_control_forced_finalize_if_needed")
    require(
        "gui_autorun_requests->toggle_streaming = true" in force_body,
        "forced-finalize helper must request the existing stream shutdown path",
    )
    require(
        'recording_drain_forced_finalize_requested' in force_body,
        "forced-finalize helper must emit a structured GUI event",
    )
    main_loop_call = (
        "gui_request_local_control_forced_finalize_if_needed(\n"
        "            &gui_local_control_stop_scheduler,"
    )
    require(
        main_loop_call in orange,
        "main GUI loop must poll the forced-finalize helper",
    )


def main() -> int:
    tests = [
        test_drain_timeout_requests_forced_stream_shutdown,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("orange_gui_local_control_wiring_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
