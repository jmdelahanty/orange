# Subject reference v3: the individual under each camera (DRAFT, 2026-10-10)

Status: schema drafted, nothing wired. To integrate after the 2026-10-10
"snow" sessions. Jeremy's ask (2026-10-09): each fish has a user-given alias
beside its canonical id; it should be in Orange's and Citrus's records,
entered once.

## What v2 cannot say

Orange's per-camera entry is a dish plus a subject count, with every fish of
the dish copied as served. With one plate dish (e.g. 19234_3, ten fish
Z1-Z10) declared on four cameras, each camera's record lists all ten fish and
cannot say which fish sits under which camera. The fish Citrus picks per
arena is in Citrus's H5 only, and no record carries the alias.

## v3 (`docs/schemas/orange_recording_subject_reference_v3.schema.json`)

All new fields are optional; a v2-shaped entry with `schema_version` 3
validates (checked against tonight's real lookup of dish 19234_3), as does
the full example `docs/schemas/examples/orange_recording_subject_reference_v3_example.json`.

- `dish_fish[].subject_label`: MetaZebrobot's label as served by
  `GET /dishes/<id>/fish` (Z1-Z10 are served there today).
- `subject`: the one individual under this camera: `fish_id`, `source`
  (gui | spec | citrus_binding), and what `GET /fish/{fish_id}` served at
  record start (revision, updated_at, subject_label, housing_unit, dish_id)
  with its own `lookup` status; `dish_id` must equal the entry's dish when
  both are present.
- `binding_subject`: verbatim copy of the subject identity Citrus returned
  in the binding acceptance for the arena (Citrus-local `citrus_subject_id`
  UUIDv4, subject_type, subject_count, dish and fish refs with revisions,
  subject_label, `acceptance_sha256`, observation_context_id), so a bound
  recording and its H5 name the same subject.
- `operator_subject_alias`: free text typed by the operator, 1-128
  characters, trimmed, no control characters, with provenance. Discouraged:
  the alias should live once, in MetaZebrobot's `subject_label`.

## Integration plan (Orange)

1. `src/recording_subject_reference.{h,cpp}`: `kSubjectReferenceSchemaVersion`
   3; `SubjectDeclaration` gains `fish_id`; copy `subject_label` into
   `dish_fish`; resolve the selected fish through `/fish/{fish_id}` (one GET,
   bounded timeout, never blocks the start); emit `subject`, and
   `binding_subject` when a binding acceptance carries one.
2. GUI Subject panel (`src/gui/registered_context_recording.cpp`): a fish
   picker per camera fed by `/dishes/<id>/fish` labels, plus the optional
   alias field. Headless spec: `fixed.subject_references.<serial>.fish_id`
   (and `.cameras` in the app config).
3. Binding: when the acceptance (Citrus, format change) carries the subject
   block, Orange copies it and uses its fish as `subject` with source
   citrus_binding; a mismatch between the operator's pick and the binding's
   fish refuses the start.
4. Validators (`src/recording_subject_reference.cpp` ValidateSubjectReferences,
   `scripts/validate_recording_artifacts.py`, `validate_gui_ptp_recording.py`)
   accept versions 1 | 2 | 3 and refuse mixed blocks; tests in
   `tools/recording_subject_reference_tests.{cpp,py}` with the local HTTP
   stand-in serving `/fish/{id}`.
5. Palette re-pins v3 (same intake rules: dish_uuid cross-check, plus
   `subject.fish_id` == H5 `mzb_fish_id` when both exist); Citrus adds the
   subject block to the acceptance; MetaZebrobot's pinned consumer endpoints
   are unchanged (`/fish/{fish_id}` is already pinned).

## Tomorrow without v3

Subject panel: dish id 19234_3 and subject count 1 on every camera; Citrus
picks the well's fish per arena; the alias resolves through fish_id and
revision in MetaZebrobot.
