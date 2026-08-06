#!/usr/bin/env bash
set -euo pipefail

PTP_CONF="${PTP_CONF:-/etc/ptp4l.conf}"
PTP_SOCKET="${PTP_SOCKET:-/var/run/ptp4l}"
PTP_LOG_DIR="${PTP_LOG_DIR:-/tmp/ptp-stack}"
PTP_IFACES_DEFAULT="mlnx1_p1_25g mlnx1_p2_25g mlnx2_p3_25g mlnx2_p4_25g"
PTP_IFACES="${PTP_IFACES:-$PTP_IFACES_DEFAULT}"

PTP4L_LOG="${PTP_LOG_DIR}/ptp4l.log"
PHC2SYS_LOG="${PTP_LOG_DIR}/phc2sys.log"
PTP_MANAGEMENT_EVIDENCE="${PTP_LOG_DIR}/management_evidence.txt"

usage() {
  cat <<EOF
Usage: ptp_stack.sh <command>

Commands:
  start      Start ptp4l and phc2sys in background
  stop       Stop phc2sys and ptp4l
  status     Show process state and TIME_STATUS_NP
  restart    Stop then start
  logs       Tail both log files

Optional environment variables:
  PTP_CONF     Default: /etc/ptp4l.conf
  PTP_SOCKET   Default: /var/run/ptp4l
  PTP_LOG_DIR  Default: /tmp/ptp-stack
  PTP_IFACES   Default: "${PTP_IFACES_DEFAULT}"
EOF
}

need_sudo() {
  sudo -v
}

is_running() {
  local name="$1"
  pgrep -x "${name}" >/dev/null 2>&1
}

prepare_runtime_paths() {
  local invoking_uid invoking_gid
  invoking_uid="$(id -u)"
  invoking_gid="$(id -g)"

  # These files may have been created by a privileged or sandboxed launcher.
  # Make the invoking operator the explicit owner so shell redirection cannot
  # fail before sudo starts the daemon.
  sudo install -d -o "${invoking_uid}" -g "${invoking_gid}" -m 0755 "${PTP_LOG_DIR}"
  sudo touch "${PTP4L_LOG}" "${PHC2SYS_LOG}"
  sudo chown "${invoking_uid}:${invoking_gid}" "${PTP4L_LOG}" "${PHC2SYS_LOG}"

  if ! is_running "ptp4l" && [[ -S "${PTP_SOCKET}" ]]; then
    echo "Removing stale PTP management socket: ${PTP_SOCKET}"
    sudo rm -f "${PTP_SOCKET}"
  fi
}

require_running() {
  local name="$1"
  local log_path="$2"
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
  echo "ERROR: ${name} did not remain stable for one second; inspect ${log_path}" >&2
  return 1
}

start_ptp4l() {
  if is_running "ptp4l"; then
    echo "ptp4l is already running."
    return
  fi

  local args=()
  local iface
  for iface in ${PTP_IFACES}; do
    args+=("-i" "${iface}")
  done

  echo "Starting ptp4l on: ${PTP_IFACES}"
  sudo -b ptp4l "${args[@]}" -f "${PTP_CONF}" -m >>"${PTP4L_LOG}" 2>&1
  require_running "ptp4l" "${PTP4L_LOG}"
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

start_phc2sys() {
  if is_running "phc2sys"; then
    echo "phc2sys is already running."
    return
  fi

  echo "Starting phc2sys (autoconfig mode)..."
  sudo -b phc2sys -a -rr -z "${PTP_SOCKET}" -m >>"${PHC2SYS_LOG}" 2>&1
  require_running "phc2sys" "${PHC2SYS_LOG}"
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
}

start_stack() {
  need_sudo
  prepare_runtime_paths
  start_ptp4l
  wait_for_socket
  start_phc2sys
  capture_management_evidence || true
  require_running "ptp4l" "${PTP4L_LOG}"
  require_running "phc2sys" "${PHC2SYS_LOG}"
  echo "PTP stack started. Logs:"
  echo "  ${PTP4L_LOG}"
  echo "  ${PHC2SYS_LOG}"
}

stop_stack() {
  need_sudo
  if is_running "phc2sys"; then
    echo "Stopping phc2sys..."
    sudo pkill -x phc2sys
  else
    echo "phc2sys is not running."
  fi

  if is_running "ptp4l"; then
    echo "Stopping ptp4l..."
    sudo pkill -x ptp4l
  else
    echo "ptp4l is not running."
  fi
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
}

logs_stack() {
  touch "${PTP4L_LOG}" "${PHC2SYS_LOG}"
  tail -n 60 -f "${PTP4L_LOG}" "${PHC2SYS_LOG}"
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
