# Recording subject references (MetaZebrobot identifiers per camera)

Date: 2026-10-05. Code: `src/recording_subject_reference.{h,cpp}`; gate in
`orange::session::write_recording_session_manifest`; schema
`docs/schemas/orange_recording_subject_reference_v1.schema.json`.

## Why

Citrus-bound sessions get subject and Zebrobot metadata from Citrus's own
MetaZebrobot lookup in the unified H5. Orange recording-only sessions
(`recording_intent = recording_only`, no Citrus) collected nothing biological.
Palette's intake decision (2026-10-05, "design c"): Orange records only
**stable identifiers** at record start, or declares explicitly that none were
collected; Palette fetches the biological record at intake and pins it by
`(dish_uuid, revision)`, so MetaZebrobot stays the single owner of biological
truth and Orange needs no Citrus and copies no biology. Declared absence is
admitted; absence is never inferred from a missing block.

## Configuration

App config (GUI) / experiment spec `fixed` (headless):

```json
"zebrobot": { "base_url": "http://delahantyj-ws1.hhmi.org", "timeout_ms": 2000 },
"subject_references": {
  "schema_version": 1,
  "default": { "dish_id": "19220_1", "subject_count": 3 },
  "cameras": { "2010094": { "dish_id": "19220_2" }, "2010095": { "subject_count": 1 } }
}
```

- `zebrobot.base_url` is `http://` only (Orange's client is a minimal socket
  GET; no TLS); `timeout_ms` bounds connect + send + receive, 100..10000.
- Each field resolves per camera: the camera's own value, else `default`,
  else undeclared. No dish is a declaration (`not_collected`,
  `no_dish_declared`), never a refusal.
- `subject_count` (Palette request 2026-10-05) is the number of animals this
  camera records, declared by the operator; it is independent of `status`
  (a camera with no dish can still declare it) and is **never derived** from
  the dish `fish_count` or `dish_fish`. Emitted as an integer ≥ 1 or `null`;
  Palette publishes an experiment setup only when it is non-null.
- GUI: "Subject (MetaZebrobot dish per camera)" under the media products
  selection: dish id and subject count (0 = not declared); "Save subject
  declaration" writes `recording.subject_references`.

## What Orange records, at record start

Per recording camera, one `orange.recording_subject_reference` v1 entry:

1. `GET {base_url}/dishes/{dish_id}/citrus-snapshot` (API `schema_version` 2):
   `dish = {dish_id, dish_uuid, revision, updated_at}` copied **as served**.
   The declared id must equal the served `dish_id`.
2. When that succeeded, `GET {base_url}/dishes/{dish_id}/fish`:
   `dish_fish = [{fish_id, revision, updated_at}]` for every served item, with
   `dish_fish_lookup.status = complete` (an empty list is a completed lookup).
   `dish_fish` are the fish subjects **registered to the dish**, not the fish
   imaged; Orange selects nothing.
3. Any failure is declared, never substituted: `status = lookup_failed` with
   `zebrobot.error {kind transport|http, detail_error, message}` and a
   `reason` (`dish_lookup_http_404`, `dish_lookup_transport_failure`,
   `dish_lookup_api_schema_mismatch`, `dish_lookup_missing_<field>`,
   `dish_lookup_identity_mismatch`, `zebrobot_not_configured`). A failed fish
   lookup keeps `status = collected` with `dish_fish_lookup.status = failed`
   and must never be read as "no individuals".

The lookups run once, bounded by `timeout_ms`, and **never block a record
start**. The block is frozen into the sealed `recording_snapshot_start.json`
(`session.subject_references`) before capture and copied verbatim into every
`recording_session.json` by the common manifest writer, which refuses a
manifest carrying a different block and requires membership equal to the
manifest's cameras. Rolling rollover, external-recorder rebuilds and post-hoc
refreshes keep it. The synthetic bundle tool emits `not_collected /
synthetic_bundle` per camera.

## Palette's rules (recorded)

- `collected` ⇔ non-null `dish` and null `zebrobot.error` (and `http_status`
  200); `lookup_failed` ⇔ non-null `zebrobot.error`; `not_collected` ⇔ null
  `zebrobot` and null `dish`.
- Every camera parent has an entry; absence is never implied by omission.
- Values as served; no biological fields (genotype, dof, ...).
- Emitted for every session, stimulus-bound included; for those Palette
  cross-checks Orange's `dish_uuid` against the one Citrus writes into the H5
  `/metadata/subject` and refuses a mismatch.
- Lives in `recording_session.json`, which the transfer snapshot already
  hashes: no transfer-v2 schema change.

## Validation

- `scripts/validate_gui_ptp_recording.py`: block present (warn when absent),
  exact camera membership, per-entry rules, identical to the frozen snapshot
  block, lookup failures reported as warnings.
- `scripts/verify_timed_recording.py`: same membership and status rules.
- Tests: `tools/recording_subject_reference_tests.cpp` (configs, every
  builder outcome against a fake HTTP function, the block rules, the gate) run
  by `tools/recording_subject_reference_tests.py`, which also starts a local
  stand-in server so the real client is exercised for 200 JSON, chunked
  bodies, the structured 404, a stalled response and a refused connection.

## MetaZebrobot contract (owner-pinned; Orange links, does not copy)

Pinned by MetaZebrobot on 2026-10-05, repo `github.com/jmdelahanty/metazebrobot`,
commit `33c0f8e442dd8ca6e1a929422ae36831baef86f5` (main):

- Response shapes: `docs/api/consumer_openapi.json`, sha256
  `cb3733267484bf8869826ba35ddfc7eefcfb59dc50435801ac6d0d62daed8495`
  (verified by Orange against the committed file; paths `/acquisition/dishes`,
  `/dishes/by-uuid/{dish_uuid}`, `/dishes/{dish_id}`,
  `/dishes/{dish_id}/citrus-snapshot`, `/dishes/{dish_id}/fish`,
  `/fish/{fish_id}`, with the 404/503 `ApiErrorResponse` models).
  `pixi run python scripts/export_consumer_openapi.py --check` prints
  `OK <sha256>`; `tests/test_consumer_contract.py` guards drift.
- Meaning: `docs/zebrobot_snapshot.md`, sections "Identity and change
  detection", "API errors" and "Contract and stability".

Orange's reliance set, every field of which is in the pinned slice:
`/dishes/{dish_id}/citrus-snapshot` v2 top-level `dish_id`, `dish_uuid`,
`revision`, `updated_at`, `schema_version`; `/dishes/{dish_id}/fish` items
`fish_id`, `revision`, `updated_at`; 404/503 `ApiErrorResponse` `detail.error`.
Orange refuses a snapshot whose `schema_version` is not 2 (declared
`lookup_failed`); v2 only gains fields, and any removal or rename ships as a
new `schema_version`. MetaZebrobot restarted the service on 2026-10-05 17:05 EDT on that
commit; the consumer slice of the live `/openapi.json` is byte-identical to
the pinned file, so either may be checked. Orange re-verified the live
snapshot (`schema_version` 2, `dish_uuid`, `revision`) after the restart.
