#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  scripts/run_direct_input_compare.sh <experiment-spec.json>

Behavior:
  - clones the provided experiment spec into two fresh /tmp specs
  - appends unique experiment_id suffixes for:
      copy
      direct_input
  - sets fixed.nvenc_direct_input to:
      1. false for the copy-path spec
      2. true for the direct-input spec
  - runs the installed benchmark wrapper twice

Notes:
  - keep pre_encoder_reference_capture disabled for the first throughput
    comparison unless direct-input capture parity is already validated
EOF
}

if [[ $# -ne 1 ]]; then
    usage
    exit 1
fi

SPEC_PATH=$1
if [[ ! -f "$SPEC_PATH" ]]; then
    echo "Spec not found: $SPEC_PATH" >&2
    exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
ORANGE_CLIENT="$REPO_ROOT/build/orange_client"

if [[ ! -x "$ORANGE_CLIENT" ]]; then
    echo "orange_client not found or not executable: $ORANGE_CLIENT" >&2
    exit 1
fi

STAMP=$(date +%s)
TMP_DIR="${TMPDIR:-/tmp}/orange_direct_input_compare_${STAMP}"
mkdir -p "$TMP_DIR"

COPY_SPEC="$TMP_DIR/copy_spec.json"
DIRECT_SPEC="$TMP_DIR/direct_input_spec.json"

python3 - "$SPEC_PATH" "$COPY_SPEC" "$DIRECT_SPEC" "$STAMP" <<'PY'
import json
import sys
from pathlib import Path

src = Path(sys.argv[1])
copy_dst = Path(sys.argv[2])
direct_dst = Path(sys.argv[3])
stamp = sys.argv[4]

with src.open("r", encoding="utf-8") as f:
    root = json.load(f)

experiment_id = root.get("experiment_id", "")
if not experiment_id:
    raise SystemExit("experiment spec requires a non-empty experiment_id")

copy_root = dict(root)
copy_fixed = dict(copy_root.get("fixed", {}))
copy_fixed["nvenc_direct_input"] = False
copy_root["fixed"] = copy_fixed
copy_root["experiment_id"] = f"{experiment_id}_copy_{stamp}"
copy_dst.write_text(json.dumps(copy_root, indent=2) + "\n", encoding="utf-8")

direct_root = dict(root)
direct_fixed = dict(direct_root.get("fixed", {}))
direct_fixed["nvenc_direct_input"] = True
direct_root["fixed"] = direct_fixed
direct_root["experiment_id"] = f"{experiment_id}_direct_input_{stamp}"
direct_dst.write_text(json.dumps(direct_root, indent=2) + "\n", encoding="utf-8")
PY

echo "[direct-input-compare] copy path spec:    $COPY_SPEC"
echo "[direct-input-compare] direct path spec:  $DIRECT_SPEC"
echo "[direct-input-compare] orange_client:     $ORANGE_CLIENT"

echo
echo "[direct-input-compare] running copy path..."
sudo -n /usr/local/bin/orange-local-benchmark "$COPY_SPEC"

echo
echo "[direct-input-compare] running direct-input path..."
sudo -n /usr/local/bin/orange-local-benchmark "$DIRECT_SPEC"

echo
echo "[direct-input-compare] done"
echo "  copy spec:   $COPY_SPEC"
echo "  direct spec: $DIRECT_SPEC"
