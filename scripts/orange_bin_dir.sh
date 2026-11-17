# Sourced by launchers: sets ORANGE_BIN_DIR to the binaries that belong with
# these scripts (docs/opt_install_plan_2026-11-14.md, step e).
#   installed release: <release>/share/orange/scripts -> <release>/bin
#                      (the release folder holds INSTALLED.json)
#   source tree:       <tree>/scripts -> <tree>/targets/release
# ORANGE_RELEASE_DIR is set (non-empty) only for an installed release.
_orange_scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$_orange_scripts_dir/../../../INSTALLED.json" && -d "$_orange_scripts_dir/../../../bin" ]]; then
  ORANGE_RELEASE_DIR="$(cd "$_orange_scripts_dir/../../.." && pwd)"
  ORANGE_BIN_DIR="$ORANGE_RELEASE_DIR/bin"
else
  ORANGE_RELEASE_DIR=""
  ORANGE_BIN_DIR="$(cd "$_orange_scripts_dir/.." && pwd)/targets/release"
fi
unset _orange_scripts_dir
