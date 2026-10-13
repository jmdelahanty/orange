# Schema-first Orange outputs: design

Status: proposal, 2026-10-12. Decision asked of Jeremy: approve the approach and the first
conversion (event logs together with the model-identity family).

Every Orange output is defined once, by its published schema in `docs/schemas/`. A generator
turns each schema into the C++ that writes and reads it and into the Markdown that documents it.
A writer can then only produce what the schema says, a reader rejects anything else, and the docs
always match. Today the writers build JSON field by field in about 100 places, the 60-odd schema
files are written by hand, and nothing ties the two together except the ratchets added on
2026-10-12.

## 1. What is the definition

| Output kind | Definition | Example |
|---|---|---|
| JSON document | the JSON Schema file Palette already pins (`docs/schemas/<id>_v<N>.schema.json`) | `orange_external_recorder_frame_identity_proof_v3` |
| JSONL line family | one schema whose root `oneOf` branches are the line kinds, selected by a const field | `orange_yolo_event_v3` (`event_kind` = `session_header` / `spatial_mask_policy` / `yolo_result`) |
| CSV file | a column schema `docs/schemas/csv/<id>_v<N>.columns.json` (new, see 1.2) | recorder `Cam*_external_meta.csv` |

The schema stays the published contract; nothing about Palette's pinning changes.

### 1.1 Generator annotations

A small set of `x-orange-*` keywords, ignored by JSON Schema validators, carry what the C++ needs:

| Keyword | Meaning |
|---|---|
| `x-orange-cpp-name` | type name of the generated struct (default: from `$id`) |
| `x-orange-decimals` | numbers are rounded to this many decimals before writing (event logs: 3 for px, 4 for confidence, 3 for ms) |
| `x-orange-counter` | which frame counter a field holds (`recording_frame_id`, `stream_frame_id`, `camera_frame_id`), checked against agent-contracts `frame_identity_v1` |
| `x-orange-digest` | the field is a digest; the generator emits a `sha256:` token type (agent-contracts `digest_form_v1`) |
| `x-orange-map-key` | type of an object's keys for `additionalProperties` maps (e.g. camera serial) |

### 1.2 CSV column schema

```json
{
  "schema_id": "orange.external_recorder.frame_metadata_csv",
  "schema_version": 3,
  "columns": [
    {"name": "recording_frame_id", "type": "uint64", "counter": "recording_frame_id"},
    {"name": "timestamp", "type": "uint64", "unit": "ns", "clock": "camera_ptp_tai"},
    {"name": "timestamp_sys", "type": "uint64", "unit": "ns", "clock": "host_realtime"}
  ]
}
```

It is itself validated by a meta-schema in `docs/schemas/csv/`. The header row is the column
names in order; readers check it exactly.

## 2. What is generated

For each schema, at build time (CMake custom command, outputs in the build tree, nothing generated
is committed except docs):

- **C++ struct** per object, typed fields in schema order: `std::optional<T>` for optional fields,
  an `enum class` for string enums and consts, nested structs for nested objects, `std::map` for
  keyed maps, a token type for digests.
- **Writer** `to_json(const T&) -> nlohmann::json`: the same objects the hand-written code builds
  today (nlohmann sorts keys; compact dump for JSONL, `dump(2)` plus a newline for JSON files via
  `outputs/common/atomic_file.h`), with the declared rounding. So a converted writer can be
  byte-identical to the current one.
- **Reader** `parse(const nlohmann::json&, T*, std::string* error) -> bool`: exact key sets,
  types, enums, `schema_id` / `schema_version`, ranges and patterns from the schema. It replaces
  the hand-written "exact keys" helpers (10 copies).
- **CSV**: a row struct, a writer that emits the header and formats each row (escaping via
  `outputs/common/csv.h`), and a reader that checks the header and parses typed rows.
- **Docs**: `docs/outputs/<id>_v<N>.md`, a field table (name, type, unit, counter, required,
  description) per schema, committed, so reviews see the doc change next to the schema change.
- **Python**: no generated code. Scripts keep validating with `jsonschema` against the same files;
  `scripts/validate_output_schemas.py` already does this for whole recordings.

### Supported schema subset

The generator accepts a deliberate subset and refuses anything else, so a schema never means more
than the generated code enforces: `type` (object, array, string, integer, number, boolean, null
via a type array), `properties`, `required`, `additionalProperties` (false, or a schema for maps),
`items`, `enum`, `const`, `minimum` / `maximum`, `minLength` / `maxLength`, `pattern`, `oneOf` with
a const discriminator, local `$ref` and `$ref` to another file in `docs/schemas/`, `$defs`.
`anyOf`, `else`, `patternProperties`, `dependentSchemas` are refused. Published schemas are
immutable, so the subset grew to cover what they already use (2026-10-12): an `allOf` of `{if,
then}` rules on an object (`if` tests const / enum values, `then` constrains const, enum, type,
lengths, pattern and ranges, both below nested `properties`, with JSON Schema's semantics: an
absent value satisfies the condition and is not constrained), arrays without `items` and
multi-type unions kept as JSON checked against the declared types, and optional nullable fields
as `std::optional<std::optional<T>>` (absent vs null). Also (2026-10-13): a whole-file `$ref`
to a sibling schema (that file's generated type, `--ref stem=namespace`), a `oneOf` below the
root (a `std::variant`; exactly one branch must parse), `required` in `then`, and optional consts
that keep their presence.

## 3. Generator

`tools/schema_codegen/` in Python (standard library plus `jsonschema` to check each schema against
its meta-schema), one run per build for all schemas. Python rather than C++ templates because the
generator is not on any runtime path, and it can share the schema-reading code with the
validator (`scripts/published_schemas.py`).

Considered and not chosen:

- **quicktype** (JSON Schema to C++ with nlohmann): a Node dependency on the rig, no control over
  rounding, strictness or key rules, and it accepts schema features it cannot enforce.
- **Code-first** (C++ types, schema generated from them): needs a reflection layer C++ lacks, and
  makes Orange's code, not the published file, the definition Palette pins.
- **Protocol Buffers / FlatBuffers**: a different wire format; the outputs are JSON, JSONL and
  CSV by contract (see the SHAMAN discussion for why binary formats did not fit there either).

## 4. Ratchets that keep it honest

Existing, unchanged: frame-identity name lint, output surface snapshot with the version-bump rule,
published-schema-file rule, digest form, writer-output validation.

Added with the generator:

- **Generated docs are current**: CI regenerates `docs/outputs/` and fails on a diff.
- **Golden bytes**: for each converted writer, a test builds a fixed struct and compares the
  generated writer's bytes with a checked-in golden file. A conversion lands only when the golden
  file equals what the hand-written writer produced, so the refactor provably changes no output.
- **Round trip**: `parse(to_json(x)) == x` for every golden struct, and every
  `docs/schemas/examples/` file parses.
- **No hand-written writer for a converted family**: the name lint learns which schema ids are
  generated and fails if their `schema_id` string literal appears outside generated code.

## 5. Converting a family

Each family goes through the same steps, each its own commit, each with the full suite green:

1. Complete the schema: every field the writer emits, the `x-orange-*` annotations, the CSV
   column schema if the family has CSVs. Validate real outputs (gate recording, synthetic bundle)
   against it with `validate_output_schemas.py`.
2. Generate; add golden-bytes tests from the current writer's output.
3. Switch the writer to the generated struct; goldens must still match.
4. Switch Orange's readers to the generated parser; delete the hand-written parsing and exact-key
   helpers for the family.
5. Contract fixes (digest tokens, names) as one versioned change: bump the schema version, publish
   the new file, reference it from agent-contracts, Palette pins, then Orange switches. The digest
   allowlist loses the family's entries.

Steps 1-4 change no output byte and need no coordination. Step 5 is the only one that does.

## 6. Order

| # | Family | Why this order | Digest families fixed |
|---|---|---|---|
| 1 | Event logs (yolo / pose v3) with model identity: engine manifest (Python writer), snapshot `models` block, `model_artifacts`, `model_ref` | Schemas exist, all readers are in C++, Palette pins them, model identity spans them | A |
| 2 | Recorder outputs: keyframes, frame identity proof, metadata CSV (merge its two writers), summary and status (no schema yet) | Palette pins proof and keyframes; the CSV gets the first column schema | — |
| 3 | Recording session: frame identity, acquisition mapping, timestamp clocks, realtime products, clip index, then the session envelope | Largest; builds on 1 and 2 | B |
| 4 | Geometry and calibration: geometry contract and assets, image sets, observations, registered context | Many GUI writers; crop-only depends on it | C, F |
| 5 | Crop: crop metadata, master journal, moving-crop receipts, recorder importance map | Crop-only is not yet run on the rig | D |
| 6 | Diagnostics CSVs, driven from `timing/diagnostics_index.h` | Least consumer impact | — |

Family E (SHAMAN `recording_identity_token`) is a fixed shared-memory field; the digest contract
is amended to name identity tokens `*_token` instead of renaming it.

## 7. First conversion in detail: event logs and model identity

1. Complete `orange_yolo_event_v3` / `orange_pose_event_v3` (session header model blocks, spatial
   mask policy, result lines) and write the missing schemas: `orange.analytics.spatial_mask_runtime`
   v1, `orange.tensorrt_engine_manifest` (published v1 exists; verify against the Python writer),
   the snapshot `models.<serial>.<detect|pose>` block.
2. Golden tests from the 2026-10-11 gate recording's lines (header, policy, results with
   detections, zero detections, timeouts) and its snapshot model blocks.
3. Generated writers in `yolo_event_log.cpp`, `pose_event_log.cpp`, `gui/recording_snapshots.cpp`
   (model blocks), `session/model_artifacts.cpp`, `session/realtime_products.cpp` (model_ref).
4. Generated readers in `event_log_reader.h`, the two event-log validators,
   `recording_master_crop_coverage.cpp`, `model_identity.cpp`.
5. Versioned change with Palette: event logs v4, engine manifest v2, snapshot models block
   (snapshot gains a `schema_id` and version), all digests as tokens, `payload_digest` and
   `engine_sha256_declared` renamed. The Python engine-manifest writer changes in the same step.

Size estimate: steps 1-4 about two days of work with the suite green throughout; step 5 depends on
Palette's pin turnaround.

### Progress (2026-10-12)

- Event logs: writers (golden bytes) and readers on the generated v3 code.
- Engine manifest: `model_identity.cpp` reads it through the generated v1 parser; all 42 rig
  manifests round-trip, the snapshot block is byte-identical.
- Snapshot models block: the published `orange.recording_detect_model.v1` and
  `orange.recording_pose_model.v2` already described it (every snapshot since 2026-10-10
  matches); they are now generated and enforced by `validate_output_schemas.py` by path, since the
  block has no `schema_id`. Writer and readers not yet switched.
- Spatial mask runtime policy: new `orange.analytics.spatial_mask_runtime` v1 schema (all 564 real
  policies match, every mode of `policy_to_json` passes the generated parser in
  `yolo_spatial_mask_tests`). Writer and readers not yet switched.

- Readers of the models block (`session/snapshot_models.h`: model artifacts, realtime products)
  and of the policy (event-log reader, YOLO validator) are generated; realtime products scans the
  event logs with the generated v3 parsers. Old and new readers are byte-identical on 9 real
  recordings.
- Spatial mask arm (`analytics_runtime.yolo_spatial_mask`): new `orange.analytics.spatial_mask_recording_arm`
  v1 schema; all 394 real arm documents (GUI and headless) and all 2,858 real policies, including
  the 32 active ones of 2026-08-25, match. Writers not yet switched.

## 8. Open questions

- Snapshot and `ptp_sync_summary.json` have no `schema_id`; converting them adds one (step 5 of
  their family). Acceptable?
- `docs/schemas/` mixes Citrus-owned ids (`citrus.*`) that Orange only checks. The generator
  produces readers for those but never writers; Citrus stays their owner.
- Whether Citrus wants to consume the same generated readers for Orange's outputs (via
  `shaman-ipc`-style pinning of a generated header package) is a later decision.
