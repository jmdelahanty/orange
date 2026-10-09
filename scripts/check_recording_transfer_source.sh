#!/usr/bin/env bash
# Pre-check a recording folder with Citrus's transfer sealer in validate-only
# mode, so Orange knows BEFORE the operator's transfer (rsync_move.sh /
# transfer_as_domain_user.sh, profile parent-recording-v2) that the folder will
# seal and that Palette's intake will accept its layout. Read-only; it hashes
# every media file, so a long recording takes minutes.
#
# Pin: Citrus package citrus-recording-transfer 3.0.0 (tag recording-transfer-v3.0.0,
# citrus commit 334f53a8; wheel sha256 eb12e5df5435eb5ca11907cd5a0ceba9092b17bcc9d17f28a757539fadbdf1c9;
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
PIN_COMMIT="334f53a8f61183b9bdf61270bf0e98d0cd3b9557"   # tag recording-transfer-v3.0.0 (2026-10-09: --validate-source-only removed; --check-structure-only as in 2.2.0; transfers, --verify-only and formats unchanged)
PKG="python/citrus_recording_transfer/src/citrus_recording_transfer"
declare -A PIN=(
  ["$PKG/sealer.py"]="e659e07154eadf6a5d2edfb84c7a630ad434f4073a502c293c3c3b530ea806fc"
  ["$PKG/snapshot.py"]="d3fd3f6e74e902cc9088f6788762e64059fbf4e2bcbbcc0af60be00ba4c6d313"
  ["$PKG/completion_marker.py"]="7fc5ff68c52ef489790373a4f76cac82483f15ecb77b50c2dd587ca72f0e684d"
  ["$PKG/__init__.py"]="fe7d43e463779c508c7aee4182a076df042d2d5664c7083462c6a0b7bb0ae4c0"
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
echo "sealer: citrus-recording-transfer 3.0.0 from $CITRUS_ROOT at $head_commit (pin $PIN_COMMIT; the six pinned files match)"
echo "checking source structure $FOLDER (no video bytes read; no transfer, no writes)"
# --check-structure-only (2.2.0, Jeremy 2026-10-09): the same checks and refusals as the
# old --validate-source-only without reading video bytes (every other artifact is
# still hashed, video sizes compared); the transfer hashes the video once and
# verifies every delivered byte from storage. Success prints "Source structure admissible: ...".
python3 "$CITRUS_ROOT/scripts/recording_transfer_v2.py" --source "$FOLDER" --check-structure-only
