# orange.recording_start_declaration v2: scope (draft)

Status: scoping draft for review by experiment-spec (orchard), Palette,
pancake-plate, citrus-agent and gui-headless-unifier. Nothing here is built.
Jeremy, 2026-11-15: "Lets start trying to scope v2"; he is working with
experiment-spec on run plans Orange can consume. Field names follow orchard
run plan v1 and run record v0 (orchard PR #4, branch
agent/experiment-spec/run-plan-placement) where they exist.

## What v1 is, and what v2 adds

v1 (production since 8d8ef2d, `src/session/recording_start_declaration.h`):
`declared_by`, `recording_intent`, `subject_references` (per camera:
`dish_id`, `subject_count`). It reaches Orange as a local-control start's
`params.declaration` or a run spec's `fixed.declaration`; Orange writes it
into `subject_confirmation.json` and, for a run spec, seals the spec bytes as
`run_spec/<sha256>.json`.

v2 adds what v1 cannot say:

1. **Where the declaration came from** (a `source` block), so a recording
   names the signed plan, session and sign-off it was declared from.
2. **A stated blank**, so "this arena is empty on purpose" differs from "nobody
   said" (the plan states blank; Orange today cannot).

Unchanged on purpose (Jeremy's decisions): Orange carries dish and count, no
fish ids; members stay with Citrus and MetaZebrobot. Orange keys by camera
serial; arena-to-camera mapping is the generator's.

## Proposed shape

```json
{
  "schema_id": "orange.recording_start_declaration",
  "schema_version": 2,
  "declared_by": "delahantyj",
  "recording_intent": "stimulus_experiment",
  "subject_references": {
    "schema_version": 2,
    "cameras": {
      "2010093": {"dish_id": "19498_2", "subject_count": 5},
      "2010096": {"blank": true}
    }
  },
  "source": {
    "kind": "orchard_run_plan",
    "repo": "jmdelahanty/orchard",
    "commit": "<40 hex>",
    "path": "specs/<spec_id>/run_plans/<plan>.plan.yaml",
    "file_sha256": "<64 hex, the plan file's bytes at commit:path>",
    "tree_dirty": false,
    "plan_id": "...", "plan_revision": 2, "session_id": "S1",
    "plan_sha256": "<64 hex>", "plan_hash_rule": "canonical_json_utf8_sort_keys_compact_v1",
    "sign_off": {"entry_id": "...", "entry_sha256": "<64 hex>", "by": "delahantyj", "at_utc": "...",
                 "record_path": "specs/<spec_id>/run_plans/<plan>.record.yaml", "record_commit": "<40 hex>"},
    "generator": {"repo": "jmdelahanty/orchard", "commit": "<40 hex>", "tool": "tools/declare.py"}
  }
}
```

`source.kind` values: `orchard_run_plan` (generated from a signed plan),
`orange_run_spec` (typed into a run spec by hand), `operator` (a person at
the GUI or the client, no file behind it). v1 declarations have no `source`
and keep meaning what they mean today.

## Rules Orange would apply

- Closed keys at every level, as v1. `source` is required in v2 (no silent
  absence); `kind` decides which fields are required.
- `orchard_run_plan`: refused unless `tree_dirty` is false, the hashes are
  64 lowercase hex, `sign_off.by` equals `declared_by`, and every camera the
  recording records is listed (declared or `blank`). Jeremy: a signed run
  from an edited plan is refused.
- Orange records `source` as given (a claim) and does not fetch or re-hash
  orchard; the checks above are on the declaration itself. `plan_hash_rule`
  is recorded as given (form only: a short token); Palette, the verifier,
  may refuse a rule it cannot check, so a new rule is never an Orange release.
- `repo` is `owner/name` (`jmdelahanty/orchard`), not a remote URL.
- `sign_off.entry_sha256` is over that one run-record entry (canonical rule);
  the record file grows after sign-off, so its file hash is not used.
- The camera-list check is Orange's: Orange knows the recording cameras, so
  orchard does not need the rig's camera list.
- `blank: true` is exclusive with `dish_id` and `subject_count`. It is emitted
  in the subject reference as status `not_collected` with a new reason
  `declared_blank` (today's undeclared camera stays `no_dish_declared`).

## What each owner would need to change

- **Orange**: parser and writer for v2 next to v1 (v1 keeps working), the
  confirmation and spec-copy paths unchanged, the new reason value
  `declared_blank` in the existing subject reference (NO schema version bump:
  Palette accepts reference versions 1 and 2 only, so a v3 would refuse every
  recording at intake until Palette pins it), tests; the orchestrator and the run spec
  path take v2 as they take v1.
- **Palette**: accept declaration v2 inside `subject_confirmation.json`
  (#442/#443 follow-up) and the `source` checks; `declared_blank` needs no
  Palette change (an optional doc-only listing in agent-contracts).
- **orchard (experiment-spec)**: `tools/declare.py` emits v2 with `source`
  and `blank` for blank arenas (today it omits them).
- **pancake-plate**: whether the plan bytes travel with the recording (a copy
  next to `run_spec/`) or by the sha256 store; Jeremy's decision is pending
  with you.
- **Citrus**: none for Orange's declaration; its own subject declaration v2
  comes from the same plan and needs an agent-contracts entry first
  (citrus-agent); declare.py's citrus block carries rig_id and the protocol
  reference.

## Answers so far (experiment-spec, 2026-11-15)

- Unknown `plan_hash_rule`: record as given (above).
- `subject_type` from Orange: experiment-spec says no (Citrus is the single
  writer of type and members; count separates one from several). Palette to
  confirm.
- `kind: operator`: keep it; in v2 `source` is required, and a stated kind is
  easier for Palette than absence.
- Who writes the run spec: the generator can only run after loading, minutes
  before START, so the per-session run spec cannot be committed ahead.
  Proposal: a committed run-spec **template** in Orange's repo (cameras,
  encoders, modes; named by the plan's `orange.preset`), plus an
  Orange-owned step that applies the generated
  `<plan>.r<rev>.<session>.orange_declaration.json` to it. The merged file
  is generated, not committed; it is trustworthy because both inputs are in
  git and named (the template by repo, commit, path and sha256, the plan by
  `source`), and its bytes are sealed as `run_spec/<sha256>.json`.
- Order at the rig: sign-off entry, load entries, run record ready,
  `declare.py --out`, Orange's merge step, orange_client with the merged spec,
  Citrus's values.

## Palette's position (2026-11-15, draft, nothing agreed until Jeremy says)

- The follow-up after #442/#443 pins `declaration.schema_id` =
  orange.recording_start_declaration with `schema_version` 1 or 2; anything
  else refuses. `declared_by` stays a top-level string in v2 (confirmed).
- `declared_blank` needs no Palette code change: the pinned subject reference
  schemas only require a non-empty reason, and Palette records it verbatim
  (`subject_lookup_reason`), so it stays distinct from `no_dish_declared`.
  No reference schema v3 unless the entry shape changes.
- `source` recorded verbatim in `orange_subject_reference_origin`; `kind`
  must agree with `confirmed_by` (below), a mismatch refuses. `plan_sha256`
  is verified only if the plan bytes are sealed in the recording; otherwise
  it is a recorded claim. Sign-off is recorded, never a review gate.
- `subject_type` from Orange: no. Citrus's H5 subject is the authority;
  Orange's dish and count are the cross-check.

`source.kind` and `confirmed_by` (Orange's mapping, for Palette's check):

| source.kind | confirmed_by |
| --- | --- |
| `orange_run_spec` | `run_spec` |
| `orchard_run_plan` | `run_spec` (merged run spec, orange_client) or `local_control` (orchestrator GUI start) |
| `operator` | `gui_operator` or `local_control` |

## Plan and sign-off copies (Jeremy via pancake-plate, 2026-11-15)

Decided: for a plan-sourced declaration the plan and its sign-off travel as
verbatim copies at the recording root, sealed like `run_spec/`:

- `run_plan/<file_sha256>.yaml`: the plan file's exact bytes at commit:path
  (the git blob); the extension follows the committed file's suffix.
- `run_plan/sign_off/<entry_sha256>.json`: the signed run-record entry as
  `canonical_json_utf8_sort_keys_compact_v1` bytes, so its sha256 equals
  `entry_sha256`.
- Named in the declaration as `source.plan_copy_relative_path` and
  `source.sign_off_copy_relative_path`; written before the seal, only for
  plan-sourced declarations, never under `recording_observation_bindings/`.
  Palette can then verify `plan_sha256` and the entry against sealed bytes.

Palette's requirements (2026-11-15):
- Field names: `source.plan_copy_relative_path` and
  `source.sign_off_copy_relative_path` (mirroring `spec_copy_relative_path`).
  Both files must be sealed, with bytes hashing to the named sha.
- `file_sha256` is the plain sha256 of the plan file's bytes, never a git
  blob id; repo, commit and path are recorded beside it.
- The sign-off entry must itself contain `file_sha256` and/or `plan_sha256`,
  so Palette can bind it to the sealed plan (orchard's to provide).
- Palette recomputes `plan_sha256` from the YAML only once orchard pins its
  loader rules and a YAML-to-`plan_sha256` test vector; until then it records
  it as given.
- "Signed" today means a named approval (`by`, `at_utc`): recorded, never a
  gate. A cryptographic signature (pinned key and algorithm) would be a
  separate decision for Jeremy.

pancake-plate (2026-11-15): the copies are written as the exact bytes
received, never re-serialized by Orange (Palette re-canonicalizes and
compares). A third copy is proposed, not decided:
`plan_canonical_copy_relative_path` = `run_plan/<plan_sha256>.json`, the
canonical JSON `plan_sha256` is computed over, so Palette can verify it
without parsing YAML.

Orange's rule, so it holds whatever files the generator adds: Orange copies
the generator's `run_plan/` folder as is and refuses unless every file in it
is named `<sha256 of its bytes>.<ext>` and every `*_copy_relative_path` in
`source` names one of them.

How the bytes reach Orange (proposal): Orange never reads orchard. The
generator writes both files beside its `orange_declaration.json` (`--out`
DIR); the merge step names them in the merged run spec; orange_client reads
each once, refuses unless its bytes hash to `source.file_sha256` /
`source.sign_off.entry_sha256`, and writes those exact bytes. The hash check
is local and proves the copy is the declared plan, without fetching git.

## Open questions

1. Plan bytes in the recording: copy (e.g. `run_plan/<file_sha256>.yaml`) or
   store-first? Same question as the spec copy, which Jeremy answered with a
   copy for run specs.
2. Should Orange refuse an `orchard_run_plan` declaration whose
   `plan_hash_rule` it does not recognise, or record it as given?
3. (Answered: no subject_type from Orange; Palette and experiment-spec agree.)
4. The merge step: where the template's identity is recorded in the merged
   spec (proposal: `fixed.declaration_template` {repo, commit, path,
   sha256}), and whether the template must be committed and clean (proposal:
   yes, refused otherwise, as for the plan).
