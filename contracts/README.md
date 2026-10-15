# Pinned contract files

`frame_identity_v1.json` is a byte copy of agent-contracts
`citrus-orange/frame_identity_v1.json` at agent-contracts main 45a93ca2 (PR #87 merged
2026-10-12; byte-identical to the PR's 6965cc6b, which it contains). agent-contracts is
private, so CI reads this copy. Update it only together with
the agent-contracts change, and record the new commit here.

`output_names_allowlist.json` lists names `scripts/check_output_names.py` accepts
although they look like unsuffixed frame references, each with its reason.

`digest_form_v1.json` is a byte copy of agent-contracts `orange-palette/digest_form_v1.json`
(PR #90, pending merge). `digest_form_allowlist.json` lists the digest fields
`scripts/validate_output_schemas.py` still accepts in non-token form, grouped by family (A-F
in the contract), each with its reason; it only shrinks.

`palette_canonical_onnx_model_manifest_v2.schema.json` is a byte copy of Palette
`src/fisheye/shared/contracts/` at 9c9746ea2a88aa84758c1e9099429a885ede40d5 (Palette PR #388,
live intake since 2026-10-13), sha256 50916e1f…657d. Orange accepts canonical ONNX manifests of
version 2 only: the v1 pin (sha256 6b218c13…94d8, bare-hex payload_digest) was removed on
2026-10-13 after both production engines were rebound to v2 manifests (Palette re-promoted the
pose model, #391); Palette keeps reading v1 for historical records. An ONNX manifest without a
schema_id (the detect training export manifest) is only hashed. Update them only with a Palette
change, and record the new commit here.

`citrus_subject_reference_v2.schema.json` (sha256 30f42c9d…31eb9) and
`citrus_recording_observation_finalized_receipt_v4.schema.json` (sha256 47d22512…6b83) are byte
copies of agent-contracts `citrus-orange/citrus_subject_reference_v2.schema.json` and
`citrus-recording-transfer-3.1/citrus_recording_observation_finalized_receipt.v4.schema.json` at
agent-contracts main 2aaa7bc (PR #105 merged 83f9ed5; subject identity v4). Receipt v4 carries
reference v2 and always answers an acceptance v3; receipt v3 keeps reference v1. Sealer 3.3.0,
Citrus and Palette pin the same digests. `tools/validate_recording_observation_bindings_tests.py`
checks the digests and validates the fixtures in `tools/fixtures/citrus_subject_v4` against them.
