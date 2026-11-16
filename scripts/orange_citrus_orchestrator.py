#!/usr/bin/env python3
"""Coordinate a local Orange/Citrus experiment through local-control sockets."""

from __future__ import annotations

import argparse
import copy
import errno
import hashlib
import json
import os
import re
import signal
import shlex
import shutil
import socket
import subprocess
import sys
import time
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
# The one builder of orange.recording_start_declaration (shared with the client).
from orange_local_control_client import build_declaration  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[1]
ORANGE_REQUEST_SCHEMA_ID = "orange.local_control.request"
CITRUS_REQUEST_SCHEMA_ID = "citrus.local_control.request"
SCHEMA_VERSION = 1
SUMMARY_SCHEMA_ID = "orange_citrus.orchestrator.summary"
SUMMARY_SCHEMA_VERSION = 1
TERMINAL_CITRUS_STATES = {"completed", "stopped", "failed", "start_rejected"}
SOCKET_RESPONSE_EVENT_FIELDS = (
    "responded_at_utc",
    "duplicate",
    "diagnostic_only",
    "queued_for_gui_thread",
)
GUI_LIFECYCLE_EVENT_FIELDS = (
    "event_at_utc",
    "received_at_utc",
    "drain_completed_at_utc",
    "drain_timed_out",
    "forced_finalize_requested",
    "forced_finalize_requested_at_utc",
    "elapsed_seconds",
    "timeout_seconds",
    "active_recorders",
    "grace_seconds",
    "health",
    "error_code",
    "action",
    "experiment_id",
    "pending_request_id",
    "existing_request_id",
    "policy",
    "stop_enabled",
    "stop_recording_enabled",
    "citrus_completion_enabled",
    "start_enabled",
)


class OrchestratorError(RuntimeError):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def json_path(payload: dict[str, Any], keys: list[str], default: Any = None) -> Any:
    current: Any = payload
    for key in keys:
        if not isinstance(current, dict) or key not in current:
            return default
        current = current[key]
    return current


def copy_present_fields(
    target: dict[str, Any],
    source: dict[str, Any],
    fields: tuple[str, ...],
) -> None:
    for field in fields:
        if field in source:
            target[field] = source[field]


def nonnegative_float(value: str) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a number") from exc
    if parsed < 0.0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return parsed


def positive_float(value: str) -> float:
    parsed = nonnegative_float(value)
    if parsed <= 0.0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def build_request(
    schema_id: str,
    method: str,
    request_id: str,
    *,
    operation_id: str | None = None,
    source: str = "orange_citrus_orchestrator",
    params: dict[str, Any] | None = None,
) -> dict[str, Any]:
    request: dict[str, Any] = {
        "schema_id": schema_id,
        "schema_version": SCHEMA_VERSION,
        "method": method,
        "request_id": request_id,
        "source": source,
        "sent_at_utc": utc_now(),
        "params": params or {},
    }
    if operation_id is not None:
        request["operation_id"] = operation_id
    return request


def build_status_request(schema_id: str, request_id: str, source: str) -> dict[str, Any]:
    return build_request(schema_id, "status", request_id, source=source)


def build_orange_start_request(
    operation_id: str,
    request_id: str,
    source: str,
    declaration: dict[str, Any] | None = None,
    *,
    declaration_files_root: str | None = None,
    empty_background: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Orange's start. With a declaration the GUI needs no Subject panel Confirm
    (src/session/recording_start_declaration.h); without one it does. An
    orchard declaration names the folder holding its run_plan/ copies, and a
    start may name an empty-dish background capture (both need the
    declaration; docs/orange_local_control_contract.md)."""
    params: dict[str, Any] = {"reason": "orchestrator_start"}
    if declaration is not None:
        params["declaration"] = declaration
    if declaration_files_root is not None:
        params["declaration_files_root"] = declaration_files_root
    if empty_background is not None:
        params["empty_background"] = empty_background
    return build_request(
        ORANGE_REQUEST_SCHEMA_ID,
        "start_recording",
        request_id,
        operation_id=operation_id,
        source=source,
        params=params,
    )


def build_orange_stop_request(
    operation_id: str,
    request_id: str,
    source: str,
    reason: str,
    grace_seconds: float,
) -> dict[str, Any]:
    return build_request(
        ORANGE_REQUEST_SCHEMA_ID,
        "stop_recording",
        request_id,
        operation_id=operation_id,
        source=source,
        params={"reason": reason, "grace_seconds": grace_seconds},
    )


def build_orange_citrus_completion_request(
    operation_id: str,
    request_id: str,
    source: str,
    experiment_id: str,
    terminal_state: str,
    reason: str,
    grace_seconds: float,
) -> dict[str, Any]:
    return build_request(
        ORANGE_REQUEST_SCHEMA_ID,
        "citrus_completion",
        request_id,
        operation_id=operation_id,
        source=source,
        params={
            "experiment_id": experiment_id,
            "terminal_state": terminal_state,
            "reason": reason,
            "grace_seconds": grace_seconds,
        },
    )


def effective_orange_stop_grace_seconds(args: argparse.Namespace) -> float:
    explicit = getattr(args, "orange_stop_grace_seconds", None)
    if explicit is not None:
        return float(explicit)
    if getattr(args, "stop_policy", "") in {
        "citrus_completion",
        "citrus_completion_notify",
    }:
        return 10.0
    return 0.0


def build_citrus_start_request(operation_id: str, request_id: str, source: str) -> dict[str, Any]:
    return build_request(
        CITRUS_REQUEST_SCHEMA_ID,
        "start_experiment",
        request_id,
        operation_id=operation_id,
        source=source,
    )


CITRUS_SUBJECT_DECLARATION_REQUEST_SCHEMA_ID = "citrus.subject_declaration_request"


def build_citrus_declare_subjects_request(
    operation_id: str, request_id: str, source: str, params: dict[str, Any]
) -> dict[str, Any]:
    return build_request(
        CITRUS_REQUEST_SCHEMA_ID,
        "declare_subjects",
        request_id,
        operation_id=operation_id,
        source=source,
        params=params,
    )


def load_citrus_subject_declaration_request(path: Path) -> dict[str, Any]:
    """A citrus.subject_declaration_request v1 (Citrus PR #120,
    docs/subject_declaration_v2_implementation.md), sent to Citrus unchanged.
    Citrus checks the whole of it; this only refuses what cannot be sent."""
    try:
        request = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"--citrus-subject-declaration-request {path}: {exc}") from exc
    if not isinstance(request, dict) or request.get("schema_id") != CITRUS_SUBJECT_DECLARATION_REQUEST_SCHEMA_ID \
            or request.get("schema_version") != 1:
        raise ValueError(f"{path} is not a citrus.subject_declaration_request v1")
    arenas = request.get("arenas")
    if not isinstance(arenas, list) or not arenas:
        raise ValueError(f"{path}: arenas must be a non-empty list")
    ids = [a.get("arena_id") if isinstance(a, dict) else None for a in arenas]
    if any(not isinstance(i, str) or not i for i in ids) or len(set(ids)) != len(ids):
        raise ValueError(f"{path}: every arena needs a distinct arena_id")
    for arena in arenas:
        if not isinstance(arena.get("declaration"), dict) or not isinstance(arena.get("camera_serial"), str):
            raise ValueError(f"{path}: {arena['arena_id']} needs camera_serial and a declaration object")
    return request


def canonical_json(value: Any) -> str:
    """The bytes Orange's cross-check compares (nlohmann dump: sorted keys,
    no spaces, UTF-8)."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def citrus_declaration_plan_mismatches(request: dict[str, Any], orange_declaration: dict[str, Any] | None) -> list[str]:
    """What Orange's pre-arm cross-check v2 would refuse, found before anything
    is sent: each arena's camera must be one Orange declares, and a plan-sourced
    arena's run_plan must equal Orange's declaration v2 source. None (headless:
    the spec declares) checks nothing here; pre-arm still does."""
    if orange_declaration is None:
        return []
    problems: list[str] = []
    source = orange_declaration.get("source") if orange_declaration.get("schema_version") == 2 else None
    orange_plan = source if isinstance(source, dict) and source.get("kind") == "orchard_run_plan" else None
    cameras = json_path(orange_declaration, ["subject_references", "cameras"], {})
    cameras = cameras if isinstance(cameras, dict) else {}
    for arena in request["arenas"]:
        arena_id = arena["arena_id"]
        if arena["camera_serial"] not in cameras:
            problems.append(f"{arena_id}: camera {arena['camera_serial']} is not in Orange's declaration")
        declaration = arena["declaration"]
        if declaration.get("source") != "orchard_run_plan":
            continue
        if orange_plan is None:
            problems.append(f"{arena_id}: plan-sourced, but Orange's declaration is not a v2 orchard_run_plan")
        elif canonical_json(declaration.get("run_plan")) != canonical_json(orange_plan):
            problems.append(f"{arena_id}: run_plan differs from Orange's declaration source")
    return problems


def accepted_citrus_subject_declarations(response: dict[str, Any]) -> dict[str, str]:
    """{arena_id: declaration_sha256} from an accepted declare_subjects reply."""
    result = json_path(response, ["effect", "subject_declarations"], {})
    if not isinstance(result, dict) or result.get("status") != "accepted":
        return {}
    accepted: dict[str, str] = {}
    for arena in result.get("arenas") or []:
        if isinstance(arena, dict) and isinstance(arena.get("arena_id"), str) \
                and isinstance(arena.get("declaration_sha256"), str):
            accepted[arena["arena_id"]] = arena["declaration_sha256"]
    return accepted


def citrus_subject_declaration_problems(status: dict[str, Any], expected: dict[str, str]) -> list[str]:
    """The executor rule (Citrus docs/subject_declaration_v2_implementation.md):
    every plan arena shows plan_subject_declaration.state "declared" with the
    sha the accepted reply gave. Citrus applies an accepted declaration on the
    GUI thread and skips it silently if an experiment armed in between;
    "overridden" means an operator changed the arena in the panel. Empty = go."""
    arenas = {a.get("arena_id"): a for a in (status.get("arenas") or []) if isinstance(a, dict)}
    problems: list[str] = []
    for arena_id, sha in sorted(expected.items()):
        arena = arenas.get(arena_id)
        if arena is None:
            problems.append(f"{arena_id}: not in Citrus status")
            continue
        plan = arena.get("plan_subject_declaration")
        if not isinstance(plan, dict):
            problems.append(f"{arena_id}: plan_subject_declaration is null (not applied, or cleared)")
            continue
        state = plan.get("state")
        if state != "declared":
            problems.append(f"{arena_id}: plan_subject_declaration.state={state!r} (need 'declared')")
        elif plan.get("declaration_sha256") != sha:
            problems.append(f"{arena_id}: declaration_sha256 {plan.get('declaration_sha256')!r} "
                            f"is not the accepted {sha!r}")
    return problems


def build_citrus_daily_registration_mode_request(
    operation_id: str,
    request_id: str,
    source: str,
    mode: str,
) -> dict[str, Any]:
    return build_request(
        CITRUS_REQUEST_SCHEMA_ID,
        "select_daily_registration_runtime_mode",
        request_id,
        operation_id=operation_id,
        source=source,
        params={
            "mode": mode,
            "select_runtime_mode_armed": True,
        },
    )


def build_citrus_stop_request(operation_id: str, request_id: str, source: str) -> dict[str, Any]:
    return build_request(
        CITRUS_REQUEST_SCHEMA_ID,
        "stop_experiment",
        request_id,
        operation_id=operation_id,
        source=source,
    )


ORCHARD_SOURCE_KIND = "orchard_run_plan"
IDENTITY_PATTERN = re.compile(r"^[a-z][a-z0-9_-]{0,31}$")
EMPTY_BACKGROUND_MODES = ("none", "capture_at_setup")
CAPTURE_RECORD_NAME = "empty_background_capture.json"
CAPTURE_RECORD_SCHEMA_ID = "orange_citrus.empty_background_capture_record"
SCENE_CONFIRMATION_NAME = "scene_confirmation.json"
SCENE_CONFIRMATION_SCHEMA_ID = "orange_citrus.scene_confirmation"
SCENE_DECISION_FISH_LOADED = "fish_loaded_scene_unchanged"
SCENE_DECISION_FISH_FREE = "fish_free_test_scene_unchanged"
SCENE_DECISION_ABORT = "abort"
SCENE_STATEMENTS = {
    SCENE_DECISION_FISH_LOADED: ("The fish are loaded; the dishes were not moved and nothing else in the scene "
                                 "changed since the empty background capture."),
    SCENE_DECISION_FISH_FREE: ("Fish-free test: no fish were loaded; nothing in the scene changed since the "
                               "empty background capture."),
    SCENE_DECISION_ABORT: "Do not start: the scene is not as captured, or the session is called off.",
}


def is_orchard_declaration(declaration: dict[str, Any] | None) -> bool:
    return bool(declaration) and declaration.get("schema_version") == 2 \
        and isinstance(declaration.get("source"), dict) and declaration["source"].get("kind") == ORCHARD_SOURCE_KIND


def declaration_file_copies(declaration: dict[str, Any]) -> list[tuple[str, str]]:
    """(relative path, sha256) of the run_plan/ copies an orchard declaration
    names (Orange's session::DeclarationFileCopies)."""
    source = declaration["source"]
    return [(source["plan_copy_relative_path"], source["file_sha256"]),
            (source["plan_json_copy_relative_path"], source["plan_sha256"]),
            (source["sign_off_copy_relative_path"], source["sign_off"]["entry_sha256"])]


def check_declaration_files(declaration: dict[str, Any], root: Path) -> list[str]:
    """Orange's rule (session::ReadDeclarationFiles), checked before anything
    is launched: every named copy exists and hashes to its name, and run_plan/
    holds nothing else. Empty = the GUI will accept the folder."""
    problems: list[str] = []
    try:
        copies = declaration_file_copies(declaration)
    except (KeyError, TypeError) as exc:
        return [f"the declaration's source does not name its run_plan copies: {exc}"]
    named = {relative for relative, _ in copies}
    run_plan = root / "run_plan"
    if run_plan.is_dir():
        for path in sorted(run_plan.rglob("*")):
            if path.is_file() and path.relative_to(root).as_posix() not in named:
                problems.append(f"run_plan file not named by the declaration: {path.relative_to(root).as_posix()}")
    for relative, sha in copies:
        path = root / relative
        if not path.is_file():
            problems.append(f"missing declaration file {relative}")
        elif hashlib.sha256(path.read_bytes()).hexdigest() != sha:
            problems.append(f"declaration file {relative} does not hash to the sha256 the declaration names")
    return problems


def plan_session_from_files(declaration: dict[str, Any], root: Path) -> dict[str, Any]:
    """The plan session the declaration was made for, from the canonical plan
    JSON copy (run_plan/<plan_sha256>.json, written by orchard declare.py)."""
    source = declaration["source"]
    plan = json.loads((root / source["plan_json_copy_relative_path"]).read_text(encoding="utf-8"))
    for session in plan.get("sessions") or []:
        if isinstance(session, dict) and session.get("session_id") == source.get("plan_session_id"):
            return session
    raise ValueError(f"plan {source.get('plan_id')} has no session {source.get('plan_session_id')!r}")


def plan_preset_problems(session: dict[str, Any], camera_config: dict[str, Any] | None) -> list[str]:
    """The plan pins the Orange camera preset (orange.preset name, sha256,
    hash_rule); the launcher's preset record must be that one (abort rule A8)."""
    preset = json_path(session, ["orange", "preset"], None)
    if preset is None:
        return []
    if not isinstance(preset, dict):
        return ["the plan's orange.preset is not an object"]
    if camera_config is None:
        return ["the plan pins a camera preset but no preset record was given (--camera-config-json)"]
    problems = []
    for key in ("name", "sha256", "hash_rule"):
        if preset.get(key) != camera_config.get(key):
            problems.append(f"preset {key}: the plan says {preset.get(key)!r}, the launched folder is "
                            f"{camera_config.get(key)!r}")
    return problems


def declaration_names_subjects(declaration: dict[str, Any] | None) -> bool:
    """True when any camera (or the default) carries a dish or a subject count."""
    references = (declaration or {}).get("subject_references") or {}
    entries = [references.get("default")] + list((references.get("cameras") or {}).values())
    return any(isinstance(e, dict) and not e.get("blank") and (e.get("dish_id") or e.get("subject_count"))
               for e in entries)


def build_orange_capture_empty_background_request(
    operation_id: str, request_id: str, source: str, params: dict[str, Any]
) -> dict[str, Any]:
    return build_request(
        ORANGE_REQUEST_SCHEMA_ID,
        "capture_empty_background",
        request_id,
        operation_id=operation_id,
        source=source,
        params=params,
    )


def empty_background_status(status: dict[str, Any]) -> dict[str, Any]:
    value = json_path(status, ["local_control", "empty_background_capture"], None)
    return value if isinstance(value, dict) else {}


def empty_background_staging_problems(status: dict[str, Any]) -> list[str]:
    """Every streaming camera must have had its burst staging reserved at
    stream start, or its capture would be refused."""
    staging = empty_background_status(status).get("staging")
    if not isinstance(staging, dict) or not staging:
        return ["Orange reports no streaming camera with burst staging (local_control.empty_background_capture.staging)"]
    return [f"camera {serial}: staging not reserved ({(entry or {}).get('error')})"
            for serial, entry in sorted(staging.items()) if not (isinstance(entry, dict) and entry.get("reserved"))]


def empty_background_capture_finished(status: dict[str, Any], request_id: str) -> bool:
    capture = empty_background_status(status)
    return capture.get("request_id") == request_id and capture.get("state") in {"captured", "failed", "refused"}


def empty_background_capture_problems(capture: dict[str, Any], staging_cameras: list[str]) -> list[str]:
    """A capture the runner may name: captured, with a descriptor, and every
    camera that streamed captured (a failed camera would record
    missing_in_capture; the runner refuses before the fish go in instead)."""
    if capture.get("state") != "captured":
        return [f"capture state {capture.get('state')!r}: {capture.get('error')}"]
    problems = []
    if not capture.get("capture_id") or not re.fullmatch(r"sha256:[0-9a-f]{64}", str(capture.get("descriptor_sha256"))):
        problems.append("the capture names no capture_id or descriptor_sha256")
    cameras = capture.get("cameras") if isinstance(capture.get("cameras"), dict) else {}
    for serial in staging_cameras:
        entry = cameras.get(serial)
        if not isinstance(entry, dict) or entry.get("capture_status") != "captured":
            reason = entry.get("failure_reason") if isinstance(entry, dict) else "not in the capture"
            problems.append(f"camera {serial}: not captured ({reason})")
    return problems


CITRUS_IDLE_FILL_RGBA_U8 = [0, 0, 0, 255]
# What Orange's capture descriptor says for Citrus's idle clear: the display is
# on, showing projector black (not zero light).
CITRUS_IDLE_ORANGE_STIMULUS_DISPLAY = {"state": "on",
                                       "shown": {"kind": "solid_color", "rgba8": CITRUS_IDLE_FILL_RGBA_U8}}


def citrus_display_read(status: dict[str, Any]) -> dict[str, Any]:
    """What Citrus's status says about its stimulus output, kept verbatim as
    evidence: status.idle_display when this Citrus reports it, every arena's
    active_preview_mode (null when not reported), the calibration scene, the
    photodiode marker state if reported, and the build."""
    arenas = [a for a in (status.get("arenas") or []) if isinstance(a, dict)]
    scene = status.get("calibration_scene")
    marker = status.get("photodiode_marker")
    if marker is None and isinstance(scene, dict):
        marker = scene.get("photodiode_marker")
    return {
        "read_at_utc": utc_now(),
        "idle_display": json_path(status, ["display", "idle_display"], None),
        "arenas": [{"arena_id": a.get("arena_id"), "active_preview_mode": a.get("active_preview_mode")}
                   for a in arenas],
        "calibration_scene": ({k: scene.get(k) for k in ("state", "active", "scene_kind", "kind", "presented")
                               if k in scene} if isinstance(scene, dict) else None),
        "active_projection_snapshot_status": json_path(status, ["active_projection_snapshot", "status"], None),
        "photodiode_marker": marker if marker is not None else "not_reported",
        "citrus_build": status.get("citrus_build"),
    }


def citrus_idle_clear_problems(read: dict[str, Any], before: dict[str, Any] | None = None
                               ) -> tuple[str, list[str]]:
    """(evidence used, problems) for 'the display shows Citrus's idle clear,
    rgba8 0,0,0,255'. status.display.idle_display (recorded by Citrus's render
    thread after each swap) is preferred: source idle_clear, fill
    [0,0,0,255], no canvas fill overlay, no previewing arena, no photodiode
    marker patches; with `before` (the read before the capture) its
    display_frame_index must have advanced (the display is live). Without the
    field, every arena must report active_preview_mode NONE and no
    calibration scene may be active. Empty problems = declared exactly."""
    idle = read.get("idle_display")
    if idle is not None:
        used = "status.display.idle_display"
        if not isinstance(idle, dict):
            return used, ["status.display.idle_display is not an object"]
        problems = []
        if idle.get("source") != "idle_clear":
            problems.append(f"idle_display.source is {idle.get('source')!r}, not 'idle_clear'")
        if idle.get("fill_rgba_u8") != CITRUS_IDLE_FILL_RGBA_U8:
            problems.append(f"idle_display.fill_rgba_u8 is {idle.get('fill_rgba_u8')!r}, not {CITRUS_IDLE_FILL_RGBA_U8}")
        if idle.get("canvas_fill_overlay"):
            problems.append(f"a canvas fill overlay is shown: {idle.get('canvas_fill_overlay')!r}")
        if idle.get("preview_arena_ids"):
            problems.append(f"arenas previewing: {idle.get('preview_arena_ids')!r}")
        if idle.get("photodiode_marker_patches_rendered") is not False:
            # Marker patches are lit pixels on the projector: not solid black.
            problems.append("photodiode_marker_patches_rendered is "
                            f"{idle.get('photodiode_marker_patches_rendered')!r} (must be false)")
        index = idle.get("display_frame_index")
        if not isinstance(index, int) or isinstance(index, bool):
            problems.append(f"idle_display.display_frame_index is {index!r}, not an integer")
        elif before is not None:
            previous = (before.get("idle_display") or {}).get("display_frame_index") \
                if isinstance(before.get("idle_display"), dict) else None
            if not isinstance(previous, int) or index <= previous:
                problems.append(f"display_frame_index did not advance between the reads ({previous!r} -> "
                                f"{index!r}): the display is not shown live")
        return used, problems
    problems = []
    if not read["arenas"]:
        problems.append("Citrus reports no arenas")
    for arena in read["arenas"]:
        mode = arena["active_preview_mode"]
        if mode is None:
            problems.append(f"{arena['arena_id']}: Citrus status does not report active_preview_mode "
                            "(needs status.display.idle_display or arenas[].active_preview_mode)")
        elif mode != "NONE":
            problems.append(f"{arena['arena_id']}: preview {mode!r} is active")
    scene = read["calibration_scene"]
    if scene is None:
        problems.append("Citrus status does not report calibration_scene")
    elif scene.get("active"):
        problems.append(f"calibration scene active (state {scene.get('state')!r})")
    if read.get("active_projection_snapshot_status") == "active":
        problems.append("Citrus's active projection snapshot is active (a calibration-grid preview or run)")
    return "arena_preview_and_calibration_scene", problems


def display_frame_index_range(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any] | None:
    indexes = [r.get("idle_display", {}).get("display_frame_index") if isinstance(r.get("idle_display"), dict)
               else None for r in (before, after)]
    if indexes == [None, None]:
        return None
    return {"before_capture": indexes[0], "after_capture": indexes[1],
            "frames": indexes[1] - indexes[0] if all(isinstance(i, int) for i in indexes) else None}


def citrus_display_contradictions(read: dict[str, Any]) -> list[str]:
    """What an operator override may not contradict: a preview or
    calibration scene Citrus positively reports as active, or an idle
    display that is not the idle clear."""
    problems = [f"{a['arena_id']}: preview {a['active_preview_mode']!r} is active" for a in read["arenas"]
                if a["active_preview_mode"] not in (None, "NONE")]
    if isinstance(read["calibration_scene"], dict) and read["calibration_scene"].get("active"):
        problems.append("calibration scene active")
    if read.get("active_projection_snapshot_status") == "active":
        problems.append("Citrus's active projection snapshot is active")
    idle = read.get("idle_display")
    if isinstance(idle, dict):
        if idle.get("source") in ("canvas_fill", "preview", "arenas_running"):
            problems.append(f"idle_display.source is {idle.get('source')!r}")
        if idle.get("canvas_fill_overlay"):
            problems.append("a canvas fill overlay is shown")
        if idle.get("preview_arena_ids"):
            problems.append(f"arenas previewing: {idle.get('preview_arena_ids')!r}")
    return problems


SCENE_CONFIRMATION_SOCKET_NAME = "scene_confirmation.sock"
SCENE_CONFIRMATION_REQUEST_SCHEMA_ID = "orange_citrus.scene_confirmation_request"
SCENE_CONFIRMATION_REQUEST_KEYS = {"schema_id", "schema_version", "capture_id", "descriptor_sha256",
                                   "decision", "confirmed_by", "note"}
SCENE_SOCKET_DECISIONS = (SCENE_DECISION_FISH_LOADED, SCENE_DECISION_ABORT)
SCENE_REQUEST_MAX_BYTES = 16 * 1024


def build_scene_confirmation_request(capture_record: dict[str, Any], confirmed_by: str, decision: str,
                                     note: str = "") -> dict[str, Any]:
    """What scripts/confirm_bound_run_scene.py sends over the run's socket."""
    return {"schema_id": SCENE_CONFIRMATION_REQUEST_SCHEMA_ID, "schema_version": 1,
            "capture_id": capture_record.get("capture_id"),
            "descriptor_sha256": capture_record.get("descriptor_sha256"),
            "decision": decision, "confirmed_by": confirmed_by, "note": note}


def scene_confirmation_request_problems(request: Any, capture_record: dict[str, Any]) -> list[str]:
    """A request must answer this capture (id and sha256), name who confirms,
    and carry fish_loaded_scene_unchanged or abort (the fish-free decision is
    the runner's own, never a caller's)."""
    if not isinstance(request, dict) or request.get("schema_id") != SCENE_CONFIRMATION_REQUEST_SCHEMA_ID \
            or request.get("schema_version") != 1:
        return [f"not an {SCENE_CONFIRMATION_REQUEST_SCHEMA_ID} v1"]
    problems = [f"unknown field {key}" for key in sorted(set(request) - SCENE_CONFIRMATION_REQUEST_KEYS)]
    for key in ("capture_id", "descriptor_sha256"):
        if request.get(key) != capture_record.get(key):
            problems.append(f"{key} {request.get(key)!r} is not this run's {capture_record.get(key)!r}")
    if not IDENTITY_PATTERN.fullmatch(str(request.get("confirmed_by", ""))):
        problems.append("confirmed_by must match ^[a-z][a-z0-9_-]{0,31}$")
    if request.get("decision") not in SCENE_SOCKET_DECISIONS:
        problems.append(f"decision must be one of {list(SCENE_SOCKET_DECISIONS)}, not {request.get('decision')!r}")
    if not isinstance(request.get("note", ""), str):
        problems.append("note must be a string")
    return problems


def invoking_identity() -> tuple[int, int]:
    """(uid, gid) of the person who launched the runner, through sudo too."""
    try:
        return int(os.environ["SUDO_UID"]), int(os.environ.get("SUDO_GID", os.getgid()))
    except (KeyError, ValueError):
        return os.getuid(), os.getgid()


def socket_peer(connection: socket.socket) -> dict[str, Any]:
    """The kernel's account of the caller (SO_PEERCRED), with its user name
    and the process's exe and command line from /proc (null when gone)."""
    import pwd
    import struct
    pid, uid, gid = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
                                                               struct.calcsize("3i")))
    try:
        user: str | None = pwd.getpwuid(uid).pw_name
    except KeyError:
        user = None
    try:
        exe: str | None = os.readlink(f"/proc/{pid}/exe")
    except OSError:
        exe = None
    try:
        raw = Path(f"/proc/{pid}/cmdline").read_bytes()
        cmdline: list[str] | None = [part.decode("utf-8", "replace") for part in raw.split(b"\0") if part]
    except OSError:
        cmdline = None
    return {"pid": pid, "uid": uid, "gid": gid, "user": user, "exe": exe, "cmdline": cmdline}


def build_scene_confirmation(capture_record: dict[str, Any], request: dict[str, Any], requester: dict[str, Any]
                             ) -> dict[str, Any]:
    """orange_citrus.scene_confirmation v1, written only by the runner."""
    decision = request["decision"]
    return {
        "schema_id": SCENE_CONFIRMATION_SCHEMA_ID,
        "schema_version": 1,
        "capture_id": capture_record.get("capture_id"),
        "descriptor_sha256": capture_record.get("descriptor_sha256"),
        "decision": decision,
        "statement": SCENE_STATEMENTS[decision],
        "confirmed_by": request["confirmed_by"],
        "confirmed_at_utc": utc_now(),
        "note": request.get("note", ""),
        "requester": requester,
    }


def send_scene_confirmation_request(socket_path: Path, request: dict[str, Any], timeout_seconds: float = 10.0
                                    ) -> dict[str, Any]:
    """Client side (scripts/confirm_bound_run_scene.py): one JSON line, one reply."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout_seconds)
        client.connect(str(socket_path))
        client.sendall(json.dumps(request).encode() + b"\n")
        client.shutdown(socket.SHUT_WR)
        data = b""
        while chunk := client.recv(65536):
            data += chunk
    return json.loads(data.decode())


class SceneConfirmationListener:
    """The run's confirmation socket (<run dir>/scene_confirmation.sock, mode
    0600, owned by the invoking user). Each connection is one request; the
    caller is identified by SO_PEERCRED, never by what it writes. A peer
    whose uid is neither the invoking user's nor root's is refused."""

    def __init__(self, path: Path, allowed_uids: set[int]):
        if len(os.fsencode(path)) >= 108:
            raise OrchestratorError(f"confirmation socket path too long for AF_UNIX: {path}")
        if path.exists() or path.is_symlink():
            raise OrchestratorError(f"{path} already exists; a run directory is used once")
        self.path = path
        self.allowed_uids = allowed_uids
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        old_umask = os.umask(0o177)
        try:
            self.server.bind(str(path))
        finally:
            os.umask(old_umask)
        os.chmod(path, 0o600)
        uid, gid = invoking_identity()
        if os.geteuid() == 0 and uid != 0:
            os.chown(path, uid, gid)
        self.server.listen(4)
        self.server.settimeout(0.0)

    def poll(self, wait_seconds: float) -> tuple[socket.socket, dict[str, Any]] | None:
        """One pending connection with its peer, or None after wait_seconds."""
        import select
        ready, _, _ = select.select([self.server], [], [], max(0.0, wait_seconds))
        if not ready:
            return None
        try:
            connection, _ = self.server.accept()
        except (BlockingIOError, InterruptedError):
            return None
        connection.settimeout(2.0)
        return connection, socket_peer(connection)

    @staticmethod
    def read_request(connection: socket.socket) -> Any:
        data = b""
        while b"\n" not in data and len(data) <= SCENE_REQUEST_MAX_BYTES:
            chunk = connection.recv(4096)
            if not chunk:
                break
            data += chunk
        if len(data) > SCENE_REQUEST_MAX_BYTES:
            raise ValueError("request too large")
        return json.loads(data.split(b"\n", 1)[0].decode())

    @staticmethod
    def reply(connection: socket.socket, payload: dict[str, Any]) -> None:
        try:
            connection.sendall(json.dumps(payload).encode() + b"\n")
        except OSError:
            pass
        finally:
            connection.close()

    def close(self) -> None:
        self.server.close()
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass


def write_json_exclusive(path: Path, payload: dict[str, Any]) -> None:
    """Writes once (a second confirmation never replaces the first), atomically."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    tmp.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    try:
        os.link(tmp, path)
    finally:
        tmp.unlink()


def send_unix_json(socket_path: str, payload: dict[str, Any], timeout_seconds: float) -> dict[str, Any]:
    rendered = json.dumps(payload, separators=(",", ":")) + "\n"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout_seconds)
        client.connect(socket_path)
        client.sendall(rendered.encode("utf-8"))
        client.shutdown(socket.SHUT_WR)

        chunks: list[bytes] = []
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    return json.loads(b"".join(chunks).decode("utf-8"))


def socket_absent_or_stale_error(exc: OSError) -> bool:
    if isinstance(exc, (FileNotFoundError, ConnectionRefusedError)):
        return True
    return exc.errno in {
        errno.ENOENT,
        errno.ECONNREFUSED,
        errno.ENOTSOCK,
    }


def response_accepted(response: dict[str, Any]) -> bool:
    return response.get("ok") is True and (
        response.get("accepted") is True or response.get("duplicate") is True
    )


def parse_env_items(items: list[str]) -> dict[str, str]:
    parsed: dict[str, str] = {}
    for item in items:
        if "=" not in item:
            raise OrchestratorError(f"environment override must be KEY=VALUE: {item}")
        key, value = item.split("=", 1)
        if not key:
            raise OrchestratorError(f"environment override has empty key: {item}")
        parsed[key] = value
    return parsed


def resolve_orange_local_control_log_path(args: argparse.Namespace) -> str:
    if args.orange_local_control_log:
        return args.orange_local_control_log
    orange_env = parse_env_items(args.orange_env)
    for key in ("ORANGE_GUI_LOCAL_CONTROL_LOG", "ORANGE_LOCAL_CONTROL_LOG"):
        value = orange_env.get(key) or os.environ.get(key, "")
        if value:
            return value
    return f"{args.orange_socket}.events.jsonl"


def orange_process_env_overlay(args: argparse.Namespace) -> dict[str, str]:
    return {
        **parse_env_items(args.orange_env),
        "ORANGE_GUI_LOCAL_CONTROL_SOCKET": args.orange_socket,
        "ORANGE_GUI_LOCAL_CONTROL_LOG": resolve_orange_local_control_log_path(args),
        "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START": "1",
        "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP": "1",
        "ORANGE_GUI_AUTORUN_START_RECORDING": "0",
        "ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE": "0",
        "ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE": "1",
    }


def parse_labeled_command_items(items: list[str]) -> list[dict[str, str]]:
    parsed: list[dict[str, str]] = []
    for item in items:
        if "=" not in item:
            raise OrchestratorError(f"validation command must be LABEL=COMMAND: {item}")
        label, command = item.split("=", 1)
        if not label:
            raise OrchestratorError(f"validation command has empty label: {item}")
        if not command.strip():
            raise OrchestratorError(f"validation command has empty command: {item}")
        parsed.append({"label": label, "command": command})
    return parsed


def parse_labeled_path_items(items: list[str]) -> dict[str, list[str]]:
    parsed: dict[str, list[str]] = {}
    for item in items:
        if "=" not in item:
            raise OrchestratorError(f"validation artifact must be LABEL=PATH: {item}")
        label, path = item.split("=", 1)
        if not label:
            raise OrchestratorError(f"validation artifact has empty label: {item}")
        if not path.strip():
            raise OrchestratorError(f"validation artifact has empty path: {item}")
        parsed.setdefault(label, []).append(path)
    return parsed


def validation_commands_from_args(args: argparse.Namespace) -> list[dict[str, str]]:
    commands = parse_labeled_command_items(args.validation_command)
    for index, command in enumerate(args.orange_validation_command, start=1):
        if not command.strip():
            raise OrchestratorError("--orange-validation-command cannot be empty")
        commands.append({"label": f"orange_validation_{index}", "command": command})
    for index, command in enumerate(args.citrus_validation_command, start=1):
        if not command.strip():
            raise OrchestratorError("--citrus-validation-command cannot be empty")
        commands.append({"label": f"citrus_validation_{index}", "command": command})
    return commands


def validation_artifacts_from_args(args: argparse.Namespace) -> dict[str, list[str]]:
    return parse_labeled_path_items(args.validation_artifact)


def tail_text(value: str, max_chars: int) -> tuple[str, bool]:
    if max_chars <= 0 or len(value) <= max_chars:
        return value, False
    return value[-max_chars:], True


def safe_artifact_name(value: str) -> str:
    safe = "".join(
        char if char.isalnum() or char in "._-" else "_"
        for char in value
    ).strip("._-")
    return safe or "artifact"


def json_bool(value: Any) -> bool:
    return value is True


def render_validation_command(
    command: str,
    *,
    operation_id: str,
    orange_status: dict[str, Any],
    citrus_status: dict[str, Any],
) -> str:
    raw_values = {
        "{operation_id}": operation_id,
        "{orange_recording_folder}": json_path(orange_status, ["recording", "folder"], ""),
        "{citrus_perf_jsonl_path}": json_path(citrus_status, ["output", "perf_jsonl_path"], ""),
    }
    for placeholder, value in raw_values.items():
        if placeholder not in command:
            continue
        if value is None or str(value) == "":
            raise OrchestratorError(
                f"validation command requires {placeholder}, but its status value is missing"
            )
    replacements = {
        placeholder: shlex.quote(str(value))
        for placeholder, value in raw_values.items()
    }
    rendered = command
    for needle, value in replacements.items():
        rendered = rendered.replace(needle, value)
    return rendered


def orange_ready_for_recording(status: dict[str, Any]) -> bool:
    if not json_bool(
        json_path(status, ["readiness", "ready_for_recording_request"], False)
    ):
        return False
    autorun_stage = str(status.get("autorun_stage", ""))
    return autorun_stage in ("", "disabled", "done")


def orange_wait_failure(
    status: dict[str, Any],
    *,
    description: str,
    operation_id: str,
) -> str:
    autorun_stage = str(status.get("autorun_stage", ""))
    if autorun_stage == "failed":
        return "Orange GUI autorun reported stage=failed"

    if description != "ready_for_citrus_experiment":
        return ""
    if not orange_recording_start_failed_for_operation(status, operation_id):
        return ""
    return (
        "Orange GUI recording start failed for operation_id="
        f"{operation_id}"
    )


def orange_recording_start_failed_for_operation(
    status: dict[str, Any],
    operation_id: str,
) -> bool:
    recording_start = json_path(status, ["local_control", "recording_start"], {})
    if not isinstance(recording_start, dict):
        return False
    if str(recording_start.get("last_event", "")) != "start_failed":
        return False
    observed_operation_id = str(recording_start.get("operation_id", ""))
    return observed_operation_id == operation_id


def orange_ready_for_citrus(status: dict[str, Any]) -> bool:
    return json_bool(json_path(status, ["readiness", "ready_for_citrus_experiment"], False))


def orange_recording_covers_active_citrus(status: dict[str, Any]) -> bool:
    return (
        json_bool(json_path(status, ["readiness", "recording_active"], False))
        and not orange_recording_finalized(status)
    )


def orange_recording_finalized(status: dict[str, Any]) -> bool:
    return json_bool(json_path(status, ["readiness", "recording_finalized"], False))


def orange_recording_folder(status: dict[str, Any]) -> str:
    value = json_path(status, ["recording", "folder"], "")
    return value if isinstance(value, str) else ""


def orange_local_control_recording_stop(status: dict[str, Any]) -> dict[str, Any]:
    value = json_path(status, ["local_control", "recording_stop"], {})
    return value if isinstance(value, dict) else {}


def orange_recording_stop_drain_timed_out(status: dict[str, Any]) -> bool:
    return json_bool(orange_local_control_recording_stop(status).get("drain_timed_out", False))


def orange_recording_stop_state(status: dict[str, Any]) -> str:
    return str(orange_local_control_recording_stop(status).get("state", ""))


def orange_recording_stop_health(status: dict[str, Any]) -> str:
    return str(orange_local_control_recording_stop(status).get("health", ""))


def orange_recording_stop_ack_state(status: dict[str, Any]) -> str:
    return str(orange_local_control_recording_stop(status).get("ack_state", ""))


def orange_recording_stop_error_code(status: dict[str, Any]) -> str:
    return str(orange_local_control_recording_stop(status).get("error_code", ""))


def orange_recording_stop_method(status: dict[str, Any]) -> str:
    return str(orange_local_control_recording_stop(status).get("method", ""))


def orange_recording_stop_operation_id(status: dict[str, Any]) -> str:
    return str(orange_local_control_recording_stop(status).get("operation_id", ""))


def orange_recording_stop_source(status: dict[str, Any]) -> str:
    stop = orange_local_control_recording_stop(status)
    return str(stop.get("command_source", stop.get("source", "")))


def orange_recording_stop_terminal_state(status: dict[str, Any]) -> str:
    return str(orange_local_control_recording_stop(status).get("terminal_state", ""))


def orange_recording_stop_reason(status: dict[str, Any]) -> str:
    return str(orange_local_control_recording_stop(status).get("reason", ""))


def orange_recording_stop_forced_finalize_requested(status: dict[str, Any]) -> bool:
    return json_bool(
        orange_local_control_recording_stop(status).get(
            "forced_finalize_requested",
            False,
        )
    )


def orange_recording_stop_forced_finalize_stream_stop_requested(
    status: dict[str, Any],
) -> bool:
    return json_bool(
        orange_local_control_recording_stop(status).get(
            "forced_finalize_stream_stop_requested",
            False,
        )
    )


def check_orange_drain_timeout_status(status: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    stop = orange_local_control_recording_stop(status)
    for field in (
        "drain_timed_out",
        "forced_finalize_requested",
        "forced_finalize_stream_stop_requested",
    ):
        if field in stop and not isinstance(stop.get(field), bool):
            failures.append(
                "Orange status local_control.recording_stop."
                f"{field}={stop.get(field)!r}; expected JSON boolean"
            )
    timed_out = orange_recording_stop_drain_timed_out(status)
    ack_state = orange_recording_stop_ack_state(status)
    error_code = orange_recording_stop_error_code(status)
    forced_finalize_requested = orange_recording_stop_forced_finalize_requested(status)
    forced_stream_stop_requested = (
        orange_recording_stop_forced_finalize_stream_stop_requested(status)
    )
    state = orange_recording_stop_state(status)
    if not timed_out:
        if ack_state == "failed_timeout":
            failures.append(
                "Orange status reports failed-timeout ACK but "
                "local_control.recording_stop.drain_timed_out is not true"
            )
        if forced_finalize_requested:
            failures.append(
                "Orange status reports forced finalize but "
                "local_control.recording_stop.drain_timed_out is not true"
            )
        if forced_stream_stop_requested:
            failures.append(
                "Orange status reports forced stream-stop but "
                "local_control.recording_stop.drain_timed_out is not true"
            )
        if state == "finalized_after_drain_timeout":
            failures.append(
                "Orange status reports finalized-after-drain-timeout state but "
                "local_control.recording_stop.drain_timed_out is not true"
            )
        return failures
    if ack_state and ack_state != "failed_timeout":
        failures.append(
            "Orange status reports drain timeout but "
            f"local_control.recording_stop.ack_state={ack_state!r}"
        )
    if error_code != "drain_timeout":
        failures.append(
            "Orange status reports drain timeout but "
            f"local_control.recording_stop.error_code={error_code!r}"
        )
    if not forced_finalize_requested:
        failures.append(
            "Orange status reports drain timeout but "
            "local_control.recording_stop.forced_finalize_requested is not true"
        )
    if (
        orange_recording_finalized(status)
        or state == "finalized_after_drain_timeout"
    ) and not forced_stream_stop_requested:
        failures.append(
            "Orange status reports finalized-after-drain-timeout but "
            "local_control.recording_stop.forced_finalize_stream_stop_requested "
            "is not true"
        )
    return failures


def summarize_orange_drain_timeout_status(
    status: dict[str, Any],
    *,
    allow_drain_timeout: bool,
) -> dict[str, Any]:
    timed_out = orange_recording_stop_drain_timed_out(status)
    consistency_failures = check_orange_drain_timeout_status(status)
    policy_failures: list[str] = []
    if timed_out and not allow_drain_timeout:
        policy_failures.append("Orange recording drain timed out")
    return {
        "ok": not consistency_failures and not policy_failures,
        "drain_timed_out": timed_out,
        "allow_drain_timeout": bool(allow_drain_timeout),
        "state": orange_recording_stop_state(status),
        "ack_state": orange_recording_stop_ack_state(status),
        "health": orange_recording_stop_health(status),
        "error_code": orange_recording_stop_error_code(status),
        "forced_finalize_requested": (
            orange_recording_stop_forced_finalize_requested(status)
        ),
        "forced_finalize_stream_stop_requested": (
            orange_recording_stop_forced_finalize_stream_stop_requested(status)
        ),
        "consistency_ok": not consistency_failures,
        "consistency_failures": consistency_failures,
        "policy_ok": not policy_failures,
        "policy_failures": policy_failures,
        "failures": [*consistency_failures, *policy_failures],
    }


def check_orange_stop_ack_status(
    status: dict[str, Any],
    *,
    stop_policy: str,
    finalized_wait_skipped: bool,
) -> list[str]:
    if stop_policy == "none":
        return []
    ack_state = orange_recording_stop_ack_state(status)
    if not ack_state:
        return ["Orange status missing local_control.recording_stop.ack_state"]
    if orange_recording_stop_drain_timed_out(status):
        return [] if ack_state == "failed_timeout" else [
            "Orange drain timed out but "
            f"local_control.recording_stop.ack_state={ack_state!r}"
        ]
    if orange_recording_finalized(status):
        return [] if ack_state == "executed" else [
            "Orange recording finalized cleanly but "
            f"local_control.recording_stop.ack_state={ack_state!r}"
        ]
    if finalized_wait_skipped and ack_state in {"accepted", "executing"}:
        return []
    if ack_state == "ignored":
        return ["Orange stop request was ignored"]
    return []


def summarize_orange_stop_ack_status(
    status: dict[str, Any],
    *,
    stop_policy: str,
    finalized_wait_skipped: bool,
) -> dict[str, Any]:
    failures = check_orange_stop_ack_status(
        status,
        stop_policy=stop_policy,
        finalized_wait_skipped=finalized_wait_skipped,
    )
    return {
        "ok": not failures,
        "stop_policy": stop_policy,
        "finalized_wait_skipped": bool(finalized_wait_skipped),
        "ack_state": orange_recording_stop_ack_state(status),
        "recording_finalized": orange_recording_finalized(status),
        "drain_timed_out": orange_recording_stop_drain_timed_out(status),
        "failures": failures,
    }


def check_orange_citrus_notify_stop_status(
    orange_status: dict[str, Any],
    citrus_status: dict[str, Any],
    *,
    operation_id: str,
) -> list[str]:
    failures: list[str] = []
    method = orange_recording_stop_method(orange_status)
    if method != "citrus_completion":
        failures.append(
            "Orange final status did not record Citrus completion stop: "
            f"method={method!r}"
        )

    source = orange_recording_stop_source(orange_status)
    if source != "citrus":
        failures.append(
            "Orange final status did not record Citrus as stop source: "
            f"source={source!r}"
        )

    stop_operation_id = orange_recording_stop_operation_id(orange_status)
    # An autorun Citrus completes under its own experiment id (citexp_...),
    # which its status reports as experiment.last_operation_id once
    # start_experiment is accepted (citrus-agent, 2026-11-15).
    citrus_operation_id = json_path(citrus_status, ["experiment", "last_operation_id"], "")
    allowed = {value for value in (operation_id, citrus_operation_id) if value}
    if allowed and stop_operation_id not in allowed:
        failures.append(
            "Orange Citrus completion stop operation id mismatch: "
            f"operation_id={stop_operation_id!r}; expected one of {sorted(allowed)!r}"
        )

    terminal_state = citrus_terminal_state(citrus_status)
    if terminal_state:
        stop_terminal_state = orange_recording_stop_terminal_state(orange_status)
        if stop_terminal_state != terminal_state:
            failures.append(
                "Orange Citrus completion stop terminal state mismatch: "
                f"terminal_state={stop_terminal_state!r}; expected {terminal_state!r}"
            )

    terminal_reason = citrus_terminal_reason(citrus_status)
    if terminal_reason:
        stop_reason = orange_recording_stop_reason(orange_status)
        if stop_reason != terminal_reason:
            failures.append(
                "Orange Citrus completion stop reason mismatch: "
                f"reason={stop_reason!r}; expected {terminal_reason!r}"
            )
    return failures


def summarize_orange_citrus_notify_stop_status(
    orange_status: dict[str, Any],
    citrus_status: dict[str, Any],
    *,
    stop_policy: str,
    operation_id: str,
) -> dict[str, Any]:
    if stop_policy != "citrus_completion_notify":
        return {
            "ok": True,
            "skipped": True,
            "stop_policy": stop_policy,
            "failures": [],
        }
    failures = check_orange_citrus_notify_stop_status(
        orange_status,
        citrus_status,
        operation_id=operation_id,
    )
    return {
        "ok": not failures,
        "skipped": False,
        "stop_policy": stop_policy,
        "operation_id": operation_id,
        "method": orange_recording_stop_method(orange_status),
        "source": orange_recording_stop_source(orange_status),
        "stop_operation_id": orange_recording_stop_operation_id(orange_status),
        "terminal_state": orange_recording_stop_terminal_state(orange_status),
        "terminal_reason": orange_recording_stop_reason(orange_status),
        "expected_terminal_state": citrus_terminal_state(citrus_status),
        "expected_terminal_reason": citrus_terminal_reason(citrus_status),
        "failures": failures,
    }


def infer_orange_finalized_status_from_session(
    base_status: dict[str, Any],
    *,
    operation_id: str,
) -> dict[str, Any] | None:
    folder = orange_recording_folder(base_status)
    if not folder:
        return None
    session_path = Path(folder) / "recording_session.json"
    try:
        payload = json.loads(session_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None

    control = json_path(payload, ["recording", "control"], {})
    if not isinstance(control, dict):
        return None

    control_operation_id = str(control.get("operation_id", ""))
    if operation_id and control_operation_id != operation_id:
        return None

    last_event = str(control.get("last_event", ""))
    drain_completed = control.get("drain_completed") is True
    if not drain_completed or last_event not in {
        "finalized",
        "finalized_after_drain_timeout",
    }:
        return None

    status = copy.deepcopy(base_status)
    status["phase"] = "recording_finalized"

    readiness = status.get("readiness", {})
    readiness = readiness if isinstance(readiness, dict) else {}
    readiness = dict(readiness)
    readiness["recording_active"] = False
    readiness["recording_finalizing"] = False
    readiness["recording_finalized"] = True
    status["readiness"] = readiness

    recording = status.get("recording", {})
    recording = recording if isinstance(recording, dict) else {}
    recording = dict(recording)
    recording["folder"] = folder
    status["recording"] = recording

    local_control = status.get("local_control", {})
    local_control = local_control if isinstance(local_control, dict) else {}
    local_control = dict(local_control)
    recording_stop = local_control.get("recording_stop", {})
    recording_stop = recording_stop if isinstance(recording_stop, dict) else {}
    recording_stop = dict(recording_stop)
    recording_stop.update(control)
    recording_stop["drain_active"] = False
    if drain_completed:
        recording_stop["drain_completed"] = True
    if "state" not in recording_stop or not recording_stop["state"]:
        if last_event == "finalized_after_drain_timeout":
            recording_stop["state"] = "finalized_after_drain_timeout"
        elif drain_completed:
            recording_stop["state"] = "finalized"
    if "ack_state" not in recording_stop or not recording_stop["ack_state"]:
        recording_stop["ack_state"] = (
            "failed_timeout" if recording_stop.get("drain_timed_out") is True else "executed"
        )
    local_control["recording_stop"] = recording_stop
    if str(recording_stop.get("method", "")) == "citrus_completion":
        local_control["citrus_completion_stop"] = dict(recording_stop)
    status["local_control"] = local_control
    status["orchestrator_inferred_recording_session"] = str(session_path)
    return status


def summarize_orange_local_control_event_log(path: str) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "path": path,
        "exists": False,
        "row_count": 0,
        "socket_event_count": 0,
        "gui_event_count": 0,
        "invalid_row_count": 0,
        "events": {},
        "socket_request_events": [],
        "gui_lifecycle_events": [],
        "request_ids": [],
        "operation_ids": [],
        "last_gui_event": "",
        "first_socket_received_at_utc": "",
        "last_socket_received_at_utc": "",
        "last_socket_responded_at_utc": "",
        "first_gui_event_at_utc": "",
        "last_gui_event_at_utc": "",
        "has_start_triggered": False,
        "has_stop_scheduled": False,
        "has_stop_triggered": False,
        "has_drain_timeout": False,
        "has_forced_finalize_requested": False,
        "has_drain_finalized": False,
    }
    if not path:
        summary["reason"] = "empty_path"
        return summary
    log_path = Path(path)
    if not log_path.exists():
        summary["reason"] = "missing"
        return summary

    summary["exists"] = True
    request_ids: set[str] = set()
    operation_ids: set[str] = set()
    events: dict[str, int] = {}
    socket_request_events: list[dict[str, Any]] = []
    gui_lifecycle_events: list[dict[str, Any]] = []
    try:
        with log_path.open("r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                summary["row_count"] += 1
                row_index = int(summary["row_count"])
                try:
                    row = json.loads(line)
                except json.JSONDecodeError:
                    summary["invalid_row_count"] += 1
                    continue
                if isinstance(row, dict) and "request" in row and "response" in row:
                    summary["socket_event_count"] += 1
                    request = row.get("request")
                    request = request if isinstance(request, dict) else {}
                    response = row.get("response")
                    response = response if isinstance(response, dict) else {}
                    request_id = response.get("request_id") or request.get("request_id")
                    operation_id = response.get("operation_id") or request.get("operation_id")
                    socket_event = {
                        "row_index": row_index,
                        "request_id": request_id or "",
                        "operation_id": operation_id or "",
                        "method": response.get("method") or request.get("method", ""),
                        "source": request.get("source", ""),
                        "received_at_utc": row.get("received_at_utc", ""),
                        "ok": response.get("ok", False),
                        "accepted": response.get("accepted", False),
                    }
                    copy_present_fields(socket_event, response, SOCKET_RESPONSE_EVENT_FIELDS)
                    socket_request_events.append(socket_event)
                    received_at_utc = str(socket_event.get("received_at_utc", ""))
                    if received_at_utc:
                        if not summary["first_socket_received_at_utc"]:
                            summary["first_socket_received_at_utc"] = received_at_utc
                        summary["last_socket_received_at_utc"] = received_at_utc
                    responded_at_utc = str(socket_event.get("responded_at_utc", ""))
                    if responded_at_utc:
                        summary["last_socket_responded_at_utc"] = responded_at_utc
                elif (
                    isinstance(row, dict)
                    and row.get("schema_id") == "orange.local_control.gui_event"
                ):
                    summary["gui_event_count"] += 1
                    event = str(row.get("event", ""))
                    if event:
                        events[event] = events.get(event, 0) + 1
                        summary["last_gui_event"] = event
                    lifecycle_event = {
                        "row_index": row_index,
                        "event": event,
                        "request_id": row.get("request_id", ""),
                        "operation_id": row.get("operation_id", ""),
                        "method": row.get("method", ""),
                        "command_source": row.get("command_source", ""),
                        "terminal_state": row.get("terminal_state", ""),
                        "reason": row.get("reason", ""),
                    }
                    copy_present_fields(lifecycle_event, row, GUI_LIFECYCLE_EVENT_FIELDS)
                    gui_lifecycle_events.append(lifecycle_event)
                    event_at_utc = str(lifecycle_event.get("event_at_utc", ""))
                    if event_at_utc:
                        if not summary["first_gui_event_at_utc"]:
                            summary["first_gui_event_at_utc"] = event_at_utc
                        summary["last_gui_event_at_utc"] = event_at_utc
                    request_id = row.get("request_id")
                    operation_id = row.get("operation_id")
                else:
                    request_id = None
                    operation_id = None

                if isinstance(request_id, str) and request_id:
                    request_ids.add(request_id)
                if isinstance(operation_id, str) and operation_id:
                    operation_ids.add(operation_id)
    except OSError as exc:
        summary["read_error"] = str(exc)

    summary["events"] = dict(sorted(events.items()))
    summary["socket_request_events"] = socket_request_events
    summary["gui_lifecycle_events"] = gui_lifecycle_events
    summary["request_ids"] = sorted(request_ids)
    summary["operation_ids"] = sorted(operation_ids)
    summary["has_start_triggered"] = events.get("recording_start_triggered", 0) > 0
    summary["has_stop_scheduled"] = events.get("recording_stop_scheduled", 0) > 0
    summary["has_stop_triggered"] = events.get("recording_stop_triggered", 0) > 0
    summary["has_drain_timeout"] = events.get("recording_drain_timeout", 0) > 0
    summary["has_forced_finalize_requested"] = (
        events.get("recording_drain_forced_finalize_requested", 0) > 0
    )
    summary["has_drain_finalized"] = events.get("recording_drain_finalized", 0) > 0
    return summary


def event_log_lifecycle_events_for_request(
    event_log: dict[str, Any],
    request_id: str,
    *,
    event_name: str | None = None,
) -> list[dict[str, Any]]:
    rows = event_log.get("gui_lifecycle_events", [])
    if not isinstance(rows, list):
        return []
    matches: list[dict[str, Any]] = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        if str(row.get("request_id", "")) != request_id:
            continue
        if event_name is not None and str(row.get("event", "")) != event_name:
            continue
        matches.append(row)
    return matches


def event_log_socket_events_for_request(
    event_log: dict[str, Any],
    request_id: str,
) -> list[dict[str, Any]]:
    rows = event_log.get("socket_request_events", [])
    if not isinstance(rows, list):
        return []
    matches: list[dict[str, Any]] = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        if str(row.get("request_id", "")) == request_id:
            matches.append(row)
    return matches


def event_log_first_row_index(rows: list[dict[str, Any]]) -> int | None:
    indexes: list[int] = []
    for row in rows:
        try:
            index = int(row.get("row_index", 0))
        except (TypeError, ValueError):
            continue
        if index > 0:
            indexes.append(index)
    return min(indexes) if indexes else None


def event_log_queued_socket_events(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [row for row in rows if row.get("queued_for_gui_thread") is True]


def event_log_row_indexes(rows: list[dict[str, Any]]) -> list[int]:
    indexes: list[int] = []
    for row in rows:
        try:
            index = int(row.get("row_index", 0))
        except (TypeError, ValueError):
            continue
        if index > 0:
            indexes.append(index)
    return sorted(indexes)


def event_log_string_values(rows: list[dict[str, Any]], field: str) -> list[str]:
    values = sorted(
        {
            str(row.get(field, ""))
            for row in rows
            if str(row.get(field, ""))
        }
    )
    return values


def event_log_bool_values(rows: list[dict[str, Any]], field: str) -> list[bool]:
    values = sorted(
        {
            row[field]
            for row in rows
            if isinstance(row.get(field), bool)
        }
    )
    return values


def summarize_event_log_request_chain(
    event_log: dict[str, Any],
    request_id: str,
) -> dict[str, Any]:
    socket_events = event_log_socket_events_for_request(event_log, request_id)
    queued_socket_events = event_log_queued_socket_events(socket_events)
    gui_accepted_events = event_log_lifecycle_events_for_request(
        event_log,
        request_id,
        event_name="gui_command_accepted",
    )
    start_queued_events = event_log_lifecycle_events_for_request(
        event_log,
        request_id,
        event_name="recording_start_queued",
    )
    start_triggered_events = event_log_lifecycle_events_for_request(
        event_log,
        request_id,
        event_name="recording_start_triggered",
    )
    stop_scheduled_events = event_log_lifecycle_events_for_request(
        event_log,
        request_id,
        event_name="recording_stop_scheduled",
    )
    stop_triggered_events = event_log_lifecycle_events_for_request(
        event_log,
        request_id,
        event_name="recording_stop_triggered",
    )
    drain_finalized_events = event_log_lifecycle_events_for_request(
        event_log,
        request_id,
        event_name="recording_drain_finalized",
    )
    drain_timeout_events = event_log_lifecycle_events_for_request(
        event_log,
        request_id,
        event_name="recording_drain_timeout",
    )
    forced_finalize_events = event_log_lifecycle_events_for_request(
        event_log,
        request_id,
        event_name="recording_drain_forced_finalize_requested",
    )
    return {
        "request_id": request_id,
        "socket_rows": len(socket_events),
        "queued_socket_rows": len(queued_socket_events),
        "gui_command_accepted_rows": len(gui_accepted_events),
        "recording_start_queued_rows": len(start_queued_events),
        "recording_start_triggered_rows": len(start_triggered_events),
        "recording_stop_scheduled_rows": len(stop_scheduled_events),
        "recording_stop_triggered_rows": len(stop_triggered_events),
        "recording_drain_finalized_rows": len(drain_finalized_events),
        "recording_drain_timeout_rows": len(drain_timeout_events),
        "recording_drain_forced_finalize_requested_rows": len(forced_finalize_events),
        "has_socket": bool(socket_events),
        "has_queued_socket": bool(queued_socket_events),
        "has_gui_command_accepted": bool(gui_accepted_events),
        "has_recording_start_queued": bool(start_queued_events),
        "has_recording_start_triggered": bool(start_triggered_events),
        "has_recording_stop_scheduled": bool(stop_scheduled_events),
        "has_recording_stop_triggered": bool(stop_triggered_events),
        "has_recording_drain_finalized": bool(drain_finalized_events),
        "has_recording_drain_timeout": bool(drain_timeout_events),
        "has_recording_drain_forced_finalize_requested": bool(forced_finalize_events),
        "socket_ok_values": event_log_bool_values(socket_events, "ok"),
        "socket_accepted_values": event_log_bool_values(socket_events, "accepted"),
        "socket_queued_for_gui_thread_values": event_log_bool_values(
            socket_events,
            "queued_for_gui_thread",
        ),
        "socket_methods": event_log_string_values(socket_events, "method"),
        "socket_sources": event_log_string_values(socket_events, "source"),
        "gui_command_accepted_methods": event_log_string_values(
            gui_accepted_events,
            "method",
        ),
        "gui_command_accepted_sources": event_log_string_values(
            gui_accepted_events,
            "command_source",
        ),
        "gui_command_accepted_start_enabled_values": event_log_bool_values(
            gui_accepted_events,
            "start_enabled",
        ),
        "gui_command_accepted_stop_enabled_values": event_log_bool_values(
            gui_accepted_events,
            "stop_enabled",
        ),
        "gui_command_accepted_stop_recording_enabled_values": event_log_bool_values(
            gui_accepted_events,
            "stop_recording_enabled",
        ),
        "gui_command_accepted_citrus_completion_enabled_values": event_log_bool_values(
            gui_accepted_events,
            "citrus_completion_enabled",
        ),
        "recording_start_queued_methods": event_log_string_values(
            start_queued_events,
            "method",
        ),
        "recording_start_queued_sources": event_log_string_values(
            start_queued_events,
            "command_source",
        ),
        "recording_start_triggered_methods": event_log_string_values(
            start_triggered_events,
            "method",
        ),
        "recording_start_triggered_sources": event_log_string_values(
            start_triggered_events,
            "command_source",
        ),
        "recording_stop_scheduled_methods": event_log_string_values(
            stop_scheduled_events,
            "method",
        ),
        "recording_stop_scheduled_sources": event_log_string_values(
            stop_scheduled_events,
            "command_source",
        ),
        "recording_stop_scheduled_terminal_states": event_log_string_values(
            stop_scheduled_events,
            "terminal_state",
        ),
        "recording_stop_scheduled_reasons": event_log_string_values(
            stop_scheduled_events,
            "reason",
        ),
        "recording_stop_triggered_methods": event_log_string_values(
            stop_triggered_events,
            "method",
        ),
        "recording_stop_triggered_sources": event_log_string_values(
            stop_triggered_events,
            "command_source",
        ),
        "recording_stop_triggered_terminal_states": event_log_string_values(
            stop_triggered_events,
            "terminal_state",
        ),
        "recording_stop_triggered_reasons": event_log_string_values(
            stop_triggered_events,
            "reason",
        ),
        "recording_drain_finalized_methods": event_log_string_values(
            drain_finalized_events,
            "method",
        ),
        "recording_drain_finalized_sources": event_log_string_values(
            drain_finalized_events,
            "command_source",
        ),
        "recording_drain_finalized_terminal_states": event_log_string_values(
            drain_finalized_events,
            "terminal_state",
        ),
        "recording_drain_finalized_reasons": event_log_string_values(
            drain_finalized_events,
            "reason",
        ),
        "recording_drain_finalized_drain_timed_out_values": (
            event_log_bool_values(drain_finalized_events, "drain_timed_out")
        ),
        "recording_drain_finalized_healths": event_log_string_values(
            drain_finalized_events,
            "health",
        ),
        "recording_drain_finalized_error_codes": event_log_string_values(
            drain_finalized_events,
            "error_code",
        ),
        "recording_drain_timeout_methods": event_log_string_values(
            drain_timeout_events,
            "method",
        ),
        "recording_drain_timeout_sources": event_log_string_values(
            drain_timeout_events,
            "command_source",
        ),
        "recording_drain_timeout_forced_finalize_requested_values": (
            event_log_bool_values(drain_timeout_events, "forced_finalize_requested")
        ),
        "recording_drain_timeout_healths": event_log_string_values(
            drain_timeout_events,
            "health",
        ),
        "recording_drain_timeout_error_codes": event_log_string_values(
            drain_timeout_events,
            "error_code",
        ),
        "recording_drain_forced_finalize_requested_methods": event_log_string_values(
            forced_finalize_events,
            "method",
        ),
        "recording_drain_forced_finalize_requested_sources": event_log_string_values(
            forced_finalize_events,
            "command_source",
        ),
        "recording_drain_forced_finalize_requested_actions": (
            event_log_string_values(forced_finalize_events, "action")
        ),
        "recording_drain_forced_finalize_requested_healths": (
            event_log_string_values(forced_finalize_events, "health")
        ),
        "recording_drain_forced_finalize_requested_error_codes": (
            event_log_string_values(forced_finalize_events, "error_code")
        ),
        "socket_row_indexes": event_log_row_indexes(socket_events),
        "queued_socket_row_indexes": event_log_row_indexes(queued_socket_events),
        "gui_command_accepted_row_indexes": event_log_row_indexes(gui_accepted_events),
        "recording_start_queued_row_indexes": event_log_row_indexes(start_queued_events),
        "recording_start_triggered_row_indexes": (
            event_log_row_indexes(start_triggered_events)
        ),
        "recording_stop_scheduled_row_indexes": (
            event_log_row_indexes(stop_scheduled_events)
        ),
        "recording_stop_triggered_row_indexes": (
            event_log_row_indexes(stop_triggered_events)
        ),
        "recording_drain_finalized_row_indexes": (
            event_log_row_indexes(drain_finalized_events)
        ),
        "recording_drain_timeout_row_indexes": event_log_row_indexes(drain_timeout_events),
        "recording_drain_forced_finalize_requested_row_indexes": (
            event_log_row_indexes(forced_finalize_events)
        ),
    }


def check_socket_queued_for_gui_thread(
    failures: list[str],
    *,
    request_id: str,
    socket_events: list[dict[str, Any]],
    label: str,
) -> list[dict[str, Any]]:
    queued_events = event_log_queued_socket_events(socket_events)
    if socket_events and not queued_events:
        failures.append(
            "Orange local-control event log "
            f"{label} socket request/response for request_id={request_id} "
            "never reported queued_for_gui_thread=true"
        )
    return queued_events


def check_event_log_row_order(
    failures: list[str],
    *,
    request_id: str,
    earlier_rows: list[dict[str, Any]],
    earlier_label: str,
    later_rows: list[dict[str, Any]],
    later_label: str,
) -> None:
    earlier_index = event_log_first_row_index(earlier_rows)
    later_index = event_log_first_row_index(later_rows)
    if earlier_index is None or later_index is None:
        return
    if earlier_index > later_index:
        failures.append(
            "Orange local-control event log out-of-order rows for "
            f"request_id={request_id}: {earlier_label} row_index={earlier_index} "
            f"after {later_label} row_index={later_index}"
        )


def check_stop_lifecycle_event_details(
    event_log: dict[str, Any],
    *,
    stop_status: dict[str, Any],
    stop_policy: str,
) -> list[str]:
    if stop_policy == "none":
        return []
    stop_request_id = str(stop_status.get("request_id", ""))
    if not stop_request_id:
        return []

    failures: list[str] = []
    expected_method = str(stop_status.get("method", ""))
    expected_source = str(stop_status.get("command_source", stop_status.get("source", "")))
    expected_operation_id = str(stop_status.get("operation_id", ""))
    expected_terminal_state = str(stop_status.get("terminal_state", ""))
    expected_reason = str(stop_status.get("reason", ""))
    if not expected_method:
        failures.append("Orange status missing local_control.recording_stop.method")
    if not expected_source:
        failures.append(
            "Orange status missing local_control.recording_stop.command_source/source"
        )
    if not expected_operation_id:
        failures.append("Orange status missing local_control.recording_stop.operation_id")
    socket_events = event_log_socket_events_for_request(event_log, stop_request_id)
    queued_socket_events = check_socket_queued_for_gui_thread(
        failures,
        request_id=stop_request_id,
        socket_events=socket_events,
        label="stop",
    )
    accepted_events = event_log_lifecycle_events_for_request(
        event_log,
        stop_request_id,
        event_name="gui_command_accepted",
    )
    scheduled_events = event_log_lifecycle_events_for_request(
        event_log,
        stop_request_id,
        event_name="recording_stop_scheduled",
    )
    trigger_events = event_log_lifecycle_events_for_request(
        event_log,
        stop_request_id,
        event_name="recording_stop_triggered",
    )
    finalize_events = event_log_lifecycle_events_for_request(
        event_log,
        stop_request_id,
        event_name="recording_drain_finalized",
    )
    timeout_events = event_log_lifecycle_events_for_request(
        event_log,
        stop_request_id,
        event_name="recording_drain_timeout",
    )
    forced_finalize_events = event_log_lifecycle_events_for_request(
        event_log,
        stop_request_id,
        event_name="recording_drain_forced_finalize_requested",
    )
    if not trigger_events:
        failures.append(
            "Orange local-control event log missing recording_stop_triggered "
            f"for request_id={stop_request_id}"
        )
    if not scheduled_events:
        failures.append(
            "Orange local-control event log missing recording_stop_scheduled "
            f"for request_id={stop_request_id}"
        )
    if not finalize_events:
        failures.append(
            "Orange local-control event log missing recording_drain_finalized "
            f"for request_id={stop_request_id}"
        )
    if not socket_events:
        failures.append(
            "Orange local-control event log missing socket request/response "
            f"for request_id={stop_request_id}"
        )
    if not accepted_events:
        failures.append(
            "Orange local-control event log missing GUI-thread gui_command_accepted "
            f"for request_id={stop_request_id}"
        )
    check_event_log_row_order(
        failures,
        request_id=stop_request_id,
        earlier_rows=queued_socket_events,
        earlier_label="queued socket request/response",
        later_rows=accepted_events,
        later_label="gui_command_accepted",
    )
    check_event_log_row_order(
        failures,
        request_id=stop_request_id,
        earlier_rows=accepted_events,
        earlier_label="gui_command_accepted",
        later_rows=scheduled_events,
        later_label="recording_stop_scheduled",
    )
    check_event_log_row_order(
        failures,
        request_id=stop_request_id,
        earlier_rows=scheduled_events,
        earlier_label="recording_stop_scheduled",
        later_rows=trigger_events,
        later_label="recording_stop_triggered",
    )
    check_event_log_row_order(
        failures,
        request_id=stop_request_id,
        earlier_rows=trigger_events,
        earlier_label="recording_stop_triggered",
        later_rows=finalize_events,
        later_label="recording_drain_finalized",
    )
    check_event_log_row_order(
        failures,
        request_id=stop_request_id,
        earlier_rows=trigger_events,
        earlier_label="recording_stop_triggered",
        later_rows=timeout_events,
        later_label="recording_drain_timeout",
    )
    check_event_log_row_order(
        failures,
        request_id=stop_request_id,
        earlier_rows=timeout_events,
        earlier_label="recording_drain_timeout",
        later_rows=forced_finalize_events,
        later_label="recording_drain_forced_finalize_requested",
    )
    check_event_log_row_order(
        failures,
        request_id=stop_request_id,
        earlier_rows=forced_finalize_events,
        earlier_label="recording_drain_forced_finalize_requested",
        later_rows=finalize_events,
        later_label="recording_drain_finalized",
    )
    stop_reports_timeout = stop_status.get("drain_timed_out") is True
    stop_reports_forced_finalize = (
        stop_status.get("forced_finalize_requested") is True
    )
    stop_reports_forced_stream_stop = (
        stop_status.get("forced_finalize_stream_stop_requested") is True
    )
    expected_finalize_health = str(
        stop_status.get("health", "warning" if stop_reports_timeout else "ok")
    )
    expected_finalize_error_code = str(
        stop_status.get("error_code", "drain_timeout" if stop_reports_timeout else "")
    )
    if timeout_events and not stop_reports_timeout:
        failures.append(
            "Orange local-control event log has recording_drain_timeout for "
            f"request_id={stop_request_id} but "
            "local_control.recording_stop.drain_timed_out is not true"
        )
    if forced_finalize_events and not stop_reports_timeout:
        failures.append(
            "Orange local-control event log has "
            "recording_drain_forced_finalize_requested for "
            f"request_id={stop_request_id} but "
            "local_control.recording_stop.drain_timed_out is not true"
        )
    if forced_finalize_events and not stop_reports_forced_finalize:
        failures.append(
            "Orange local-control event log has "
            "recording_drain_forced_finalize_requested for "
            f"request_id={stop_request_id} but "
            "local_control.recording_stop.forced_finalize_requested is not true"
        )
    if forced_finalize_events and finalize_events and not stop_reports_forced_stream_stop:
        failures.append(
            "Orange local-control event log has finalized forced-finalize "
            f"evidence for request_id={stop_request_id} but "
            "local_control.recording_stop.forced_finalize_stream_stop_requested "
            "is not true"
        )
    for row in finalize_events:
        if row.get("drain_timed_out") is not stop_reports_timeout:
            failures.append(
                "Orange local-control event log recording_drain_finalized "
                f"drain_timed_out={row.get('drain_timed_out')!r}; expected "
                f"{stop_reports_timeout!r} from final Orange stop status"
            )
        if str(row.get("health", "")) != expected_finalize_health:
            failures.append(
                "Orange local-control event log recording_drain_finalized "
                f"health={row.get('health', '')!r}; expected "
                f"{expected_finalize_health!r}"
            )
        if str(row.get("error_code", "")) != expected_finalize_error_code:
            failures.append(
                "Orange local-control event log recording_drain_finalized "
                f"error_code={row.get('error_code', '')!r}; expected "
                f"{expected_finalize_error_code!r}"
            )
    for row in timeout_events:
        if row.get("forced_finalize_requested") is not True:
            failures.append(
                "Orange local-control event log recording_drain_timeout "
                "forced_finalize_requested="
                f"{row.get('forced_finalize_requested')!r}; expected True"
            )
        if str(row.get("health", "")) != "critical":
            failures.append(
                "Orange local-control event log recording_drain_timeout "
                f"health={row.get('health', '')!r}; expected 'critical'"
            )
        if str(row.get("error_code", "")) != "drain_timeout":
            failures.append(
                "Orange local-control event log recording_drain_timeout "
                f"error_code={row.get('error_code', '')!r}; "
                "expected 'drain_timeout'"
            )
    for row in forced_finalize_events:
        if str(row.get("action", "")) != "stream_shutdown":
            failures.append(
                "Orange local-control event log "
                "recording_drain_forced_finalize_requested "
                f"action={row.get('action', '')!r}; expected 'stream_shutdown'"
            )
        if str(row.get("health", "")) != "critical":
            failures.append(
                "Orange local-control event log "
                "recording_drain_forced_finalize_requested "
                f"health={row.get('health', '')!r}; expected 'critical'"
            )
        if str(row.get("error_code", "")) != "drain_timeout":
            failures.append(
                "Orange local-control event log "
                "recording_drain_forced_finalize_requested "
                f"error_code={row.get('error_code', '')!r}; "
                "expected 'drain_timeout'"
            )

    for row in socket_events:
        if row.get("ok") is not True:
            failures.append(
                "Orange local-control event log socket request "
                f"ok={row.get('ok', False)!r}; expected True"
            )
        if row.get("accepted") is not True:
            failures.append(
                "Orange local-control event log socket request "
                f"accepted={row.get('accepted', False)!r}; expected True"
            )
        if expected_method and str(row.get("method", "")) != expected_method:
            failures.append(
                "Orange local-control event log socket request "
                f"method={row.get('method', '')!r}; expected {expected_method!r}"
            )
        if expected_source and str(row.get("source", "")) != expected_source:
            failures.append(
                "Orange local-control event log socket request "
                f"source={row.get('source', '')!r}; expected {expected_source!r}"
            )
        if expected_operation_id and str(row.get("operation_id", "")) != expected_operation_id:
            failures.append(
                "Orange local-control event log socket request "
                f"operation_id={row.get('operation_id', '')!r}; "
                f"expected {expected_operation_id!r}"
            )

    for row in accepted_events:
        row_method = str(row.get("method", ""))
        if row_method == "citrus_completion":
            has_citrus_gate = "citrus_completion_enabled" in row
            citrus_enabled = row.get("citrus_completion_enabled")
            stop_enabled = row.get("stop_enabled")
            if has_citrus_gate:
                if citrus_enabled is not True:
                    failures.append(
                        "Orange local-control event log gui_command_accepted "
                        "citrus_completion_enabled="
                        f"{citrus_enabled!r}; expected True"
                    )
            elif stop_enabled is not True:
                failures.append(
                    "Orange local-control event log gui_command_accepted "
                    f"stop_enabled={stop_enabled!r}; expected True for "
                    "legacy citrus_completion row"
                )
        elif row.get("stop_enabled") is not True:
            failures.append(
                "Orange local-control event log gui_command_accepted "
                f"stop_enabled={row.get('stop_enabled', False)!r}; expected True"
            )
        if expected_method and str(row.get("method", "")) != expected_method:
            failures.append(
                "Orange local-control event log gui_command_accepted "
                f"method={row.get('method', '')!r}; expected {expected_method!r}"
            )
        if expected_source and str(row.get("command_source", "")) != expected_source:
            failures.append(
                "Orange local-control event log gui_command_accepted "
                f"command_source={row.get('command_source', '')!r}; "
                f"expected {expected_source!r}"
            )
        if expected_operation_id and str(row.get("operation_id", "")) != expected_operation_id:
            failures.append(
                "Orange local-control event log gui_command_accepted "
                f"operation_id={row.get('operation_id', '')!r}; "
                f"expected {expected_operation_id!r}"
            )

    for event_name, rows in (
        ("recording_stop_scheduled", scheduled_events),
        ("recording_stop_triggered", trigger_events),
        ("recording_drain_timeout", timeout_events),
        ("recording_drain_forced_finalize_requested", forced_finalize_events),
        ("recording_drain_finalized", finalize_events),
    ):
        for row in rows:
            if expected_method and str(row.get("method", "")) != expected_method:
                failures.append(
                    "Orange local-control event log "
                    f"{event_name} method={row.get('method', '')!r}; "
                    f"expected {expected_method!r}"
                )
            if expected_source and str(row.get("command_source", "")) != expected_source:
                failures.append(
                    "Orange local-control event log "
                    f"{event_name} command_source={row.get('command_source', '')!r}; "
                    f"expected {expected_source!r}"
                )
            if expected_operation_id and str(row.get("operation_id", "")) != expected_operation_id:
                failures.append(
                    "Orange local-control event log "
                    f"{event_name} operation_id={row.get('operation_id', '')!r}; "
                    f"expected {expected_operation_id!r}"
                )
            if expected_terminal_state and str(row.get("terminal_state", "")) != expected_terminal_state:
                failures.append(
                    "Orange local-control event log "
                    f"{event_name} terminal_state={row.get('terminal_state', '')!r}; "
                    f"expected {expected_terminal_state!r}"
                )
            if expected_reason and str(row.get("reason", "")) != expected_reason:
                failures.append(
                    "Orange local-control event log "
                    f"{event_name} reason={row.get('reason', '')!r}; "
                    f"expected {expected_reason!r}"
                )
    if stop_policy == "citrus_completion_notify":
        if expected_method != "citrus_completion":
            failures.append(
                "Orange local-control event log expected final stop method "
                f"'citrus_completion' for notify policy, got {expected_method!r}"
            )
        if expected_source != "citrus":
            failures.append(
                "Orange local-control event log expected final stop source "
                f"'citrus' for notify policy, got {expected_source!r}"
            )
    return failures


def check_start_lifecycle_event_details(
    event_log: dict[str, Any],
    *,
    operation_id: str,
) -> list[str]:
    if not operation_id:
        return []

    start_request_id = f"{operation_id}:orange:start_recording"
    failures: list[str] = []
    socket_events = event_log_socket_events_for_request(event_log, start_request_id)
    queued_socket_events = check_socket_queued_for_gui_thread(
        failures,
        request_id=start_request_id,
        socket_events=socket_events,
        label="start",
    )
    accepted_events = event_log_lifecycle_events_for_request(
        event_log,
        start_request_id,
        event_name="gui_command_accepted",
    )
    queued_events = event_log_lifecycle_events_for_request(
        event_log,
        start_request_id,
        event_name="recording_start_queued",
    )
    trigger_events = event_log_lifecycle_events_for_request(
        event_log,
        start_request_id,
        event_name="recording_start_triggered",
    )
    if not socket_events:
        failures.append(
            "Orange local-control event log missing start socket request/response "
            f"for request_id={start_request_id}"
        )
    if not trigger_events:
        failures.append(
            "Orange local-control event log missing recording_start_triggered "
            f"for request_id={start_request_id}"
        )
    if not accepted_events:
        failures.append(
            "Orange local-control event log missing start GUI-thread gui_command_accepted "
            f"for request_id={start_request_id}"
        )
    if not queued_events:
        failures.append(
            "Orange local-control event log missing recording_start_queued "
            f"for request_id={start_request_id}"
        )
    check_event_log_row_order(
        failures,
        request_id=start_request_id,
        earlier_rows=queued_socket_events,
        earlier_label="queued start socket request/response",
        later_rows=accepted_events,
        later_label="gui_command_accepted",
    )
    check_event_log_row_order(
        failures,
        request_id=start_request_id,
        earlier_rows=accepted_events,
        earlier_label="gui_command_accepted",
        later_rows=queued_events,
        later_label="recording_start_queued",
    )
    check_event_log_row_order(
        failures,
        request_id=start_request_id,
        earlier_rows=queued_events,
        earlier_label="recording_start_queued",
        later_rows=trigger_events,
        later_label="recording_start_triggered",
    )

    expected_source = ""
    for row in socket_events:
        if row.get("ok") is not True:
            failures.append(
                "Orange local-control event log start socket request "
                f"ok={row.get('ok', False)!r}; expected True"
            )
        if row.get("accepted") is not True:
            failures.append(
                "Orange local-control event log start socket request "
                f"accepted={row.get('accepted', False)!r}; expected True"
            )
        if str(row.get("method", "")) != "start_recording":
            failures.append(
                "Orange local-control event log start socket request "
                f"method={row.get('method', '')!r}; expected 'start_recording'"
            )
        if str(row.get("operation_id", "")) != operation_id:
            failures.append(
                "Orange local-control event log start socket request "
                f"operation_id={row.get('operation_id', '')!r}; "
                f"expected {operation_id!r}"
            )
        row_source = str(row.get("source", ""))
        if not row_source:
            failures.append(
                "Orange local-control event log start socket request "
                "source is empty"
            )
        elif not expected_source:
            expected_source = row_source

    for row in accepted_events:
        if row.get("start_enabled") is not True:
            failures.append(
                "Orange local-control event log start gui_command_accepted "
                f"start_enabled={row.get('start_enabled', False)!r}; expected True"
            )
        if str(row.get("method", "")) != "start_recording":
            failures.append(
                "Orange local-control event log start gui_command_accepted "
                f"method={row.get('method', '')!r}; expected 'start_recording'"
            )
        if str(row.get("operation_id", "")) != operation_id:
            failures.append(
                "Orange local-control event log start gui_command_accepted "
                f"operation_id={row.get('operation_id', '')!r}; "
                f"expected {operation_id!r}"
            )
        if expected_source and str(row.get("command_source", "")) != expected_source:
            failures.append(
                "Orange local-control event log start gui_command_accepted "
                f"command_source={row.get('command_source', '')!r}; "
                f"expected {expected_source!r}"
            )

    for row in queued_events:
        if str(row.get("method", "")) != "start_recording":
            failures.append(
                "Orange local-control event log recording_start_queued "
                f"method={row.get('method', '')!r}; expected 'start_recording'"
            )
        if str(row.get("operation_id", "")) != operation_id:
            failures.append(
                "Orange local-control event log recording_start_queued "
                f"operation_id={row.get('operation_id', '')!r}; "
                f"expected {operation_id!r}"
            )
        if expected_source and str(row.get("command_source", "")) != expected_source:
            failures.append(
                "Orange local-control event log recording_start_queued "
                f"command_source={row.get('command_source', '')!r}; "
                f"expected {expected_source!r}"
            )

    for row in trigger_events:
        if str(row.get("method", "")) != "start_recording":
            failures.append(
                "Orange local-control event log recording_start_triggered "
                f"method={row.get('method', '')!r}; expected 'start_recording'"
            )
        if str(row.get("operation_id", "")) != operation_id:
            failures.append(
                "Orange local-control event log recording_start_triggered "
                f"operation_id={row.get('operation_id', '')!r}; "
                f"expected {operation_id!r}"
            )
        if expected_source and str(row.get("command_source", "")) != expected_source:
            failures.append(
                "Orange local-control event log recording_start_triggered "
                f"command_source={row.get('command_source', '')!r}; "
                f"expected {expected_source!r}"
            )
    return failures


def check_orange_local_control_event_log(
    event_log: dict[str, Any],
    *,
    required: bool,
    operation_id: str,
    stop_policy: str,
    orange_status: dict[str, Any],
) -> dict[str, Any]:
    expected_request_ids = [f"{operation_id}:orange:start_recording"] if operation_id else []
    stop_status = orange_local_control_recording_stop(orange_status)
    stop_request_id = str(stop_status.get("request_id", ""))
    if stop_policy != "none" and stop_request_id:
        expected_request_ids.append(stop_request_id)

    check: dict[str, Any] = {
        "required": bool(required),
        "ok": True,
        "failures": [],
        "expected_request_ids": expected_request_ids,
        "expected_operation_id": operation_id,
        "request_chains": [
            summarize_event_log_request_chain(event_log, request_id)
            for request_id in expected_request_ids
            if request_id
        ],
    }
    if not required:
        return check

    failures: list[str] = []
    if not event_log.get("exists", False):
        failures.append(
            f"Orange local-control event log missing: {event_log.get('path', '')}"
        )
    if int(event_log.get("row_count") or 0) <= 0:
        failures.append("Orange local-control event log has no rows")
    invalid_rows = int(event_log.get("invalid_row_count") or 0)
    if invalid_rows != 0:
        failures.append(f"Orange local-control event log has {invalid_rows} invalid JSON row(s)")
    if int(event_log.get("socket_event_count") or 0) <= 0:
        failures.append("Orange local-control event log has no socket request/response rows")
    if int(event_log.get("gui_event_count") or 0) <= 0:
        failures.append("Orange local-control event log has no GUI-thread lifecycle rows")
    if not event_log.get("has_start_triggered", False):
        failures.append("Orange local-control event log missing recording_start_triggered")
    failures.extend(
        check_start_lifecycle_event_details(
            event_log,
            operation_id=operation_id,
        )
    )
    if stop_policy != "none":
        if not stop_request_id:
            failures.append("Orange status missing local_control.recording_stop.request_id")
        if not event_log.get("has_stop_triggered", False):
            failures.append("Orange local-control event log missing recording_stop_triggered")
        if not event_log.get("has_drain_finalized", False):
            failures.append("Orange local-control event log missing recording_drain_finalized")
        failures.extend(
            check_stop_lifecycle_event_details(
                event_log,
                stop_status=stop_status,
                stop_policy=stop_policy,
            )
        )
    if orange_recording_stop_drain_timed_out(orange_status):
        if not event_log.get("has_drain_timeout", False):
            failures.append("Orange status reports drain timeout but event log lacks recording_drain_timeout")
        if stop_request_id and not event_log_lifecycle_events_for_request(
            event_log,
            stop_request_id,
            event_name="recording_drain_timeout",
        ):
            failures.append(
                "Orange status reports drain timeout but event log lacks "
                f"recording_drain_timeout for request_id={stop_request_id}"
            )
        if not event_log.get("has_forced_finalize_requested", False):
            failures.append(
                "Orange status reports drain timeout but event log lacks "
                "recording_drain_forced_finalize_requested"
            )
        if stop_request_id and not event_log_lifecycle_events_for_request(
            event_log,
            stop_request_id,
            event_name="recording_drain_forced_finalize_requested",
        ):
            failures.append(
                "Orange status reports drain timeout but event log lacks "
                "recording_drain_forced_finalize_requested for "
                f"request_id={stop_request_id}"
            )

    operation_ids = set(str(item) for item in event_log.get("operation_ids", []))
    if operation_id and operation_id not in operation_ids:
        failures.append(
            f"Orange local-control event log missing operation_id={operation_id}"
        )
    request_ids = set(str(item) for item in event_log.get("request_ids", []))
    for request_id in expected_request_ids:
        if request_id and request_id not in request_ids:
            failures.append(
                f"Orange local-control event log missing request_id={request_id}"
            )

    check["failures"] = failures
    check["ok"] = not failures
    return check


def citrus_ready_to_start(status: dict[str, Any]) -> bool:
    return json_bool(json_path(status, ["readiness", "ready_to_start"], False))


def citrus_start_blockers(status: dict[str, Any], expected_arena_count: int | None) -> list[str]:
    """What keeps a bound Orange+Citrus start from going ahead, from one Citrus
    status reply (citrus-agent, 2026-11-15, citrus main be2cd47). Empty = go."""
    blockers: list[str] = []

    def need(path: list[str], expected: Any, label: str) -> None:
        value = json_path(status, path, None)
        if value != expected:
            blockers.append(f"{label}={value!r} (need {expected!r})")

    need(["readiness", "ready_to_start"], True, "readiness.ready_to_start")
    need(["readiness", "start_blocked_reason"], None, "readiness.start_blocked_reason")
    if expected_arena_count is not None:
        need(["readiness", "selected_arena_count"], expected_arena_count, "readiness.selected_arena_count")
        need(["readiness", "selected_protocol_loaded_count"], expected_arena_count,
             "readiness.selected_protocol_loaded_count")
    need(["runtime_rig_canvas_commissioning_compatibility", "compatible"], True, "commissioning.compatible")
    need(["runtime_rig_canvas_commissioning_compatibility", "all_selected_runtime_safe"], True,
         "commissioning.all_selected_runtime_safe")
    need(["runtime_homography_compatibility", "all_selected_compatible"], True, "homography.all_selected_compatible")
    scale = ["runtime_projected_surface_scale_compatibility"]
    need(scale + ["all_selected_compatible"], True, "scale.all_selected_compatible")
    need(scale + ["all_selected_runtime_safe"], True, "scale.all_selected_runtime_safe")
    need(scale + ["selected_invalid_pointer_count"], 0, "scale.selected_invalid_pointer_count")
    need(scale + ["selected_missing_pointer_count"], 0, "scale.selected_missing_pointer_count")
    need(["daily_registration", "runtime", "all_selected_runtime_safe"], True,
         "daily_registration.runtime.all_selected_runtime_safe")
    need(["experiment", "terminal_state"], "not_started", "experiment.terminal_state")
    return blockers


CITRUS_FINALIZATION_DELIVERED = "delivered"

# Waits that span a protocol or a finalization (minutes): polled at
# --long-wait-poll-interval-seconds. Every status reply is logged in full by
# Citrus (~63 KB), so 4 polls/s added ~90 MB to its log in one evening
# (mhs-helper, 2026-11-15). Readiness waits (about a second) keep the short
# interval.
LONG_WAIT_DESCRIPTIONS = {
    "terminal_state",
    "receipts_delivered",
    "recording_finalized",
    "recording_finalized_after_citrus_completion_notify",
    "headless_recording_bound",
    "perf_jsonl_path_known",
}


def poll_interval_for(args: argparse.Namespace, description: str) -> float:
    if description in LONG_WAIT_DESCRIPTIONS:
        return args.long_wait_poll_interval_seconds
    return args.poll_interval_seconds


def resolve_citrus_local_control_log_path(args: argparse.Namespace) -> str:
    """Citrus's lifecycle event log: CITRUS_GUI_LOCAL_CONTROL_LOG, else
    <citrus socket>.events.jsonl (Citrus fde5467; renamed to .1 at start)."""
    if getattr(args, "citrus_local_control_log", ""):
        return args.citrus_local_control_log
    value = parse_env_items(getattr(args, "citrus_env", []) or []).get("CITRUS_GUI_LOCAL_CONTROL_LOG") or \
        os.environ.get("CITRUS_GUI_LOCAL_CONTROL_LOG", "")
    return value or f"{args.citrus_socket}.events.jsonl"


# Rows that never wake a wait: Citrus logs every socket request, our own
# status polls included, as local_control_request (Orange does not log status).
EVENT_LOG_IGNORED_EVENTS = {"local_control_request"}


class EventLogFollower:
    """Follows one append-only JSONL event log by file identity and size.

    Orange truncates its log when its control server starts and Citrus renames
    its log to .1 at start, so a new inode or a shrink is a new log, read from
    its first byte; never an offset to resume from
    (docs/orange_local_control_events_v1.md). Rows already in the file when the
    follower is made belong to earlier runs and are skipped. Only complete
    lines are parsed; a line that is not JSON is counted, not raised."""

    def __init__(self, label: str, path: str):
        self.label = label
        self.path = path
        self.identity: tuple[int, int] | None = None
        self.offset = 0
        self.partial = b""
        self.rows_seen = 0
        self.waking_rows = 0
        self.unparsed_rows = 0
        self.resets = 0
        self.event_counts: dict[str, int] = {}
        self.receipt_delivery_states: list[str] = []
        try:
            st = os.stat(path)
            self.identity = (st.st_dev, st.st_ino)
            self.offset = st.st_size
        except OSError:
            pass

    @property
    def available(self) -> bool:
        return os.path.exists(self.path)

    def poll(self) -> list[dict[str, Any]]:
        """New complete rows since the last poll that should wake a wait."""
        try:
            st = os.stat(self.path)
        except OSError:
            return []
        identity = (st.st_dev, st.st_ino)
        if identity != self.identity or st.st_size < self.offset:
            if self.identity is not None:
                self.resets += 1
            self.identity, self.offset, self.partial = identity, 0, b""
        if st.st_size == self.offset:
            return []
        try:
            with open(self.path, "rb") as handle:
                handle.seek(self.offset)
                data = handle.read(st.st_size - self.offset)
        except OSError:
            return []
        self.offset += len(data)
        lines = (self.partial + data).split(b"\n")
        self.partial = lines.pop()
        waking: list[dict[str, Any]] = []
        for line in lines:
            if not line.strip():
                continue
            self.rows_seen += 1
            try:
                row = json.loads(line)
            except ValueError:
                self.unparsed_rows += 1
                continue
            if not isinstance(row, dict):
                self.unparsed_rows += 1
                continue
            event = row.get("event") if isinstance(row.get("event"), str) else "socket_request"
            self.event_counts[event] = self.event_counts.get(event, 0) + 1
            if event == "receipt_delivery" and isinstance(row.get("state"), str):
                self.receipt_delivery_states.append(row["state"])
            if event in EVENT_LOG_IGNORED_EVENTS:
                continue
            self.waking_rows += 1
            waking.append(row)
        return waking

    def record(self) -> dict[str, Any]:
        return {
            "path": self.path,
            "available": self.available,
            "rows_seen": self.rows_seen,
            "waking_rows": self.waking_rows,
            "unparsed_rows": self.unparsed_rows,
            "log_resets": self.resets,
            "event_counts": dict(sorted(self.event_counts.items())),
            **({"receipt_delivery_states": self.receipt_delivery_states}
               if self.receipt_delivery_states else {}),
        }


class LifecycleWaker:
    """Sleeps for a long wait, waking as soon as Orange or Citrus writes a
    lifecycle row. Status stays the authority: a wake only triggers the next
    status read. With no log on disk the wait is the plain poll interval
    (fallback); with one, the status heartbeat stretches to
    --lifecycle-event-heartbeat-seconds. The files are checked every
    --lifecycle-event-check-seconds (a stat, no socket traffic)."""

    def __init__(self, followers: list[EventLogFollower], check_seconds: float, heartbeat_seconds: float,
                 clock: Callable[[], float] = time.monotonic, sleep: Callable[[float], None] = time.sleep):
        self.followers = followers
        self.check_seconds = check_seconds
        self.heartbeat_seconds = heartbeat_seconds
        self.clock = clock
        self.sleep = sleep
        self.wakes_by_event = 0
        self.wakes_by_heartbeat = 0
        self.fallback_waits = 0

    @property
    def available(self) -> bool:
        return any(f.available for f in self.followers)

    def interval(self, poll_interval: float) -> float:
        return max(poll_interval, self.heartbeat_seconds) if self.available else poll_interval

    def drain(self) -> int:
        return sum(len(f.poll()) for f in self.followers)

    def wait(self, seconds: float) -> str:
        """'event' when a lifecycle row arrived, 'heartbeat' at the interval,
        'fallback' when no log exists (plain sleep)."""
        if seconds <= 0:
            return "heartbeat"
        if not self.available:
            self.fallback_waits += 1
            self.sleep(seconds)
            return "fallback"
        deadline = self.clock() + seconds
        while True:
            if self.drain():
                self.wakes_by_event += 1
                return "event"
            remaining = deadline - self.clock()
            if remaining <= 0:
                self.wakes_by_heartbeat += 1
                return "heartbeat"
            self.sleep(min(self.check_seconds, remaining))

    def record(self) -> dict[str, Any]:
        return {
            "heartbeat_seconds": self.heartbeat_seconds,
            "check_seconds": self.check_seconds,
            "wakes_by_event": self.wakes_by_event,
            "wakes_by_heartbeat": self.wakes_by_heartbeat,
            "fallback_waits": self.fallback_waits,
            "logs": {f.label: f.record() for f in self.followers},
        }


def citrus_finalization_state(status: dict[str, Any]) -> str:
    value = json_path(status, ["recording_observation_finalization", "state"], "")
    return value if isinstance(value, str) else ""


# Citrus receipt_delivery states (citrus-agent, 2026-11-15): idle, delivering,
# verifying, delivered, unconfirmed, refused.
CITRUS_FINALIZATION_FAILED = {"unconfirmed", "refused"}


def citrus_finalization_settled(status: dict[str, Any]) -> bool:
    """Delivered, or a state Citrus will not leave on its own."""
    state = citrus_finalization_state(status)
    return state == CITRUS_FINALIZATION_DELIVERED or state in CITRUS_FINALIZATION_FAILED


def citrus_finalization_never_started(status: dict[str, Any]) -> bool:
    """Idle with no attempts: Citrus built no receipts (an H5 sealed FAILED,
    "Receipt build failed closed"), so none will come."""
    return (
        citrus_finalization_state(status) == "idle"
        and json_path(status, ["recording_observation_finalization", "attempts"], None) == 0
    )


def citrus_receipt_build_error(status: dict[str, Any]) -> str:
    """Citrus's reason for building no receipts (status
    recording_observation_finalization.receipt_build_error, null while none;
    citrus PR #119). Non-empty means no receipts will come."""
    value = json_path(status, ["recording_observation_finalization", "receipt_build_error"], None)
    return value if isinstance(value, str) else ""


def citrus_receipt_wait_limit_seconds(recording_folder: str) -> float:
    """Citrus's own bound, as Orange's GUI exit uses it
    (session::CitrusReceiptWaitLimit): 330 s for the manifest, then 120 s +
    3 s per 100 MB of the folder, plus 60 s."""
    total = 0
    for path in Path(recording_folder).rglob("*"):
        try:
            if path.is_file() and not path.is_symlink():
                total += path.stat().st_size
        except OSError:
            continue
    per_100mb = -(-total // (100 * 1000 * 1000))
    return float(330 + 120 + 3 * per_100mb + 60)


def citrus_h5_session_statuses(recording_folder: str) -> dict[str, str]:
    """/metadata/session session_status of every Citrus H5 in the folder."""
    statuses: dict[str, str] = {}
    try:
        import h5py  # noqa: PLC0415  (only bound runs need it)
    except ImportError:
        return {"error": "h5py not available"}
    for path in sorted(Path(recording_folder).rglob("*.h5")):
        key = str(path.relative_to(recording_folder))
        try:
            with h5py.File(path, "r") as h5:
                node = h5.get("/metadata/session")
                value = None
                if node is not None:
                    value = node.attrs.get("session_status")
                    if value is None and isinstance(node, h5py.Group) and "session_status" in node:
                        value = node["session_status"][()]
                if isinstance(value, bytes):
                    value = value.decode("utf-8", "replace")
                statuses[key] = "" if value is None else str(value)
        except OSError as exc:
            statuses[key] = f"unreadable: {exc}"
    return statuses


def is_citrus_binary_cmdline(argv: list[str]) -> bool:
    """The process's program is the Citrus binary (<citrus>/targets/citrus),
    not a shell or tool whose arguments merely mention it."""
    return bool(argv) and (argv[0] == "./targets/citrus" or argv[0].endswith("/targets/citrus"))


def other_citrus_processes(own_pids: set[int]) -> list[str]:
    """Running Citrus binaries this orchestrator did not start. A second Citrus
    unlinks and takes over the live one's socket path at bind."""
    found = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit() or int(entry.name) in own_pids:
            continue
        try:
            argv = (entry / "cmdline").read_bytes().split(b"\0")
        except OSError:
            continue
        argv = [part.decode("utf-8", "replace") for part in argv if part]
        if is_citrus_binary_cmdline(argv):
            found.append(f"{entry.name} {' '.join(argv)}")
    return found


def citrus_ready_for_runtime_selection(status: dict[str, Any]) -> bool:
    return (
        json_bool(json_path(status, ["experiment", "loaded"], False))
        and not citrus_active_or_armed(status)
        and isinstance(json_path(status, ["readiness", "selected_arena_count"], None), int)
        and json_path(status, ["readiness", "selected_arena_count"], 0) > 0
    )


def citrus_active_or_armed(status: dict[str, Any]) -> bool:
    return json_bool(json_path(status, ["experiment", "active"], False)) or json_bool(
        json_path(status, ["experiment", "armed"], False)
    )


def citrus_terminal_state(status: dict[str, Any]) -> str:
    value = json_path(status, ["experiment", "terminal_state"], "")
    return value if isinstance(value, str) else ""


def citrus_terminal_reason(status: dict[str, Any]) -> str:
    value = json_path(status, ["experiment", "terminal_reason"], "")
    return value if isinstance(value, str) else ""


def citrus_is_terminal(status: dict[str, Any]) -> bool:
    return citrus_terminal_state(status) in TERMINAL_CITRUS_STATES


def citrus_perf_jsonl_enabled(status: dict[str, Any]) -> bool:
    return json_bool(json_path(status, ["output", "perf_jsonl_enabled"], False))


def citrus_perf_jsonl_path_known(status: dict[str, Any]) -> bool:
    return json_bool(json_path(status, ["output", "perf_jsonl_path_known"], False))


# Headless mode (--orange-mode headless): Citrus is launched first with autorun
# CITRUS_GUI_AUTORUN_START=wait_for_control (Citrus PR #109), which loads the
# protocol and selects the arenas but starts nothing; orange_client then binds,
# arms and records; this orchestrator sends the one start_experiment.
CITRUS_PRE_ARM_DECISION_RELATIVE_PATH = "recording_observation_bindings/pre_arm_decision.json"


def citrus_automation_error(status: dict[str, Any]) -> str:
    value = json_path(status, ["automation", "error"], "")
    return value if isinstance(value, str) else ""


def citrus_autorun_status(status: dict[str, Any]) -> dict[str, Any]:
    value = json_path(status, ["automation", "autorun"], {})
    return value if isinstance(value, dict) else {}


def citrus_waiting_for_control_start(status: dict[str, Any]) -> bool:
    autorun = citrus_autorun_status(status)
    return (
        citrus_ready_to_start(status)
        and not citrus_automation_error(status)
        and autorun.get("enabled") is True
        and autorun.get("start_mode") == "wait_for_control"
        and autorun.get("waiting_for_start") is True
        and autorun.get("start_source") is None
    )


def citrus_headless_wait_failure(status: dict[str, Any], start_gate: str) -> str:
    error = citrus_automation_error(status)
    if error:
        return f"Citrus automation error: {error}"
    if start_gate == "wait_for_control" and isinstance(status.get("automation"), dict) and not isinstance(
        json_path(status, ["automation", "autorun"], None), dict
    ):
        return "Citrus status has no automation.autorun: this Citrus build predates wait_for_control (PR #109)"
    autorun = citrus_autorun_status(status)
    if autorun.get("start_source") is not None:
        return (
            "Citrus already accepted a start "
            f"(start_source={autorun.get('start_source')!r}) before this orchestrator sent one"
        )
    if citrus_active_or_armed(status):
        return "Citrus experiment is already active or armed before this orchestrator started it"
    return ""


def orange_headless_ready_for_citrus(status: dict[str, Any]) -> bool:
    # orange_client arms only after Citrus accepted the binding (a refused
    # pre-arm ends the run), so recording_active here means bound and recording.
    return (
        status.get("process") == "orange_client"
        and orange_ready_for_citrus(status)
        and bool(orange_recording_folder(status))
    )


def orange_headless_citrus_completion_enabled(status: dict[str, Any]) -> bool:
    # orange_client reports fixed.local_control.stop_on_citrus_completion as
    # local_control.citrus_completion_stop.enabled (src/orange_local_control.cpp).
    return json_bool(json_path(status, ["local_control", "citrus_completion_stop", "enabled"], False))


def read_pre_arm_lifecycle_status(recording_folder: str) -> str:
    path = Path(recording_folder) / CITRUS_PRE_ARM_DECISION_RELATIVE_PATH
    try:
        decision = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return ""
    value = decision.get("lifecycle_status", "") if isinstance(decision, dict) else ""
    return value if isinstance(value, str) else ""


def read_headless_run_entry(recording_folder: str) -> dict[str, Any]:
    """runs.json entry of an orange_client run: <experiment folder>/runs.json."""
    folder = Path(recording_folder)
    runs_path = folder.parent / "runs.json"
    try:
        payload = json.loads(runs_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    runs = payload.get("runs", []) if isinstance(payload, dict) else []
    for run in runs if isinstance(runs, list) else []:
        if not isinstance(run, dict):
            continue
        observed = str(run.get("recording_folder", "")).rstrip("/")
        if observed == str(folder).rstrip("/"):
            return run
    return {}


@dataclass
class StepLog:
    name: str
    started_at_utc: str = field(default_factory=utc_now)
    finished_at_utc: str = ""
    ok: bool = False
    detail: dict[str, Any] = field(default_factory=dict)

    def finish(self, *, ok: bool = True, **detail: Any) -> None:
        self.finished_at_utc = utc_now()
        self.ok = ok
        self.detail.update(detail)


class Orchestrator:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.steps: list[StepLog] = []
        self.started_processes: list[dict[str, Any]] = []
        self.processes: dict[str, subprocess.Popen[Any]] = {}
        self.validation_results: list[dict[str, Any]] = []
        self.orange_recording_started = False
        self.citrus_control_complete = False
        self.citrus_daily_registration_mode_response: dict[str, Any] | None = None
        self.last_orange_status: dict[str, Any] = {}
        self.last_citrus_status: dict[str, Any] = {}
        self.last_orange_coverage_check_monotonic = 0.0
        self.citrus_start_sent = False
        self.headless_result: dict[str, Any] = {}
        # Empty background (capture -> operator confirmation -> start params).
        self.empty_background_record: dict[str, Any] | None = None
        self.empty_background_start: dict[str, Any] | None = None
        self.scene_refused_requests: list[dict[str, Any]] = []
        # Long waits follow Orange's and Citrus's lifecycle event logs (made
        # before either process starts, so earlier runs' rows are skipped).
        self.lifecycle_waker: LifecycleWaker | None = None
        if getattr(args, "lifecycle_events", "auto") == "auto":
            self.lifecycle_waker = LifecycleWaker(
                [EventLogFollower("orange", resolve_orange_local_control_log_path(args)),
                 EventLogFollower("citrus", resolve_citrus_local_control_log_path(args))],
                check_seconds=args.lifecycle_event_check_seconds,
                heartbeat_seconds=args.lifecycle_event_heartbeat_seconds,
            )

    def long_wait(self, poll_interval: float, remaining: float) -> tuple[float, bool]:
        """(seconds to wait, final) for one step of a long wait."""
        interval = self.lifecycle_waker.interval(poll_interval) if self.lifecycle_waker else poll_interval
        if interval >= remaining:
            return remaining, True
        return interval, False

    def pause(self, seconds: float) -> None:
        if self.lifecycle_waker is not None:
            self.lifecycle_waker.wait(seconds)
        else:
            time.sleep(seconds)

    def step(self, name: str) -> StepLog:
        item = StepLog(name=name)
        self.steps.append(item)
        return item

    def send(self, socket_path: str, request: dict[str, Any]) -> dict[str, Any]:
        return send_unix_json(socket_path, request, self.args.socket_timeout_seconds)

    def status(self, label: str, schema_id: str, socket_path: str) -> tuple[dict[str, Any], dict[str, Any]]:
        request_id = f"{self.args.operation_id}:{label}:status:{uuid.uuid4()}"
        response = self.send(
            socket_path,
            build_status_request(schema_id, request_id, self.args.source),
        )
        if not response.get("ok", False):
            raise OrchestratorError(f"{label} status request failed: {response}")
        status = response.get("status", {})
        if not isinstance(status, dict):
            raise OrchestratorError(f"{label} status response did not include object status")
        if label == "orange":
            self.last_orange_status = status
        elif label == "citrus":
            self.last_citrus_status = status
        return response, status

    def preflight_launch_socket(
        self,
        label: str,
        schema_id: str,
        socket_path: str,
        allow_preexisting: bool,
    ) -> None:
        if allow_preexisting or not socket_path:
            return
        step = self.step(f"preflight_{label}_launch_socket")
        request_id = f"{self.args.operation_id}:{label}:prelaunch_status:{uuid.uuid4()}"
        try:
            response = send_unix_json(
                socket_path,
                build_status_request(schema_id, request_id, self.args.source),
                self.args.launch_socket_preflight_timeout_seconds,
            )
        except OSError as exc:
            if socket_absent_or_stale_error(exc):
                step.finish(
                    ok=True,
                    socket_path=socket_path,
                    existing_server=False,
                    error=str(exc),
                )
                return
            step.finish(ok=False, socket_path=socket_path, existing_server=True, error=str(exc))
            raise OrchestratorError(
                f"{label} local-control socket {socket_path} exists before launch "
                f"but could not be preflighted safely: {exc}"
            ) from exc
        except (json.JSONDecodeError, TimeoutError) as exc:
            step.finish(ok=False, socket_path=socket_path, existing_server=True, error=str(exc))
            raise OrchestratorError(
                f"{label} local-control socket {socket_path} answered before launch "
                f"but did not return a clean status response; refusing to launch over it"
            ) from exc

        step.finish(ok=False, socket_path=socket_path, existing_server=True, response=response)
        raise OrchestratorError(
            f"{label} local-control socket {socket_path} is already answering before launch; "
            f"use attach mode or --allow-preexisting-{label}-socket if this is intentional"
        )

    def wait_for_status(
        self,
        label: str,
        schema_id: str,
        socket_path: str,
        predicate: Callable[[dict[str, Any]], bool],
        timeout_seconds: float,
        description: str,
    ) -> dict[str, Any]:
        step = self.step(f"wait_{label}_{description}")
        deadline = time.monotonic() + timeout_seconds
        last_error = ""
        last_status: dict[str, Any] = {}
        final_check = False
        while True:
            if label == "orange":
                inferred_status = self.infer_orange_finalized_after_clean_exit(
                    description=description,
                    predicate=predicate,
                    last_status=last_status,
                )
                if inferred_status is not None:
                    step.finish(
                        ok=True,
                        status=inferred_status,
                        accepted_clean_process_exit=True,
                    )
                    return inferred_status
            self.raise_if_started_process_exited(f"waiting for {label} {description}")
            status_received = False
            try:
                _, status = self.status(label, schema_id, socket_path)
                last_status = status
                if predicate(status):
                    step.finish(ok=True, status=status)
                    return status
                status_received = True
            except (OSError, TimeoutError, json.JSONDecodeError, OrchestratorError) as exc:
                last_error = str(exc)
            if (
                status_received
                and label == "citrus"
                and description == "terminal_state"
                and self.orange_recording_started
            ):
                self.check_orange_recording_coverage(last_status, step)
            if (
                label == "citrus"
                and last_status
                and self.args.orange_mode == "headless"
                and not self.citrus_start_sent
            ):
                failure = citrus_headless_wait_failure(last_status, self.args.citrus_start_gate)
                if failure:
                    step.finish(ok=False, terminal_failure=failure, last_status=last_status)
                    raise OrchestratorError(failure)
            if label == "orange" and last_status:
                failure = orange_wait_failure(
                    last_status,
                    description=description,
                    operation_id=self.args.operation_id,
                )
                if failure:
                    if orange_recording_start_failed_for_operation(
                        last_status,
                        self.args.operation_id,
                    ):
                        # The local-control socket ACK only queued a GUI command;
                        # the GUI has now proven that no recording became active.
                        # Clear the provisional flag so failure cleanup terminates
                        # an Orange process owned by this orchestration run.
                        self.orange_recording_started = False
                    step.finish(
                        ok=False,
                        terminal_failure=failure,
                        last_status=last_status,
                    )
                    raise OrchestratorError(failure)
            if final_check:
                break
            # Never sleep past the deadline, and check once more at it.
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            interval = poll_interval_for(self.args, description)
            if description in LONG_WAIT_DESCRIPTIONS:
                interval, final_check = self.long_wait(interval, remaining)
                self.pause(interval)
                continue
            if interval >= remaining:
                interval = remaining
                final_check = True
            time.sleep(interval)
        step.finish(ok=False, last_error=last_error, last_status=last_status)
        raise OrchestratorError(f"timed out waiting for {label} {description}: {last_error}")

    def check_orange_recording_coverage(
        self,
        citrus_status: dict[str, Any],
        step: StepLog,
    ) -> None:
        now = time.monotonic()
        if now - self.last_orange_coverage_check_monotonic < 1.0:
            return
        _, orange_status = self.status(
            "orange",
            ORANGE_REQUEST_SCHEMA_ID,
            self.args.orange_socket,
        )
        self.last_orange_coverage_check_monotonic = now
        if orange_recording_covers_active_citrus(orange_status):
            return
        step.finish(
            ok=False,
            outcome="orange_recording_ended_before_citrus",
            status=citrus_status,
            orange_status=orange_status,
        )
        raise OrchestratorError(
            "Orange recording ended before Citrus reached a terminal state; "
            "refusing an acquisition interval with unbound Shaman identities"
        )

    def wait_for_citrus_terminal_or_run_duration(self) -> dict[str, Any]:
        if self.args.citrus_run_seconds <= 0.0:
            return self.wait_for_status(
                "citrus",
                CITRUS_REQUEST_SCHEMA_ID,
                self.args.citrus_socket,
                citrus_is_terminal,
                self.args.citrus_terminal_timeout_seconds,
                "terminal_state",
            )

        step = self.step("wait_citrus_terminal_or_run_duration")
        deadline = time.monotonic() + self.args.citrus_terminal_timeout_seconds
        active_since: float | None = None
        last_error = ""
        last_status: dict[str, Any] = {}
        while time.monotonic() <= deadline:
            self.raise_if_started_process_exited("waiting for Citrus terminal state or run duration")
            status_received = False
            try:
                _, status = self.status("citrus", CITRUS_REQUEST_SCHEMA_ID, self.args.citrus_socket)
                last_status = status
                if citrus_is_terminal(status):
                    step.finish(ok=True, outcome="terminal", status=status)
                    return status
                status_received = True
            except (OSError, TimeoutError, json.JSONDecodeError, OrchestratorError) as exc:
                last_error = str(exc)
            if status_received:
                now = time.monotonic()
                self.check_orange_recording_coverage(last_status, step)
                if citrus_active_or_armed(last_status):
                    if active_since is None:
                        active_since = now
                    active_elapsed_s = now - active_since
                    if active_elapsed_s >= self.args.citrus_run_seconds:
                        step.finish(
                            ok=True,
                            outcome="run_duration_elapsed",
                            run_seconds=self.args.citrus_run_seconds,
                            active_elapsed_s=active_elapsed_s,
                            status=last_status,
                        )
                        return last_status
            # With a run duration set, wake at least at the poll interval so
            # the duration is measured as before.
            remaining = deadline - time.monotonic()
            if remaining > 0:
                self.pause(min(self.args.long_wait_poll_interval_seconds, remaining))
        step.finish(ok=False, last_error=last_error, last_status=last_status)
        raise OrchestratorError(
            "timed out waiting for Citrus terminal state or "
            f"{self.args.citrus_run_seconds}s active run duration: {last_error}"
        )

    def request_citrus_stop_for_run_duration(self) -> dict[str, Any]:
        step = self.step("citrus_stop_experiment_run_duration")
        response = self.send(
            self.args.citrus_socket,
            build_citrus_stop_request(
                self.args.operation_id,
                f"{self.args.operation_id}:citrus:stop_experiment:run_duration",
                self.args.source,
            ),
        )
        if not response_accepted(response):
            step.finish(ok=False, response=response)
            raise OrchestratorError(f"Citrus stop_experiment was not accepted: {response}")
        step.finish(ok=True, response=response)
        return response

    def refresh_started_processes(self) -> None:
        for info in self.started_processes:
            label = str(info.get("label", ""))
            process = self.processes.get(label)
            if process is not None:
                info["returncode"] = process.poll()

    def raise_if_started_process_exited(self, context: str) -> None:
        self.refresh_started_processes()
        for info in self.started_processes:
            returncode = info.get("returncode")
            if returncode is None:
                continue
            label = info.get("label", "<unknown>")
            if label == "citrus" and self.citrus_control_complete:
                continue
            if label == "orange" and returncode == 0 and getattr(self, "orange_exit_expected", False):
                # A bound run's Orange GUI exits by itself once it has stored
                # Citrus's receipts, often before Citrus's status says
                # "delivered" (2026-11-15, 2026_11_14_23_28_06).
                continue
            pid = info.get("pid", "<unknown>")
            log_path = info.get("log_path", "")
            raise OrchestratorError(
                f"{label} process exited while {context}: "
                f"pid={pid} returncode={returncode} log_path={log_path}"
            )

    def infer_orange_finalized_after_clean_exit(
        self,
        *,
        description: str,
        predicate: Callable[[dict[str, Any]], bool],
        last_status: dict[str, Any],
    ) -> dict[str, Any] | None:
        if not description.startswith("recording_finalized"):
            return None
        self.refresh_started_processes()
        process = self.processes.get("orange")
        if process is None or process.poll() != 0:
            return None
        status = infer_orange_finalized_status_from_session(
            last_status or self.last_orange_status,
            operation_id=self.args.operation_id,
        )
        if status is None or not predicate(status):
            return None
        self.last_orange_status = status
        return status

    def cleanup_started_processes(
        self,
        *,
        labels: set[str] | None = None,
        terminate_timeout_seconds: float = 5.0,
    ) -> None:
        self.refresh_started_processes()
        for info in self.started_processes:
            label = str(info.get("label", ""))
            if labels is not None and label not in labels:
                continue
            process = self.processes.get(label)
            if process is None or process.poll() is not None:
                if process is not None:
                    info["returncode"] = process.returncode
                continue

            step = self.step(f"cleanup_{label}_process")
            pid = process.pid
            action = "none"
            try:
                try:
                    os.killpg(pid, signal.SIGTERM)
                    action = "terminate_process_group"
                except ProcessLookupError:
                    action = "already_exited"
                except OSError:
                    process.terminate()
                    action = "terminate_process"

                try:
                    process.wait(timeout=terminate_timeout_seconds)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(pid, signal.SIGKILL)
                        action = f"{action}_then_kill_process_group"
                    except ProcessLookupError:
                        action = f"{action}_then_already_exited"
                    except OSError:
                        process.kill()
                        action = f"{action}_then_kill_process"
                    process.wait(timeout=terminate_timeout_seconds)
                info["returncode"] = process.returncode
                info["cleanup_action"] = action
                step.finish(ok=True, pid=pid, action=action, returncode=process.returncode)
            except Exception as exc:  # pragma: no cover - best-effort process cleanup.
                info["cleanup_action"] = action
                info["cleanup_error"] = str(exc)
                info["returncode"] = process.poll()
                step.finish(ok=False, pid=pid, action=action, error=str(exc))

    def orange_client_still_running(self) -> str:
        """After a pre-start cleanup, whether orange_client still answers.

        orange_client runs as root under sudo, so the signal to the launcher's
        process group need not reach it. Returns "" when it is gone.
        """
        step = self.step("check_orange_client_exited")
        try:
            _, status = self.status("orange", ORANGE_REQUEST_SCHEMA_ID, self.args.orange_socket)
        except Exception:  # no socket or no answer: it is gone
            step.finish(ok=True)
            return ""
        folder = orange_recording_folder(status)
        message = (
            "orange_client is still running (root under sudo; the launcher's signal did not reach it); "
            f"recording {folder or '-'} ends at the spec's record_for_seconds ceiling, then it waits "
            "fixed.local_control.receipt_wait_seconds for receipts before it exits"
        )
        step.finish(ok=False, error=message, recording_folder=folder)
        return message

    def cleanup_launched_socket_files(self) -> None:
        launched_labels = {
            str(info.get("label", ""))
            for info in self.started_processes
            if (
                self.processes.get(str(info.get("label", ""))) is not None
                and self.processes[str(info.get("label", ""))].poll() is not None
            )
        }
        socket_by_label = {
            "orange": self.args.orange_socket,
            "citrus": self.args.citrus_socket,
        }
        for label in sorted(launched_labels):
            socket_path = socket_by_label.get(label, "")
            if not socket_path:
                continue
            step = self.step(f"cleanup_{label}_socket")
            path = Path(socket_path)
            try:
                if not path.exists():
                    step.finish(ok=True, socket_path=socket_path, removed=False, reason="absent")
                    continue
                path.unlink()
                step.finish(ok=True, socket_path=socket_path, removed=True)
            except Exception as exc:  # pragma: no cover - filesystem permissions detail only.
                step.finish(ok=False, socket_path=socket_path, removed=False, error=str(exc))

    def start_process(
        self,
        label: str,
        command: str,
        env_overlay: dict[str, str],
        log_path: str,
    ) -> None:
        if not command:
            return
        step = self.step(f"start_{label}_process")
        argv = shlex.split(command)
        if not argv:
            step.finish(ok=False, error="empty command")
            raise OrchestratorError(f"{label} command is empty")
        env = os.environ.copy()
        env.update(env_overlay)
        log_file = None
        stdout = None
        if log_path:
            path = Path(log_path)
            path.parent.mkdir(parents=True, exist_ok=True)
            log_file = path.open("wb")
            stdout = log_file
        try:
            process = subprocess.Popen(
                argv,
                env=env,
                stdout=stdout,
                stderr=subprocess.STDOUT if stdout else None,
                start_new_session=True,
            )
        finally:
            if log_file is not None:
                log_file.close()
        self.started_processes.append(
            {
                "label": label,
                "pid": process.pid,
                "command": argv,
                "log_path": log_path,
                "returncode": None,
            }
        )
        self.processes[label] = process
        step.finish(ok=True, pid=process.pid, command=argv, env_overlay=env_overlay)

    @property
    def bound_record(self) -> dict[str, Any]:
        record = self.__dict__.get("_bound_record")
        if record is None:
            record = self.__dict__["_bound_record"] = {}
        return record

    def settle_bound_receipts(self, final_orange_status: dict[str, Any]) -> dict[str, Any]:
        """Bound run, after Orange's finalize: wait until Citrus's receipts are
        settled, then let Orange's GUI exit by itself. Nothing may stop Orange
        or Citrus before this returns: Citrus reads Orange's status once per
        arena, so an early exit loses the later arenas (2026-11-15,
        2026_11_14_22_59_07). Raises when the receipts were not delivered."""
        # Citrus reads Orange's final recording_session.json, seals its
        # H5s and delivers the receipts; Orange's GUI stays up for them.
        # Set before waiting: the failure path must not wait a second time.
        self.bound_receipts_settled = True
        # From here a clean Orange exit is the expected end, not a failure.
        self.orange_exit_expected = True
        final_citrus_status: dict[str, Any] = {}
        folder = orange_recording_folder(final_orange_status)
        limit = self.args.citrus_receipt_timeout_seconds or (
            citrus_receipt_wait_limit_seconds(folder) if folder else 900.0
        )
        grace_started = time.monotonic()

        def receipts_settled_or_never(status: dict[str, Any]) -> bool:
            if citrus_finalization_settled(status) or citrus_receipt_build_error(status):
                return True
            return (
                citrus_finalization_never_started(status)
                and time.monotonic() - grace_started >= self.args.citrus_receipt_idle_grace_seconds
            )

        final_citrus_status = self.wait_for_status(
            "citrus",
            CITRUS_REQUEST_SCHEMA_ID,
            self.args.citrus_socket,
            receipts_settled_or_never,
            limit,
            "receipts_delivered",
        )
        state = citrus_finalization_state(final_citrus_status)
        self.bound_record["receipts"] = {
            "state": state,
            "limit_s": limit,
            "waited_s": round(time.monotonic() - grace_started, 3),
            "receipt_build_error": citrus_receipt_build_error(final_citrus_status) or None,
            "orange_completion_sent": json_path(final_citrus_status,
                                                ["experiment", "orange_completion_sent"], None),
        }
        if folder:
            self.bound_record["citrus_h5_session_status"] = citrus_h5_session_statuses(folder)
        if state != CITRUS_FINALIZATION_DELIVERED:
            build_error = citrus_receipt_build_error(final_citrus_status)
            if build_error:
                reason = f"Citrus built no receipts: {build_error}"
            elif citrus_finalization_never_started(final_citrus_status):
                reason = "Citrus built no receipts (idle, 0 attempts)"
            else:
                reason = f"recording_observation_finalization.state={state!r}"
            raise OrchestratorError("Citrus receipts not delivered: " + reason)
        # Orange's GUI exits by itself once the receipts are stored.
        orange_process = self.processes.get("orange")
        if orange_process is not None:
            try:
                orange_process.wait(timeout=120)
            except subprocess.TimeoutExpired:
                self.bound_record["orange_exit"] = "still running 120 s after delivery; cleanup stops it"

        return final_citrus_status

    def bound_run(self) -> bool:
        """A start declared as a stimulus experiment binds Citrus."""
        declaration = getattr(self.args, "declaration", None)
        return bool(declaration) and declaration.get("recording_intent") == "stimulus_experiment"

    def declare_citrus_subjects(self) -> None:
        """Bound runs with --citrus-subject-declaration-request: send Citrus's
        declare_subjects before Orange binds (the acceptance carries the
        subject), then wait until Citrus shows every arena declared."""
        request = getattr(self.args, "citrus_subject_declaration_request", None)
        if request is None:
            return
        step = self.step("citrus_declare_subjects")
        mismatches = citrus_declaration_plan_mismatches(request, getattr(self.args, "declaration", None))
        if mismatches:
            step.finish(ok=False, problems=mismatches)
            raise OrchestratorError("the Citrus subject declaration does not match Orange's: " + "; ".join(mismatches))
        response = self.send(
            self.args.citrus_socket,
            build_citrus_declare_subjects_request(
                self.args.operation_id,
                f"{self.args.operation_id}:citrus:declare_subjects",
                self.args.source,
                request,
            ),
        )
        result = json_path(response, ["effect", "subject_declarations"], None)
        self.bound_record["citrus_subject_declarations"] = {"reply": result, "error": response.get("error")}
        accepted = accepted_citrus_subject_declarations(response)
        requested = {arena["arena_id"] for arena in request["arenas"]}
        if not response_accepted(response) or set(accepted) != requested:
            step.finish(ok=False, response=response)
            detail = result.get("detail") if isinstance(result, dict) else None
            raise OrchestratorError(
                f"Citrus refused declare_subjects: {response.get('error')!r}"
                + (f" ({detail})" if detail else "")
                + ("" if set(accepted) == requested or not accepted
                   else f"; accepted arenas {sorted(accepted)} are not the requested {sorted(requested)}")
            )
        self.citrus_declared_subjects = accepted
        step.finish(ok=True, response=response)
        self.check_citrus_subject_declarations("applied", wait=True)

    def check_citrus_subject_declarations(self, when: str, *, wait: bool = False) -> dict[str, Any]:
        """Refuses unless Citrus shows the accepted declarations (executor
        rule). wait=True gives Citrus's GUI thread time to apply them."""
        expected = getattr(self, "citrus_declared_subjects", None)
        if not expected:
            return {}
        if wait:
            try:
                status = self.wait_for_status(
                    "citrus",
                    CITRUS_REQUEST_SCHEMA_ID,
                    self.args.citrus_socket,
                    lambda s: not citrus_subject_declaration_problems(s, expected),
                    self.args.citrus_subject_declaration_apply_timeout_seconds,
                    f"subjects_declared_{when}",
                )
            except OrchestratorError:
                status, _ = self.status("citrus", CITRUS_REQUEST_SCHEMA_ID, self.args.citrus_socket)
        else:
            status, _ = self.status("citrus", CITRUS_REQUEST_SCHEMA_ID, self.args.citrus_socket)
        problems = citrus_subject_declaration_problems(status, expected)
        self.bound_record.setdefault("citrus_subject_declaration_checks", []).append(
            {"when": when, "problems": problems})
        if problems:
            raise OrchestratorError(f"Citrus subject declarations not in force ({when}): " + "; ".join(problems))
        return status

    def capture_empty_background_and_wait(self) -> None:
        """The plan's capture_at_setup, in this GUI process and stream (the
        start names the capture by id and sha256): capture with the filled,
        empty dishes, write the capture record, then wait for the operator's
        confirmation that the fish are loaded and nothing else moved.
        scene_unchanged_confirmed comes only from that confirmation."""
        args = self.args
        record: dict[str, Any] = {"mode": "capture_at_setup", "state": "staging"}
        self.empty_background_record = record
        status = self.wait_for_status(
            "orange", ORANGE_REQUEST_SCHEMA_ID, args.orange_socket,
            lambda s: bool(empty_background_status(s).get("staging")) and orange_ready_for_recording(s),
            args.timeout_seconds, "empty_background_staging",
        )
        staging_problems = empty_background_staging_problems(status)
        staging_cameras = sorted(empty_background_status(status).get("staging") or {})
        record["staging"] = empty_background_status(status).get("staging")
        if staging_problems:
            record["state"] = "refused"
            raise OrchestratorError("empty background: " + "; ".join(staging_problems))

        # The display, from Citrus, immediately before the capture.
        before = self.read_citrus_display("before_capture")
        step = self.step("orange_capture_empty_background")
        request_id = f"{args.operation_id}:orange:capture_empty_background"
        params = empty_background_capture_params(args, before.get("citrus_build"))
        response = self.send(args.orange_socket, build_orange_capture_empty_background_request(
            args.operation_id, request_id, args.source, params))
        record.update(state="requested", request_id=request_id, params=params)
        if not response_accepted(response):
            step.finish(ok=False, response=response)
            record["state"] = "refused"
            raise OrchestratorError(f"Orange capture_empty_background was not accepted: {response.get('error')}")
        step.finish(ok=True, response=response)
        status = self.wait_for_status(
            "orange", ORANGE_REQUEST_SCHEMA_ID, args.orange_socket,
            lambda s: empty_background_capture_finished(s, request_id),
            args.background_capture_timeout_seconds, "empty_background_captured",
        )
        capture = empty_background_status(status)
        record["capture"] = {k: v for k, v in capture.items() if k != "staging"}
        after = self.read_citrus_display("after_capture")
        display_declaration = record["stimulus_display_declaration"]
        display_declaration["evidence"]["after_capture"] = after
        display_declaration["display_frame_index_range"] = display_frame_index_range(
            display_declaration["evidence"]["before_capture"], after)
        if display_declaration["source"] == "citrus_idle_clear":
            used, display_problems = citrus_idle_clear_problems(
                after, display_declaration["evidence"]["before_capture"])
            display_declaration["evidence_used"]["after_capture"] = used
        else:
            display_problems = citrus_display_contradictions(after)
        if display_problems:
            record["state"] = "display_changed_during_capture"
            raise OrchestratorError("the stimulus display was not as declared after the capture (the capture is "
                                    "never named): " + "; ".join(display_problems))
        problems = empty_background_capture_problems(capture, staging_cameras)
        if problems:
            record["state"] = "failed"
            raise OrchestratorError("empty background capture not usable (before fish loading): "
                                    + "; ".join(problems))
        camera_config = args.camera_config or {}
        capture_record = {
            "schema_id": CAPTURE_RECORD_SCHEMA_ID,
            "schema_version": 1,
            "operation_id": args.operation_id,
            "request_id": request_id,
            "capture_id": capture["capture_id"],
            "descriptor_path": capture.get("descriptor_path"),
            "descriptor_sha256": capture["descriptor_sha256"],
            "started_at_utc": capture.get("started_at_utc"),
            "finished_at_utc": capture.get("finished_at_utc"),
            "recorded_at_utc": utc_now(),
            "cameras": {serial: (capture.get("cameras") or {}).get(serial) for serial in staging_cameras},
            "scene_declaration": params,
            "stimulus_display_declaration": record["stimulus_display_declaration"],
            "camera_config": {key: camera_config.get(key) for key in ("name", "path", "hash_rule", "sha256")}
            if args.camera_config else None,
            "plan": plan_record(args),
            "scene_confirmation_path": str(args.run_dir / SCENE_CONFIRMATION_NAME),
            "scene_confirmation_socket": str(args.run_dir / SCENE_CONFIRMATION_SOCKET_NAME),
            "next_step": ("Load the fish without moving the dishes or changing anything else, then run "
                          f"scripts/confirm_bound_run_scene.py --run-dir {args.run_dir} --confirmed-by <identity> "
                          "(or --abort)."),
        }
        record_path = args.run_dir / CAPTURE_RECORD_NAME
        write_json_exclusive(record_path, capture_record)
        record.update(state="awaiting_scene_confirmation", capture_record_path=str(record_path),
                      capture_record=capture_record)
        print(f"[bound-run] empty background {capture['capture_id']} captured ({capture['descriptor_sha256']}); "
              f"record {record_path}. {capture_record['next_step']}", file=sys.stderr, flush=True)

        confirmation_path = args.run_dir / SCENE_CONFIRMATION_NAME
        step = self.step("wait_scene_confirmation")
        if confirmation_path.exists():
            raise OrchestratorError(f"{confirmation_path} exists before the capture's confirmation; not starting")
        if args.fish_free_test:
            # The runner's own confirmation, recorded as such.
            confirmation = build_scene_confirmation(
                capture_record,
                {"decision": SCENE_DECISION_FISH_FREE, "confirmed_by": args.background_declared_by,
                 "note": "--fish-free-test"},
                {"transport": "runner_fish_free_test",
                 "peer": {"pid": os.getpid(), "uid": os.getuid(), "gid": os.getgid(),
                          "user": os.environ.get("SUDO_USER") or os.environ.get("USER") or None,
                          "exe": sys.executable, "cmdline": sys.argv}})
            write_json_exclusive(confirmation_path, confirmation)
        else:
            confirmation = self.wait_for_scene_confirmation_request(capture_record, step)
            write_json_exclusive(confirmation_path, confirmation)
        record["scene_confirmation"] = {"path": str(confirmation_path), "content": confirmation,
                                        "refused_requests": self.scene_refused_requests}
        if confirmation["decision"] == SCENE_DECISION_ABORT:
            step.finish(ok=False, decision=SCENE_DECISION_ABORT, confirmed_by=confirmation["confirmed_by"])
            record["state"] = "aborted_by_operator"
            raise OrchestratorError(f"the operator ({confirmation['confirmed_by']}) aborted before the start")
        step.finish(ok=True, decision=confirmation["decision"], confirmed_by=confirmation["confirmed_by"])
        self.empty_background_start = {
            "capture_id": capture_record["capture_id"],
            "descriptor_sha256": capture_record["descriptor_sha256"],
            "scene_unchanged_confirmed": True,
        }
        record.update(state="confirmed", start_params=self.empty_background_start)

    def read_citrus_display(self, when: str) -> dict[str, Any]:
        """One Citrus status read as display evidence. Before the capture it
        also decides the declaration: Citrus's idle clear (status.idle_display
        preferred, else every arena's preview NONE and no calibration scene),
        or the operator's override, which Citrus may not contradict. Refuses
        otherwise; never declares unknown."""
        _, status = self.status("citrus", CITRUS_REQUEST_SCHEMA_ID, self.args.citrus_socket)
        read = citrus_display_read(status)
        record = self.empty_background_record
        if when == "before_capture":
            source = empty_background_display_source(self.args)
            record["stimulus_display_declaration"] = {
                "source": source,
                "fill_rgba_u8": CITRUS_IDLE_FILL_RGBA_U8 if source == "citrus_idle_clear" else None,
                "orange_stimulus_display": (copy.deepcopy(CITRUS_IDLE_ORANGE_STIMULUS_DISPLAY)
                                            if source == "citrus_idle_clear"
                                            else self.args.background_stimulus_display_json),
                "citrus_build": read.get("citrus_build"),
                "evidence_used": {},
                "evidence": {"before_capture": read},
            }
            declaration = record["stimulus_display_declaration"]
            if source == "citrus_idle_clear":
                used, problems = citrus_idle_clear_problems(read)
                declaration["evidence_used"]["before_capture"] = used
            else:
                problems = citrus_display_contradictions(read)
            if problems:
                record["state"] = "display_not_declarable"
                raise OrchestratorError("empty background: the stimulus display cannot be declared "
                                        f"({source}) before the capture: " + "; ".join(problems))
        return read

    def wait_for_scene_confirmation_request(self, capture_record: dict[str, Any], step: StepLog
                                            ) -> dict[str, Any]:
        """Listens on <run dir>/scene_confirmation.sock until an allowed peer
        sends a valid request for this capture, or the timeout. A refused
        request is answered, recorded and the wait goes on."""
        args = self.args
        uid, _ = invoking_identity()
        allowed = getattr(self, "scene_allowed_uids", None) or {uid, 0}
        listener = SceneConfirmationListener(args.run_dir / SCENE_CONFIRMATION_SOCKET_NAME, allowed)
        record = self.empty_background_record
        record["scene_confirmation_socket"] = {"path": str(listener.path), "mode": "0600",
                                               "allowed_uids": sorted(allowed)}
        deadline = time.monotonic() + args.scene_confirmation_timeout_seconds
        last_orange_check = 0.0
        try:
            while True:
                self.raise_if_started_process_exited("waiting for the operator's scene confirmation")
                if time.monotonic() - last_orange_check >= 5.0:
                    last_orange_check = time.monotonic()
                    try:
                        self.status("orange", ORANGE_REQUEST_SCHEMA_ID, args.orange_socket)
                    except (OSError, TimeoutError, json.JSONDecodeError, OrchestratorError):
                        pass  # judged after the wait: Orange must be ready for the start
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    step.finish(ok=False, timed_out=True, refused_requests=self.scene_refused_requests)
                    record["state"] = "confirmation_timed_out"
                    record["scene_confirmation"] = {"refused_requests": self.scene_refused_requests}
                    raise OrchestratorError(
                        f"no scene confirmation on {listener.path} within "
                        f"{args.scene_confirmation_timeout_seconds:g} s of the capture; not starting")
                pending = listener.poll(min(poll_interval_for(args, "scene_confirmation") * 10, remaining, 0.5))
                if pending is None:
                    continue
                connection, peer = pending
                received_at = utc_now()
                problems: list[str]
                request: Any = None
                try:
                    request = listener.read_request(connection)
                except (OSError, ValueError) as exc:
                    problems = [f"unreadable request: {exc}"]
                else:
                    problems = scene_confirmation_request_problems(request, capture_record)
                if peer["uid"] not in allowed:
                    # Read (so the reply reaches the caller) but never trusted or kept.
                    request = None
                    problems = [f"peer uid {peer['uid']} is neither the invoking user's nor root's"]
                if problems:
                    self.scene_refused_requests.append({"received_at_utc": received_at, "peer": peer,
                                                        "request": request, "problems": problems})
                    listener.reply(connection, {"ok": False, "accepted": False, "problems": problems})
                    continue
                confirmation = build_scene_confirmation(capture_record, request, {
                    "transport": "scene_confirmation_socket", "socket_path": str(listener.path),
                    "peer": peer, "received_at_utc": received_at})
                listener.reply(connection, {"ok": True, "accepted": True, "decision": request["decision"],
                                            "statement": confirmation["statement"],
                                            "scene_confirmation_path": str(args.run_dir / SCENE_CONFIRMATION_NAME)})
                return confirmation
        finally:
            listener.close()

    def recheck_citrus_after_scene_wait(self) -> dict[str, Any]:
        """The wait for the operator can be long: Orange must still be ready,
        and for a bound run Citrus must still pass every start check."""
        self.wait_for_status("orange", ORANGE_REQUEST_SCHEMA_ID, self.args.orange_socket,
                             orange_ready_for_recording, self.args.timeout_seconds,
                             "ready_for_recording_after_scene_confirmation")
        _, citrus = self.status("citrus", CITRUS_REQUEST_SCHEMA_ID, self.args.citrus_socket)
        if self.bound_run():
            blockers = citrus_start_blockers(citrus, self.args.expected_citrus_arenas)
            self.bound_record["start_blockers_after_scene_confirmation"] = blockers
            if blockers:
                raise OrchestratorError("Citrus is no longer ready after the scene confirmation: "
                                        + "; ".join(blockers))
            self.check_citrus_subject_declarations("after_scene_confirmation")
        elif not citrus_ready_to_start(citrus):
            raise OrchestratorError("Citrus is no longer ready to start after the scene confirmation")
        return citrus

    def run(self) -> dict[str, Any]:
        if self.args.orange_mode == "headless":
            return self.run_headless()
        final_citrus_status: dict[str, Any] = {}
        final_orange_status: dict[str, Any] = {}

        if self.args.orange_command:
            self.preflight_launch_socket(
                "orange",
                ORANGE_REQUEST_SCHEMA_ID,
                self.args.orange_socket,
                self.args.allow_preexisting_orange_socket,
            )
        if self.args.citrus_command:
            self.preflight_launch_socket(
                "citrus",
                CITRUS_REQUEST_SCHEMA_ID,
                self.args.citrus_socket,
                self.args.allow_preexisting_citrus_socket,
            )
        self.start_process(
            "orange",
            self.args.orange_command,
            orange_process_env_overlay(self.args),
            self.args.orange_log,
        )
        final_orange_status = self.wait_for_status(
            "orange",
            ORANGE_REQUEST_SCHEMA_ID,
            self.args.orange_socket,
            orange_ready_for_recording,
            self.args.timeout_seconds,
            "ready_for_recording_request",
        )

        if self.args.citrus_command:
            others = other_citrus_processes({os.getpid()})
            if others:
                raise OrchestratorError(
                    "another Citrus is running; it would lose its socket to ours: " + " | ".join(others)
                )
        citrus_env = {
            **parse_env_items(self.args.citrus_env),
            "CITRUS_GUI_LOCAL_CONTROL_SOCKET": self.args.citrus_socket,
        }
        if self.args.require_citrus_perf_jsonl:
            citrus_env["CITRUS_PERF_JSONL"] = "1"
        self.start_process(
            "citrus",
            self.args.citrus_command,
            citrus_env,
            self.args.citrus_log,
        )
        if self.args.daily_registration_mode != "preserve":
            final_citrus_status = self.wait_for_status(
                "citrus",
                CITRUS_REQUEST_SCHEMA_ID,
                self.args.citrus_socket,
                citrus_ready_for_runtime_selection,
                self.args.timeout_seconds,
                "ready_for_runtime_selection",
            )
            step = self.step("citrus_select_daily_registration_runtime_mode")
            self.citrus_daily_registration_mode_response = self.send(
                self.args.citrus_socket,
                build_citrus_daily_registration_mode_request(
                    self.args.operation_id,
                    f"{self.args.operation_id}:citrus:select_daily_registration_runtime_mode",
                    self.args.source,
                    self.args.daily_registration_mode,
                ),
            )
            if not response_accepted(self.citrus_daily_registration_mode_response):
                step.finish(ok=False, response=self.citrus_daily_registration_mode_response)
                raise OrchestratorError(
                    "Citrus daily-registration runtime-mode selection was not accepted: "
                    f"{self.citrus_daily_registration_mode_response}"
                )
            step.finish(ok=True, response=self.citrus_daily_registration_mode_response)
        final_citrus_status = self.wait_for_status(
            "citrus",
            CITRUS_REQUEST_SCHEMA_ID,
            self.args.citrus_socket,
            citrus_ready_to_start,
            self.args.timeout_seconds,
            "ready_to_start",
        )
        if self.args.require_citrus_perf_jsonl and not citrus_perf_jsonl_enabled(final_citrus_status):
            raise OrchestratorError(
                "Citrus status does not report output.perf_jsonl_enabled=true"
            )
        if self.bound_run():
            step = self.step("citrus_start_checks")
            blockers = citrus_start_blockers(final_citrus_status, self.args.expected_citrus_arenas)
            self.bound_record["citrus_build"] = {
                "git_commit_hash": json_path(final_citrus_status, ["citrus_build", "git_commit_hash"], None),
                "git_dirty": json_path(final_citrus_status, ["citrus_build", "git_dirty"], None),
            }
            self.bound_record["start_blockers"] = blockers
            step.finish(ok=not blockers, blockers=blockers)
            if blockers:
                raise OrchestratorError("Citrus is not ready for a bound start: " + "; ".join(blockers))
            self.declare_citrus_subjects()

        if self.args.empty_background == "capture_at_setup":
            self.capture_empty_background_and_wait()
            final_citrus_status = self.recheck_citrus_after_scene_wait()

        step = self.step("orange_start_recording")
        orange_start_response = self.send(
            self.args.orange_socket,
            build_orange_start_request(
                self.args.operation_id,
                f"{self.args.operation_id}:orange:start_recording",
                self.args.source,
                self.args.declaration,
                declaration_files_root=(str(self.args.declaration_files_root)
                                        if self.args.declaration_files_root else None),
                empty_background=self.empty_background_start,
            ),
        )
        if not response_accepted(orange_start_response):
            step.finish(ok=False, response=orange_start_response)
            raise OrchestratorError(f"Orange start_recording was not accepted: {orange_start_response}")
        self.orange_recording_started = True
        step.finish(ok=True, response=orange_start_response)

        final_orange_status = self.wait_for_status(
            "orange",
            ORANGE_REQUEST_SCHEMA_ID,
            self.args.orange_socket,
            orange_ready_for_citrus,
            self.args.timeout_seconds,
            "ready_for_citrus_experiment",
        )

        self.check_citrus_subject_declarations("before_start")
        step = self.step("citrus_start_experiment")
        citrus_start_response = self.send(
            self.args.citrus_socket,
            build_citrus_start_request(
                self.args.operation_id,
                f"{self.args.operation_id}:citrus:start_experiment",
                self.args.source,
            ),
        )
        if not response_accepted(citrus_start_response):
            step.finish(ok=False, response=citrus_start_response)
            raise OrchestratorError(f"Citrus start_experiment was not accepted: {citrus_start_response}")
        self.citrus_start_sent = True
        step.finish(ok=True, response=citrus_start_response)

        final_citrus_status = self.wait_for_citrus_terminal_or_run_duration()
        if self.bound_run():
            self.bound_record["citrus_terminal_state"] = citrus_terminal_state(final_citrus_status)
            self.bound_record["citrus_terminal_reason"] = citrus_terminal_reason(final_citrus_status)
            if citrus_terminal_state(final_citrus_status) == "start_rejected":
                raise OrchestratorError(
                    "Citrus refused the start: terminal_reason=" + repr(citrus_terminal_reason(final_citrus_status))
                )
        if not citrus_is_terminal(final_citrus_status):
            self.request_citrus_stop_for_run_duration()
            final_citrus_status = self.wait_for_status(
                "citrus",
                CITRUS_REQUEST_SCHEMA_ID,
                self.args.citrus_socket,
                citrus_is_terminal,
                self.args.citrus_terminal_timeout_seconds,
                "terminal_state",
            )
        if self.args.require_citrus_perf_jsonl and not citrus_perf_jsonl_path_known(final_citrus_status):
            final_citrus_status = self.wait_for_status(
                "citrus",
                CITRUS_REQUEST_SCHEMA_ID,
                self.args.citrus_socket,
                citrus_perf_jsonl_path_known,
                self.args.timeout_seconds,
                "perf_jsonl_path_known",
            )
        self.citrus_control_complete = True

        if self.args.stop_policy == "citrus_completion_notify":
            final_orange_status = self.wait_for_orange_citrus_notify_stop()
            if self.bound_run():
                final_citrus_status = self.settle_bound_receipts(final_orange_status)
            self.require_orange_citrus_notify_stop_status(
                final_orange_status,
                final_citrus_status,
            )
            self.orange_recording_started = False
        elif self.args.stop_policy != "none":
            final_orange_status = self.request_orange_stop(final_citrus_status)
            if self.bound_run():
                final_citrus_status = self.settle_bound_receipts(final_orange_status)
            self.orange_recording_started = False

        self.require_orange_local_control_event_log(final_orange_status)
        self.validation_results = self.run_validations(
            final_orange_status,
            final_citrus_status,
        )
        self.cleanup_started_processes()
        self.cleanup_launched_socket_files()

        return self.summary(
            "pass",
            final_orange_status=final_orange_status,
            final_citrus_status=final_citrus_status,
        )

    def run_headless(self) -> dict[str, Any]:
        """Citrus (waiting for control) -> orange_client (bound, recording) ->
        start_experiment -> Citrus terminal -> orange_client stops on Citrus's
        completion, finalizes, waits for the receipts and exits."""
        if not self.args.orange_command:
            raise OrchestratorError("--orange-mode headless needs --orange-command (the orange_client run)")
        if self.args.stop_policy not in ("citrus_completion_notify", "none"):
            raise OrchestratorError(
                "--orange-mode headless: orange_client stops itself on Citrus's completion; "
                "use --stop-policy citrus_completion_notify"
            )
        if self.args.citrus_command:
            self.preflight_launch_socket(
                "citrus",
                CITRUS_REQUEST_SCHEMA_ID,
                self.args.citrus_socket,
                self.args.allow_preexisting_citrus_socket,
            )
        self.preflight_launch_socket(
            "orange",
            ORANGE_REQUEST_SCHEMA_ID,
            self.args.orange_socket,
            self.args.allow_preexisting_orange_socket,
        )

        # Citrus first: orange_client's pre-arm needs Citrus's socket.
        if self.args.citrus_command:
            others = other_citrus_processes({os.getpid()})
            if others:
                raise OrchestratorError(
                    "another Citrus is running; it would lose its socket to ours: " + " | ".join(others)
                )
        citrus_env = {
            **parse_env_items(self.args.citrus_env),
            "CITRUS_GUI_LOCAL_CONTROL_SOCKET": self.args.citrus_socket,
        }
        if self.args.require_citrus_perf_jsonl:
            citrus_env["CITRUS_PERF_JSONL"] = "1"
        self.start_process("citrus", self.args.citrus_command, citrus_env, self.args.citrus_log)
        citrus_gate = (
            citrus_waiting_for_control_start
            if self.args.citrus_start_gate == "wait_for_control"
            else citrus_ready_to_start
        )
        final_citrus_status = self.wait_for_status(
            "citrus",
            CITRUS_REQUEST_SCHEMA_ID,
            self.args.citrus_socket,
            citrus_gate,
            self.args.timeout_seconds,
            f"ready_{self.args.citrus_start_gate}",
        )
        if self.args.require_citrus_perf_jsonl and not citrus_perf_jsonl_enabled(final_citrus_status):
            raise OrchestratorError("Citrus status does not report output.perf_jsonl_enabled=true")
        # Before orange_client binds: Citrus's acceptance carries the subject.
        self.declare_citrus_subjects()

        self.start_process(
            "orange",
            self.args.orange_command,
            orange_process_env_overlay(self.args),
            self.args.orange_log,
        )
        final_orange_status = self.wait_for_status(
            "orange",
            ORANGE_REQUEST_SCHEMA_ID,
            self.args.orange_socket,
            orange_headless_ready_for_citrus,
            self.args.orange_ready_timeout_seconds,
            "headless_recording_bound",
        )
        self.orange_recording_started = True
        recording_folder = orange_recording_folder(final_orange_status)
        step = self.step("check_orange_headless_pre_arm")
        lifecycle = read_pre_arm_lifecycle_status(recording_folder)
        completion_enabled = orange_headless_citrus_completion_enabled(final_orange_status)
        if lifecycle != "accepted_pending_finalization" or not completion_enabled:
            step.finish(
                ok=False,
                recording_folder=recording_folder,
                pre_arm_lifecycle_status=lifecycle,
                citrus_completion_enabled=completion_enabled,
            )
            reasons = []
            if lifecycle != "accepted_pending_finalization":
                reasons.append(
                    f"pre_arm lifecycle_status={lifecycle!r} (needs accepted_pending_finalization)"
                )
            if not completion_enabled:
                reasons.append(
                    "Citrus completion is off (needs fixed.local_control.stop_on_citrus_completion)"
                )
            raise OrchestratorError(
                "orange_client is recording but not ready for a Citrus-ended run: " + "; ".join(reasons)
            )
        step.finish(
            ok=True,
            recording_folder=recording_folder,
            pre_arm_lifecycle_status=lifecycle,
            citrus_completion_enabled=True,
        )

        # The start goes only to a Citrus that is still waiting for it.
        final_citrus_status = self.wait_for_status(
            "citrus",
            CITRUS_REQUEST_SCHEMA_ID,
            self.args.citrus_socket,
            citrus_gate,
            self.args.socket_timeout_seconds,
            f"still_{self.args.citrus_start_gate}",
        )
        self.check_citrus_subject_declarations("before_start")
        step = self.step("citrus_start_experiment")
        self.citrus_start_sent = True
        citrus_start_response = self.send(
            self.args.citrus_socket,
            build_citrus_start_request(
                self.args.operation_id,
                f"{self.args.operation_id}:citrus:start_experiment",
                self.args.source,
            ),
        )
        if not response_accepted(citrus_start_response):
            step.finish(ok=False, response=citrus_start_response)
            raise OrchestratorError(f"Citrus start_experiment was not accepted: {citrus_start_response}")
        self.citrus_start_sent = True
        step.finish(ok=True, response=citrus_start_response)

        final_citrus_status = self.wait_for_citrus_terminal_or_run_duration()
        if self.bound_run():
            self.bound_record["citrus_terminal_state"] = citrus_terminal_state(final_citrus_status)
            self.bound_record["citrus_terminal_reason"] = citrus_terminal_reason(final_citrus_status)
            if citrus_terminal_state(final_citrus_status) == "start_rejected":
                raise OrchestratorError(
                    "Citrus refused the start: terminal_reason=" + repr(citrus_terminal_reason(final_citrus_status))
                )
        if not citrus_is_terminal(final_citrus_status):
            self.request_citrus_stop_for_run_duration()
            final_citrus_status = self.wait_for_status(
                "citrus",
                CITRUS_REQUEST_SCHEMA_ID,
                self.args.citrus_socket,
                citrus_is_terminal,
                self.args.citrus_terminal_timeout_seconds,
                "terminal_state",
            )
        self.citrus_control_complete = True
        start_source = citrus_autorun_status(final_citrus_status).get("start_source")
        if self.args.citrus_start_gate == "wait_for_control" and start_source != "local_control":
            raise OrchestratorError(
                f"Citrus reports start_source={start_source!r} for this run, expected 'local_control'"
            )

        # orange_client: Citrus's completion -> stop after the grace -> finalize
        # -> Citrus's receipts -> exit. Its socket goes away with it.
        returncode = self.wait_for_process_exit("orange", self.args.orange_exit_timeout_seconds)
        self.orange_recording_started = False
        run_entry = read_headless_run_entry(recording_folder)
        receipts = run_entry.get("citrus_receipts", {}) if isinstance(run_entry, dict) else {}
        receipts_status = receipts.get("status", "") if isinstance(receipts, dict) else ""
        self.headless_result = {
            "recording_folder": recording_folder,
            "orange_returncode": returncode,
            "run_status": run_entry.get("status", ""),
            "run_pass_fail": run_entry.get("pass_fail", ""),
            "run_reason": run_entry.get("reason", ""),
            "citrus_receipts": receipts,
            "citrus_start_source": start_source,
        }
        final_orange_status = {
            "process": "orange_client",
            "recording": {"folder": recording_folder},
            "readiness": {"recording_finalized": returncode == 0},
        }

        if self.args.citrus_command:
            self.wait_for_process_exit("citrus", self.args.citrus_exit_timeout_seconds, required=False)
        self.validation_results = self.run_validations(final_orange_status, final_citrus_status)
        self.cleanup_started_processes()
        self.cleanup_launched_socket_files()

        failures = []
        if returncode != 0:
            failures.append(f"orange_client exited {returncode}")
        if receipts_status != "finalized":
            failures.append(f"citrus_receipts status={receipts_status!r}")
        if run_entry.get("pass_fail") != "pass":
            failures.append(f"run pass_fail={run_entry.get('pass_fail')!r} reason={run_entry.get('reason')!r}")
        return self.summary(
            "fail" if failures else "pass",
            error="; ".join(failures),
            final_orange_status=final_orange_status,
            final_citrus_status=final_citrus_status,
        )

    def wait_for_process_exit(self, label: str, timeout_seconds: float, *, required: bool = True) -> int | None:
        step = self.step(f"wait_{label}_process_exit")
        process = self.processes.get(label)
        if process is None:
            step.finish(ok=not required, reason="not_launched")
            if required:
                raise OrchestratorError(f"{label} process was not launched by this orchestrator")
            return None
        try:
            returncode = process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            step.finish(ok=not required, timeout_seconds=timeout_seconds, still_running=True)
            if required:
                raise OrchestratorError(f"{label} process did not exit within {timeout_seconds}s")
            return None
        self.refresh_started_processes()
        step.finish(ok=True, returncode=returncode)
        return returncode

    def run_validations(
        self,
        orange_status: dict[str, Any],
        citrus_status: dict[str, Any],
    ) -> list[dict[str, Any]]:
        commands = validation_commands_from_args(self.args)
        validation_artifacts = validation_artifacts_from_args(self.args)
        if not commands:
            return []

        results: list[dict[str, Any]] = []
        env = os.environ.copy()
        env.update(parse_env_items(self.args.validation_env))
        cwd = self.args.validation_cwd or str(REPO_ROOT)

        for item in commands:
            label = item["label"]
            rendered_command = render_validation_command(
                item["command"],
                operation_id=self.args.operation_id,
                orange_status=orange_status,
                citrus_status=citrus_status,
            )
            argv = shlex.split(rendered_command)
            step = self.step(f"validation_{label}")
            started = time.monotonic()
            result: dict[str, Any] = {
                "label": label,
                "command": argv,
                "command_text": rendered_command,
                "cwd": cwd,
                "returncode": None,
                "timed_out": False,
                "duration_seconds": None,
                "stdout": "",
                "stderr": "",
                "stdout_truncated": False,
                "stderr_truncated": False,
                "artifact_paths": validation_artifacts.get(label, []),
            }
            try:
                completed = subprocess.run(
                    argv,
                    cwd=cwd,
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=self.args.validation_timeout_seconds,
                    check=False,
                    errors="replace",
                )
                duration = time.monotonic() - started
                stdout, stdout_truncated = tail_text(
                    completed.stdout,
                    self.args.validation_output_max_chars,
                )
                stderr, stderr_truncated = tail_text(
                    completed.stderr,
                    self.args.validation_output_max_chars,
                )
                result.update(
                    {
                        "returncode": completed.returncode,
                        "duration_seconds": duration,
                        "stdout": stdout,
                        "stderr": stderr,
                        "stdout_truncated": stdout_truncated,
                        "stderr_truncated": stderr_truncated,
                    }
                )
                step.finish(ok=completed.returncode == 0, result=result)
                results.append(result)
                if completed.returncode != 0:
                    self.validation_results = results
                    raise OrchestratorError(
                        f"validation command {label} failed with exit code {completed.returncode}"
                    )
            except subprocess.TimeoutExpired as exc:
                duration = time.monotonic() - started
                stdout_raw = exc.stdout or ""
                stderr_raw = exc.stderr or ""
                if isinstance(stdout_raw, bytes):
                    stdout_raw = stdout_raw.decode("utf-8", errors="replace")
                if isinstance(stderr_raw, bytes):
                    stderr_raw = stderr_raw.decode("utf-8", errors="replace")
                stdout, stdout_truncated = tail_text(
                    stdout_raw,
                    self.args.validation_output_max_chars,
                )
                stderr, stderr_truncated = tail_text(
                    stderr_raw,
                    self.args.validation_output_max_chars,
                )
                result.update(
                    {
                        "timed_out": True,
                        "duration_seconds": duration,
                        "stdout": stdout,
                        "stderr": stderr,
                        "stdout_truncated": stdout_truncated,
                        "stderr_truncated": stderr_truncated,
                    }
                )
                step.finish(ok=False, result=result)
                results.append(result)
                self.validation_results = results
                raise OrchestratorError(
                    f"validation command {label} timed out after "
                    f"{self.args.validation_timeout_seconds}s"
                ) from exc
        return results

    def request_orange_stop(self, citrus_status: dict[str, Any]) -> dict[str, Any]:
        if self.args.stop_policy == "stop_recording":
            request = build_orange_stop_request(
                self.args.operation_id,
                f"{self.args.operation_id}:orange:stop_recording",
                self.args.source,
                "orchestrator_stop",
                effective_orange_stop_grace_seconds(self.args),
            )
        elif self.args.stop_policy == "citrus_completion":
            terminal_state = citrus_terminal_state(citrus_status) or "completed"
            terminal_reason = citrus_terminal_reason(citrus_status) or "orchestrator_completion"
            request = build_orange_citrus_completion_request(
                self.args.operation_id,
                f"citrus_completion:{self.args.operation_id}:{terminal_state}:{terminal_reason}",
                self.args.source,
                self.args.operation_id,
                terminal_state,
                terminal_reason,
                effective_orange_stop_grace_seconds(self.args),
            )
        else:
            raise OrchestratorError(f"unsupported stop policy: {self.args.stop_policy}")

        step = self.step(f"orange_{request['method']}")
        response = self.send(self.args.orange_socket, request)
        if not response_accepted(response):
            step.finish(ok=False, response=response)
            raise OrchestratorError(f"Orange {request['method']} was not accepted: {response}")
        step.finish(ok=True, response=response)

        if self.args.skip_wait_orange_finalized:
            _, status = self.status("orange", ORANGE_REQUEST_SCHEMA_ID, self.args.orange_socket)
            self.require_orange_drain_not_timed_out(status)
            self.require_orange_stop_ack_status(status)
            return status
        status = self.wait_for_status(
            "orange",
            ORANGE_REQUEST_SCHEMA_ID,
            self.args.orange_socket,
            orange_recording_finalized,
            self.args.orange_finalize_timeout_seconds,
            "recording_finalized",
        )
        self.require_orange_drain_not_timed_out(status)
        self.require_orange_stop_ack_status(status)
        return status

    def wait_for_orange_citrus_notify_stop(self) -> dict[str, Any]:
        if self.args.skip_wait_orange_finalized:
            _, status = self.status(
                "orange",
                ORANGE_REQUEST_SCHEMA_ID,
                self.args.orange_socket,
            )
            self.require_orange_drain_not_timed_out(status)
            self.require_orange_stop_ack_status(status)
            return status

        status = self.wait_for_status(
            "orange",
            ORANGE_REQUEST_SCHEMA_ID,
            self.args.orange_socket,
            orange_recording_finalized,
            self.args.orange_finalize_timeout_seconds,
            "recording_finalized_after_citrus_completion_notify",
        )
        self.require_orange_drain_not_timed_out(status)
        self.require_orange_stop_ack_status(status)
        return status

    def require_orange_citrus_notify_stop_status(
        self,
        orange_status: dict[str, Any],
        citrus_status: dict[str, Any],
    ) -> None:
        check = summarize_orange_citrus_notify_stop_status(
            orange_status,
            citrus_status,
            stop_policy=self.args.stop_policy,
            operation_id=self.args.operation_id,
        )
        step = self.step("orange_citrus_notify_stop_status_check")
        step.finish(ok=check["ok"], check=check)
        if check["ok"]:
            return
        failure_text = "; ".join(str(item) for item in check["failures"])
        raise OrchestratorError(
            f"Orange Citrus completion stop status mismatch: {failure_text}"
        )

    def require_orange_local_control_event_log(self, status: dict[str, Any]) -> None:
        event_log = summarize_orange_local_control_event_log(
            resolve_orange_local_control_log_path(self.args)
        )
        check = check_orange_local_control_event_log(
            event_log,
            required=self.args.require_orange_local_control_event_log,
            operation_id=self.args.operation_id,
            stop_policy=self.args.stop_policy,
            orange_status=status,
        )
        step = self.step("orange_local_control_event_log_check")
        step.finish(ok=check["ok"], check=check)
        if check["ok"]:
            return
        failure_text = "; ".join(str(item) for item in check["failures"])
        raise OrchestratorError(f"Orange local-control event-log check failed: {failure_text}")

    def require_orange_drain_not_timed_out(self, status: dict[str, Any]) -> None:
        status_check = summarize_orange_drain_timeout_status(
            status,
            allow_drain_timeout=self.args.allow_orange_drain_timeout,
        )
        step = self.step("orange_drain_timeout_status_check")
        step.finish(ok=status_check["ok"], check=status_check)
        timeout_status_failures = status_check["consistency_failures"]
        if timeout_status_failures:
            failure_text = "; ".join(timeout_status_failures)
            raise OrchestratorError(
                "Orange recording drain timeout status is inconsistent: "
                f"{failure_text}"
            )
        if status_check["policy_ok"]:
            return
        stop_status = orange_local_control_recording_stop(status)
        raise OrchestratorError(
            "Orange recording drain timed out before finalization: "
            f"request_id={stop_status.get('request_id', '')} "
            f"operation_id={stop_status.get('operation_id', '')} "
            f"elapsed_seconds={stop_status.get('drain_elapsed_seconds', '')} "
            f"timeout_seconds={stop_status.get('drain_timeout_seconds', '')} "
            f"last_event={stop_status.get('last_event', '')}"
        )

    def require_orange_stop_ack_status(self, status: dict[str, Any]) -> None:
        ack_check = summarize_orange_stop_ack_status(
            status,
            stop_policy=self.args.stop_policy,
            finalized_wait_skipped=self.args.skip_wait_orange_finalized,
        )
        step = self.step("orange_stop_ack_status_check")
        step.finish(ok=ack_check["ok"], check=ack_check)
        if ack_check["ok"]:
            return
        failure_text = "; ".join(str(item) for item in ack_check["failures"])
        raise OrchestratorError(f"Orange stop ACK-state check failed: {failure_text}")

    def persist_artifacts(self, summary: dict[str, Any]) -> None:
        artifacts: dict[str, Any] = {
            "copy_to_recording_enabled": bool(self.args.copy_artifacts_to_recording),
            "recording_folder": json_path(summary, ["orange", "recording_folder"], ""),
            "artifact_dir": "",
            "logs": {},
            "validations": {},
        }
        summary["artifacts"] = artifacts
        if not self.args.copy_artifacts_to_recording:
            artifacts["reason"] = "disabled"
            return
        if self.args.orange_mode == "headless":
            artifacts["copy_to_recording_enabled"] = False
            artifacts["reason"] = "headless_recording_sealed_by_orange_client"
            return

        recording_folder = str(artifacts["recording_folder"] or "")
        if not recording_folder:
            artifacts["reason"] = "missing_orange_recording_folder"
            return

        artifact_dir = Path(recording_folder) / self.args.artifact_dir_name
        artifact_dir.mkdir(parents=True, exist_ok=True)
        artifacts["artifact_dir"] = str(artifact_dir)
        artifacts["logs"]["orange"] = self.copy_artifact_file(
            self.args.orange_log,
            artifact_dir / "orange.log",
        )
        artifacts["logs"]["orange_local_control"] = self.copy_artifact_file(
            resolve_orange_local_control_log_path(self.args),
            artifact_dir / "orange_local_control.events.jsonl",
        )
        artifacts["logs"]["citrus"] = self.copy_artifact_file(
            self.args.citrus_log,
            artifact_dir / "citrus.log",
        )
        artifacts["logs"]["citrus_local_control"] = self.copy_artifact_file(
            resolve_citrus_local_control_log_path(self.args),
            artifact_dir / "citrus_local_control.events.jsonl",
        )
        for validation in summary.get("validations", []):
            if not isinstance(validation, dict):
                continue
            label = str(validation.get("label") or "validation")
            safe_label = safe_artifact_name(label)
            copied_items: list[dict[str, Any]] = []
            for path in validation.get("artifact_paths", []):
                if not isinstance(path, str):
                    continue
                source_path = Path(path)
                target_name = f"{safe_label}_{source_path.name}" if source_path.name else safe_label
                copied_items.append(
                    self.copy_artifact_file(
                        str(source_path),
                        artifact_dir / target_name,
                    )
                )
            if copied_items:
                artifacts["validations"][label] = copied_items
        artifact_summary = artifact_dir / "orchestrator_summary.json"
        artifacts["summary_json_artifact"] = str(artifact_summary)
        artifact_summary.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    @staticmethod
    def copy_artifact_file(source: str, target: Path) -> dict[str, Any]:
        if not source:
            return {"source": source, "path": str(target), "copied": False, "reason": "empty_source"}
        source_path = Path(source)
        result: dict[str, Any] = {
            "source": str(source_path),
            "path": str(target),
            "copied": False,
        }
        if not source_path.exists():
            result["reason"] = "missing_source"
            return result
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            if source_path.resolve() == target.resolve():
                result["reason"] = "source_is_target"
            else:
                shutil.copy2(source_path, target)
                result["copied"] = True
        except Exception as exc:  # pragma: no cover - filesystem detail only.
            result["reason"] = "copy_failed"
            result["error"] = str(exc)
            return result
        if target.exists():
            result["bytes"] = target.stat().st_size
        return result

    def best_effort_stop_after_failure(self) -> dict[str, Any] | None:
        if not self.args.stop_orange_on_failure or not self.orange_recording_started:
            return None
        if orange_recording_finalized(self.last_orange_status):
            return None
        try:
            request = build_orange_stop_request(
                self.args.operation_id,
                f"{self.args.operation_id}:orange:stop_recording:failure",
                self.args.source,
                "orchestrator_failure",
                0.0,
            )
            return self.send(self.args.orange_socket, request)
        except Exception as exc:  # pragma: no cover - failure path detail only.
            return {"ok": False, "error": str(exc)}

    def summary(
        self,
        result: str,
        *,
        error: str = "",
        final_orange_status: dict[str, Any] | None = None,
        final_citrus_status: dict[str, Any] | None = None,
        failure_stop_response: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        citrus_status = final_citrus_status or self.last_citrus_status or {}
        orange_status = final_orange_status or self.last_orange_status or {}
        orange_event_log = summarize_orange_local_control_event_log(
            resolve_orange_local_control_log_path(self.args)
        )
        orange_event_log_check = check_orange_local_control_event_log(
            orange_event_log,
            required=self.args.require_orange_local_control_event_log,
            operation_id=self.args.operation_id,
            stop_policy=self.args.stop_policy,
            orange_status=orange_status,
        )
        orange_drain_timeout_status_check = summarize_orange_drain_timeout_status(
            orange_status,
            allow_drain_timeout=self.args.allow_orange_drain_timeout,
        )
        orange_stop_ack_status_check = summarize_orange_stop_ack_status(
            orange_status,
            stop_policy=self.args.stop_policy,
            finalized_wait_skipped=self.args.skip_wait_orange_finalized,
        )
        orange_citrus_notify_stop_status_check = (
            summarize_orange_citrus_notify_stop_status(
                orange_status,
                citrus_status,
                stop_policy=self.args.stop_policy,
                operation_id=self.args.operation_id,
            )
        )
        self.refresh_started_processes()
        return {
            "schema_id": SUMMARY_SCHEMA_ID,
            "schema_version": SUMMARY_SCHEMA_VERSION,
            "generated_at_utc": utc_now(),
            "result": result,
            "error": error,
            "operation_id": self.args.operation_id,
            "bound_run": self.bound_record if self.bound_run() else None,
            "camera_config": self.args.camera_config,
            "plan": plan_record(self.args),
            "empty_background": self.empty_background_record or empty_background_plan(self.args),
            "lifecycle_events": (self.lifecycle_waker.record() if self.lifecycle_waker is not None
                                 else {"mode": "off"}),
            "mode": "execute" if self.args.execute else "dry_run",
            "orange": {
                "socket": self.args.orange_socket,
                "log_path": self.args.orange_log,
                "local_control_event_log_path": resolve_orange_local_control_log_path(self.args),
                "local_control_event_log": orange_event_log,
                "local_control_event_log_check": orange_event_log_check,
                "recording_folder": json_path(orange_status, ["recording", "folder"], ""),
                "phase": orange_status.get("phase", ""),
                "ready_for_recording_request": orange_ready_for_recording(orange_status),
                "ready_for_citrus_experiment": orange_ready_for_citrus(orange_status),
                "recording_finalized": orange_recording_finalized(orange_status),
                "local_control_recording_stop": orange_local_control_recording_stop(orange_status),
                "local_control_stop_drain_timed_out": orange_recording_stop_drain_timed_out(orange_status),
                "local_control_stop_state": orange_recording_stop_state(orange_status),
                "local_control_stop_ack_state": orange_recording_stop_ack_state(orange_status),
                "local_control_stop_health": orange_recording_stop_health(orange_status),
                "local_control_stop_error_code": orange_recording_stop_error_code(orange_status),
                "local_control_stop_ack_status_check": (
                    orange_stop_ack_status_check
                ),
                "local_control_citrus_notify_stop_status_check": (
                    orange_citrus_notify_stop_status_check
                ),
                "local_control_stop_timeout_status_check": (
                    orange_drain_timeout_status_check
                ),
                "allow_drain_timeout": self.args.allow_orange_drain_timeout,
            },
            "citrus": {
                "socket": self.args.citrus_socket,
                "log_path": self.args.citrus_log,
                "run_seconds": self.args.citrus_run_seconds,
                "terminal_state": citrus_terminal_state(citrus_status),
                "terminal_reason": citrus_terminal_reason(citrus_status),
                "perf_jsonl_enabled": citrus_perf_jsonl_enabled(citrus_status),
                "perf_jsonl_path_known": citrus_perf_jsonl_path_known(citrus_status),
                "perf_jsonl_path": json_path(citrus_status, ["output", "perf_jsonl_path"], ""),
                "daily_registration_mode": self.args.daily_registration_mode,
                "daily_registration_mode_response": self.citrus_daily_registration_mode_response,
            },
            "orange_mode": self.args.orange_mode,
            "headless": self.headless_result if self.args.orange_mode == "headless" else None,
            "validations": self.validation_results,
            "started_processes": self.started_processes,
            "steps": [step.__dict__ for step in self.steps],
            "failure_stop_response": failure_stop_response,
        }


def default_operation_id() -> str:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return f"orange_citrus_{stamp}_{uuid.uuid4().hex[:8]}"


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--execute", action="store_true", help="Run the orchestration. Default prints a dry-run plan.")
    parser.add_argument("--operation-id", default=default_operation_id(), help="Idempotent operation id for Orange/Citrus mutating requests.")
    parser.add_argument("--source", default="orange_citrus_orchestrator", help="Source label used in local-control requests.")
    parser.add_argument("--orange-socket", default=os.environ.get("ORANGE_GUI_LOCAL_CONTROL_SOCKET", "/tmp/orange_local_control.sock"))
    parser.add_argument("--citrus-socket", default=os.environ.get("CITRUS_GUI_LOCAL_CONTROL_SOCKET", "/tmp/citrus_local_control.sock"))
    parser.add_argument("--orange-command", default="", help="Optional Orange command to start before polling its socket.")
    parser.add_argument("--citrus-command", default="", help="Optional Citrus command to start before polling its socket.")
    parser.add_argument("--orange-env", action="append", default=[], help="Extra Orange process env override, KEY=VALUE. Repeatable.")
    parser.add_argument("--citrus-env", action="append", default=[], help="Extra Citrus process env override, KEY=VALUE. Repeatable.")
    parser.add_argument("--orange-log", default="/tmp/orange_citrus_orchestrator_orange.log")
    parser.add_argument(
        "--orange-local-control-log",
        default="",
        help=(
            "Orange local-control JSONL event log path. Defaults to "
            "ORANGE_GUI_LOCAL_CONTROL_LOG / ORANGE_LOCAL_CONTROL_LOG if set, "
            "otherwise <orange-socket>.events.jsonl."
        ),
    )
    parser.add_argument("--citrus-log", default="/tmp/orange_citrus_orchestrator_citrus.log")
    parser.add_argument("--summary-json", default="", help="Optional path for the combined run summary JSON.")
    parser.add_argument(
        "--artifact-dir-name",
        default="orchestrator",
        help="Subdirectory name under the Orange recording folder for orchestrator logs/summaries.",
    )
    parser.add_argument(
        "--no-copy-artifacts-to-recording",
        dest="copy_artifacts_to_recording",
        action="store_false",
        help="Do not copy orchestrator process logs and summary into the Orange recording folder.",
    )
    parser.set_defaults(copy_artifacts_to_recording=True)
    parser.add_argument("--poll-interval-seconds", type=positive_float, default=0.25,
                        help="Status polling for the readiness waits (about a second).")
    parser.add_argument("--long-wait-poll-interval-seconds", type=positive_float, default=1.5,
                        help="Status polling for the waits that span a protocol or a finalization.")
    parser.add_argument("--lifecycle-events", choices=("auto", "off"), default="auto",
                        help="auto: long waits wake on new rows in Orange's and Citrus's lifecycle "
                             "event logs and read status then (status stays the authority); polling "
                             "when no log exists. off: poll only.")
    parser.add_argument("--lifecycle-event-heartbeat-seconds", type=positive_float, default=5.0,
                        help="With an event log on disk: the longest a long wait goes without a status read.")
    parser.add_argument("--lifecycle-event-check-seconds", type=positive_float, default=0.1,
                        help="How often the event logs are checked (a stat; no socket traffic).")
    parser.add_argument("--citrus-local-control-log", default="",
                        help="Citrus lifecycle event log. Defaults to CITRUS_GUI_LOCAL_CONTROL_LOG, else "
                             "<citrus-socket>.events.jsonl.")
    parser.add_argument("--socket-timeout-seconds", type=positive_float, default=2.0)
    parser.add_argument("--launch-socket-preflight-timeout-seconds", type=positive_float, default=0.2)
    parser.add_argument("--timeout-seconds", type=positive_float, default=120.0)
    parser.add_argument("--citrus-terminal-timeout-seconds", type=positive_float, default=300.0)
    parser.add_argument(
        "--citrus-run-seconds",
        type=nonnegative_float,
        default=0.0,
        help=(
            "If positive, stop Citrus after this many seconds of active/armed "
            "experiment time, then wait for terminal state."
        ),
    )
    parser.add_argument(
        "--daily-registration-mode",
        choices=("preserve", "base_only"),
        default="preserve",
        help=(
            "Runtime geometry selection before readiness polling. preserve leaves "
            "the operator's current selection untouched; base_only performs the "
            "explicit armed Citrus transition to commissioned base geometry."
        ),
    )
    parser.add_argument("--orange-finalize-timeout-seconds", type=positive_float, default=180.0)
    parser.add_argument("--orange-stop-grace-seconds", type=nonnegative_float, default=None)
    parser.add_argument(
        "--allow-orange-drain-timeout",
        action="store_true",
        help=(
            "Do not fail the orchestrator if Orange local-control status reports "
            "recording_stop.drain_timed_out=true."
        ),
    )
    parser.add_argument("--require-citrus-perf-jsonl", action="store_true")
    parser.add_argument(
        "--validation-command",
        action="append",
        default=[],
        help="Post-run validation command as LABEL=COMMAND. Repeatable. Non-zero exits fail the orchestrator.",
    )
    parser.add_argument(
        "--validation-artifact",
        action="append",
        default=[],
        help="Validation output artifact as LABEL=PATH. Repeatable. Copied into the Orange recording folder.",
    )
    parser.add_argument(
        "--orange-validation-command",
        action="append",
        default=[],
        help="Post-run Orange validation command. Repeatable. Label is orange_validation_N.",
    )
    parser.add_argument(
        "--citrus-validation-command",
        action="append",
        default=[],
        help="Post-run Citrus validation command. Repeatable. Label is citrus_validation_N.",
    )
    parser.add_argument(
        "--validation-env",
        action="append",
        default=[],
        help="Extra env override for validation commands, KEY=VALUE. Repeatable.",
    )
    parser.add_argument("--validation-cwd", default=str(REPO_ROOT))
    parser.add_argument("--validation-timeout-seconds", type=positive_float, default=120.0)
    parser.add_argument("--validation-output-max-chars", type=int, default=20000)
    parser.add_argument("--skip-wait-orange-finalized", action="store_true")
    parser.add_argument("--allow-preexisting-orange-socket", action="store_true")
    parser.add_argument("--allow-preexisting-citrus-socket", action="store_true")
    parser.add_argument(
        "--require-orange-local-control-event-log",
        action="store_true",
        help=(
            "Fail execute-mode runs unless the Orange local-control event log "
            "contains matching socket rows and GUI-thread start/stop/drain "
            "lifecycle events for this operation."
        ),
    )
    # The recording's declaration (GUI mode): who declares, the intent and the
    # subjects, as orange_local_control_client.py start-recording takes them.
    # Without it the Orange operator must Confirm the Subject panel first.
    parser.add_argument("--declared-by", help="Identity of the person declaring the recording (e.g. delahantyj).")
    parser.add_argument("--intent", choices=("stimulus_experiment", "recording_only"),
                        help="Recording intent declared for this recording.")
    declaration_subjects = parser.add_mutually_exclusive_group()
    declaration_subjects.add_argument("--blank-subjects", action="store_true",
                                      help="Declare every camera blank (no subject information).")
    declaration_subjects.add_argument("--subjects-json", type=Path,
                                      help="File holding the subject_references object (app config form).")
    declaration_subjects.add_argument("--declaration-file", type=Path,
                                      help="File holding the whole orange.recording_start_declaration.")
    declaration_subjects.add_argument("--declaration-from-spec", type=Path,
                                      help="Experiment spec whose fixed.declaration declares the recording.")
    parser.add_argument("--declaration-files-root", type=Path, default=None,
                        help="An orchard (declaration v2, source.kind orchard_run_plan) --declaration-file: the "
                             "folder holding its run_plan/ copies (orchard tools/declare.py --out DIR). Default: "
                             "the declaration file's folder. Orange reads and seals them in the recording.")
    parser.add_argument("--empty-background", choices=("auto",) + EMPTY_BACKGROUND_MODES, default="auto",
                        help="capture_at_setup: once Orange streams (and Citrus is checked), capture an empty-dish "
                             "background, write <run-dir>/" + CAPTURE_RECORD_NAME + ", then wait for the "
                             "operator's confirmation on <run-dir>/" + SCENE_CONFIRMATION_SOCKET_NAME + " (scripts/confirm_bound_run_scene.py; "
                             "the runner writes " + SCENE_CONFIRMATION_NAME + " with the SO_PEERCRED peer) "
                             "before the start names the capture. auto: what an orchard plan session's "
                             "orange.empty_background says, else none.")
    parser.add_argument("--run-dir", type=Path, default=None,
                        help="Folder for the capture record and the operator's scene confirmation "
                             "(the launcher's out dir). Needed with an empty background.")
    parser.add_argument("--background-declared-by", default=None,
                        help="Who declares the dishes empty at the capture (default: the declaration's declared_by).")
    parser.add_argument("--background-stimulus-display", default=None,
                        help="Operator override of the capture's display (JSON, Orange's stimulus_display: state "
                             "off|on|not_displayed, shown...), recorded as source operator_override; Citrus is still "
                             "read before and after and may not contradict it. Default: declared from Citrus's "
                             "status as its idle clear rgba8 0,0,0,255, or the capture is refused.")
    parser.add_argument("--background-note", default="",
                        help="Free-text note for the capture's scene declaration.")
    parser.add_argument("--background-capture-timeout-seconds", type=positive_float, default=60.0,
                        help="Time for Orange's 17-frame bursts, median and files.")
    parser.add_argument("--scene-confirmation-timeout-seconds", type=positive_float, default=3600.0,
                        help="How long to wait for the operator's confirmation after the capture; then refuse.")
    parser.add_argument("--fish-free-test", action="store_true",
                        help="No fish are loaded: the runner confirms the scene itself (decision "
                             + SCENE_DECISION_FISH_FREE + ", recorded as such). Refused when the declaration "
                             "names any subject.")
    parser.add_argument("--citrus-subject-declaration-request", type=Path, default=None,
                        help="Bound runs: a citrus.subject_declaration_request v1 (an orchard plan's subjects) "
                             "sent as Citrus declare_subjects before Orange binds; START is refused unless "
                             "Citrus then shows every arena declared with the accepted sha. Needs Citrus "
                             "acceptance v4.")
    parser.add_argument("--citrus-subject-declaration-apply-timeout-seconds", type=positive_float, default=10.0,
                        help="How long Citrus's GUI thread gets to apply an accepted declaration.")
    parser.add_argument("--expected-citrus-arenas", type=int, default=None,
                        help="Selected arenas Citrus must report before a bound start (e.g. 4).")
    parser.add_argument("--citrus-receipt-timeout-seconds", type=positive_float, default=None,
                        help="Bound runs: how long to wait for Citrus's receipts. Default: Citrus's own "
                             "bound sized from the recording folder (330 + 120 + 3 s/100 MB + 60 s).")
    parser.add_argument("--citrus-receipt-idle-grace-seconds", type=positive_float, default=60.0,
                        help="Bound runs: fail this long after Orange's finalize if Citrus is still idle "
                             "with 0 attempts (it built no receipts).")
    parser.add_argument(
        "--orange-mode",
        choices=("gui", "headless"),
        default="gui",
        help=(
            "gui: the Orange GUI is started and stopped over its socket. headless: Citrus is "
            "launched first, --orange-command runs orange_client (fixed.local_control with "
            "stop_on_citrus_completion), and this orchestrator only sends Citrus start_experiment."
        ),
    )
    parser.add_argument(
        "--citrus-start-gate",
        choices=("wait_for_control", "ready_only"),
        default="wait_for_control",
        help=(
            "Headless only. wait_for_control: Citrus must report automation.autorun "
            "start_mode=wait_for_control and waiting_for_start (CITRUS_GUI_AUTORUN_START). "
            "ready_only: a Citrus whose protocol and arenas were set in its GUI."
        ),
    )
    parser.add_argument("--orange-ready-timeout-seconds", type=positive_float, default=300.0,
                        help="Headless: time for orange_client to open, bind and start recording.")
    parser.add_argument("--orange-exit-timeout-seconds", type=positive_float, default=1800.0,
                        help="Headless: time after Citrus's terminal state for orange_client to finalize, "
                             "receive Citrus's receipts and exit.")
    parser.add_argument("--citrus-exit-timeout-seconds", type=positive_float, default=180.0,
                        help="Headless: time for a launched Citrus to exit on its own after orange_client exits "
                             "(CITRUS_GUI_AUTORUN_EXIT_AFTER_COMPLETE) before it is terminated.")
    parser.add_argument("--camera-config-json", type=Path, default=None,
                        help="The launcher's camera preset record (scripts/camera_config_dir_digest.py "
                             "--write: name, path, hash_rule, files, sha256, expected_sha256), copied "
                             "into the summary.")
    parser.add_argument("--no-stop-orange-on-failure", dest="stop_orange_on_failure", action="store_false")
    parser.set_defaults(stop_orange_on_failure=True)
    parser.add_argument(
        "--stop-policy",
        choices=(
            "stop_recording",
            "citrus_completion",
            "citrus_completion_notify",
            "none",
        ),
        default="stop_recording",
        help=(
            "How Orange stops after Citrus terminal state. "
            "citrus_completion_notify waits for Citrus to send Orange "
            "citrus_completion itself."
        ),
    )
    args = parser.parse_args(argv)
    try:
        args.declaration = build_declaration(args)
    except ValueError as exc:
        parser.error(str(exc))
    args.camera_config = None
    if args.camera_config_json is not None:
        try:
            args.camera_config = json.loads(args.camera_config_json.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            parser.error(f"--camera-config-json: {exc}")
    if args.declaration is not None and args.orange_mode == "headless":
        parser.error("headless runs declare in the experiment spec (fixed.declaration), not on this command line")
    path = args.citrus_subject_declaration_request
    if path is not None:
        try:
            args.citrus_subject_declaration_request = load_citrus_subject_declaration_request(path)
        except ValueError as exc:
            parser.error(str(exc))
        if args.orange_mode == "gui" and (args.declaration or {}).get("recording_intent") != "stimulus_experiment":
            parser.error("--citrus-subject-declaration-request needs a declared stimulus_experiment (a bound run)")
    try:
        resolve_plan_and_background(args)
    except ValueError as exc:
        parser.error(str(exc))
    return args


def resolve_plan_and_background(args: argparse.Namespace) -> None:
    """Orchard declarations: the run_plan/ folder, the plan session (its preset
    pin and empty-background rule), and the Citrus gate. Then the empty
    background phase. Raises ValueError for anything that must refuse before
    a launch."""
    args.plan_session = None
    args.plan_preset_check = None
    declaration = args.declaration
    orchard = is_orchard_declaration(declaration)
    if args.declaration_files_root is not None and not orchard:
        raise ValueError("--declaration-files-root is only for an orchard_run_plan declaration")
    if orchard:
        root = args.declaration_files_root
        if root is None:
            root = args.declaration_file.parent if args.declaration_file is not None else None
        if root is None:
            raise ValueError("an orchard_run_plan declaration needs --declaration-files-root")
        root = root.resolve()
        problems = check_declaration_files(declaration, root)
        if problems:
            raise ValueError(f"declaration files in {root}: " + "; ".join(problems))
        args.declaration_files_root = root
        session = plan_session_from_files(declaration, root)
        args.plan_session = {
            "plan_id": declaration["source"].get("plan_id"),
            "plan_revision": declaration["source"].get("plan_revision"),
            "plan_session_id": declaration["source"].get("plan_session_id"),
            "plan_sha256": declaration["source"].get("plan_sha256"),
            "orange": session.get("orange"),
        }
        problems = plan_preset_problems(session, args.camera_config)
        args.plan_preset_check = {"preset": json_path(session, ["orange", "preset"], None), "problems": problems}
        if problems:
            raise ValueError("the camera preset is not the one the plan pins: " + "; ".join(problems))
        if args.orange_mode == "gui" and declaration.get("recording_intent") == "stimulus_experiment" \
                and args.citrus_subject_declaration_request is None:
            # Orange's pre-arm cross-check v2 compares Citrus's run_plan with
            # Orange's declaration source on every camera: without Citrus's
            # plan declaration (declare_subjects, acceptance v4) the arm is
            # refused after both programs launched. Refuse now instead.
            raise ValueError(
                "an orchard plan declaration in a bound run needs --citrus-subject-declaration-request "
                "(Citrus declare_subjects, acceptance v4): without it Orange's pre-arm cross-check refuses "
                "the arm (run_plan / blank_pairing differ on every camera)")
    mode = args.empty_background
    plan_mode = json_path(args.plan_session or {}, ["orange", "empty_background"], None)
    if plan_mode not in (None, "none") + EMPTY_BACKGROUND_MODES:
        raise ValueError(f"the plan's orange.empty_background {plan_mode!r} is not one this runner knows")
    if mode == "auto":
        mode = plan_mode or "none"
    elif plan_mode == "capture_at_setup" and mode != "capture_at_setup":
        raise ValueError("the plan says orange.empty_background capture_at_setup; --empty-background none "
                         "would record without it")
    args.empty_background = mode
    args.background_stimulus_display_json = None
    if mode != "capture_at_setup":
        if args.fish_free_test:
            raise ValueError("--fish-free-test only applies to an empty background capture")
        return
    if args.orange_mode != "gui" or declaration is None:
        raise ValueError("an empty background capture needs a GUI run with a declared start")
    if args.run_dir is None:
        raise ValueError("an empty background capture needs --run-dir (capture record and scene confirmation)")
    args.background_declared_by = args.background_declared_by or declaration.get("declared_by")
    if not IDENTITY_PATTERN.fullmatch(str(args.background_declared_by or "")):
        raise ValueError("--background-declared-by must match ^[a-z][a-z0-9_-]{0,31}$")
    if args.background_stimulus_display is not None:
        try:
            display = json.loads(args.background_stimulus_display)
        except json.JSONDecodeError as exc:
            raise ValueError(f"--background-stimulus-display is not JSON: {exc}") from exc
        # An override states what is shown; "unknown" is never declared.
        if not isinstance(display, dict) or display.get("state") not in {"off", "on", "not_displayed"}:
            raise ValueError("--background-stimulus-display needs state off, on or not_displayed")
        args.background_stimulus_display_json = display
    if args.fish_free_test and declaration_names_subjects(declaration):
        raise ValueError("--fish-free-test with a declaration that names subjects: the runner would confirm "
                         "'no fish loaded' for a recording declared with fish")


def empty_background_display_source(args: argparse.Namespace) -> str:
    return "operator_override" if args.background_stimulus_display_json is not None else "citrus_idle_clear"


def empty_background_capture_params(args: argparse.Namespace, citrus_build: Any = None) -> dict[str, Any]:
    note = args.background_note or (f"Dishes declared empty by {args.background_declared_by} when the bound run "
                                    f"{args.operation_id} was launched (before fish loading).")
    if args.background_stimulus_display_json is not None:
        display = args.background_stimulus_display_json
        note += " Display: operator override."
    else:
        display = copy.deepcopy(CITRUS_IDLE_ORANGE_STIMULUS_DISPLAY)
        commit = citrus_build.get("git_commit_hash") if isinstance(citrus_build, dict) else None
        note += (f" Display: Citrus idle clear rgba8 0,0,0,255 (projector black), read from Citrus status "
                 f"before and after the capture (Citrus {commit or 'build unknown'}); evidence in the runner's "
                 f"{CAPTURE_RECORD_NAME}.")
    return {"declared_by": args.background_declared_by, "dishes_empty": True,
            "note": note, "stimulus_display": display}


def plan_record(args: argparse.Namespace) -> dict[str, Any] | None:
    if args.plan_session is None:
        return None
    return {**args.plan_session, "declaration_files_root": str(args.declaration_files_root),
            "preset_check": args.plan_preset_check}


def empty_background_plan(args: argparse.Namespace) -> dict[str, Any]:
    if args.empty_background != "capture_at_setup":
        return {"mode": args.empty_background}
    return {
        "mode": "capture_at_setup",
        "stimulus_display_source": empty_background_display_source(args),
        "capture_request": build_orange_capture_empty_background_request(
            args.operation_id, f"{args.operation_id}:orange:capture_empty_background", args.source,
            empty_background_capture_params(args)),
        "capture_record_path": str(args.run_dir / CAPTURE_RECORD_NAME),
        "scene_confirmation_path": str(args.run_dir / SCENE_CONFIRMATION_NAME),
        "scene_confirmation_socket": str(args.run_dir / SCENE_CONFIRMATION_SOCKET_NAME),
        "scene_confirmation_timeout_seconds": args.scene_confirmation_timeout_seconds,
        "confirmation": ("fish_free_test: the runner confirms, decision " + SCENE_DECISION_FISH_FREE
                         if args.fish_free_test else
                         "operator: scripts/confirm_bound_run_scene.py --run-dir " + str(args.run_dir)
                         + " --confirmed-by <identity>"),
    }


def dry_run_summary(args: argparse.Namespace) -> dict[str, Any]:
    return {
        "schema_id": SUMMARY_SCHEMA_ID,
        "schema_version": SUMMARY_SCHEMA_VERSION,
        "generated_at_utc": utc_now(),
        "result": "dry_run",
        "operation_id": args.operation_id,
        "mode": "dry_run",
        "execute_required": True,
        "orange_mode": args.orange_mode,
        "citrus_start_gate": args.citrus_start_gate if args.orange_mode == "headless" else None,
        "camera_config": args.camera_config,
        "plan": plan_record(args),
        "empty_background": empty_background_plan(args),
        "orange": {
            "socket": args.orange_socket,
            "log_path": args.orange_log,
            "local_control_event_log_path": resolve_orange_local_control_log_path(args),
            "command": shlex.split(args.orange_command) if args.orange_command else [],
            "env_overlay": orange_process_env_overlay(args),
            "start_request": build_orange_start_request(
                args.operation_id,
                f"{args.operation_id}:orange:start_recording",
                args.source,
                args.declaration,
                declaration_files_root=str(args.declaration_files_root) if args.declaration_files_root else None,
                empty_background=(
                    {"capture_id": "<from the capture>", "descriptor_sha256": "<from the capture>",
                     "scene_unchanged_confirmed": "<from the operator's confirmation>"}
                    if args.empty_background == "capture_at_setup" else None),
            ),
            "stop_policy": args.stop_policy,
            "stop_grace_seconds": effective_orange_stop_grace_seconds(args),
            "allow_drain_timeout": args.allow_orange_drain_timeout,
            "require_local_control_event_log": args.require_orange_local_control_event_log,
            "preflight_existing_socket": bool(args.orange_command)
            and not args.allow_preexisting_orange_socket,
        },
        "citrus": {
            "socket": args.citrus_socket,
            "log_path": args.citrus_log,
            "command": shlex.split(args.citrus_command) if args.citrus_command else [],
            "run_seconds": args.citrus_run_seconds,
            "env_overlay": {
                **parse_env_items(args.citrus_env),
                "CITRUS_GUI_LOCAL_CONTROL_SOCKET": args.citrus_socket,
                **({"CITRUS_PERF_JSONL": "1"} if args.require_citrus_perf_jsonl else {}),
            },
            "daily_registration_mode": args.daily_registration_mode,
            "daily_registration_mode_request": (
                build_citrus_daily_registration_mode_request(
                    args.operation_id,
                    f"{args.operation_id}:citrus:select_daily_registration_runtime_mode",
                    args.source,
                    args.daily_registration_mode,
                )
                if args.daily_registration_mode != "preserve"
                else None
            ),
            "declare_subjects_request": build_citrus_declare_subjects_request(
                args.operation_id,
                f"{args.operation_id}:citrus:declare_subjects",
                args.source,
                args.citrus_subject_declaration_request,
            ) if args.citrus_subject_declaration_request is not None else None,
            "declare_subjects_mismatches": citrus_declaration_plan_mismatches(
                args.citrus_subject_declaration_request, args.declaration,
            ) if args.citrus_subject_declaration_request is not None else [],
            "start_request": build_citrus_start_request(
                args.operation_id,
                f"{args.operation_id}:citrus:start_experiment",
                args.source,
            ),
            "stop_request": build_citrus_stop_request(
                args.operation_id,
                f"{args.operation_id}:citrus:stop_experiment:run_duration",
                args.source,
            ) if args.citrus_run_seconds > 0.0 else None,
            "require_perf_jsonl": args.require_citrus_perf_jsonl,
            "preflight_existing_socket": bool(args.citrus_command)
            and not args.allow_preexisting_citrus_socket,
        },
        "validations": [
            {
                **item,
                "cwd": args.validation_cwd,
                "timeout_seconds": args.validation_timeout_seconds,
                "artifact_paths": validation_artifacts_from_args(args).get(item["label"], []),
            }
            for item in validation_commands_from_args(args)
        ],
    }


def write_summary(path: str, summary: dict[str, Any]) -> None:
    if not path:
        return
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if not args.execute:
        try:
            summary = dry_run_summary(args)
            write_summary(args.summary_json, summary)
            print(json.dumps(summary, indent=2, sort_keys=True))
            return 0
        except Exception as exc:
            print(f"orchestrator dry-run failed: {exc}", file=sys.stderr)
            return 2

    orchestrator = Orchestrator(args)
    try:
        summary = orchestrator.run()
        orchestrator.persist_artifacts(summary)
        write_summary(args.summary_json, summary)
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0 if summary.get("result") in {"pass", "dry_run"} else 1
    except Exception as exc:
        if args.orange_mode == "headless":
            # Before the start: end both (orange_client finalizes on SIGTERM; give
            # it time, a SIGKILL leaves the cameras refusing to open). After the
            # start: leave both running so Citrus completes and orange_client
            # stops, finalizes and waits for the receipts on its own.
            failure_stop_response = None
            error = str(exc)
            if not orchestrator.citrus_start_sent:
                orchestrator.cleanup_started_processes(
                    labels={"orange"},
                    terminate_timeout_seconds=args.orange_ready_timeout_seconds,
                )
                orchestrator.cleanup_started_processes(labels={"citrus"})
                if orchestrator.processes.get("orange") is not None:
                    still_running = orchestrator.orange_client_still_running()
                    if still_running:
                        error = f"{error}; {still_running}"
        else:
            error = str(exc)
            if (
                orchestrator.bound_run()
                and orchestrator.citrus_start_sent
                and not getattr(orchestrator, "bound_receipts_settled", False)
            ):
                # Citrus ran: never stop Orange or Citrus before the receipts
                # settle; the failure is reported after.
                try:
                    orchestrator.settle_bound_receipts(orchestrator.last_orange_status or {})
                except Exception as settle_exc:  # the run already failed; record why
                    error = f"{error}; receipts: {settle_exc}"
            failure_stop_response = orchestrator.best_effort_stop_after_failure()
            cleanup_labels = {"citrus"}
            if (
                not orchestrator.orange_recording_started
                or orange_recording_finalized(orchestrator.last_orange_status)
            ):
                cleanup_labels.add("orange")
            orchestrator.cleanup_started_processes(labels=cleanup_labels)
        orchestrator.cleanup_launched_socket_files()
        summary = orchestrator.summary(
            "fail",
            error=error,
            failure_stop_response=failure_stop_response,
        )
        orchestrator.persist_artifacts(summary)
        write_summary(args.summary_json, summary)
        print(json.dumps(summary, indent=2, sort_keys=True), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
