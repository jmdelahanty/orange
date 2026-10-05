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
  "default": { "dish_id": "19220_1" },
  "cameras": { "2010094": { "dish_id": "19220_2" } }
}
```

- `zebrobot.base_url` is `http://` only (Orange's client is a minimal socket
  GET; no TLS); `timeout_ms` bounds connect + send + receive, 100..10000.
- A camera takes its own `cameras` entry, else `default`, else no dish. No
  dish is a declaration (`not_collected`, `no_dish_declared`), never a refusal.
- GUI: "Subject (MetaZebrobot dish per camera)" under the media products
  selection; "Save dish id" writes `recording.subject_references`.

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

## MetaZebrobot facts the implementation relies on (live, 2026-10-05)

- `/dishes/{dish_id}/citrus-snapshot` v2 is flat: `dish_id`, `dish_uuid`,
  `revision`, `updated_at` at the top level (plus biology, not copied).
- `/dishes/{dish_id}/fish` returns `{"items": [...]}` from `fish_subjects`;
  items carry `fish_id`, `dish_id`, `dish_uuid`, `subject_label`,
  `current_unit_id`, `revision`, `updated_at`.
- Errors: 404 `{"detail": {"error": "dish_not_found", "dish_id": ...}}`,
  503 `database_error`.
- Revisions are trigger-bumped on every row write; there is no as-of-time
  read, so Palette compares the recorded revision with the one it fetches.
