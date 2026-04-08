#!/usr/bin/env bash
set -euo pipefail

ORANGE_ROOT="/home/jeremy/orange-jeremy"
ORANGE_CLIENT="$ORANGE_ROOT/build/orange_client"
ALLOWED_SPEC_DIR_1="$ORANGE_ROOT/experiment_specs"
ALLOWED_SPEC_DIR_2="/tmp"

usage() {
  cat <<'EOF'
Usage:
  orange_local_benchmark_wrapper.sh <experiment-spec.json>

Behavior:
  - Runs orange_client in local experiment mode as root.
  - Only accepts spec files under:
      /home/jeremy/orange-jeremy/experiment_specs
      /tmp
  - After the run, chowns the experiment output folder back to the invoking user
    when launched via sudo.
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 2
fi

if [[ "${EUID}" -ne 0 ]]; then
  echo "This wrapper must be run as root (typically via sudo)." >&2
  exit 1
fi

if [[ ! -x "$ORANGE_CLIENT" ]]; then
  echo "orange_client not found or not executable at $ORANGE_CLIENT" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required to parse the experiment spec." >&2
  exit 1
fi

SPEC_PATH="$(realpath -e "$1")"
case "$SPEC_PATH" in
  "$ALLOWED_SPEC_DIR_1"/*|"$ALLOWED_SPEC_DIR_2"/*)
    ;;
  *)
    echo "Refusing to use experiment spec outside allowed roots: $SPEC_PATH" >&2
    exit 2
    ;;
esac

mapfile -t SPEC_FIELDS < <(python3 - "$SPEC_PATH" <<'PY'
import json
import sys
from pathlib import Path

spec_path = Path(sys.argv[1])
with spec_path.open("r", encoding="utf-8") as f:
    spec = json.load(f)

experiment_id = spec.get("experiment_id", "")
fixed = spec.get("fixed", {})
output_root = fixed.get("output_root", "")
if not experiment_id or not output_root:
    raise SystemExit("experiment spec must define experiment_id and fixed.output_root")

print(experiment_id)
print(output_root)
PY
)

if [[ "${#SPEC_FIELDS[@]}" -ne 2 ]]; then
  echo "Failed to resolve experiment_id/output_root from $SPEC_PATH" >&2
  exit 2
fi

EXPERIMENT_ID="${SPEC_FIELDS[0]}"
OUTPUT_ROOT="${SPEC_FIELDS[1]}"
EXPERIMENT_ROOT="$(realpath -m "$OUTPUT_ROOT/$EXPERIMENT_ID")"

echo "[sudo-wrapper] running experiment_id=$EXPERIMENT_ID"
echo "[sudo-wrapper] spec=$SPEC_PATH"
echo "[sudo-wrapper] output_root=$EXPERIMENT_ROOT"

set +e
"$ORANGE_CLIENT" --mode local --experiment-spec "$SPEC_PATH"
STATUS=$?
set -e

if [[ -n "${SUDO_UID:-}" && -n "${SUDO_GID:-}" && -d "$EXPERIMENT_ROOT" ]]; then
  chown -R "${SUDO_UID}:${SUDO_GID}" "$EXPERIMENT_ROOT"
fi

exit "$STATUS"
