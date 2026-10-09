#!/usr/bin/env bash
# Pre-check a recording folder with Citrus's transfer sealer in validate-only
# mode, so Orange knows BEFORE the operator's transfer (rsync_move.sh /
# transfer_as_domain_user.sh, profile parent-recording-v2) that the folder will
# seal and that Palette's intake will accept its layout. Read-only; it hashes
# every media file, so a long recording takes minutes.
#
# Pin: Citrus package citrus-recording-transfer 2.0.2 (tag recording-transfer-v2.0.2,
# citrus commit 5e4737bd; wheel sha256 4caced9713e857b2e11911ec0921791cfc3b0848288abf19bda76555ea001448;
# writes completion marker v3 with sealer provenance; refuses control characters
# in source/destination paths).
# The package files are verified in a Citrus checkout by sha256 (agreed with
# Citrus 2026-10-06) and the CLI is called through the pinned shim
# scripts/recording_transfer_v2.py, which loads the package. A digest mismatch
# refuses: ask Citrus for the new release before re-pinning.
#
# Usage: scripts/check_recording_transfer_source.sh <recording_folder>
#        CITRUS_ROOT=/path/to/citrus (default /home/jeremy/citrus)
set -euo pipefail
[ $# -eq 1 ] || { sed -n 2,14p "$0"; exit 2; }
FOLDER="$1"
CITRUS_ROOT="${CITRUS_ROOT:-/home/jeremy/citrus}"
PIN_COMMIT="5e4737bda41350c27263d783e7bf411faacf7605"   # tag recording-transfer-v2.0.2 (2026-10-09: destination check with four parallel fsync + O_DIRECT readers; source side unchanged)
PKG="python/citrus_recording_transfer/src/citrus_recording_transfer"
declare -A PIN=(
  ["$PKG/sealer.py"]="a3a90a2af6b8cdddbd9d2babd5d48112ee33311598a53574494883bda47e4482"
  ["$PKG/snapshot.py"]="e40b416b540aa187ab74997b9030f2c83d533dfc0cdc371b19dd31f9b7f3f511"
  ["$PKG/completion_marker.py"]="7fc5ff68c52ef489790373a4f76cac82483f15ecb77b50c2dd587ca72f0e684d"
  ["$PKG/__init__.py"]="44465c95c12f0d8c58f1ebb2ef5169c58eefc06731af5bb6af9b26ffa773aa8c"
  ["$PKG/schemas/recording_transfer_v2.schema.json"]="c12832b35657f21514392215d838f401be670e76ac22ae6dab28bf304391503c"
  ["scripts/recording_transfer_v2.py"]="c7d8c67890949859330f8aa8d704e5cf36e29dfbdd3d238f31a2d2d6a99de2ff"
)
[ -d "$FOLDER" ] || { echo "not a directory: $FOLDER" >&2; exit 2; }
for rel in "${!PIN[@]}"; do
    f="$CITRUS_ROOT/$rel"
    [ -f "$f" ] || { echo "missing $f (set CITRUS_ROOT)" >&2; exit 2; }
    got="$(sha256sum "$f" | cut -d' ' -f1)"
    [ "$got" = "${PIN[$rel]}" ] || { echo "sealer file $rel sha256 $got differs from the pin ${PIN[$rel]}; ask Citrus before re-pinning" >&2; exit 2; }
done
head_commit="$(git -C "$CITRUS_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "sealer: citrus-recording-transfer 2.0.2 from $CITRUS_ROOT at $head_commit (pin $PIN_COMMIT; the six pinned files match)"
echo "validating source $FOLDER (hashes all media; no transfer, no writes)"
python3 "$CITRUS_ROOT/scripts/recording_transfer_v2.py" --source "$FOLDER" --validate-source-only --marker _citrus_transfer_complete.json
