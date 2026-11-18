#!/usr/bin/env bash
set -euo pipefail
# shellcheck source=orange_wrapper_install_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/orange_wrapper_install_lib.sh"

usage() {
  cat <<'EOF'
Usage:
  install_orange_gui_validation_wrapper.sh [options]

Installs this repo's narrow sudo GUI validation wrapper.
Installs two commands from scripts/orange_gui_validation_wrapper.sh; the installed name sets the mode:
  /usr/local/bin/orange-gui-validation      installed releases only (default /opt/orange/current)
  /usr/local/bin/orange-dev-gui-validation  the development tree only (/home/jeremy/orange)

Options:
EOF
  orange_wrapper_install_options_help
  cat <<'EOF'
  --help

After installation, the GUI launcher (scripts/orange_bin_dir.sh picks the
command that matches the launcher's own tree) uses:
  sudo -n /usr/local/bin/orange-gui-validation ...      (from an installed release)
  sudo -n /usr/local/bin/orange-dev-gui-validation ...  (from the dev tree)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --help|-h)
      usage
      exit 0
      ;;
  esac
  orange_wrapper_install_option "$@" || exit 2
  if (( ORANGE_WRAPPER_ARGS_CONSUMED > 0 )); then
    shift "$ORANGE_WRAPPER_ARGS_CONSUMED"
    continue
  fi
  echo "Unsupported argument: $1" >&2
  usage >&2
  exit 2
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE="$REPO_ROOT/scripts/orange_gui_validation_wrapper.sh"
RELEASE_TARGET="$ORANGE_WRAPPER_BIN_DIR/orange-gui-validation"
DEV_TARGET="$ORANGE_WRAPPER_BIN_DIR/orange-dev-gui-validation"

orange_wrapper_install install-gui-wrapper "$SOURCE" gui-validation

for target in "$RELEASE_TARGET" "$DEV_TARGET"; do
  orange_wrapper_require_help "$target" \
    "--orange-bin" \
    "--env KEY=VALUE" \
    "--ptp-stack-mode" \
    "ORANGE_GUI_AUTORUN" \
    "/home/jeremy/orange/targets/release/orange"
done

verify_dry_run() {
  "$1" --dry-run \
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
  --env ORANGE_GUI_ARENA_CENTERING_BOOTSTRAP_CENTERS_ONLY=0 \
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
}
verify_dry_run "$DEV_TARGET"
if orange_wrapper_release_installed; then
  verify_dry_run "$RELEASE_TARGET"
else
  echo "[install-gui-wrapper] no /opt/orange/current yet: release command dry run skipped"
fi

echo "[install-gui-wrapper] installed and verified"
echo "[install-gui-wrapper] try:"
echo "  sudo -n $RELEASE_TARGET --help"
echo "  sudo -n $DEV_TARGET --help"
