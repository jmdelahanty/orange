# Citrus subject declaration v2 fixtures

Byte copies of agent-contracts `citrus-orange/fixtures/subject_declaration_v2/` at main 47b3459
(#126 merged df49b8d, erratum #127 merged 47b3459: acceptance v4 outer and inner schema_version 4).
Update them only together with an agent-contracts change, and record the new commit here.
`request_two_arenas.json` (Citrus's declare_subjects request) is not copied; Orange does not read it.

- `accepted/`, `refused/`: `citrus.subject_declaration` v2 documents (7 accepted, 9 refused;
  `refused/expected.json` says whether the schema or the contract test's D1-D3 refuses each).
  `refused/v1_document.json` is a valid v1 document, refused only as v2; inside an acceptance v4
  it is accepted, as v4 carries v1 or v2.
- `acceptance_v4_v1_subject.json`, `acceptance_v4_v2_subject.json`: acceptance v4 envelopes. Their
  `contract_sha256` / `acceptance_id` are schema-shape placeholders (the same value in both, not the
  contract's digest), so Orange's tests reseal the contract instead of comparing the envelope.

Pin: `contracts/citrus_recording_observation_binding_acceptance_v4.schema.json` (sha256 f0a4da8a…62f3).
Tests: `tools/recording_observation_binding_tests.cpp` (validator, pairing, pre-arm cross-check) and
`tools/validate_recording_observation_bindings_tests.py` (Python checker, pinned schema).
