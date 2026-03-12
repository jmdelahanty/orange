#!/usr/bin/env bash
set -euo pipefail

PTP_CONF="${PTP_CONF:-/etc/ptp4l.conf}"
PTP_SOCKET="${PTP_SOCKET:-/var/run/ptp4l}"
PTP_LOG_DIR="${PTP_LOG_DIR:-/tmp/ptp-stack}"
PTP_IFACES_DEFAULT="mlnx1_p1_25g mlnx1_p2_25g mlnx1_p3_25g mlnx1_p4_25g"
PTP_IFACES="${PTP_IFACES:-$PTP_IFACES_DEFAULT}"

PTP4L_LOG="${PTP_LOG_DIR}/ptp4l.log"
PHC2SYS_LOG="${PTP_LOG_DIR}/phc2sys.log"

usage() {
  cat <<'EOF'
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
  PTP_IFACES   Default: "mlnx1_p1_25g mlnx1_p2_25g mlnx1_p3_25g mlnx1_p4_25g"
EOF
}

need_sudo() {
  sudo -v
}

is_running() {
  local name="$1"
  pgrep -x "${name}" >/dev/null 2>&1
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
  sudo ptp4l "${args[@]}" -f "${PTP_CONF}" -m >>"${PTP4L_LOG}" 2>&1 &
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
  sudo phc2sys -a -rr -z "${PTP_SOCKET}" -m >>"${PHC2SYS_LOG}" 2>&1 &
}

start_stack() {
  mkdir -p "${PTP_LOG_DIR}"
  need_sudo
  start_ptp4l
  wait_for_socket
  start_phc2sys
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
  if [[ -S "${PTP_SOCKET}" ]]; then
    sudo pmc -u -b 0 -s "${PTP_SOCKET}" "GET TIME_STATUS_NP" || true
  else
    echo "(socket ${PTP_SOCKET} not found)"
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
