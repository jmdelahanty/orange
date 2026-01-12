#!/usr/bin/env bash
set -euo pipefail

GROUP_NAME="ipc"
TARGET_USER="${SUDO_USER:-$(id -un)}"

usage() {
  cat <<'EOF'
Usage: sudo scripts/setup_ipc_group.sh [--user USER]

Creates the ipc group (if missing) and adds the user to it.
Log out/in or run `newgrp ipc` afterward for the group to take effect.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -u|--user)
      shift
      if [[ $# -eq 0 ]]; then
        echo "ERROR: --user requires a value" >&2
        exit 1
      fi
      TARGET_USER="$1"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
  shift
done

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "ERROR: This script must be run as root (use sudo)." >&2
  exit 1
fi

if ! getent group "${GROUP_NAME}" >/dev/null 2>&1; then
  echo "Creating group: ${GROUP_NAME}"
  groupadd "${GROUP_NAME}"
else
  echo "Group already exists: ${GROUP_NAME}"
fi

echo "Adding user '${TARGET_USER}' to group '${GROUP_NAME}'"
usermod -aG "${GROUP_NAME}" "${TARGET_USER}"

cat <<EOF
Done.
Next steps:
  - Log out/in, or run: newgrp ${GROUP_NAME}
  - Restart the writer so /dev/shm/shm_cam_* gets group ${GROUP_NAME}
EOF
