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
    snapshot_body = function_body(orange, "gui_control_stop_snapshot")
    require(
        "snapshot.forced_finalize_stream_stop_requested =\n"
        "        scheduler.forced_finalize_stream_stop_requested;" in snapshot_body,
        "local-control status must expose whether forced finalize requested stream shutdown",
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


def test_stop_commands_keep_gui_thread_lifecycle_authority() -> None:
    orange = read("src/orange.cpp")
    body = function_body(orange, "gui_drain_local_control_commands")
    require(
        "command.method == \"citrus_completion\" ||\n"
        "            command.method == \"stop_recording\"" in body,
        "GUI command drain must treat Citrus completion and stop_recording as stop commands",
    )
    require(
        "if (!camera_control || !camera_control->record_video)" in body,
        "GUI stop command handling must no-op when Orange is not recording",
    )
    require(
        "gui_note_local_control_stop_event(stop_scheduler, \"ignored_not_recording\")" in body,
        "idle stop command must record ignored_not_recording state",
    )
    require(
        '{"reason", "orange_not_recording"}' in body,
        "idle stop command must log an orange_not_recording reason",
    )
    require(
        "stop_scheduler->deadline <= deadline" in body,
        "stop scheduler must keep an existing earlier deadline",
    )
    require(
        '{"event", "recording_stop_schedule_kept"}' in body,
        "same/late stop requests must log schedule-kept evidence",
    )
    require(
        '{"policy", "earliest_deadline"}' in body,
        "schedule-kept event must name the earliest-deadline policy",
    )


def test_completion_and_stop_grace_defaults_are_distinct() -> None:
    orange = read("src/orange.cpp")
    body = function_body(orange, "gui_drain_local_control_commands")
    require(
        'const double default_grace_seconds =\n'
        '            command.method == "stop_recording" ? 0.0 : 10.0;' in body,
        "citrus_completion must default to 10s grace while stop_recording defaults to 0s",
    )
    require(
        '"grace_seconds",\n'
        "                default_grace_seconds" in body,
        "GUI stop scheduler must use the default when params.grace_seconds is omitted",
    )
    require(
        '{"grace_seconds", stop_scheduler->grace_seconds}' in body,
        "scheduled stop event must log the resolved grace seconds",
    )


def main() -> int:
    tests = [
        test_drain_timeout_requests_forced_stream_shutdown,
        test_stop_commands_keep_gui_thread_lifecycle_authority,
        test_completion_and_stop_grace_defaults_are_distinct,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("orange_gui_local_control_wiring_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
