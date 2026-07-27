#!/usr/bin/env python3

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = (
    ROOT
    / "experiment_specs"
    / "2010096_external_ipc_vbr_cq_quality_base_100fps_a16_gpu7_8.json"
)
MANIFEST_PATH = (
    ROOT
    / "experiment_specs"
    / "encoding_master_singlecam_100fps_vbr_cq_screen_manifest.json"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    base = json.loads(BASE_PATH.read_text(encoding="utf-8"))
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))

    require(base["selection"] == {"camera_serials": ["2010096"], "gpu_ids": [7]},
            "base must select only Cam2010096 on analytics GPU 7")
    fixed = base["fixed"]
    require(fixed["sync_mode"] == "ptp_gate", "base must retain PTP gating")
    require(fixed["ptp_register_read_decimate"] == 100,
            "base must keep PTP control-plane reads off the per-frame hot path")
    require(fixed["recording_sink_mode"] == "external_ipc",
            "base must use the external recorder")
    require(fixed["duration_s"] == 15 and fixed["warmup_s"] == 2,
            "base duration and warmup must remain explicit")
    stream = fixed["external_recorder_contract"]["streams"]["2010096"]
    require(stream["expected_shard_gpu_ids"] == [7, 8],
            "base must use the validated Cam2010096 recorder GPU pair")
    require(stream["encode_fps"] == 100 and stream["encode_max_fps"] == 0,
            "base must encode every 100 fps source frame")
    require(stream["importance_map"] == {"mode": "off"},
            "stage-one base must explicitly disable the QP map")
    require(base["policy"]["require_valid_video_content"] is True,
            "live-fish screen must fail invalid video content")

    runs = manifest["runs"]
    expected = {
        "vbr150_control": ("p1", "vbr", 20, 150_000_000, 150_000_000),
        "vbr150_p3_preset_screen": ("p3", "vbr", 20, 150_000_000, 150_000_000),
        "vbr_cq18_max250": ("p1", "vbr_cq", 18, 150_000_000, 250_000_000),
        "vbr_cq20_max250": ("p1", "vbr_cq", 20, 150_000_000, 250_000_000),
        "vbr_cq20_p3_preset_screen": ("p3", "vbr_cq", 20, 150_000_000, 250_000_000),
        "vbr_cq22_max250": ("p1", "vbr_cq", 22, 150_000_000, 250_000_000),
        "vbr_cq24_max250": ("p1", "vbr_cq", 24, 150_000_000, 250_000_000),
    }
    require([run["run_id"] for run in runs] == list(expected),
            "matrix run order or membership changed")
    for run in runs:
        preset, mode, quality, average_bps, max_bps = expected[run["run_id"]]
        override = run["stream_overrides"]["2010096"]
        matrix = run["matrix_overrides"]
        require(override["preset"] == preset,
                f"{run['run_id']} stream preset mismatch")
        require(override["rate_control_mode"] == mode,
                f"{run['run_id']} stream rate-control mismatch")
        require(override["quality_value"] == quality,
                f"{run['run_id']} stream quality mismatch")
        require(override["bitrate_bps"] == average_bps,
                f"{run['run_id']} average bitrate mismatch")
        require(override["max_bitrate_bps"] == max_bps,
                f"{run['run_id']} maximum bitrate mismatch")
        require(override["vbv_buffer_size"] == max_bps,
                f"{run['run_id']} VBV mismatch")
        require(override["importance_map"] == {"mode": "off"},
                f"{run['run_id']} must keep external QP map off")
        require(matrix["rate_control_mode"] == [mode],
                f"{run['run_id']} analytics rate-control mismatch")
        require(matrix["preset"] == [preset],
                f"{run['run_id']} analytics preset mismatch")
        require(matrix["quality_value"] == [quality],
                f"{run['run_id']} analytics quality mismatch")
        require(matrix["importance_map_mode"] == ["off"],
                f"{run['run_id']} analytics QP map must be off")

    print("vbr_cq_quality_matrix_tests passed")


if __name__ == "__main__":
    main()
