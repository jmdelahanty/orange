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
    require(
        'recording_run->stop_control["health"] = "critical"' in timeout_body,
        "drain-timeout path must preserve critical health in stop control",
    )
    require(
        'recording_run->stop_control["ack_state"] = "failed_timeout"' in timeout_body,
        "drain-timeout path must persist failed-timeout ACK state in stop control",
    )
    snapshot_body = function_body(orange, "gui_control_stop_snapshot")
    require(
        "snapshot.forced_finalize_stream_stop_requested =\n"
        "        scheduler.forced_finalize_stream_stop_requested;" in snapshot_body,
        "local-control status must expose whether forced finalize requested stream shutdown",
    )
    force_body = function_body(orange, "gui_request_local_control_forced_finalize_if_needed")
    require(
        'recording_run->stop_control["forced_finalize_stream_stop_requested"] = true'
        in force_body,
        "forced-finalize helper must persist stream-shutdown request in stop control",
    )
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


def test_local_control_env_flags_override_app_config() -> None:
    orange = read("src/orange.cpp")
    override_body = function_body(orange, "gui_env_flag_override")
    require(
        "gui_env_flag_value(gui_name)" in override_body,
        "local-control env override helper must inspect GUI-specific env first",
    )
    require(
        "gui_env_flag_value(generic_name)" in override_body,
        "local-control env override helper must inspect generic env second",
    )
    require(
        "return fallback;" in override_body,
        "local-control env override helper must fall back to app config/default",
    )

    disabled_body = function_body(orange, "gui_local_control_disabled")
    require(
        "gui_env_flag_override(\n"
        "        \"ORANGE_GUI_LOCAL_CONTROL_DISABLE\",\n"
        "        \"ORANGE_LOCAL_CONTROL_DISABLE\"" in disabled_body,
        "GUI-specific local-control disable flag must override stale generic disable env",
    )
    require(
        "gui_env_flag_enabled(\"ORANGE_GUI_LOCAL_CONTROL_DISABLE\", false) ||"
        not in disabled_body,
        "local-control disable must not OR GUI-specific and generic env flags",
    )

    stop_enabled_body = function_body(orange, "gui_local_control_stop_recording_enabled")
    require(
        "gui_env_flag_override(\n"
        "        \"ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP\",\n"
        "        \"ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_STOP\"" in stop_enabled_body,
        "generic stop_recording gate must allow explicit env false to override app config true",
    )
    require(
        "gui_env_flag_enabled(\"ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP\", false) ||"
        not in stop_enabled_body,
        "generic stop_recording gate must not OR env and app config",
    )

    citrus_enabled_body = function_body(
        orange,
        "gui_local_control_citrus_completion_stop_enabled",
    )
    require(
        "if (gui_local_control_stop_recording_enabled(app_storage_config))" in citrus_enabled_body,
        "generic stop_recording enabled should continue to imply Citrus completion stop",
    )
    require(
        "gui_env_flag_override(\n"
        "        \"ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP\",\n"
        "        \"ORANGE_LOCAL_CONTROL_ENABLE_CITRUS_STOP\"" in citrus_enabled_body,
        "Citrus-only stop gate must allow explicit env false to override app config true",
    )

    start_enabled_body = function_body(orange, "gui_local_control_recording_start_enabled")
    require(
        "gui_env_flag_override(\n"
        "        \"ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START\",\n"
        "        \"ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_START\"" in start_enabled_body,
        "recording start gate must allow explicit env false to override app config true",
    )
    exit_enabled_body = function_body(orange, "gui_local_control_exit_after_finalize_enabled")
    require(
        "gui_env_flag_override(\n"
        "        \"ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE\",\n"
        "        \"ORANGE_LOCAL_CONTROL_EXIT_AFTER_FINALIZE\"" in exit_enabled_body,
        "exit-after-finalize gate must allow explicit env false to override app config true",
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


def test_recording_session_stop_control_carries_drain_evidence() -> None:
    orange = read("src/orange.cpp")
    manifest_body = function_body(orange, "gui_local_control_stop_manifest_control")
    for needle, description in (
        ('{"received_at_utc", scheduler.received_at_utc}', "request receive timestamp"),
        ('{"drain_completed", false}', "initial drain-completed state"),
        ('{"drain_timed_out", scheduler.drain_timed_out}', "initial drain-timeout state"),
        (
            '{"forced_finalize_requested", scheduler.forced_finalize_requested}',
            "initial forced-finalize state",
        ),
        (
            '{"forced_finalize_stream_stop_requested",\n'
            '         scheduler.forced_finalize_stream_stop_requested}',
            "initial forced stream-stop state",
        ),
        ('{"ack_state", "executing"}', "initial executing ACK state"),
    ):
        require(needle in manifest_body, f"recording manifest control must include {description}")

    finalizer = read("src/gui/recording_finalizer.cpp")
    finalized_body = function_body(
        finalizer,
        "gui_update_local_control_stop_manifest_for_finalized_drain",
    )
    require(
        'run->stop_control["drain_completed"] = true' in finalized_body,
        "finalized drain helper must persist drain completion",
    )
    require(
        'run->stop_control["drain_completed_at_utc"] =' in finalized_body,
        "finalized drain helper must persist drain completion timestamp",
    )
    require(
        'run->stop_control["last_event"] =\n'
        '        drain_timed_out ? "finalized_after_drain_timeout" : "finalized";'
        in finalized_body,
        "finalized drain helper must persist final local-control event",
    )
    require(
        'run->stop_control["ack_state"] =\n'
        '        drain_timed_out ? "failed_timeout" : "executed";'
        in finalized_body,
        "finalized drain helper must persist terminal ACK state",
    )
    # The finalize is phased (gate / prepare / run / complete): the
    # stop-control evidence update happens in the GUI-thread prepare phase,
    # which snapshots the run BEFORE the background run phase writes the
    # manifest. Both the async driver and the synchronous composition go
    # through gui_prepare_recording_finalize.
    prepare_body = function_body(finalizer, "gui_prepare_recording_finalize")
    require(
        "gui_update_local_control_stop_manifest_for_finalized_drain(run);" in prepare_body,
        "recording finalize prepare phase must update stop-control evidence"
        " before the manifest is written",
    )
    finalize_body = function_body(finalizer, "gui_finalize_recording_session_if_ready")
    require(
        "gui_prepare_recording_finalize(" in finalize_body,
        "the synchronous finalizer must run the prepare phase (which updates"
        " stop-control evidence) before writing the manifest",
    )
    copy_body = function_body(orange, "gui_copy_local_control_event_log_to_recording_session")
    for needle, description in (
        ('"orange_local_control.events.jsonl"', "artifact-local event-log filename"),
        ('(*control)["event_log"] = std::move(event_log);', "manifest event-log metadata patch"),
        ('{"relative_path", target_path.filename().string()}', "relative event-log path"),
        ('{"copied", !copy_error}', "copy status"),
        ('event_log["bytes"] = size;', "copied byte count"),
    ):
        require(needle in copy_body, f"event-log capture helper must write {description}")
    drain_body = function_body(orange, "gui_mark_local_control_drain_completed")
    require(
        "gui_copy_local_control_event_log_to_recording_session(\n"
        "            event_log_path,\n"
        "            recording_folder);"
        in drain_body,
        "drain-finalized local-control path must capture the event log into the recording folder",
    )


def test_local_control_event_log_preserves_request_provenance() -> None:
    orange = read("src/orange.cpp")

    drain_body = function_body(orange, "gui_drain_local_control_commands")
    for needle, description in (
        ('{"event", "recording_start_queued"}', "start queued event"),
        ('{"method", "start_recording"}', "start queued method"),
        (
            '{"received_at_utc", start_request->received_at_utc}',
            "start queued receive timestamp",
        ),
        ('{"event", "recording_stop_scheduled"}', "stop scheduled event"),
        (
            '{"received_at_utc", stop_scheduler->received_at_utc}',
            "stop scheduled receive timestamp",
        ),
        ('{"event", "recording_stop_schedule_kept"}', "stop schedule-kept event"),
        (
            '{"received_at_utc", command.received_at_utc}',
            "ignored/schedule-kept command receive timestamp",
        ),
    ):
        require(needle in drain_body, f"GUI command drain must log {description}")

    start_body = function_body(orange, "gui_poll_local_control_start_request")
    for needle, description in (
        (
            '{"event", started ? "recording_start_triggered" : "recording_start_failed"}',
            "start trigger/failure event",
        ),
        ('{"method", "start_recording"}', "start trigger/failure method"),
        (
            '{"received_at_utc", start_request->received_at_utc}',
            "start trigger/failure receive timestamp",
        ),
    ):
        require(needle in start_body, f"start polling must log {description}")

    stop_body = function_body(orange, "gui_poll_local_control_stop_scheduler")
    for needle, description in (
        ('{"event", "recording_stop_triggered"}', "stop triggered event"),
        (
            '{"received_at_utc", stop_scheduler->received_at_utc}',
            "stop triggered receive timestamp",
        ),
    ):
        require(needle in stop_body, f"stop polling must log {description}")

    timeout_body = function_body(orange, "gui_poll_local_control_drain_timeout")
    for needle, description in (
        ('{"event", "recording_drain_timeout"}', "drain-timeout event"),
        (
            '{"received_at_utc", stop_scheduler->received_at_utc}',
            "drain-timeout receive timestamp",
        ),
        (
            '{"forced_finalize_requested", stop_scheduler->forced_finalize_requested}',
            "drain-timeout forced-finalize flag",
        ),
        ('{"health", "critical"}', "drain-timeout critical health"),
        ('{"error_code", "drain_timeout"}', "drain-timeout error code"),
    ):
        require(needle in timeout_body, f"drain-timeout polling must log {description}")

    force_body = function_body(orange, "gui_request_local_control_forced_finalize_if_needed")
    for needle, description in (
        (
            '{"event", "recording_drain_forced_finalize_requested"}',
            "forced-finalize event",
        ),
        (
            '{"received_at_utc", stop_scheduler->received_at_utc}',
            "forced-finalize receive timestamp",
        ),
        ('{"action", "stream_shutdown"}', "forced-finalize stream-shutdown action"),
        ('{"health", "critical"}', "forced-finalize critical health"),
        ('{"error_code", "drain_timeout"}', "forced-finalize error code"),
    ):
        require(needle in force_body, f"forced-finalize helper must log {description}")

    finalized_body = function_body(orange, "gui_mark_local_control_drain_completed")
    for needle, description in (
        ('{"event", "recording_drain_finalized"}', "drain-finalized event"),
        (
            '{"received_at_utc", stop_scheduler->received_at_utc}',
            "drain-finalized receive timestamp",
        ),
        (
            '{"drain_completed_at_utc", stop_scheduler->drain_completed_at_utc}',
            "drain completion timestamp",
        ),
        (
            '{"drain_timed_out", stop_scheduler->drain_timed_out}',
            "drain-finalized timeout flag",
        ),
        (
            '{"health", stop_scheduler->drain_timed_out ? "warning" : "ok"}',
            "drain-finalized health",
        ),
        (
            '{"error_code", stop_scheduler->drain_timed_out ? "drain_timeout" : ""}',
            "drain-finalized error code",
        ),
    ):
        require(needle in finalized_body, f"drain completion must log {description}")


def test_diagnostic_finalize_stall_can_exercise_drain_timeout() -> None:
    finalizer = read("src/gui/recording_finalizer.cpp")
    env_body = function_body(
        finalizer,
        "gui_local_control_diagnostic_finalize_stall_seconds",
    )
    require(
        "ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS" in env_body,
        "diagnostic finalize stall must have a GUI env knob",
    )
    require(
        "ORANGE_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS" in env_body,
        "diagnostic finalize stall must have a non-GUI alias",
    )
    # The finalize is phased: the diagnostic stall knob is consulted by the
    # per-frame drain gate, which both the async driver and the synchronous
    # composition run before any finalize work starts.
    gate_body = function_body(finalizer, "gui_recording_finalize_gate_ready")
    require(
        "gui_local_control_diagnostic_finalize_stall_seconds()" in gate_body,
        "the finalize drain gate must consult the diagnostic stall knob",
    )
    require(
        'run->stop_control["diagnostic_finalize_stall_active"] = true' in gate_body,
        "diagnostic stall must persist active-state evidence in stop control",
    )
    require(
        "return false;" in gate_body,
        "diagnostic stall must defer finalization until the stall interval elapses",
    )
    finalize_body = function_body(finalizer, "gui_finalize_recording_session_if_ready")
    require(
        "gui_recording_finalize_gate_ready(" in finalize_body,
        "the synchronous finalizer must run the drain gate (which consults"
        " the diagnostic stall knob)",
    )
    poll_body = function_body(finalizer, "gui_poll_async_recording_finalize")
    require(
        "gui_recording_finalize_gate_ready(" in poll_body,
        "the async finalize poll must run the drain gate (which consults"
        " the diagnostic stall knob)",
    )
    wrapper = read("scripts/orange_gui_validation_wrapper.sh")
    require(
        "ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS" in wrapper,
        "sudo validation wrapper must allow the diagnostic stall env through",
    )
    launcher = read("scripts/run_gui_aq_off_validation.sh")
    require(
        "ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS" in launcher,
        "GUI validation launcher must pass the diagnostic stall env through",
    )


def main() -> int:
    tests = [
        test_drain_timeout_requests_forced_stream_shutdown,
        test_local_control_env_flags_override_app_config,
        test_stop_commands_keep_gui_thread_lifecycle_authority,
        test_completion_and_stop_grace_defaults_are_distinct,
        test_recording_session_stop_control_carries_drain_evidence,
        test_local_control_event_log_preserves_request_provenance,
        test_diagnostic_finalize_stall_can_exercise_drain_timeout,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
    print("orange_gui_local_control_wiring_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
