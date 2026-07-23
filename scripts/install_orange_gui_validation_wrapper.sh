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

"$TARGET" --dry-run \
  --env ORANGE_GUI_GUIDED_CAPTURE_RECIPE_SEQUENCE=black_reference,uniform_gray,arena_outline,homography_rings,verification_dots \
  --env ORANGE_GUI_FIXTURE_APERTURE_SHAPE=circle \
  --env ORANGE_GUI_GUIDED_CAPTURE_SWEEP_FOREGROUND_GRAYS_U8=80,96,112 \
  --env ORANGE_GUI_GUIDED_CAPTURE_SWEEP_REPEATS=5 \
  --env ORANGE_GUI_GUIDED_CAPTURE_INCLUDE_ARENA_OUTLINE_REFERENCE=1 \
  --env ORANGE_GUI_PROJECTED_SURFACE_SCALE_TARGETS_READY=1 \
  --env ORANGE_GUI_ACCEPT_PROJECTED_SURFACE_SCALES_ARMED=0 \
  --env ORANGE_GUI_GUIDED_CAPTURE_FIT_HOMOGRAPHIES=0 \
  --env ORANGE_GUI_GUIDED_CAPTURE_RESULT_JSON=/tmp/orange_guided_install_check.json \
  --env ORANGE_GUI_ARENA_CENTERING_AUTORUN=1 \
  --env ORANGE_GUI_ARENA_CENTERING_CAMERAS=2010093,2010094,2010095,2010096 \
  --env ORANGE_GUI_ARENA_CENTERING_FOREGROUND_GRAY_U8=72 \
  --env ORANGE_GUI_ARENA_CENTERING_PROJECTOR_INTENSITY_REPORT_PATH=/home/jeremy/orange_data/calibrations/commissioning/projector_intensity_20260719T014235Z/commissioning_report.json \
  --env ORANGE_GUI_ARENA_CENTERING_PROJECTOR_INTENSITY_REPORT_SHA256=e402dfe457eacb1ffec0515715cf451e29ef349a6963c964ffd50cbaedfbb029 \
  --env ORANGE_GUI_ARENA_CENTERING_PROBE_CANVAS_PX=3 \
  --env ORANGE_GUI_ARENA_CENTERING_FIT_HOMOGRAPHIES=1 \
  --env ORANGE_GUI_ARENA_CENTERING_ACCEPT_HOMOGRAPHIES_ARMED=0 \
  --env ORANGE_GUI_HOMOGRAPHY_MAXIMUM_HOLDOUT_RMS_ERROR_CANVAS_PX=0.75 \
  --env ORANGE_GUI_HOMOGRAPHY_SATURATION_PIXEL_THRESHOLD_U8=250 \
  --env ORANGE_GUI_HOMOGRAPHY_MAXIMUM_DOT_CORE_SATURATION_FRACTION=0.005 \
  --env ORANGE_GUI_HOMOGRAPHY_MINIMUM_DOT_BACKGROUND_CONTRAST_U8=20 \
  --env ORANGE_GUI_ARENA_CENTERING_RESULT_JSON=/tmp/orange_centering_install_check.json \
  >/dev/null

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
