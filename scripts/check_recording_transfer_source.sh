#!/usr/bin/env bash
# Pre-check a recording folder with Citrus's transfer sealer in validate-only
# mode, so Orange knows BEFORE the operator's transfer (rsync_move.sh /
# transfer_as_domain_user.sh, profile parent-recording-v2) that the folder will
# seal and that Palette's intake will accept its layout. Read-only; it hashes
# every media file, so a long recording takes minutes.
#
# Pin: Citrus package citrus-recording-transfer 1.0.0 (tag recording-transfer-v1.0.0,
# citrus commit 7528b2c5; wheel sha256 17d0b059adb10c15865abf9468e8e95c0e694fd3777bd25bc11e09477affdcad).
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
PIN_COMMIT="7528b2c588fcf726772490dfc3bec709c9e96f9a"   # tag recording-transfer-v1.0.0
PKG="python/citrus_recording_transfer/src/citrus_recording_transfer"
declare -A PIN=(
  ["$PKG/sealer.py"]="4b60f8dda30a6f847a958978483c67fe6b918470ce2839547e2e544586c2e20b"
  ["$PKG/snapshot.py"]="03f19b9746edc68057eaa25d6ef2cda49ae980afdd24f756a45fb44297f6babb"
  ["$PKG/completion_marker.py"]="7fc5ff68c52ef489790373a4f76cac82483f15ecb77b50c2dd587ca72f0e684d"
  ["$PKG/__init__.py"]="0f7b2a2e5a8160f57ab0278e9ec6edc4a0ffc700ee48c4164bec06e133098dee"
  ["$PKG/schemas/recording_transfer_v2.schema.json"]="4cf611312e911e165e2a9bbfeb0eb8ce0efb62b68cf3672239055e1c58ab22f1"
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
echo "sealer: citrus-recording-transfer 1.0.0 from $CITRUS_ROOT at $head_commit (pin $PIN_COMMIT; the six pinned files match)"
echo "validating source $FOLDER (hashes all media; no transfer, no writes)"
python3 "$CITRUS_ROOT/scripts/recording_transfer_v2.py" --source "$FOLDER" --validate-source-only --marker _citrus_transfer_complete.json
