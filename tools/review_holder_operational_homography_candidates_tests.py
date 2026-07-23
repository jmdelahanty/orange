#!/usr/bin/env python3

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import review_holder_operational_homography_candidates as review  # noqa: E402


def main() -> int:
    with tempfile.TemporaryDirectory() as temporary_text:
        root = Path(temporary_text)
        set_id = "homography_set_holder_test"
        set_dir = root / set_id
        rows = []
        for arena_id, camera_id in (("arena_1", "2010093"), ("arena_2", "2010094")):
            candidate = {
                "schema_id": review.CANDIDATE_SCHEMA,
                "schema_version": 1,
                "status": "ready_for_review",
                "candidate_set_id": set_id,
                "arena_id": arena_id,
                "camera_id": camera_id,
                "homography_role": "operational_candidate",
            }
            path = set_dir / f"{arena_id}_{camera_id}" / "candidate.json"
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(candidate) + "\n", encoding="utf-8")
            rows.append(candidate)
        canvas_sha = "sha256:" + "a" * 64
        manifest = {
            "schema_id": review.CANDIDATE_SET_SCHEMA,
            "schema_version": 1,
            "state": "ready_for_review",
            "candidate_set_id": set_id,
            "transaction_id": "holder_test",
            "canvas_checksum": canvas_sha,
            "candidates": rows,
        }
        (set_dir / "candidate_set.json").write_text(
            json.dumps(manifest) + "\n", encoding="utf-8"
        )
        loaded, targets = review.load_candidate_set(set_dir, set_id, canvas_sha)
        assert loaded["transaction_id"] == "holder_test"
        assert len(targets) == 2

        evidence_path = root / "holder_evidence" / "manifest.json"
        evidence_path.parent.mkdir()
        evidence = {
            "schema_id": review.EVIDENCE_SCHEMA,
            "schema_version": 1,
            "source_image_sets_modified": False,
            "physical_state": {"state_id": "holder_installed_dish_absent"},
            "operational_candidate_assessment": {"status": "passed"},
            "camera_observations": [
                {"arena_id": row["arena_id"], "camera_serial": row["camera_id"]}
                for row in targets
            ],
        }
        evidence_path.write_text(json.dumps(evidence) + "\n", encoding="utf-8")
        _, checksum = review.validate_evidence(evidence_path, targets)
        assert checksum.startswith("sha256:") and len(checksum) == 71
        ready_status = {
            "state": "ready_for_review",
            "candidate_set_id": set_id,
            "homography_role": "operational_candidate",
            "canvas_checksum": canvas_sha,
            "revalidation": {"status": "passed"},
        }
        assert review.matching_review_status(ready_status, set_id, canvas_sha)
        assert not review.matching_review_status(
            {**ready_status, "revalidation": {"status": "failed"}},
            set_id,
            canvas_sha,
        )

        rows[0]["homography_role"] = "validation_only"
        bad_path = set_dir / "arena_1_2010093" / "candidate.json"
        bad_path.write_text(json.dumps(rows[0]) + "\n", encoding="utf-8")
        try:
            review.load_candidate_set(set_dir, set_id, canvas_sha)
        except ValueError:
            pass
        else:
            raise AssertionError("validation-only homography candidate was accepted")
    print("review_holder_operational_homography_candidates_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
