#!/usr/bin/env bash
# Pre-check a recording folder with Citrus's transfer sealer in validate-only
# mode, so Orange knows BEFORE the operator's transfer (rsync_move.sh /
# transfer_as_domain_user.sh, profile parent-recording-v2) that the folder will
# seal and that Palette's intake will accept its layout. Read-only; it hashes
# every media file, so a long recording takes minutes.
#
# Pin: Citrus package citrus-recording-transfer 2.2.0 (tag recording-transfer-v2.2.0,
# citrus commit befd8a8d; wheel sha256 44b659d95e3c277aef70158044012383f5cc2868db2067ade668b94c09de4515;
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
PIN_COMMIT="befd8a8da8109fcebfe4d8d40ef2784430e36b4a"   # tag recording-transfer-v2.2.0 (2026-10-09: --check-structure-only pre-check; same refusals as the old validate mode without reading video bytes; video bytes are hashed once during the transfer and verified from storage)
PKG="python/citrus_recording_transfer/src/citrus_recording_transfer"
declare -A PIN=(
  ["$PKG/sealer.py"]="540541bddaafc523a27dd61526ea2089f5f6e969ec6e8b264bbfb3277506c365"
  ["$PKG/snapshot.py"]="62950e3e8db64d2d57608a6c5fe59049545079158cefaf5d7f48f5b20ffe6c0c"
  ["$PKG/completion_marker.py"]="7fc5ff68c52ef489790373a4f76cac82483f15ecb77b50c2dd587ca72f0e684d"
  ["$PKG/__init__.py"]="910889f4c9239d2b77e7b2e3b0531dfd3566d168aad3cb27e7d2a6d4b29233be"
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
echo "sealer: citrus-recording-transfer 2.2.0 from $CITRUS_ROOT at $head_commit (pin $PIN_COMMIT; the six pinned files match)"
echo "checking source structure $FOLDER (no video bytes read; no transfer, no writes)"
# --check-structure-only (2.2.0, Jeremy 2026-10-09): the same checks and refusals as the
# old --validate-source-only without reading video bytes (every other artifact is
# still hashed, video sizes compared); the transfer hashes the video once and
# verifies every delivered byte from storage. Success prints "Source structure admissible: ...".
python3 "$CITRUS_ROOT/scripts/recording_transfer_v2.py" --source "$FOLDER" --check-structure-only
