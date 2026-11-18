# Sourced by launchers: sets ORANGE_BIN_DIR to the binaries that belong with
# these scripts (docs/opt_install_plan_2026-11-14.md, step e).
#   installed release: <release>/share/orange/scripts -> <release>/bin
#                      (the release folder holds INSTALLED.json)
#   source tree:       <tree>/scripts -> <tree>/targets/release
# ORANGE_RELEASE_DIR is set (non-empty) only for an installed release.
# ORANGE_WRAPPER_PREFIX names the sudo wrappers that run those binaries
# (P3): "${ORANGE_WRAPPER_PREFIX}gui-validation" is /usr/local/bin/orange-gui-validation
# for a release (releases only) and /usr/local/bin/orange-dev-gui-validation
# for a source tree (the dev tree only). Physical
# paths (pwd -P): a run started through /opt/orange/current stays on that
# release even if current is flipped while it runs.
_orange_scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
if [[ -f "$_orange_scripts_dir/../../../INSTALLED.json" && -d "$_orange_scripts_dir/../../../bin" ]]; then
  ORANGE_RELEASE_DIR="$(cd "$_orange_scripts_dir/../../.." && pwd -P)"
  ORANGE_BIN_DIR="$ORANGE_RELEASE_DIR/bin"
  ORANGE_WRAPPER_PREFIX="/usr/local/bin/orange-"
else
  ORANGE_RELEASE_DIR=""
  ORANGE_BIN_DIR="$(cd "$_orange_scripts_dir/.." && pwd)/targets/release"
  ORANGE_WRAPPER_PREFIX="/usr/local/bin/orange-dev-"
fi
unset _orange_scripts_dir
