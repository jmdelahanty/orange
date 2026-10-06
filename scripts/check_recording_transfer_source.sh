#!/usr/bin/env bash
# Pre-check a recording folder with Citrus's transfer sealer in validate-only
# mode, so Orange knows BEFORE the operator's transfer (rsync_move.sh /
# transfer_as_domain_user.sh, profile parent-recording-v2) that the folder will
# seal and that Palette's intake will accept its layout. Read-only; it hashes
# every media file, so a long recording takes minutes.
#
# Interim until Citrus publishes the sealer as a package: the sealer is run
# from a Citrus checkout, and the four files are verified against the pinned
# sha256 values below (agreed with Citrus 2026-10-06, citrus main ce758122;
# the files were last changed 2026-09-08/24/26). A digest mismatch refuses.
#
# Usage: scripts/check_recording_transfer_source.sh <recording_folder>
#        CITRUS_ROOT=/path/to/citrus (default /home/jeremy/citrus)
set -euo pipefail
[ $# -eq 1 ] || { sed -n 2,14p "$0"; exit 2; }
FOLDER="$1"
CITRUS_ROOT="${CITRUS_ROOT:-/home/jeremy/citrus}"
PIN_COMMIT="ce758122bc66695f83ba9003d65b8485489d7847"
declare -A PIN=(
  ["scripts/recording_transfer_v2.py"]="559955a27a3e44916c8287ddf51f3aa11c24a73f829f2756f2de78fbe9463481"
  ["scripts/recording_transfer_snapshot.py"]="03f19b9746edc68057eaa25d6ef2cda49ae980afdd24f756a45fb44297f6babb"
  ["scripts/transfer_completion_marker.py"]="7fc5ff68c52ef489790373a4f76cac82483f15ecb77b50c2dd587ca72f0e684d"
  ["src/schemas/recording_transfer_v2.schema.json"]="4cf611312e911e165e2a9bbfeb0eb8ce0efb62b68cf3672239055e1c58ab22f1"
)
[ -d "$FOLDER" ] || { echo "not a directory: $FOLDER" >&2; exit 2; }
for rel in "${!PIN[@]}"; do
    f="$CITRUS_ROOT/$rel"
    [ -f "$f" ] || { echo "missing $f (set CITRUS_ROOT)" >&2; exit 2; }
    got="$(sha256sum "$f" | cut -d' ' -f1)"
    [ "$got" = "${PIN[$rel]}" ] || { echo "sealer file $rel sha256 $got differs from the pin ${PIN[$rel]}; ask Citrus before re-pinning" >&2; exit 2; }
done
head_commit="$(git -C "$CITRUS_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "sealer: $CITRUS_ROOT at $head_commit (pin $PIN_COMMIT; the four sealer files match the pinned digests)"
echo "validating source $FOLDER (hashes all media; no transfer, no writes)"
python3 "$CITRUS_ROOT/scripts/recording_transfer_v2.py" --source "$FOLDER" --validate-source-only --marker _citrus_transfer_complete.json
