# Citrus subject identity v4 fixtures

Byte copies from agent-contracts main 2aaa7bc. Update them only together with an
agent-contracts change, and record the new commit here.

- `reference_v2/accepted`, `reference_v2/refused`: `citrus-orange/fixtures/subject_reference_v2/`
  (5 accepted, 6 refused `citrus.subject_reference` v2 documents).
- `example_receipt_v4.json`: `citrus-recording-transfer-3.1/example_receipt_v4.json` (a hand-made dish_group).
- `witnesses/`: `citrus-palette/fixtures/subject_v4/` (PR #115). These are the receipt v4 and acceptance v3
  of each sealed session from Citrus's production writer (Citrus 368dc42). The H5 files stay in
  agent-contracts. `README.citrus.md` is Citrus's description of every case.
  - The two `_refused` sessions fail on H5 content (an attribute type; an H5 that no longer matches
    its receipt). Their receipt and acceptance documents are valid, and Orange's tests treat them so.

The schema pins are in `contracts/` (see `contracts/README.md`). Tests:
`tools/recording_observation_binding_tests.cpp` (C++ validator, reseal of every witness,
declaration/receipt agreement, receipt v4 finalize) and
`tools/validate_recording_observation_bindings_tests.py` (Python checker, pinned schemas).
