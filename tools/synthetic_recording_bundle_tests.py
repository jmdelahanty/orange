#!/usr/bin/env python3
"""Run synthetic_recording_bundle with a synthetic fixture rig and check that the
production writers produced a complete, consistent, clearly labelled bundle.

Usage: synthetic_recording_bundle_tests.py <path-to-synthetic_recording_bundle>
"""
from __future__ import annotations

import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
CAMERAS = ("2010093", "2010094", "2010095", "2010096")


def sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def load(path: Path) -> dict:
    return json.loads(path.read_text())


def run(binary: Path, out: Path, rig: Path, *extra: str) -> dict:
    cmd = [str(binary), "--out", str(out), "--fixture-rig", str(rig),
           "--recording-id", out.name, *(f for c in CAMERAS for f in ("--camera", c)), *extra]
    env = dict(os.environ)
    env.pop("ORANGE_CITRUS_BINDING_REQUEST_VERSION", None)
    env.pop("ORANGE_CITRUS_OBSERVATION_BINDING_MODE", None)
    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    assert result.returncode == 0, f"{' '.join(cmd)}\n{result.stdout}\n{result.stderr}"
    # The writers log to stdout before the summary; the root label is the summary.
    return load(out / "synthetic_bundle.json")


def check_bundle(out: Path, summary: dict, *, expect_subtype: bool, expect_requests: bool,
                 request_version: int) -> None:
    label = load(out / "synthetic_bundle.json")
    assert label["schema_id"] == "orange.synthetic_recording_bundle" and label["synthetic"] is True
    assert label["data_origin"] == "synthetic"
    assert (out / "SYNTHETIC_BUNDLE_README.txt").read_text().startswith("SYNTHETIC RECORDING BUNDLE")

    # Two snapshots: the sealed start is read-only, referenced by digest from the
    # mutable one, and byte-identical to the mutable snapshot as of sealing.
    start_path = out / "recording_snapshot_start.json"
    snapshot_path = out / "recording_snapshot.json"
    assert not (start_path.stat().st_mode & stat.S_IWUSR), "start snapshot must be read-only"
    start = load(start_path)
    snapshot = load(snapshot_path)
    reference = snapshot["immutable_recording_start_snapshot"]
    assert reference["relative_path"] == "recording_snapshot_start.json"
    assert reference["sha256"] == sha256(start_path) == label["artifacts"]["recording_snapshot_start"]["sha256"]
    assert "immutable_recording_start_snapshot" not in start
    assert start["recording_id"] == snapshot["recording_id"] == out.name
    assert set(start["source_camera_streams"]) == set(CAMERAS)
    assert set(start["cameras"]) == set(CAMERAS)
    for serial in CAMERAS:
        assert start["cameras"][serial]["synthetic_input"]["synthetic"] is True
        assert start["camera_runtime"][serial]["source"] if "source" in start["camera_runtime"][serial] else True
    assert start["session"]["synthetic_bundle"]["synthetic"] is True

    # Frozen recording contexts: synthetic origin, one per camera, the requested
    # context version, identical in both snapshots.
    contexts = start["session"]["recording_contexts"]
    assert set(contexts) == set(CAMERAS)
    for entry in contexts.values():
        assert entry["data_origin"] == "synthetic"
        assert ("recording_subtype" in entry) is expect_subtype
        assert entry["schema_version"] == (1 if expect_subtype else 2)
    assert snapshot["session"]["recording_contexts"] == contexts

    # Geometry contract: resolved from the synthetic rig, referenced by exact
    # digest from both snapshots, with a complete recording-local asset bundle.
    contract_path = out / "recording_geometry_contract.json"
    contract = load(contract_path)
    assert contract["schema_id"] == "orange.recording.geometry_contract"
    assert contract["status"] == "resolved", contract.get("errors")
    assert {k: v["status"] for k, v in contract["cameras"].items()} == {c: "resolved" for c in CAMERAS}
    for snap in (start, snapshot):
        ref = snap["recording_geometry_contract"]
        assert ref["relative_path"] == "recording_geometry_contract.json"
        assert ref["sha256"] == sha256(contract_path)
        assert ref["status"] == "resolved"
    assets = contract["materialized_assets"]
    manifest_path = out / "recording_geometry_assets" / "manifest.json"
    manifest = load(manifest_path)
    assert assets["status"] == "complete", manifest.get("failures")
    assert assets["sha256"] == sha256(manifest_path)
    assert manifest["status"] == "complete" and not manifest["failures"]
    assert manifest["materialized_file_count"] == len(manifest["files"]) == assets["file_count"]
    assert set(manifest["scope"]["camera_serials"]) == set(CAMERAS)
    assert manifest["scope"]["daily_registration"]["mode"] == "selected_daily_registration"
    roles = {}
    for row in manifest["files"]:
        copied = out / "recording_geometry_assets" / row["relative_path"]
        assert copied.is_file(), row["relative_path"]
        assert row["sha256"] == sha256(copied), row["relative_path"]
        assert row["exact_source_bytes"] is True
        assert Path(row["source_path"]).read_bytes() == copied.read_bytes(), row["relative_path"]
        if "declared_source_sha256" in row:
            assert row["declared_checksum_verified"] is True, row["relative_path"]
        roles[row["role"]] = roles.get(row["role"], 0) + 1
    per_camera_roles = ("daily_rim_observation", "daily_rim_manifest", "daily_rim_image_set",
                        "daily_rim_spatial_mask_export", "daily_rim_palette_mask_export",
                        "active_homography_pointer", "homography_candidate", "homography_matrix_yaml",
                        "active_projected_surface_scale_pointer", "projected_surface_scale_candidate",
                        "projected_surface_scale_observation")
    for role in per_camera_roles:
        assert roles.get(role) == len(CAMERAS), (role, roles)
    for role in ("daily_registration_runtime_selection", "daily_registration_acceptance",
                 "daily_registration_candidate", "tank_design"):
        assert roles.get(role) == 1, (role, roles)
    # The daily rim observation is exposed in the snapshot with the recording-local path.
    for serial in CAMERAS:
        rim = start["calibrations"][serial]["dish_top_rim_observation"]
        local = rim["recording_local_assets"]["observation_relative_path"]
        assert local == f"recording_geometry_assets/cameras/Cam{serial}/daily_registration/rim_observation/observation.json"
        assert (out / local).is_file()

    # Orange's own artifact validator agrees, for both snapshots.
    sys.path.insert(0, str(REPO_ROOT / "scripts"))
    from validate_recording_artifacts import Reporter, validate_recording_geometry_artifacts  # noqa: E402

    for snap in (start, snapshot):
        reporter = Reporter()
        result = validate_recording_geometry_artifacts(out, snap, reporter)
        assert not reporter.failures, reporter.failures
        assert result["asset_status"] == "complete" and result["files"] == assets["file_count"], result

    # Binding requests: Orange's sealed inputs for Citrus, only for a bound
    # (stimulus_experiment) session; re-materialization verifies unchanged.
    bindings = out / "recording_observation_bindings"
    if expect_requests:
        collection = load(bindings / "request_collection.json")
        assert collection["status"] == "materialized"
        assert collection["request_count"] == len(CAMERAS) if "request_count" in collection else True
        assert summary["observation_binding_requests"]["status"] == "materialized"
        for row in summary["observation_binding_requests"]["requests"]:
            request = load(out / row["relative_path"])
            assert request["schema_version"] == request["contract"]["schema_version"] == request_version
            camera = request["contract"]["target"]["camera_id"]
            if request_version == 2:
                assert request["contract"]["recording_context"] == contexts[camera]
            assert request["contract"]["recording"]["recording_snapshot"]["sha256"] == reference["sha256"]
            assert request["contract"]["recording_geometry_contract"]["status"] == "available"
            assert request["contract"]["recording_geometry_contract"]["sha256"] == sha256(contract_path)
        assert snapshot["observation_binding_requests"]["status"] == "materialized"
        assert "observation_binding_requests" not in start
    else:
        assert not bindings.exists()
        assert summary["observation_binding_requests"]["binding_mode"] == "not_applicable"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = Path(sys.argv[1])
    assert binary.is_file(), f"missing binary {binary}"

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        # Bound, subtype-free (context v2), request v2, with the placeholder manifest.
        out = root / "synthetic_bound_v2"
        summary = run(binary, out, root / "rig_a", "--intent", "stimulus_experiment",
                      "--recording-subtype", "omit", "--binding-mode", "required",
                      "--request-version", "2", "--manifest")
        check_bundle(out, summary, expect_subtype=False, expect_requests=True, request_version=2)
        manifest = load(out / "recording_session.json")
        assert manifest["recording_contexts"] == load(out / "recording_snapshot_start.json")["session"]["recording_contexts"]
        assert manifest["metadata"]["recording_geometry_contract"]["sha256"] == sha256(out / "recording_geometry_contract.json")
        assert manifest["producer"] == "orange_synthetic_recording_bundle"
        for serial in CAMERAS:
            assert (out / f"Cam{serial}.mp4").read_text().startswith("SYNTHETIC PLACEHOLDER")

        # Re-running against the same folder is refused (never patch finalized evidence).
        result = subprocess.run([str(binary), "--out", str(out), "--fixture-rig", str(root / "rig_a"),
                                 "--camera", "2010093"], capture_output=True, text=True)
        assert result.returncode != 0 and "not empty" in result.stderr

        # Recording-only, explicit subtype (context v1): no binding requests at all.
        out2 = root / "synthetic_recording_only_v1"
        summary2 = run(binary, out2, root / "rig_b", "--intent", "recording_only",
                       "--recording-subtype", "dish_freeswim")
        check_bundle(out2, summary2, expect_subtype=True, expect_requests=False, request_version=1)

        # Bound, request v1 (default): the request carries no context field.
        out3 = root / "synthetic_bound_v1"
        summary3 = run(binary, out3, root / "rig_c", "--intent", "stimulus_experiment",
                       "--recording-subtype", "dish_stimulus", "--binding-mode", "optional")
        check_bundle(out3, summary3, expect_subtype=True, expect_requests=True, request_version=1)
        for row in summary3["observation_binding_requests"]["requests"]:
            assert "recording_context" not in load(out3 / row["relative_path"])["contract"]

        # An acquired origin cannot be requested: data_origin is fixed by the tool.
        for f in (out / "synthetic_bundle.json", out2 / "synthetic_bundle.json", out3 / "synthetic_bundle.json"):
            assert all(e["data_origin"] == "synthetic" for e in load(f)["recording_contexts"].values())

    print("synthetic_recording_bundle_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
