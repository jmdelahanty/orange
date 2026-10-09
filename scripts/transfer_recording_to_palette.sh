#!/usr/bin/env bash
# Transfer one Orange recording to Palette's live staging, sealed by the
# pinned Citrus sealer, without hand-copying the long paths.
#
# Usage (run as yourself; the domain-user transfer prompts for Kerberos):
#   scripts/transfer_recording_to_palette.sh                      # the DEFAULT_RUN below
#   scripts/transfer_recording_to_palette.sh --latest <spec name>  # newest experiment of that spec
#   scripts/transfer_recording_to_palette.sh <run folder>          # explicit run_0001 folder
#   add --dry-run-only to stop after the sealer pre-check and the rsync dry run
#
# What it does: resolves the run folder (the experiment folder's single
# run_0001 folder), runs scripts/check_recording_transfer_source.sh (the
# sealer in validate-only mode), the transfer dry run, then the real transfer
# with --verify quick (profile parent-recording-v2 seals the envelope), to
#   /groups/johnson/johnsonlab/jeremy/staging/<experiment folder name>
# with --no-dest-parent, as agreed with Palette (unique_run_id specs make the
# experiment folder name unique). Prints the sealed digest line at the end.
set -euo pipefail

# Edited per delivery by the Orange session; --latest/explicit folder override it.
#
# Sealer 2.1.0 (2026-10-09) is single pass: it copies and hashes itself (no
# rsync), verifies every delivered file from storage (fsync + O_DIRECT) and
# journals progress. An interrupted transfer resumes when re-run (Ctrl-C
# prints how) and recopies only unfinished files; it refuses if the SOURCE
# changed between attempts (bound by _citrus_transfer/reservation.json), so a
# changed source needs a new destination name; a destination half-written by
# a 2.0.x sealer must be finished with that version or re-sent to a new name.
DEFAULT_RUN="/home/jeremy/orange_data/exp/unsorted/fourcam_palette_fish_19220_1_rolling_crops_shadow_20261009_004349/fourcam_palette_fish_19220_1_rolling_crops_shadow_20261009_004349__run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25__aq_off__tempaq_off__lookahead_off"

DOMAIN_USER="${ORANGE_TRANSFER_DOMAIN_USER:-delahantyj@hhmi.org}"
STAGING_ROOT="${ORANGE_PALETTE_STAGING_ROOT:-/groups/johnson/johnsonlab/jeremy/staging}"
TRANSFER_SCRIPT="${CITRUS_TRANSFER_SCRIPT:-$HOME/citrus/scripts/transfer_as_domain_user.sh}"
EXP_ROOT="${ORANGE_EXP_ROOT:-$HOME/orange_data/exp/unsorted}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

dry_run_only=0
run=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run-only) dry_run_only=1 ;;
    --latest)
      shift; [ $# -gt 0 ] || { echo "--latest needs a spec name" >&2; exit 2; }
      exp="$(ls -d "$EXP_ROOT/$1"_[0-9]*_[0-9]* 2>/dev/null | sort | tail -1 || true)"
      [ -n "$exp" ] || { echo "no experiment folder for spec $1 under $EXP_ROOT" >&2; exit 2; }
      run="$(ls -d "$exp"/*run_0001* 2>/dev/null | head -1 || true)" ;;
    -h|--help) sed -n 2,19p "$0"; exit 0 ;;
    *) run="$1" ;;
  esac
  shift
done
[ -n "$run" ] || run="$DEFAULT_RUN"
run="${run%/}"
[ -d "$run" ] || { echo "run folder does not exist: $run" >&2; exit 2; }
[ -f "$run/recording_session.json" ] || { echo "not a finalized run folder (no recording_session.json): $run" >&2; exit 2; }
[ -x "$TRANSFER_SCRIPT" ] || { echo "transfer script missing: $TRANSFER_SCRIPT" >&2; exit 2; }

exp_name="$(basename "$(dirname "$run")")"
dest="$STAGING_ROOT/$exp_name"

echo "[transfer] run:  $run"
echo "[transfer] dest: $dest"
echo "[transfer] 1/3 sealer pre-check (validate only; hashes all media)"
"$REPO_ROOT/scripts/check_recording_transfer_source.sh" "$run"
echo "[transfer] 2/3 dry run"
"$TRANSFER_SCRIPT" --no-dest-parent "$DOMAIN_USER" "$run" johnsonlab-staging "$dest" --dry-run
if [ "$dry_run_only" = 1 ]; then
  echo "[transfer] stopped after the dry run (--dry-run-only)"
  exit 0
fi
echo "[transfer] 3/3 transfer with --verify quick (seals the envelope)"
"$TRANSFER_SCRIPT" --no-dest-parent "$DOMAIN_USER" "$run" johnsonlab-staging "$dest" --verify quick
echo "[transfer] done: $dest"
echo "[transfer] tell the Orange session the sealed digest printed above so Palette can be notified"
