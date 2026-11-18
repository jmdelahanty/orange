#!/usr/bin/env bash
# /usr/local/bin/orange-gui (docs/opt_install_plan_2026-11-14.md, 4f): runs
# the four-camera GUI launcher (scripts/run_gui_fourcam_external_ipc_validation.sh) of the
# installed Orange release, with that release's binaries and the user's data
# root. No sudo here: the launcher calls the release's sudo wrapper itself.
# /opt/orange/current is resolved once, so the run stays on that release if
# current flips while it runs. Installed by scripts/install_orange_entry_points.sh.
set -euo pipefail
CURRENT="${ORANGE_CURRENT_RELEASE:-/opt/orange/current}"
release="$(realpath -e -- "$CURRENT" 2>/dev/null)" || {
  echo "No installed Orange release at $CURRENT" >&2
  exit 1
}
exec "$release/share/orange/scripts/run_gui_fourcam_external_ipc_validation.sh" "$@"
