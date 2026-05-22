#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  stage_detect_onnx_export.sh --onnx <path> --onnx-manifest <path> [options]

Stages a Palette detect ONNX export for Orange TensorRT builds.

The staged source directory is separate from both runtime detect artifacts and
per-build TensorRT output:

  source ONNXs:  /home/jeremy/orange_data/model_sources/detect/<run_id>/
  TRT builds:    /home/jeremy/orange_data/trt_builds/detect/<run_id>/<build_id>_<stamp>/
  runtime:       /home/jeremy/orange_data/detect/

Options:
  --onnx <path>                 Palette ONNX export.
  --onnx-manifest <path>        Palette ONNX export manifest JSON.
  --run-id <id>                 Override run_id. Default: read from manifest.
  --source-stage-root <path>    Default /home/jeremy/orange_data/model_sources/detect.
  --force                       Allow replacing staged source files.
  --help

After staging, build with:

  scripts/build_tensorrt_detect_engine.sh \
    --onnx <stage-dir>/source.onnx \
    --onnx-manifest <stage-dir>/source.onnx.manifest.json \
    --run-id <run_id> \
    --device 5
EOF
}

ONNX=""
ONNX_MANIFEST=""
RUN_ID=""
SOURCE_STAGE_ROOT="/home/jeremy/orange_data/model_sources/detect"
FORCE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --help|-h)
      usage
      exit 0
      ;;
    --onnx)
      shift
      [[ $# -gt 0 ]] || { echo "--onnx requires a value." >&2; exit 2; }
      ONNX="$1"
      shift
      ;;
    --onnx-manifest)
      shift
      [[ $# -gt 0 ]] || { echo "--onnx-manifest requires a value." >&2; exit 2; }
      ONNX_MANIFEST="$1"
      shift
      ;;
    --run-id)
      shift
      [[ $# -gt 0 ]] || { echo "--run-id requires a value." >&2; exit 2; }
      RUN_ID="$1"
      shift
      ;;
    --source-stage-root)
      shift
      [[ $# -gt 0 ]] || { echo "--source-stage-root requires a value." >&2; exit 2; }
      SOURCE_STAGE_ROOT="$1"
      shift
      ;;
    --force)
      FORCE=1
      shift
      ;;
    *)
      echo "Unsupported argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[[ -n "$ONNX" ]] || { echo "--onnx is required." >&2; exit 2; }
[[ -n "$ONNX_MANIFEST" ]] || { echo "--onnx-manifest is required." >&2; exit 2; }

ONNX="$(realpath -e "$ONNX")"
ONNX_MANIFEST="$(realpath -e "$ONNX_MANIFEST")"
SOURCE_STAGE_ROOT="$(realpath -m "$SOURCE_STAGE_ROOT")"

if [[ -z "$RUN_ID" ]]; then
  RUN_ID="$(python3 - "$ONNX_MANIFEST" <<'PY'
import json
import sys
from pathlib import Path

manifest = json.loads(Path(sys.argv[1]).read_text())
run_id = manifest.get("run_id") or manifest.get("onnx", {}).get("metadata_props", {}).get("run_id")
if not run_id:
    raise SystemExit("could not determine run_id from ONNX manifest; pass --run-id")
print(run_id)
PY
)"
fi

EXPECTED_ONNX_SHA="$(python3 - "$ONNX_MANIFEST" <<'PY'
import json
import sys
from pathlib import Path

manifest = json.loads(Path(sys.argv[1]).read_text())
print(manifest.get("onnx", {}).get("sha256", ""))
PY
)"
ACTUAL_ONNX_SHA="$(sha256sum "$ONNX" | awk '{print $1}')"
if [[ -n "$EXPECTED_ONNX_SHA" && "$EXPECTED_ONNX_SHA" != "$ACTUAL_ONNX_SHA" ]]; then
  echo "ONNX sha256 mismatch:" >&2
  echo "  expected: $EXPECTED_ONNX_SHA" >&2
  echo "  actual:   $ACTUAL_ONNX_SHA" >&2
  exit 1
fi

STAGE_DIR="$SOURCE_STAGE_ROOT/$RUN_ID"
DEST_ONNX="$STAGE_DIR/source.onnx"
DEST_ONNX_MANIFEST="$STAGE_DIR/source.onnx.manifest.json"
DEST_STAGE_MANIFEST="$STAGE_DIR/source_stage_manifest.json"
DEST_README="$STAGE_DIR/README.md"

mkdir -p "$STAGE_DIR"

copy_checked() {
  local src="$1"
  local dst="$2"
  if [[ -e "$dst" && "$FORCE" -ne 1 ]]; then
    if cmp -s "$src" "$dst"; then
      return 0
    fi
    echo "destination exists with different content: $dst (use --force to replace)" >&2
    exit 1
  fi
  cp "$src" "$dst"
}

copy_checked "$ONNX" "$DEST_ONNX"
copy_checked "$ONNX_MANIFEST" "$DEST_ONNX_MANIFEST"

python3 - "$RUN_ID" "$ONNX" "$ONNX_MANIFEST" "$DEST_ONNX" "$DEST_ONNX_MANIFEST" "$DEST_STAGE_MANIFEST" <<'PY'
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

run_id = sys.argv[1]
source_onnx = Path(sys.argv[2])
source_manifest = Path(sys.argv[3])
staged_onnx = Path(sys.argv[4])
staged_manifest = Path(sys.argv[5])
stage_manifest = Path(sys.argv[6])

def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

source_manifest_json = json.loads(source_manifest.read_text())
out = {
    "schema_id": "orange.detect_onnx_source_stage",
    "schema_version": 1,
    "created_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
    "run_id": run_id,
    "source": {
        "onnx": {
            "original_path": str(source_onnx),
            "sha256": sha256(source_onnx),
        },
        "onnx_manifest": {
            "original_path": str(source_manifest),
            "sha256": sha256(source_manifest),
        },
    },
    "staged": {
        "onnx": {
            "path": str(staged_onnx),
            "sha256": sha256(staged_onnx),
            "bytes": staged_onnx.stat().st_size,
        },
        "onnx_manifest": {
            "path": str(staged_manifest),
            "sha256": sha256(staged_manifest),
            "bytes": staged_manifest.stat().st_size,
        },
    },
    "palette_export": {
        "weights": source_manifest_json.get("weights", {}),
        "onnx": {
            "outputs": source_manifest_json.get("onnx", {}).get("outputs", []),
            "plugin_contract": source_manifest_json.get("onnx", {}).get("plugin_contract", {}),
            "metadata_props": source_manifest_json.get("onnx", {}).get("metadata_props", {}),
        },
        "export": source_manifest_json.get("export", {}),
        "source_manifest": source_manifest_json.get("source_manifest", {}),
    },
}
stage_manifest.write_text(json.dumps(out, indent=2) + "\n")
PY

cat > "$DEST_README" <<EOF
# Detect ONNX Source Stage: $RUN_ID

This directory stores the Palette ONNX export used as source input for Orange
TensorRT builds.

Use this source with:

\`\`\`bash
scripts/build_tensorrt_detect_engine.sh \\
  --onnx $DEST_ONNX \\
  --onnx-manifest $DEST_ONNX_MANIFEST \\
  --run-id $RUN_ID \\
  --device 5
\`\`\`

Runtime TensorRT engines should live in:

\`\`\`text
/home/jeremy/orange_data/detect/
\`\`\`

Per-build TensorRT logs and profile artifacts should live in:

\`\`\`text
/home/jeremy/orange_data/trt_builds/detect/$RUN_ID/
\`\`\`
EOF

(
  cd "$STAGE_DIR"
  sha256sum README.md source.onnx source.onnx.manifest.json source_stage_manifest.json > SHA256SUMS
)

echo "[onnx-stage] run_id=$RUN_ID"
echo "[onnx-stage] stage_dir=$STAGE_DIR"
echo "[onnx-stage] onnx=$DEST_ONNX"
echo "[onnx-stage] onnx_manifest=$DEST_ONNX_MANIFEST"
echo "[onnx-stage] source_stage_manifest=$DEST_STAGE_MANIFEST"
