#!/usr/bin/env python3

import copy
import importlib.util
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = (
    ROOT
    / "experiment_specs"
    / "2010096_external_ipc_vbr_quality_base_30fps_a16_gpu7_8.json"
)
MANIFEST_PATH = (
    ROOT
    / "experiment_specs"
    / "encoding_master_singlecam_30fps_vbr_bitrate_screen_manifest.json"
)
RUNNER_PATH = ROOT / "scripts" / "run_encoding_master_experiment.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def load_runner():
    spec = importlib.util.spec_from_file_location("encoding_master_runner", RUNNER_PATH)
    require(spec is not None and spec.loader is not None, "could not load matrix runner")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> None:
    runner = load_runner()
    base = json.loads(BASE_PATH.read_text(encoding="utf-8"))
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))

    base_profile = base["fixed"]["recording_profile"]
    require(base_profile["target_bitrate_bps"] == 150_000_000,
            "base profile must declare the 150 Mbps control")
    require(base_profile["aq"] == "off" and base_profile["temporal_aq"] == "off",
            "base profile must explicitly disable both AQ modes")
    require(base_profile["lookahead"] == "off" and base_profile["lookahead_depth"] == 0,
            "base profile must explicitly disable lookahead")

    for run in manifest["runs"]:
        generated = copy.deepcopy(base)
        runner.patch_matrix(generated, run)
        runner.patch_recording_profile_from_matrix(generated)
        matrix = generated["matrix"]
        profile = generated["fixed"]["recording_profile"]
        stream = generated["fixed"]["external_recorder_contract"]["streams"]["2010096"]
        override = run["stream_overrides"]["2010096"]

        for field in (
            "codec",
            "preset",
            "tuning",
            "rate_control_mode",
            "quality_value",
            "gop_length",
            "aq",
            "temporal_aq",
            "lookahead",
        ):
            require(profile[field] == matrix[field][0],
                    f"{run['run_id']} profile/matrix mismatch for {field}")
        for field in ("target_bitrate_bps", "max_bitrate_bps", "vbv_buffer_size"):
            require(profile[field] == matrix[field][0],
                    f"{run['run_id']} profile/matrix mismatch for {field}")
        require(profile["target_bitrate_bps"] == override["bitrate_bps"],
                f"{run['run_id']} profile/recorder target bitrate mismatch")
        require(profile["max_bitrate_bps"] == override["max_bitrate_bps"],
                f"{run['run_id']} profile/recorder maximum bitrate mismatch")
        require(profile["vbv_buffer_size"] == override["vbv_buffer_size"],
                f"{run['run_id']} profile/recorder VBV mismatch")
        require(profile["preset"] == override["preset"],
                f"{run['run_id']} profile/recorder preset mismatch")
        require(stream["encode_fps"] == 30 and stream["encode_max_fps"] == 0,
                f"{run['run_id']} must encode every 30 fps input frame")

    invalid = copy.deepcopy(base)
    invalid["matrix"]["preset"] = ["p1", "p3"]
    try:
        runner.patch_recording_profile_from_matrix(invalid)
    except ValueError as error:
        require("matrix.preset" in str(error), "multi-value error must name matrix.preset")
    else:
        raise AssertionError("recording profile must reject a multi-value matrix field")

    print("encoding_master_recording_profile_tests passed")


if __name__ == "__main__":
    main()
