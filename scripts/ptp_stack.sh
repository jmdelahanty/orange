#!/usr/bin/env bash
set -euo pipefail

# ptp4l and phc2sys run only as the systemd units pancakebatter's
# install_ptp_units.sh installs (interfaces, CPU affinity, and the ExecStartPost
# setter that declares the TAI-UTC offset valid). A ptp4l started by hand never
# runs that setter, so cameras classify ieee1588_ptp_timescale_untraceable.
PTP_UNITS=(ptp4l.service phc2sys.service)
PTP_SOCKET="${PTP_SOCKET:-/var/run/ptp4l}"
PTP_LOG_DIR="${PTP_LOG_DIR:-/tmp/ptp-stack}"

PTP_MANAGEMENT_EVIDENCE="${PTP_LOG_DIR}/management_evidence.txt"

usage() {
  cat <<EOF
Usage: ptp_stack.sh <command>

Commands:
  start      Start ptp4l.service and phc2sys.service
  stop       Stop both units (and any ptp4l/phc2sys started outside them)
  status     Show process state, TIME_STATUS_NP and who started the daemons
  restart    Stop then start; replaces daemons started outside the units
  logs       Follow both units' journal

Stopping or restarting ptp4l drops the cameras' PTP lock: do it only with no
orange or orange_client running.

The units come from pancakebatter: sudo ~/pancakebatter/install_ptp_units.sh

Optional environment variables:
  PTP_SOCKET   Default: /var/run/ptp4l
  PTP_LOG_DIR  Default: /tmp/ptp-stack (management evidence)
EOF
}

need_sudo() {
  sudo -v
}

is_running() {
  local name="$1"
  pgrep -x "${name}" >/dev/null 2>&1
}

unit_active() {
  systemctl is-active --quiet "$1"
}

# Prints "<name> pid <pid>" for each ptp4l/phc2sys process that is not its
# unit's main process.
outside_unit_daemons() {
  local name main_pid pid
  for name in ptp4l phc2sys; do
    main_pid="$(systemctl show -p MainPID --value "${name}.service" 2>/dev/null || echo 0)"
    for pid in $(pgrep -x "${name}" || true); do
      if [[ "${pid}" != "${main_pid}" ]]; then
        echo "${name} pid ${pid}"
      fi
    done
  done
}

require_units() {
  local unit load_state
  for unit in "${PTP_UNITS[@]}"; do
    load_state="$(systemctl show -p LoadState --value "${unit}" 2>/dev/null || true)"
    if [[ "${load_state}" != "loaded" ]]; then
      echo "ERROR: ${unit} is not installed (LoadState=${load_state:-unknown})." >&2
      echo "Install the units with: sudo ~/pancakebatter/install_ptp_units.sh" >&2
      return 1
    fi
  done
}

prepare_runtime_paths() {
  local invoking_uid invoking_gid
  invoking_uid="$(id -u)"
  invoking_gid="$(id -g)"

  # The evidence directory may have been created by a privileged or sandboxed
  # launcher. Make the invoking operator the explicit owner so the evidence
  # write cannot fail.
  sudo install -d -o "${invoking_uid}" -g "${invoking_gid}" -m 0755 "${PTP_LOG_DIR}"

  if ! is_running "ptp4l" && [[ -S "${PTP_SOCKET}" ]]; then
    echo "Removing stale PTP management socket: ${PTP_SOCKET}"
    sudo rm -f "${PTP_SOCKET}"
  fi
}

require_running() {
  local name="$1"
  local tries=30
  local stable_samples=0
  while (( tries > 0 )); do
    if is_running "${name}"; then
      stable_samples=$((stable_samples + 1))
      if (( stable_samples >= 10 )); then
        return 0
      fi
    else
      stable_samples=0
    fi
    sleep 0.1
    tries=$((tries - 1))
  done
  echo "ERROR: ${name} did not remain stable for one second; inspect: journalctl -u ${name}.service" >&2
  return 1
}

start_units() {
  local outside
  outside="$(outside_unit_daemons)"
  if [[ -n "${outside}" ]]; then
    echo "ERROR: running outside the systemd units:" >&2
    sed 's/^/  /' <<<"${outside}" >&2
    echo "A ptp4l started by hand never declares the TAI-UTC offset valid." >&2
    echo "Replace it with the units while no orange or orange_client runs: ptp_stack.sh restart" >&2
    return 1
  fi
  if unit_active ptp4l.service && unit_active phc2sys.service; then
    echo "ptp4l.service and phc2sys.service are already active."
    return
  fi
  echo "Starting ${PTP_UNITS[*]}..."
  sudo systemctl start "${PTP_UNITS[@]}"
}

wait_for_socket() {
  local tries=50
  while (( tries > 0 )); do
    if [[ -S "${PTP_SOCKET}" ]]; then
      return 0
    fi
    sleep 0.2
    tries=$((tries - 1))
  done
  echo "ERROR: Timed out waiting for PTP socket at ${PTP_SOCKET}" >&2
  return 1
}

capture_management_evidence() {
  local tmp_path="${PTP_MANAGEMENT_EVIDENCE}.tmp.$$"
  local pmc_output pmc_status
  if ! is_running "ptp4l"; then
    echo "WARNING: Cannot capture PTP management evidence: ptp4l is not running" >&2
    return 1
  fi
  if [[ ! -S "${PTP_SOCKET}" ]]; then
    echo "WARNING: Cannot capture PTP management evidence: socket ${PTP_SOCKET} is absent" >&2
    return 1
  fi
  set +e
  pmc_output="$(sudo -n timeout 3s pmc -u -b 0 -s "${PTP_SOCKET}" \
    "GET TIME_PROPERTIES_DATA_SET" \
    "GET PARENT_DATA_SET" \
    "GET DEFAULT_DATA_SET" 2>&1)"
  pmc_status=$?
  set -e

  {
    echo "captured_at_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "socket=${PTP_SOCKET}"
    echo "pmc_exit_status=${pmc_status}"
    printf '%s\n' "${pmc_output}"
  } >"${tmp_path}"

  if ! grep -q "RESPONSE MANAGEMENT TIME_PROPERTIES_DATA_SET" "${tmp_path}"; then
    rm -f "${tmp_path}"
    echo "WARNING: PTP management evidence did not include TIME_PROPERTIES_DATA_SET" >&2
    if grep -q "RESPONSE MANAGEMENT TIME_PROPERTIES_DATA_SET" \
        "${PTP_MANAGEMENT_EVIDENCE}" 2>/dev/null; then
      echo "WARNING: Preserving prior valid evidence at ${PTP_MANAGEMENT_EVIDENCE}" >&2
    else
      echo "WARNING: No valid prior management evidence is available" >&2
    fi
    return 1
  fi
  mv -f "${tmp_path}" "${PTP_MANAGEMENT_EVIDENCE}"
  echo "PTP management evidence: ${PTP_MANAGEMENT_EVIDENCE}"
  if ! grep -Eq "^[[:space:]]*currentUtcOffsetValid[[:space:]]+1[[:space:]]*$" "${PTP_MANAGEMENT_EVIDENCE}"; then
    echo "WARNING: the grandmaster does not declare the TAI-UTC offset valid (currentUtcOffsetValid is not 1);" \
         "recordings will classify cameras ieee1588_ptp_timescale_untraceable." >&2
    echo "WARNING: the setter runs at ptp4l.service start; to apply it now without a restart:" \
         "sudo /usr/local/libexec/pancakebatter/ptp4l_gm_settings" >&2
  fi
}

start_stack() {
  need_sudo
  require_units
  prepare_runtime_paths
  start_units
  require_running "ptp4l"
  wait_for_socket
  require_running "phc2sys"
  capture_management_evidence || true
  echo "PTP stack started (${PTP_UNITS[*]}). Logs: journalctl -u ptp4l.service -u phc2sys.service"
}

stop_stack() {
  need_sudo
  require_units
  echo "Stopping ${PTP_UNITS[*]}..."
  sudo systemctl stop phc2sys.service ptp4l.service

  # Daemons started by hand (older versions of this script did that).
  local name
  for name in phc2sys ptp4l; do
    if is_running "${name}"; then
      echo "Stopping ${name} running outside its unit..."
      sudo pkill -x "${name}"
    fi
  done
}

status_stack() {
  echo "Process state:"
  pgrep -af "ptp4l|phc2sys" || echo "(no ptp4l/phc2sys process)"
  echo
  echo "PTP TIME_STATUS_NP:"
  if is_running "ptp4l" && [[ -S "${PTP_SOCKET}" ]]; then
    sudo -n pmc -u -b 0 -s "${PTP_SOCKET}" "GET TIME_STATUS_NP" || true
  elif [[ -S "${PTP_SOCKET}" ]]; then
    echo "(ptp4l is not running; ${PTP_SOCKET} is stale)"
  else
    echo "(ptp4l is not running; socket ${PTP_SOCKET} not found)"
  fi
  echo
  echo "PTP management evidence:"
  if is_running "ptp4l"; then
    capture_management_evidence || true
  elif grep -q "RESPONSE MANAGEMENT TIME_PROPERTIES_DATA_SET" \
      "${PTP_MANAGEMENT_EVIDENCE}" 2>/dev/null; then
    echo "(not refreshed because ptp4l is not running; prior valid evidence preserved at ${PTP_MANAGEMENT_EVIDENCE})"
  else
    echo "(not refreshed because ptp4l is not running; no valid prior evidence)"
  fi
  echo
  echo "Systemd units:"
  local unit
  for unit in "${PTP_UNITS[@]}"; do
    echo "  ${unit}: $(systemctl is-active "${unit}" 2>/dev/null || true)," \
         "$(systemctl is-enabled "${unit}" 2>/dev/null || echo not-installed) at boot"
  done
  local outside
  outside="$(outside_unit_daemons)"
  if [[ -n "${outside}" ]]; then
    echo "WARNING: running outside the systemd units (no TAI-UTC offset setter):" >&2
    sed 's/^/  /' <<<"${outside}" >&2
    echo "WARNING: replace them while no orange or orange_client runs: ptp_stack.sh restart" >&2
  fi
}

logs_stack() {
  journalctl -u ptp4l.service -u phc2sys.service -n 60 -f
}

if [[ $# -ne 1 ]]; then
  usage
  exit 1
fi

case "$1" in
  start)
    start_stack
    ;;
  stop)
    stop_stack
    ;;
  status)
    status_stack
    ;;
  restart)
    stop_stack
    sleep 0.4
    start_stack
    ;;
  logs)
    logs_stack
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    echo "ERROR: Unknown command: $1" >&2
    usage
    exit 1
    ;;
esac
