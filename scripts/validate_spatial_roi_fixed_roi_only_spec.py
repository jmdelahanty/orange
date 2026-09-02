#!/usr/bin/env python3
"""Static validation for the 2010093 fixed-ROI-only diagnostic profile.

This validator reads experiment/config/authority JSON only. It deliberately
does not invoke orange_client, enumerate cameras, query CUDA/NVENC, or create
recording artifacts.
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SPEC = (
    REPO_ROOT
    / "experiment_specs"
    / "2010093_spatial_roi_fixed_roi_only_registered_context_100fps_gpu3_v1.json"
)
COMBINED_SPEC = (
    REPO_ROOT
    / "experiment_specs"
    / "2010093_spatial_roi_diagnostic_plumbing_100fps_gpu3_v1.json"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"[spatial-roi-fixed-only] invalid spec: {message}")


def sha256(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def load_json(path: Path) -> dict:
    require(path.is_file() and not path.is_symlink(), f"missing or symlinked JSON: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SystemExit(f"[spatial-roi-fixed-only] cannot read {path}: {error}")
    require(isinstance(value, dict), f"JSON root must be an object: {path}")
    return value


def validate(spec_path: Path) -> None:
    spec = load_json(spec_path)
    combined = load_json(COMBINED_SPEC)
    expected_id = "2010093_spatial_roi_fixed_roi_only_registered_context_100fps_gpu3_v1"
    require(spec.get("experiment_id") == expected_id, "unexpected experiment_id")
    require(spec.get("selection") == {"camera_serials": ["2010093"], "gpu_ids": [3]},
            "selection must contain only camera 2010093 on source GPU 3")

    fixed = spec.get("fixed")
    require(isinstance(fixed, dict), "fixed must be an object")
    require(fixed.get("duration_s") == 3 and fixed.get("warmup_s") == 1,
            "duration/warmup must remain 3s/1s")
    require(fixed.get("display") is False and fixed.get("yolo") is False,
            "display and YOLO must be disabled")
    require(fixed.get("yolo_worker") is False and fixed.get("pose_worker") is False,
            "YOLO and pose workers must be disabled")
    require(fixed.get("sync_mode") == "free_run", "sync mode must remain free_run")
    require(fixed.get("stream_only") is False,
            "stream_only must remain false so recording identities and ROI media run")
    require(fixed.get("recording_sink_mode") == "immediate_recycle",
            "ROI-only recording must recycle the full-frame ingress immediately")
    require(fixed.get("spatial_roi_media_policy") == {
                "schema_id": "orange.spatial_roi_recording.media_policy",
                "schema_version": 1,
                "media_policy": "fixed_rois_with_registered_context",
                "retained_products": {
                    "full_frame": False,
                    "fixed_rois": True,
                    "registered_context": True,
                },
                "sink_backend": "external_ipc",
            }, "ROI-only policy must be the closed registered-context v1 envelope")
    require(fixed.get("registered_scene_context") == {
                "schema_id": "orange.recording.registered_scene_context.capture_declaration",
                "schema_version": 1,
                "registration_authority_status":
                    "diagnostic_not_physical_acceptance",
                "subject_presence": "unknown",
                "dish_setup_complete": True,
                "nir_illumination_fixed": True,
                "camera_configuration_fixed": True,
                "rig_fixed": True,
            }, "diagnostic context declaration must be closed and must not claim physical acceptance")
    require("external_recorder_contract" not in fixed,
            "ROI-only profile must not contain a continuous full-frame recorder contract")
    require("pre_encoder_reference_capture" not in fixed,
            "ROI-only profile must not request a pre-encoder reference dump")

    # The source camera profile and ROI products are intentionally identical to
    # the accepted combined plumbing profile. Only media selection and the
    # full-frame sink topology differ.
    combined_fixed = combined.get("fixed", {})
    require(fixed.get("config_folder") == combined_fixed.get("config_folder"),
            "config_folder must remain the validated four-camera profile")
    require(fixed.get("output_root") == combined_fixed.get("output_root"),
            "output_root must remain the durable local acquisition root")
    require(fixed.get("nvenc_direct_input") == combined_fixed.get("nvenc_direct_input"),
            "nvenc_direct_input must remain unchanged")
    require(fixed.get("spatial_roi_recording") ==
            combined_fixed.get("spatial_roi_recording"),
            "four fixed P1 ROI products must match the combined profile exactly")
    require(fixed.get("spatial_roi_recorder_runtime") ==
            combined_fixed.get("spatial_roi_recorder_runtime"),
            "per-stream ROI GPU mapping must match the combined profile exactly")
    require(spec.get("matrix") == combined.get("matrix"),
            "P1/VBR-Q20/GOP-25 diagnostic matrix must remain unchanged")

    config_path = REPO_ROOT / fixed["config_folder"] / "2010093.json"
    camera_config = load_json(config_path)
    require(camera_config.get("device_serial_number") == "2010093",
            "camera config identity must be 2010093")
    require(camera_config.get("width") == 4512 and camera_config.get("height") == 4512,
            "camera config must be 4512x4512")
    require(camera_config.get("frame_rate") == 100 and
            camera_config.get("pixel_format") == "Mono8" and
            camera_config.get("source_gpu_id") == 3 and
            camera_config.get("gpu_direct") is True,
            "camera config must be 100 FPS Mono8 GPUDirect on source GPU 3")
    split_gop = camera_config.get("recording", {}).get("split_gop", {})
    require(camera_config.get("recording", {}).get("profile_name") ==
            "validated_split_gop_hevc_100fps_gop25_fourcam_a16" and
            camera_config.get("recording", {}).get("mode") == "split_gop" and
            split_gop.get("encoder_gpu_ids") == [3, 4] and
            split_gop.get("strict") is True,
            "source camera profile must retain the validated strict split-GOP [3,4] setup")

    authority_index = spec.get("diagnostic_authority", {})
    require(authority_index.get("schema_id") ==
            "orange.spatial_roi_diagnostic_authority.index" and
            authority_index.get("schema_version") == 1 and
            authority_index.get("closed") is True and
            authority_index.get("status") == "diagnostic_not_physical_acceptance",
            "authority index must remain closed diagnostic metadata")
    require(authority_index.get("camera_serial") == "2010093" and
            authority_index.get("runtime_camera_id") == 0 and
            authority_index.get("coordinate_space") == "camera_native_0_indexed" and
            authority_index.get("native_raster") == {"width": 4512, "height": 4512},
            "authority index camera-native identity mismatch")
    artifacts = authority_index.get("artifacts", {})
    require(set(artifacts) == {"layout", "materialization", "registration"},
            "authority index must contain exactly three authority roles")
    require(artifacts == combined.get("diagnostic_authority", {}).get("artifacts"),
            "authority artifact references must match the combined diagnostic profile")
    for role, descriptor in artifacts.items():
        relative_path = Path(descriptor["path"])
        require(not relative_path.is_absolute(),
                f"{role} authority path must be repository-relative")
        raw_path = REPO_ROOT / relative_path
        require(raw_path.is_file() and not raw_path.is_symlink(),
                f"{role} authority artifact is missing or symlinked")
        path = raw_path.resolve()
        try:
            path.relative_to(REPO_ROOT)
        except ValueError:
            raise SystemExit(
                f"[spatial-roi-fixed-only] invalid spec: {role} authority path escapes repository")
        bytes_value = path.read_bytes()
        require(len(bytes_value) == descriptor["size_bytes"],
                f"{role} authority size does not match descriptor")
        require(sha256(bytes_value) == descriptor["sha256"],
                f"{role} authority checksum does not match descriptor")

    print("[spatial-roi-fixed-only] spec=pass")
    print("[spatial-roi-fixed-only] mode=fixed_rois_with_registered_context")
    print("[spatial-roi-fixed-only] continuous_full_frame=omitted_by_policy")
    print("[spatial-roi-fixed-only] external_full_frame_recorder=absent")
    print("[spatial-roi-fixed-only] pre_encoder_reference_dump=disabled")
    print("[spatial-roi-fixed-only] camera=2010093 raster=4512x4512 fps=100 source_gpu=3")
    print("[spatial-roi-fixed-only] roi_products=4 P1/VBR-Q20/GOP-25 roi_gpus=[1,2,1,2]")
    print("[spatial-roi-fixed-only] validation=static_json_only_no_camera_access")


def main(argv: list[str]) -> int:
    if len(argv) > 2 or (len(argv) == 2 and argv[1] in {"--help", "-h"}):
        if len(argv) == 2 and argv[1] in {"--help", "-h"}:
            print(f"Usage: {argv[0]} [spec.json]")
            return 0
        raise SystemExit(f"Usage: {argv[0]} [spec.json]")
    path = Path(argv[1]) if len(argv) == 2 else DEFAULT_SPEC
    if not path.is_absolute():
        path = REPO_ROOT / path
    validate(path.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
