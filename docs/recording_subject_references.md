# Recording subject references (MetaZebrobot identifiers per camera)

Date: 2026-10-05. Code: `src/recording_subject_reference.{h,cpp}`; gate in
`orange::session::write_recording_session_manifest`; schemas
`docs/schemas/orange_recording_subject_reference_v1.schema.json` (recordings
before 2026-10-05) and `..._v2.schema.json` (**emitted since 2026-10-05**, see
"Schema version 2" below; Palette's pin merged on Palette main as
a625279c, PR #258).

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

Per recording camera, one `orange.recording_subject_reference` v2 entry:

0. Once per start, when any camera declares a dish: `GET {base_url}/version`;
   `service_commit`, `service_commit_dirty` and `consumer_schema_sha256` are
   copied into every entry's `zebrobot` block when they match the schema
   patterns (7..40 hex, boolean, 64 hex), else left null. A failed read is
   null, never a failure of the entry.
1. `GET {base_url}/dishes/{dish_id}/citrus-snapshot` (API `schema_version` 2):
   `dish = {dish_id, dish_uuid, revision, updated_at}` copied **as served**.
   Time semantics (MetaZebrobot `docs/zebrobot_snapshot.md` "Dates and
   times", commit 49cc930): `updated_at` (and `created_at`,
   `cache_updated_at`) are **UTC without an offset** ("2026-10-02 16:28:20"
   is 12:28 EDT); calendar days such as `dof` are lab-local America/New_York
   days; `*_utc` fields carry an explicit `Z`. Orange copies these strings
   verbatim and derives no dates or ages from them; a lab calendar day from a
   UTC instant (e.g. `queried_at_utc`) is obtained by converting to
   America/New_York first. The pinned schema files are unchanged (their
   digests are Palette's pins).
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

## Schema version 2 (emitted since 2026-10-05)

Palette pins the schema by file digest and every object in it is closed
(`additionalProperties: false`), so new fields need a new schema version, not
an addition to v1. `orange.recording_subject_reference` **v2**
(`docs/schemas/orange_recording_subject_reference_v2.schema.json`) differs
from v1 only in `schema_version: 2` and three new required keys in the
`zebrobot` block, read once at record start from MetaZebrobot `GET /version`
(adopted by MetaZebrobot 2026-10-05, commit 509a3eb8; not live until the
service restarts):

| Field | Type | Meaning |
| --- | --- | --- |
| `service_commit` | string (7..40 hex) or null | git SHA of the code the MetaZebrobot process loaded |
| `service_commit_dirty` | boolean or null | the process ran with uncommitted changes to tracked files |
| `consumer_schema_sha256` | string (64 hex) or null | digest of the consumer OpenAPI slice computed from the running app; Palette cross-checks it against its MetaZebrobot pin |

All three are null when MetaZebrobot served null, or when the version read
failed or was not attempted (a `not_collected` entry still has `zebrobot:
null`). The version read shares the lookup's timeout and never blocks a
start. Checked with jsonschema (Draft 2020-12): v1 entries are refused by v2
and v2 entries (and v1 entries carrying the new fields) are refused by v1, so
the two files never admit each other's recordings. Digests:

| File | sha256 |
| --- | --- |
| `orange_recording_subject_reference_v1.schema.json` | `3c4ba74f0f95f76dbb8b3d65aa8bd39fd409383388ee8debec3845ef26c1a930` (Palette pin) |
| `orange_recording_subject_reference_v2.schema.json` | `d0f300fdbd71747f44219baf7bb5aea262ebbd275f85f877df9f1a00aa92e935` |

Order of operations (as agreed with Palette, completed 2026-10-05): v2 was
published on agent-contracts PR 53 (879ae3f), Palette pinned the digest and
accepts both versions (palette PR #258, merged on main as a625279c, CI run
37394130991), and only then did Orange switch `kSubjectReferenceSchemaVersion`
to 2, add `ReadZebrobotVersion` (one GET per start) to `BuildSubjectReferences`,
and extend the emitted-block validator, both Python validators (v1 or v2 per
entry; a block mixing versions is refused, as Palette refuses it) and the
tests (fake routes, the stand-in server's `/version`). Recordings made under
v1 keep validating against the v1 pin. Palette records the served build and
whether `consumer_schema_sha256` matches its MetaZebrobot pin (f5280e43…); a
mismatch is recorded, not refused.

## MetaZebrobot contract (owner-pinned; Orange links, does not copy)

Pinned by MetaZebrobot on 2026-10-05 (second pin, superseding 33c0f8e4 /
cb373326 after `GET /version` joined the slice), repo
`github.com/jmdelahanty/metazebrobot`, commit
`509a3eb88d6ff44fe07e7ea20d212be5eafe46b7` (main):

- Response shapes: `docs/api/consumer_openapi.json`, sha256
  `f5280e430d4b5f10c3643cb89a6187eacc45fdac7af55b2e81f5e315b7b754dc`
  (verified by Orange against the committed file at that commit; paths
  `/acquisition/dishes`, `/dishes/by-uuid/{dish_uuid}`, `/dishes/{dish_id}`,
  `/dishes/{dish_id}/citrus-snapshot`, `/dishes/{dish_id}/fish`,
  `/fish/{fish_id}`, `/version`, with the 404/503 `ApiErrorResponse` models).
  `pixi run python scripts/export_consumer_openapi.py --check` prints
  `OK <sha256>`; `tests/test_consumer_contract.py` guards drift; a running
  service reports the digest as `consumer_schema_sha256` on `GET /version`.
- Meaning: `docs/zebrobot_snapshot.md`, sections "Identity and change
  detection", "API errors", "Contract and stability" and "Which MetaZebrobot
  am I talking to?".
- Consumer expectations: agent-contracts `metazebrobot-consumers/`
  (PR 54): `consumers.json` holds the producer pin and each consumer's
  `relies_on`; `verify_consumers.py` (stdlib only) checks them against the
  pinned file, or with `--live http://<host>` against the running service
  (`/version` digest and `/openapi.json`). PR 54 merged on 2026-10-05
  (agent-contracts main 5fc735fe, `consumers.json` sha256 8d2b3878…);
  `scripts/check_metazebrobot_consumer_pin.sh [--live http://host]` fetches
  both files at that commit, checks the digest and runs the verifier for
  Orange. It runs in Orange CI (`.github/workflows/ci.yml`) and before every
  rig day (Shadow runbook precheck). On 2026-10-05 the pinned check passed for Orange, and the live
  check passed after the service restart (below).

Orange's reliance set, every field of which is in the pinned slice:
`/dishes/{dish_id}/citrus-snapshot` v2 top-level `dish_id`, `dish_uuid`,
`revision`, `updated_at`, `schema_version`; `/dishes/{dish_id}/fish` items
`fish_id`, `revision`, `updated_at`; 404/503 `ApiErrorResponse` `detail.error`;
and, for schema v2, `/version` `service_commit`, `service_commit_dirty`,
`consumer_schema_sha256`. Orange refuses a snapshot whose `schema_version` is
not 2 (declared `lookup_failed`); v2 only gains fields, and any removal or
rename ships as a new `schema_version`. MetaZebrobot restarted the service on
2026-10-05 17:05 EDT on 33c0f8e4 and again at 17:18:59 EDT (21:18:59Z) on
509a3eb8. After the second restart `GET /version` serves
`service_commit` 509a3eb8…, `service_commit_dirty` false (the running code
is exactly that commit), `consumer_schema_sha256` f5280e43… (equal to the
pin), `api_schema_version` 2, `started_at_utc` 2026-10-05T21:18:59Z, and
Orange's live verifier run passed (every expectation present in the live
`/openapi.json`, digest equal to the pin).
