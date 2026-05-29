#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'EOF'
Usage:
  scripts/run_orange_citrus_fourcam_orchestrator.sh [options]

Runs the local four-camera Orange/Citrus orchestration profile.

Default mode is a dry-run: it prints the Orange/Citrus commands, env overlays,
socket paths, and request shapes. Add --execute to launch/control processes.

Defaults:
  - Orange command:
      scripts/run_gui_fourcam_external_ipc_validation.sh --citrus-display-safe
  - Citrus command:
      /home/jeremy/citrus/targets/citrus
  - Orange socket: /tmp/orange_local_control.sock
  - Citrus socket: /tmp/citrus_local_control.sock
  - Display defaults for tmux/ssh sessions:
      DISPLAY=:1
      XAUTHORITY=/run/user/1000/gdm/Xauthority when present
      XDG_RUNTIME_DIR=/run/user/1000 when present
      XDG_SESSION_TYPE=x11

Options:
  --execute                      Run instead of printing a dry-run plan.
  --operation-id <id>            Idempotent operation id.
  --summary-json <path>          Combined orchestrator summary path.
  --orange-command <command>     Override Orange launch command.
  --citrus-command <command>     Override Citrus launch command.
  --record-seconds <seconds>     Orange recording_control record_for_seconds
                                  for manifests/rolling contracts.
  --warmup-seconds <seconds>     Orange stream warmup before recording.
  --clip-seconds <seconds>       Enable Orange rolling clips with this duration.
  --attach-orange                Do not launch Orange; attach to its socket.
  --attach-citrus                Do not launch Citrus; attach to its socket.
  --allow-preexisting-sockets    Permit launch mode when default sockets already answer.
  --orange-socket <path>         Override Orange local-control socket.
  --citrus-socket <path>         Override Citrus local-control socket.
  --orange-log <path>            Orange process stdout/stderr log.
                                  Default: /tmp/<operation_id>_orange.log
  --citrus-log <path>            Citrus process stdout/stderr log.
                                  Default: /tmp/<operation_id>_citrus.log
  --display <value>              DISPLAY to pass to launched GUIs.
  --xauthority <path>            XAUTHORITY to pass to launched GUIs.
  --xdg-runtime-dir <path>       XDG_RUNTIME_DIR to pass to launched GUIs.
  --xdg-session-type <value>     XDG_SESSION_TYPE to pass to launched GUIs.
  --wayland-display <value>      WAYLAND_DISPLAY to pass when needed.
  --orange-env KEY=VALUE         Extra Orange env override; repeatable.
  --citrus-env KEY=VALUE         Extra Citrus env override; repeatable.
  --citrus-rig <id>              Citrus autorun loader rig id.
  --citrus-canvas <name>         Citrus autorun loader canvas name.
  --citrus-protocol <name>       Citrus autorun loader protocol name/path.
  --citrus-protocol-path <path>  Citrus autorun loader absolute protocol path.
  --citrus-autorun-start-delay-seconds <seconds>
                                  Long delay used so local control owns start.
  --citrus-autorun-run-seconds <seconds>
                                  Optional Citrus autorun stop duration.
  --no-citrus-autorun-loader     Do not set Citrus autorun loader envs.
  --enable-citrus-orange-completion-notify
                                  Let Citrus also notify Orange on terminal state.
  --skip-orange-validation        Do not run the default Orange artifact validator.
  --orange-validation-command <command>
                                  Override the default Orange validator command.
  --orange-validation-json <path> Orange validator JSON output path.
                                  Copied into <recording_folder>/orchestrator/.
  --validation-timeout-seconds <seconds>
  --orange-stop-grace-seconds <seconds>
  --stop-policy <policy>         stop_recording, citrus_completion, or none.
  --timeout-seconds <seconds>    Orange/Citrus readiness timeout.
  --citrus-terminal-timeout-seconds <seconds>
  --orange-finalize-timeout-seconds <seconds>
  --allow-missing-citrus-perf-jsonl
                                  Do not require Citrus perf JSONL status/path.
  --help
EOF
}

default_xauthority() {
  if [[ -n "${XAUTHORITY:-}" ]]; then
    printf '%s\n' "${XAUTHORITY}"
  elif [[ -f /run/user/1000/gdm/Xauthority ]]; then
    printf '%s\n' /run/user/1000/gdm/Xauthority
  fi
  return 0
}

default_xdg_runtime_dir() {
  if [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
    printf '%s\n' "${XDG_RUNTIME_DIR}"
  elif [[ -d /run/user/1000 ]]; then
    printf '%s\n' /run/user/1000
  fi
  return 0
}

default_xdg_session_type() {
  case "${XDG_SESSION_TYPE:-}" in
    x11|wayland)
      printf '%s\n' "${XDG_SESSION_TYPE}"
      ;;
    *)
      printf '%s\n' x11
      ;;
  esac
}

join_command() {
  local quoted=()
  local arg
  for arg in "$@"; do
    case "${arg}" in
      "{operation_id}"|"{orange_recording_folder}"|"{citrus_perf_jsonl_path}")
        quoted+=("${arg}")
        ;;
      *)
        quoted+=("$(printf '%q' "${arg}")")
        ;;
    esac
  done
  local IFS=" "
  printf '%s\n' "${quoted[*]}"
}

require_value() {
  local option="$1"
  local value_count="$2"
  if (( value_count == 0 )); then
    echo "${option} requires a value" >&2
    exit 2
  fi
}

is_positive_integer() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

is_nonnegative_integer() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

EXECUTE=0
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OPERATION_ID="${ORANGE_CITRUS_OPERATION_ID:-orange_citrus_fourcam_${STAMP}}"
SUMMARY_JSON="${ORANGE_CITRUS_SUMMARY_JSON:-}"
ORANGE_SOCKET="${ORANGE_GUI_LOCAL_CONTROL_SOCKET:-${ORANGE_LOCAL_CONTROL_SOCKET:-/tmp/orange_local_control.sock}}"
CITRUS_SOCKET="${CITRUS_GUI_LOCAL_CONTROL_SOCKET:-${CITRUS_LOCAL_CONTROL_SOCKET:-/tmp/citrus_local_control.sock}}"
ORANGE_COMMAND="${ORANGE_CITRUS_ORANGE_COMMAND:-}"
ORANGE_COMMAND_MODE="default"
if [[ -n "${ORANGE_COMMAND}" ]]; then
  ORANGE_COMMAND_MODE="override"
fi
CITRUS_COMMAND="${ORANGE_CITRUS_CITRUS_COMMAND:-/home/jeremy/citrus/targets/citrus}"
ORANGE_LOG="${ORANGE_CITRUS_ORANGE_LOG:-}"
CITRUS_LOG="${ORANGE_CITRUS_CITRUS_LOG:-}"
DISPLAY_VALUE="${ORANGE_CITRUS_DISPLAY:-${DISPLAY:-:1}}"
XAUTHORITY_VALUE="${ORANGE_CITRUS_XAUTHORITY:-$(default_xauthority)}"
XDG_RUNTIME_DIR_VALUE="${ORANGE_CITRUS_XDG_RUNTIME_DIR:-$(default_xdg_runtime_dir)}"
XDG_SESSION_TYPE_VALUE="${ORANGE_CITRUS_XDG_SESSION_TYPE:-$(default_xdg_session_type)}"
WAYLAND_DISPLAY_VALUE="${ORANGE_CITRUS_WAYLAND_DISPLAY:-${WAYLAND_DISPLAY:-}}"
CITRUS_RIG="${ORANGE_CITRUS_CITRUS_RIG:-omnifin0}"
CITRUS_CANVAS="${ORANGE_CITRUS_CITRUS_CANVAS:-shadow}"
CITRUS_PROTOCOL="${ORANGE_CITRUS_CITRUS_PROTOCOL:-good_cop_bad_cop_demo.json}"
CITRUS_PROTOCOL_PATH="${ORANGE_CITRUS_CITRUS_PROTOCOL_PATH:-}"
CITRUS_AUTORUN_START_DELAY_SECONDS="${ORANGE_CITRUS_CITRUS_AUTORUN_START_DELAY_SECONDS:-86400}"
CITRUS_AUTORUN_RUN_SECONDS="${ORANGE_CITRUS_CITRUS_AUTORUN_RUN_SECONDS:-}"
CITRUS_AUTORUN_LOADER=1
CITRUS_ORANGE_COMPLETION_NOTIFY=0
ALLOW_PREEXISTING_SOCKETS=0
REQUIRE_CITRUS_PERF_JSONL=1
ORANGE_VALIDATION_ENABLED="${ORANGE_CITRUS_ORANGE_VALIDATION_ENABLED:-1}"
ORANGE_VALIDATION_COMMAND="${ORANGE_CITRUS_ORANGE_VALIDATION_COMMAND:-}"
ORANGE_VALIDATION_JSON="${ORANGE_CITRUS_ORANGE_VALIDATION_JSON:-}"
STOP_POLICY="${ORANGE_CITRUS_STOP_POLICY:-stop_recording}"
TIMEOUT_SECONDS="${ORANGE_CITRUS_TIMEOUT_SECONDS:-180}"
CITRUS_TERMINAL_TIMEOUT_SECONDS="${ORANGE_CITRUS_TERMINAL_TIMEOUT_SECONDS:-600}"
ORANGE_FINALIZE_TIMEOUT_SECONDS="${ORANGE_CITRUS_ORANGE_FINALIZE_TIMEOUT_SECONDS:-240}"
ORANGE_STOP_GRACE_SECONDS="${ORANGE_CITRUS_ORANGE_STOP_GRACE_SECONDS:-0}"
VALIDATION_TIMEOUT_SECONDS="${ORANGE_CITRUS_VALIDATION_TIMEOUT_SECONDS:-300}"
ORANGE_RECORD_SECONDS="${ORANGE_CITRUS_ORANGE_RECORD_SECONDS:-}"
ORANGE_WARMUP_SECONDS="${ORANGE_CITRUS_ORANGE_WARMUP_SECONDS:-}"
ORANGE_CLIP_SECONDS="${ORANGE_CITRUS_ORANGE_CLIP_SECONDS:-}"
ORANGE_PROFILE_ENV=()
ORANGE_EXTRA_ENV=()
CITRUS_EXTRA_ENV=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --execute)
      EXECUTE=1
      shift
      ;;
    --operation-id)
      shift
      require_value "--operation-id" "$#"
      OPERATION_ID="$1"
      shift
      ;;
    --summary-json)
      shift
      require_value "--summary-json" "$#"
      SUMMARY_JSON="$1"
      shift
      ;;
    --orange-command)
      shift
      require_value "--orange-command" "$#"
      ORANGE_COMMAND="$1"
      ORANGE_COMMAND_MODE="override"
      shift
      ;;
    --citrus-command)
      shift
      require_value "--citrus-command" "$#"
      CITRUS_COMMAND="$1"
      shift
      ;;
    --attach-orange)
      ORANGE_COMMAND=""
      ORANGE_COMMAND_MODE="attach"
      shift
      ;;
    --record-seconds)
      shift
      require_value "--record-seconds" "$#"
      is_positive_integer "$1" || { echo "--record-seconds must be a positive integer" >&2; exit 2; }
      ORANGE_RECORD_SECONDS="$1"
      shift
      ;;
    --warmup-seconds)
      shift
      require_value "--warmup-seconds" "$#"
      is_nonnegative_integer "$1" || { echo "--warmup-seconds must be a non-negative integer" >&2; exit 2; }
      ORANGE_WARMUP_SECONDS="$1"
      shift
      ;;
    --clip-seconds)
      shift
      require_value "--clip-seconds" "$#"
      is_positive_integer "$1" || { echo "--clip-seconds must be a positive integer" >&2; exit 2; }
      ORANGE_CLIP_SECONDS="$1"
      shift
      ;;
    --attach-citrus)
      CITRUS_COMMAND=""
      CITRUS_AUTORUN_LOADER=0
      shift
      ;;
    --allow-preexisting-sockets)
      ALLOW_PREEXISTING_SOCKETS=1
      shift
      ;;
    --orange-socket)
      shift
      require_value "--orange-socket" "$#"
      ORANGE_SOCKET="$1"
      shift
      ;;
    --citrus-socket)
      shift
      require_value "--citrus-socket" "$#"
      CITRUS_SOCKET="$1"
      shift
      ;;
    --orange-log)
      shift
      require_value "--orange-log" "$#"
      ORANGE_LOG="$1"
      shift
      ;;
    --citrus-log)
      shift
      require_value "--citrus-log" "$#"
      CITRUS_LOG="$1"
      shift
      ;;
    --display)
      shift
      require_value "--display" "$#"
      DISPLAY_VALUE="$1"
      shift
      ;;
    --xauthority)
      shift
      require_value "--xauthority" "$#"
      XAUTHORITY_VALUE="$1"
      shift
      ;;
    --xdg-runtime-dir)
      shift
      require_value "--xdg-runtime-dir" "$#"
      XDG_RUNTIME_DIR_VALUE="$1"
      shift
      ;;
    --xdg-session-type)
      shift
      require_value "--xdg-session-type" "$#"
      XDG_SESSION_TYPE_VALUE="$1"
      shift
      ;;
    --wayland-display)
      shift
      require_value "--wayland-display" "$#"
      WAYLAND_DISPLAY_VALUE="$1"
      shift
      ;;
    --orange-env)
      shift
      require_value "--orange-env" "$#"
      ORANGE_EXTRA_ENV+=("$1")
      shift
      ;;
    --citrus-env)
      shift
      require_value "--citrus-env" "$#"
      CITRUS_EXTRA_ENV+=("$1")
      shift
      ;;
    --citrus-rig)
      shift
      require_value "--citrus-rig" "$#"
      CITRUS_RIG="$1"
      shift
      ;;
    --citrus-canvas)
      shift
      require_value "--citrus-canvas" "$#"
      CITRUS_CANVAS="$1"
      shift
      ;;
    --citrus-protocol)
      shift
      require_value "--citrus-protocol" "$#"
      CITRUS_PROTOCOL="$1"
      shift
      ;;
    --citrus-protocol-path)
      shift
      require_value "--citrus-protocol-path" "$#"
      CITRUS_PROTOCOL_PATH="$1"
      shift
      ;;
    --citrus-autorun-start-delay-seconds)
      shift
      require_value "--citrus-autorun-start-delay-seconds" "$#"
      CITRUS_AUTORUN_START_DELAY_SECONDS="$1"
      shift
      ;;
    --citrus-autorun-run-seconds)
      shift
      require_value "--citrus-autorun-run-seconds" "$#"
      is_positive_integer "$1" || { echo "--citrus-autorun-run-seconds must be a positive integer" >&2; exit 2; }
      CITRUS_AUTORUN_RUN_SECONDS="$1"
      shift
      ;;
    --no-citrus-autorun-loader)
      CITRUS_AUTORUN_LOADER=0
      shift
      ;;
    --enable-citrus-orange-completion-notify)
      CITRUS_ORANGE_COMPLETION_NOTIFY=1
      shift
      ;;
    --skip-orange-validation)
      ORANGE_VALIDATION_ENABLED=0
      shift
      ;;
    --orange-validation-command)
      shift
      require_value "--orange-validation-command" "$#"
      ORANGE_VALIDATION_COMMAND="$1"
      ORANGE_VALIDATION_ENABLED=1
      shift
      ;;
    --orange-validation-json)
      shift
      require_value "--orange-validation-json" "$#"
      ORANGE_VALIDATION_JSON="$1"
      shift
      ;;
    --validation-timeout-seconds)
      shift
      require_value "--validation-timeout-seconds" "$#"
      VALIDATION_TIMEOUT_SECONDS="$1"
      shift
      ;;
    --orange-stop-grace-seconds)
      shift
      require_value "--orange-stop-grace-seconds" "$#"
      ORANGE_STOP_GRACE_SECONDS="$1"
      shift
      ;;
    --stop-policy)
      shift
      require_value "--stop-policy" "$#"
      STOP_POLICY="$1"
      shift
      ;;
    --timeout-seconds)
      shift
      require_value "--timeout-seconds" "$#"
      TIMEOUT_SECONDS="$1"
      shift
      ;;
    --citrus-terminal-timeout-seconds)
      shift
      require_value "--citrus-terminal-timeout-seconds" "$#"
      CITRUS_TERMINAL_TIMEOUT_SECONDS="$1"
      shift
      ;;
    --orange-finalize-timeout-seconds)
      shift
      require_value "--orange-finalize-timeout-seconds" "$#"
      ORANGE_FINALIZE_TIMEOUT_SECONDS="$1"
      shift
      ;;
    --allow-missing-citrus-perf-jsonl)
      REQUIRE_CITRUS_PERF_JSONL=0
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unsupported argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -n "${ORANGE_RECORD_SECONDS}" ]]; then
  is_positive_integer "${ORANGE_RECORD_SECONDS}" || {
    echo "ORANGE_CITRUS_ORANGE_RECORD_SECONDS must be a positive integer" >&2
    exit 2
  }
fi
if [[ -n "${ORANGE_WARMUP_SECONDS}" ]]; then
  is_nonnegative_integer "${ORANGE_WARMUP_SECONDS}" || {
    echo "ORANGE_CITRUS_ORANGE_WARMUP_SECONDS must be a non-negative integer" >&2
    exit 2
  }
fi
if [[ -n "${ORANGE_CLIP_SECONDS}" ]]; then
  is_positive_integer "${ORANGE_CLIP_SECONDS}" || {
    echo "ORANGE_CITRUS_ORANGE_CLIP_SECONDS must be a positive integer" >&2
    exit 2
  }
fi
if [[ -n "${CITRUS_AUTORUN_RUN_SECONDS}" ]]; then
  is_positive_integer "${CITRUS_AUTORUN_RUN_SECONDS}" || {
    echo "ORANGE_CITRUS_CITRUS_AUTORUN_RUN_SECONDS must be a positive integer" >&2
    exit 2
  }
fi
if [[ -n "${ORANGE_RECORD_SECONDS}" ]]; then
  ORANGE_PROFILE_ENV+=("ORANGE_GUI_RECORD_FOR_SECONDS=${ORANGE_RECORD_SECONDS}")
fi

if [[ "${ORANGE_COMMAND_MODE}" == "default" ]]; then
  ORANGE_COMMAND_ARGS=(
    "${REPO_ROOT}/scripts/run_gui_fourcam_external_ipc_validation.sh"
    "--citrus-display-safe"
  )
  if [[ -n "${ORANGE_RECORD_SECONDS}" ]]; then
    ORANGE_COMMAND_ARGS+=("--record-seconds" "${ORANGE_RECORD_SECONDS}")
  fi
  if [[ -n "${ORANGE_WARMUP_SECONDS}" ]]; then
    ORANGE_COMMAND_ARGS+=("--warmup-seconds" "${ORANGE_WARMUP_SECONDS}")
  fi
  if [[ -n "${ORANGE_CLIP_SECONDS}" ]]; then
    ORANGE_COMMAND_ARGS+=("--clip-seconds" "${ORANGE_CLIP_SECONDS}")
  fi
  ORANGE_COMMAND="$(join_command "${ORANGE_COMMAND_ARGS[@]}")"
fi

if [[ -z "${SUMMARY_JSON}" ]]; then
  SUMMARY_JSON="/tmp/${OPERATION_ID}_orchestrator_summary.json"
fi
LOG_STEM="${OPERATION_ID//[^A-Za-z0-9_.-]/_}"
if [[ -z "${ORANGE_LOG}" ]]; then
  ORANGE_LOG="/tmp/${LOG_STEM}_orange.log"
fi
if [[ -z "${CITRUS_LOG}" ]]; then
  CITRUS_LOG="/tmp/${LOG_STEM}_citrus.log"
fi
if [[ -z "${ORANGE_VALIDATION_JSON}" ]]; then
  ORANGE_VALIDATION_JSON="/tmp/${OPERATION_ID}_orange_gui_validation.json"
fi
ORANGE_VALIDATION_MODE_ARGS=()
if [[ -n "${ORANGE_CLIP_SECONDS}" ]]; then
  ORANGE_VALIDATION_MODE_ARGS+=("--expect-recording-mode" "rolling_clips")
fi
if [[ -n "${ORANGE_RECORD_SECONDS}" ]]; then
  ORANGE_VALIDATION_MODE_ARGS+=("--expect-record-for-seconds" "${ORANGE_RECORD_SECONDS}")
fi
if [[ -n "${ORANGE_CLIP_SECONDS}" ]]; then
  ORANGE_VALIDATION_MODE_ARGS+=("--expect-clip-seconds" "${ORANGE_CLIP_SECONDS}")
fi
if [[ "${ORANGE_VALIDATION_ENABLED}" == "1" && -z "${ORANGE_VALIDATION_COMMAND}" ]]; then
  ORANGE_VALIDATION_COMMAND="$(join_command \
    "${REPO_ROOT}/scripts/validate_gui_ptp_recording.py" \
    "{orange_recording_folder}" \
    "--expected-cameras" "2010093,2010094,2010095,2010096" \
    "${ORANGE_VALIDATION_MODE_ARGS[@]}" \
    "--require-crop-recording-artifacts" \
    "--require-crop-preview-counters" \
    "--expect-crop-preview-max-fps" "15" \
    "--expect-crop-preview-disabled" "0" \
    "--expect-crop-preview-display-enabled" "0" \
    "--min-crop-frame-pool-size" "256" \
    "--expect-external-crop-encode-queue-depth" "128" \
    "--require-external-crop-backend-metadata" \
    "--require-external-crop-recorder-gpu-separate-from-analytics" \
    "--expect-external-crop-recorder-gpu" "2010093=4" \
    "--expect-external-crop-recorder-gpu" "2010094=2" \
    "--expect-external-crop-recorder-gpu" "2010095=8" \
    "--expect-external-crop-recorder-gpu" "2010096=6" \
    "--require-external-recorder-status" \
    "--require-external-recorder-storage-preflight" \
    "--require-external-recorder-protocol-hello" \
    "--require-source-version" \
    "--expect-source-git-command-user-mode" "sudo_invoking_user" \
    "--expect-source-dirty-tracked" "0" \
    "--expect-yolo-affinity" "2010093=6" \
    "--expect-yolo-affinity" "2010094=8" \
    "--expect-yolo-affinity" "2010095=10" \
    "--expect-yolo-affinity" "2010096=12" \
    "--require-isolated-cpus" "6,8,10,12,38,40,42,44" \
    "--require-kernel-cmdline-cpus" "isolcpus=6,8,10,12,38,40,42,44" \
    "--require-kernel-cmdline-cpus" "nohz_full=6,8,10,12,38,40,42,44" \
    "--require-kernel-cmdline-cpus" "rcu_nocbs=6,8,10,12,38,40,42,44" \
    "--expect-gui-stream-downsample" "4" \
    "--expect-display-preview-max-fps" "10" \
    "--expect-gui-swap-interval" "1" \
    "--expect-gui-frame-max-fps" "30" \
    "--expect-yolo-speed-graphs-enabled" "0" \
    "--require-gui-timing-telemetry" \
    "--require-imgui-glfw-size-cache" \
    "--json-out" "${ORANGE_VALIDATION_JSON}")"
fi

ARGS=(
  "${REPO_ROOT}/scripts/orange_citrus_orchestrator.py"
  "--operation-id" "${OPERATION_ID}"
  "--source" "orange_citrus_fourcam_profile"
  "--orange-socket" "${ORANGE_SOCKET}"
  "--citrus-socket" "${CITRUS_SOCKET}"
  "--orange-log" "${ORANGE_LOG}"
  "--citrus-log" "${CITRUS_LOG}"
  "--summary-json" "${SUMMARY_JSON}"
  "--stop-policy" "${STOP_POLICY}"
  "--timeout-seconds" "${TIMEOUT_SECONDS}"
  "--citrus-terminal-timeout-seconds" "${CITRUS_TERMINAL_TIMEOUT_SECONDS}"
  "--orange-finalize-timeout-seconds" "${ORANGE_FINALIZE_TIMEOUT_SECONDS}"
  "--orange-stop-grace-seconds" "${ORANGE_STOP_GRACE_SECONDS}"
  "--validation-timeout-seconds" "${VALIDATION_TIMEOUT_SECONDS}"
)

if (( EXECUTE )); then
  ARGS+=("--execute")
fi

if [[ -n "${ORANGE_COMMAND}" ]]; then
  ARGS+=("--orange-command" "${ORANGE_COMMAND}")
fi
if [[ -n "${CITRUS_COMMAND}" ]]; then
  ARGS+=("--citrus-command" "${CITRUS_COMMAND}")
fi
if (( REQUIRE_CITRUS_PERF_JSONL )); then
  ARGS+=("--require-citrus-perf-jsonl")
fi
if (( ALLOW_PREEXISTING_SOCKETS )); then
  ARGS+=("--allow-preexisting-orange-socket" "--allow-preexisting-citrus-socket")
fi
if [[ "${ORANGE_VALIDATION_ENABLED}" == "1" ]]; then
  ARGS+=("--orange-validation-command" "${ORANGE_VALIDATION_COMMAND}")
  ARGS+=("--validation-artifact" "orange_validation_1=${ORANGE_VALIDATION_JSON}")
fi

DISPLAY_ENV_ITEMS=()
if [[ -n "${DISPLAY_VALUE}" ]]; then
  DISPLAY_ENV_ITEMS+=("DISPLAY=${DISPLAY_VALUE}")
fi
if [[ -n "${XAUTHORITY_VALUE}" ]]; then
  DISPLAY_ENV_ITEMS+=("XAUTHORITY=${XAUTHORITY_VALUE}")
fi
if [[ -n "${XDG_RUNTIME_DIR_VALUE}" ]]; then
  DISPLAY_ENV_ITEMS+=("XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR_VALUE}")
fi
if [[ -n "${XDG_SESSION_TYPE_VALUE}" ]]; then
  DISPLAY_ENV_ITEMS+=("XDG_SESSION_TYPE=${XDG_SESSION_TYPE_VALUE}")
fi
if [[ -n "${WAYLAND_DISPLAY_VALUE}" ]]; then
  DISPLAY_ENV_ITEMS+=("WAYLAND_DISPLAY=${WAYLAND_DISPLAY_VALUE}")
fi

for item in "${DISPLAY_ENV_ITEMS[@]}"; do
  ARGS+=("--orange-env" "${item}" "--citrus-env" "${item}")
done
for item in "${ORANGE_PROFILE_ENV[@]}"; do
  ARGS+=("--orange-env" "${item}")
done
for item in "${ORANGE_EXTRA_ENV[@]}"; do
  ARGS+=("--orange-env" "${item}")
done

if (( CITRUS_AUTORUN_LOADER )) && [[ -n "${CITRUS_COMMAND}" ]]; then
  CITRUS_EXTRA_ENV+=(
    "CITRUS_GUI_AUTORUN=1"
    "CITRUS_GUI_AUTORUN_RIG=${CITRUS_RIG}"
    "CITRUS_GUI_AUTORUN_CANVAS=${CITRUS_CANVAS}"
    "CITRUS_GUI_AUTORUN_PROTOCOL=${CITRUS_PROTOCOL}"
    "CITRUS_GUI_AUTORUN_START_DELAY_SECONDS=${CITRUS_AUTORUN_START_DELAY_SECONDS}"
    "CITRUS_GUI_AUTORUN_EXIT_AFTER_COMPLETE=0"
    "CITRUS_ORANGE_COMPLETION_NOTIFY=${CITRUS_ORANGE_COMPLETION_NOTIFY}"
    "CITRUS_ORANGE_LOCAL_CONTROL_SOCKET=${ORANGE_SOCKET}"
    "CITRUS_THREADING_SUMMARY_LOG=1"
  )
  if [[ -n "${CITRUS_PROTOCOL_PATH}" ]]; then
    CITRUS_EXTRA_ENV+=("CITRUS_GUI_AUTORUN_PROTOCOL_PATH=${CITRUS_PROTOCOL_PATH}")
  fi
  if [[ -n "${CITRUS_AUTORUN_RUN_SECONDS}" ]]; then
    CITRUS_EXTRA_ENV+=("CITRUS_GUI_AUTORUN_RUN_SECONDS=${CITRUS_AUTORUN_RUN_SECONDS}")
  fi
fi
for item in "${CITRUS_EXTRA_ENV[@]}"; do
  ARGS+=("--citrus-env" "${item}")
done

exec "${ARGS[@]}"
