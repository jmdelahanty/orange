#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  run_spatial_roi_diagnostic_2010096.sh [--spec <path>] [--orange-client <path>] [--execute]

Validates and prints the versioned one-camera/four-fixed-ROI diagnostic smoke
for camera 2010096 (4512x4512 at 100 FPS, GPU 5). The four quadrants are
plumbing/encoder validation geometry only; they are not accepted physical
compartment geometry.

Default: validate/print only. No camera, CUDA, NVENC, or media work occurs.
--execute: run orange_client against the camera and write full-frame plus
           spatial-ROI recording artifacts under
           /home/jeremy/orange_data/exp/unsorted. Hardware/media execution
           requires this explicit flag.
The first combined full-frame plus fixed-ROI product uses a supervised
external_ipc full-frame split-GOP contract on GPUs 5 and 6 so the parent can
make one summed storage admission for both recorder families. The existing
in-process full-frame sink remains supported for full-frame-only runs;
combined in-process admission is deferred until it has the same
summed-capacity proof.
After a successful --execute, run the read-only acceptance verifier against
the exact recording folder printed by orange_client (do not select an old
folder by globbing):
  python3 tools/validate_spatial_roi_recording.py --require-ffprobe \
    /home/jeremy/orange_data/exp/unsorted/2010096_spatial_roi_diagnostic_plumbing_100fps_gpu5_v1/<run-id>

Options:
  --spec <path>              Experiment spec (default: the versioned repo spec).
  --orange-client <path>     orange_client binary (default: targets/release/orange_client).
  --execute                  Opt in to hardware/media execution.
  --help
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SPEC="$REPO_ROOT/experiment_specs/2010096_spatial_roi_diagnostic_plumbing_100fps_gpu5_v1.json"
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

python3 - "$SPEC" "$REPO_ROOT" <<'PY'
import json
import hashlib
import sys
from pathlib import Path

spec_path = Path(sys.argv[1])
repo_root = Path(sys.argv[2])
spec = json.loads(spec_path.read_text(encoding="utf-8"))

def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"[spatial-roi-diagnostic] invalid spec: {message}")

require(
    spec.get("experiment_id") ==
    "2010096_spatial_roi_diagnostic_plumbing_100fps_gpu5_v1",
    "unexpected experiment_id",
)
selection = spec.get("selection", {})
require(selection.get("camera_serials") == ["2010096"],
        "selection must contain only camera 2010096")
require(selection.get("gpu_ids") == [5],
        "selection must bind camera 2010096 to GPU 5")
fixed = spec.get("fixed", {})
require(fixed.get("output_root") == "/home/jeremy/orange_data/exp/unsorted",
        "execute output_root must be the durable local acquisition root")
require(fixed.get("stream_only") is False,
        "full-frame recording must remain enabled (stream_only=false)")
recording_sink_mode = fixed.get("recording_sink_mode")
require(recording_sink_mode == "external_ipc",
        "the first combined full-frame plus ROI product requires external_ipc")
external_contract = fixed.get("external_recorder_contract", {})
require(isinstance(external_contract, dict) and
        external_contract.get("mode") == "diagnostic_ipc_v1" and
        external_contract.get("supervise_processes") is True,
        "combined ROI recording requires an enabled supervised external recorder contract")
require(external_contract.get("recorder_tool_path") ==
        "targets/release/external_recorder_ipc_probe",
        "external recorder tool must be the repo-relative release binary")
recorder_tool = repo_root / external_contract["recorder_tool_path"]
require(recorder_tool.is_file() and not recorder_tool.is_symlink(),
        "external recorder release binary is missing or symlinked")
require(recorder_tool.stat().st_mode & 0o111,
        "external recorder release binary is not executable")
external_stream = external_contract.get("streams", {}).get("2010096", {})
require(external_stream.get("routing_policy") == "gop_modulo" and
        external_stream.get("expected_shard_gpu_ids") == [5, 6],
        "full-frame recording must use the validated 2010096 split-GOP pair [5,6]")
require(fixed.get("display") is False, "display must be disabled")
require(fixed.get("yolo") is False, "YOLO must be disabled")
require(fixed.get("yolo_worker") is False, "yolo_worker must be disabled")
require(fixed.get("pose_worker") is False, "pose_worker must be disabled")
require(fixed.get("config_folder") ==
        "config/validated_split_gop_hevc_100fps_gop25_recabled_a16",
        "config_folder must remain repo-relative")

config_folder = Path(fixed["config_folder"])
if not config_folder.is_absolute():
    config_folder = repo_root / config_folder
camera_config_path = config_folder / "2010096.json"
require(camera_config_path.is_file(),
        f"camera config is missing: {camera_config_path}")
camera_config = json.loads(camera_config_path.read_text(encoding="utf-8"))
require(camera_config.get("width") == 4512 and
        camera_config.get("height") == 4512,
        "camera config must be 4512x4512")
require(camera_config.get("frame_rate") == 100,
        "camera config must be 100 FPS")
require(camera_config.get("pixel_format") == "Mono8",
        "camera config must be Mono8")
require(camera_config.get("source_gpu_id") == 5,
        "camera config source GPU must be 5")

roi_config = fixed.get("spatial_roi_recording", {})
require(roi_config.get("enabled") is True,
        "spatial ROI recording must be enabled")
require(roi_config.get("schema_id") == "orange.spatial_roi_recording.config" and
        roi_config.get("schema_version") == 2,
        "spatial ROI config must be schema v2")
require(roi_config.get("strict") is True,
        "spatial ROI config must be strict")
cameras = roi_config.get("cameras", {})
require(list(cameras) == ["2010096"],
        "spatial ROI config must contain only camera 2010096")
camera = cameras["2010096"]
require(camera.get("camera_id") == 3,
        "current four-camera rig mapping requires camera 2010096 to use runtime camera_id 3")
require(camera.get("camera_serial") == "2010096",
        "spatial ROI camera serial mismatch")
require(camera.get("native_raster") == {"width": 4512, "height": 4512},
        "spatial ROI native raster mismatch")
require(camera.get("source_frame_rate") == 100,
        "spatial ROI source frame rate mismatch")
require(camera.get("allow_roi_overlap") is False,
        "diagnostic quadrants must not overlap")

authority_index = spec.get("diagnostic_authority", {})
require(authority_index.get("schema_id") ==
        "orange.spatial_roi_diagnostic_authority.index" and
        authority_index.get("schema_version") == 1,
        "diagnostic authority index must be version 1")
require(authority_index.get("closed") is True,
        "diagnostic authority index must be closed")
require(authority_index.get("status") == "diagnostic_not_physical_acceptance",
        "diagnostic authority index must not claim physical acceptance")
require(authority_index.get("camera_serial") == "2010096" and
        authority_index.get("runtime_camera_id") == 3 and
        authority_index.get("runtime_mapping") ==
        "normal_four_camera_rig_inventory_2010093_to_2010096_maps_0_to_3" and
        authority_index.get("coordinate_space") == "camera_native_0_indexed" and
        authority_index.get("native_raster") == {"width": 4512, "height": 4512},
        "diagnostic authority index camera-native identity mismatch")
authority_artifacts = authority_index.get("artifacts", {})
for authority_role in ("layout", "materialization", "registration"):
    descriptor = authority_artifacts.get(authority_role, {})
    expected_ref = camera[authority_role]
    require(descriptor.get("id") == expected_ref.get("id") and
            descriptor.get("sha256") == expected_ref.get("sha256"),
            f"{authority_role} authority index does not match spatial ROI reference")
    relative_path = descriptor.get("path")
    require(isinstance(relative_path, str) and relative_path and
            not Path(relative_path).is_absolute(),
            f"{authority_role} authority path must be repo-relative")
    authority_path = repo_root / relative_path
    require(authority_path.is_file() and not authority_path.is_symlink(),
            f"{authority_role} authority artifact is missing or symlinked")
    authority_path = authority_path.resolve()
    try:
        authority_path.relative_to(repo_root.resolve())
    except ValueError:
        raise SystemExit(f"[spatial-roi-diagnostic] invalid spec: {authority_role} authority path escapes repository")
    authority_bytes = authority_path.read_bytes()
    require(descriptor.get("size_bytes") == len(authority_bytes),
            f"{authority_role} authority artifact size does not match exact bytes")
    actual_sha256 = "sha256:" + hashlib.sha256(authority_bytes).hexdigest()
    require(descriptor.get("sha256") == actual_sha256,
            f"{authority_role} authority artifact sha256 does not match exact bytes")
    authority_json = json.loads(authority_bytes.decode("utf-8"))
    require(authority_json.get("authority_id") == descriptor.get("id") and
            authority_json.get("schema_version") == 1 and
            authority_json.get("closed") is True and
            authority_json.get("status") == "diagnostic_not_physical_acceptance" and
            authority_json.get("camera_serial") == "2010096" and
            authority_json.get("coordinate_space") == "camera_native_0_indexed" and
            authority_json.get("native_raster") == {"width": 4512, "height": 4512},
            f"{authority_role} authority artifact metadata is not closed camera-native diagnostic metadata")

expected = [
    ("quadrant_top_left", 0, 0),
    ("quadrant_top_right", 2256, 0),
    ("quadrant_bottom_left", 0, 2256),
    ("quadrant_bottom_right", 2256, 2256),
]
rois = camera.get("rois", [])
require(len(rois) == 4, "spatial ROI config must contain exactly four ROIs")
for roi, (roi_id, x, y) in zip(rois, expected):
    require(roi.get("roi_id") == roi_id,
            f"ROI order/id mismatch; expected {roi_id}")
    require(roi.get("required") is True,
            f"{roi_id} must be required")
    require(roi.get("content_rect") ==
            {"x": x, "y": y, "width": 2256, "height": 2256},
            f"{roi_id} must be the expected 0-indexed quadrant")
    require(roi.get("logical_stream_id") ==
            f"2010096_spatial_roi_{roi_id}",
            f"{roi_id} logical stream id is not canonical")
    require(roi.get("artifact_stem") ==
            f"Cam2010096_spatial_roi_{roi_id}",
            f"{roi_id} artifact stem is not canonical")

print("[spatial-roi-diagnostic] spec=pass")
print("[spatial-roi-diagnostic] camera=2010096 runtime_camera_id=3 raster=4512x4512 fps=100 gpu=5")
print("[spatial-roi-diagnostic] geometry=quadrants(top-left,top-right,bottom-left,bottom-right)")
print("[spatial-roi-diagnostic] geometry_status=plumbing/encoder_validation_only")
print("[spatial-roi-diagnostic] full_frame_recording=enabled yolo=off pose=off display=off")
print("[spatial-roi-diagnostic] authority=closed/versioned/camera-native diagnostic_not_physical_acceptance")
PY

if [[ "$EXECUTE" -eq 0 ]]; then
  echo "[spatial-roi-diagnostic] dry-run: validation/print only; no hardware or media execution"
  (
    cd "$REPO_ROOT"
    "$ORANGE_CLIENT" --mode local --experiment-spec "$SPEC" --validate-experiment-spec
  )
  exit 0
fi

echo "[spatial-roi-diagnostic] EXECUTE requested: hardware and media will be used"
echo "[spatial-roi-diagnostic] repo_root=$REPO_ROOT"
echo "[spatial-roi-diagnostic] spec=$SPEC"
echo "[spatial-roi-diagnostic] orange_client=$ORANGE_CLIENT"
echo "[spatial-roi-diagnostic] output_root=/home/jeremy/orange_data/exp/unsorted"
(
  cd "$REPO_ROOT"
  "$ORANGE_CLIENT" --mode local --experiment-spec "$SPEC"
)
