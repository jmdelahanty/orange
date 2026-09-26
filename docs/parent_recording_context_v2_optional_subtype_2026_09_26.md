# citrus.parent_recording_context version 2: optional recording_subtype

Date: 2026-09-26. Status: **proposal for the coordinated contract revision**
(Orange, Citrus, Palette). Orange's side is implemented and tested on the
integration branch; no live recording, canvas, validator on the Citrus or
Palette side, or transfer has changed.

## Why

The owner wants to omit `recording_subtype` for the Shadow recordings rather
than invent a classification. Today every path that carries the parent context
(Orange emission and manifest gate, the binding request v2 contract, the Citrus
acceptance parser and H5 session metadata, the transfer-v2 schema and Palette
intake) requires a non-empty subtype, so deleting it from the canvas alone would
block recording.

## The revision

`citrus.parent_recording_context` **version 2** is version 1 with exactly one
change: `recording_subtype` is optional.

- Omission means "not specified". The key is **absent**. `null`, `""`, and any
  substituted value (`dish_stimulus`, `unspecified`, ...) are invalid.
- An explicitly supplied subtype is preserved byte for byte, under the same
  producer-label rules as `recording_type` (non-empty, at most 1024 UTF-8
  bytes, well-formed UTF-8, no C0/DEL/C1 control, no leading or trailing
  ECMA-262 `\s` whitespace, never trimmed).
- `recording_type`, `behavior_mode`, `recording_intent`, `data_origin`,
  `schema_id`, `schema_version` stay required with their version-1 rules.
- Version 1 is unchanged and remains valid: subtype required.

### Emission rule (Orange)

Each entry is emitted with the version that describes it: **version 1 when a
subtype is present** (byte-identical to every recording made so far),
**version 2 when the subtype is omitted**. A recording may therefore mix
versions across cameras; each entry is self-describing and consumers validate
entries independently. Consumers that accept version 2 must accept a version-2
entry with or without a subtype.

### JSON Schema fragment (shared)

```json
"recording_context": {
  "type": "object",
  "required": ["schema_id", "schema_version", "recording_type",
               "behavior_mode", "recording_intent", "data_origin"],
  "properties": {
    "schema_id": {"const": "citrus.parent_recording_context"},
    "schema_version": {"type": "integer", "enum": [1, 2]},
    "recording_type": {"$ref": "#/$defs/recordingLabel"},
    "recording_subtype": {"$ref": "#/$defs/recordingLabel"},
    "behavior_mode": {"enum": ["free", "embedded", "none"]},
    "recording_intent": {"enum": ["stimulus_experiment", "recording_only"]},
    "data_origin": {"enum": ["acquired", "synthetic"]}
  },
  "additionalProperties": false,
  "if": {"properties": {"schema_version": {"const": 1}}},
  "then": {"required": ["recording_subtype"]}
}
```

`recordingLabel` is the definition in
`docs/schemas/orange_citrus_recording_observation_binding_request_v2.schema.json`
(minLength 1 excludes `""`; `null` fails the string type).

## Where omission must be preserved

| Path | Version-2 behaviour |
| --- | --- |
| Orange app config `recording.contexts` / GUI panel | key omitted (panel: blank subtype); `null`/`""` refused at load |
| Orange start snapshot, `recording_session.json` `recording_contexts` | entry emitted as version 2 without the key |
| Binding request v2 `contract.recording_context` | same entry, inside the digest |
| Citrus expected Arena context, acceptance, H5 `/evidence/recording_binding` | compare the closed object as-is; no default; H5 `/metadata/session` has **no** `recording_subtype` attribute |
| Citrus finalized receipt v1 | unchanged (carries no context) |
| Transfer-v2 schema / Palette intake | accept version 2; treat absence as not specified; never fill |

## Orange implementation (integration branch, 2026-09-26)

- `RecordingContext::recording_subtype` is `std::optional<std::string>`;
  `Parse` accepts an omitted key and refuses `null`/`""` with "omit the key";
  `ParseEmitted` accepts version 1 (subtype required) and version 2 (optional);
  `ToEmittedJson` emits version 1 with a subtype, version 2 without.
- GUI panel: "Recording subtype (blank = not specified)"; per-camera list shows
  "(no subtype)"; a hint names the version-2 consequence.
- Validators: `validate_gui_ptp_recording.py`, `verify_timed_recording.py`
  accept both versions with the closed key set per version;
  `validate_recording_observation_bindings.py` requires the H5 session
  metadata to omit `recording_subtype` when the accepted context omits it.
- Request v2 schema: `recording_context.schema_version` enum `[1, 2]` with the
  conditional `required` above.
- Tests: `recording_context_tests` (omission, `null`/`""` refusal, v1/v2
  emission and round trip, mixed block), `validate_recording_observation_bindings_tests`
  (v2 fixture without subtype passes; a substituted H5 subtype is refused).

## Status 2026-09-26 (later): Citrus accepted, Palette pending

Citrus implemented the revision on its review branch (their
`docs/parent_recording_context_v2_optional_subtype_2026-09-26.md`): v1 keeps
the subtype required, v2 permits omission, omission stays absent in the H5
session metadata and in transfer output, mixed context versions across cameras
are accepted, mixed binding-request versions in one batch stay invalid. No H5
metadata-version bump was needed (the session attribute was already
optional-by-policy). Their transfer schema now carries
`$id = urn:citrus:recording-transfer:parent-context-v2`, which Palette must
admit and pin explicitly. Joint software gate against Orange `a8a0234`: 8/8
transport cases (the original four plus four mixed-context cases, with
replay); Orange's native probe passes their three fresh fixture chains (v1,
subtype-free v2, explicit-subtype v2).

Orange cross-check on those three chains (this tree at `a8a0234`): sealed
request/acceptance v2 and receipt v1, context equal to the sealed start
snapshot, unified H5 layout with the embedded chain identical to the sealed
files, and the H5 `/metadata/session` attributes equal to the accepted
context with `recording_subtype` **absent** for the subtype-free chain and
present for the other two.

## Open on the other sides

- Citrus: done on the review branch (see status above); publish that slice.
- Palette: `recording_transfer_v2.schema.json` `recording_type`/`recording_subtype`
  block and `parent_recording_intake_v2` validation; intake metadata must
  carry "not specified", not a filled value.
- Joint: rerun the request-v2/acceptance-v2 transport cases with one
  subtype-free camera, then the Shadow live fixture with the subtype omitted.
