#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  run_spatial_roi_fixed_roi_only_diagnostic_2010093.sh [--spec <path>] [--orange-client <path>] [--execute]

Statically validates the versioned one-camera fixed-ROI-only diagnostic
profile for camera 2010093 (4512x4512 at 100 FPS, source GPU 3). It checks
that the four P1 fixed ROI products and [1,2,1,2] recorder mapping match the
combined plumbing profile, while the continuous full-frame product and
external full-frame recorder are omitted.

Default: run the static validator and orange_client's parse/resolve-only
experiment validation. No camera or media work occurs.

--execute: explicitly opt in to the three-second hardware/media diagnostic.
It writes four fixed ROI videos plus one native registered context artifact;
it does not start or retain a continuous full-frame recorder.

Options:
  --spec <path>           Validate/run a different fixed-ROI-only JSON spec.
  --orange-client <path>  Binary to validate/run (default: targets/release/orange_client).
  --execute               Opt in to camera and media execution.
  --help
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SPEC="$REPO_ROOT/experiment_specs/2010093_spatial_roi_fixed_roi_only_registered_context_100fps_gpu3_v1.json"
ORANGE_CLIENT="$REPO_ROOT/targets/release/orange_client"
EXECUTE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --spec)
      shift
      [[ $# -gt 0 ]] || { echo "--spec requires a value." >&2; exit 2; }
      SPEC="$1"
      shift
      ;;
    --orange-client)
      shift
      [[ $# -gt 0 ]] || { echo "--orange-client requires a value." >&2; exit 2; }
      ORANGE_CLIENT="$1"
      shift
      ;;
    --execute)
      EXECUTE=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unsupported argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "$SPEC" != /* ]]; then
  SPEC="$REPO_ROOT/$SPEC"
fi
if [[ "$ORANGE_CLIENT" != /* ]]; then
  ORANGE_CLIENT="$REPO_ROOT/$ORANGE_CLIENT"
fi
SPEC="$(realpath -e "$SPEC")"
ORANGE_CLIENT="$(realpath -e "$ORANGE_CLIENT")"

python3 "$REPO_ROOT/scripts/validate_spatial_roi_fixed_roi_only_spec.py" "$SPEC"

if [[ "$EXECUTE" -eq 0 ]]; then
  echo "[spatial-roi-fixed-only] dry-run: orange_client parse/resolve validation only"
  (
    cd "$REPO_ROOT"
    "$ORANGE_CLIENT" --mode local --experiment-spec "$SPEC" --validate-experiment-spec
  )
  exit 0
fi

echo "[spatial-roi-fixed-only] EXECUTE requested: camera and ROI media will be used"
echo "[spatial-roi-fixed-only] continuous_full_frame=omitted_by_policy"
echo "[spatial-roi-fixed-only] spec=$SPEC"
echo "[spatial-roi-fixed-only] orange_client=$ORANGE_CLIENT"
(
  cd "$REPO_ROOT"
  "$ORANGE_CLIENT" --mode local --experiment-spec "$SPEC"
)
