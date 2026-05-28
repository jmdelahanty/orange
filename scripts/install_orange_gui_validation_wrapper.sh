#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  install_orange_gui_validation_wrapper.sh [options]

Installs this repo's narrow sudo GUI validation wrapper.

Options:
  --target <path>          Install target. Default /usr/local/bin/orange-gui-validation
  --install-sudoers        Install a narrow NOPASSWD sudoers entry for jeremy.
  --sudoers-target <path>  Sudoers target. Default /etc/sudoers.d/orange-gui-validation
  --help

After installation, the GUI launcher can use:
  sudo -n /usr/local/bin/orange-gui-validation ...
EOF
}

TARGET="/usr/local/bin/orange-gui-validation"
INSTALL_SUDOERS=0
SUDOERS_TARGET="/etc/sudoers.d/orange-gui-validation"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --help|-h)
      usage
      exit 0
      ;;
    --target)
      shift
      [[ $# -gt 0 ]] || { echo "--target requires a value." >&2; exit 2; }
      TARGET="$1"
      shift
      ;;
    --install-sudoers)
      INSTALL_SUDOERS=1
      shift
      ;;
    --sudoers-target)
      shift
      [[ $# -gt 0 ]] || { echo "--sudoers-target requires a value." >&2; exit 2; }
      SUDOERS_TARGET="$1"
      shift
      ;;
    *)
      echo "Unsupported argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE="$REPO_ROOT/scripts/orange_gui_validation_wrapper.sh"

if [[ ! -f "$SOURCE" ]]; then
  echo "Wrapper source not found: $SOURCE" >&2
  exit 1
fi

bash -n "$SOURCE"

echo "[install-gui-wrapper] source=$SOURCE"
echo "[install-gui-wrapper] target=$TARGET"

if [[ "${EUID}" -eq 0 ]]; then
  install -o root -g root -m 0755 "$SOURCE" "$TARGET"
else
  sudo install -o root -g root -m 0755 "$SOURCE" "$TARGET"
fi

HELP_TEXT="$("$TARGET" --help)"
for required in \
  "--orange-bin" \
  "--env KEY=VALUE" \
  "--ptp-stack-mode" \
  "ORANGE_GUI_AUTORUN" \
  "/home/jeremy/orange-gop-split-a16/targets/release/orange"; do
  if [[ "$HELP_TEXT" != *"$required"* ]]; then
    echo "Installed wrapper help is missing expected text: $required" >&2
    exit 1
  fi
done

if [[ "$INSTALL_SUDOERS" == "1" ]]; then
  SUDOERS_LINE="jeremy ALL=(root) NOPASSWD: $TARGET"
  echo "[install-gui-wrapper] sudoers=$SUDOERS_TARGET"
  if [[ "${EUID}" -eq 0 ]]; then
    printf '%s\n' "$SUDOERS_LINE" > "$SUDOERS_TARGET"
    chmod 0440 "$SUDOERS_TARGET"
    visudo -cf "$SUDOERS_TARGET"
  else
    printf '%s\n' "$SUDOERS_LINE" | sudo tee "$SUDOERS_TARGET" >/dev/null
    sudo chmod 0440 "$SUDOERS_TARGET"
    sudo visudo -cf "$SUDOERS_TARGET"
  fi
fi

echo "[install-gui-wrapper] installed and verified"
echo "[install-gui-wrapper] try:"
echo "  sudo -n $TARGET --help"
