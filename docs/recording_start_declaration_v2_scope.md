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
    "plan_sha256": "<64 hex>", "plan_hash_rule": "<orchard's rule name>",
    "sign_off": {"entry_id": "...", "entry_sha256": "<64 hex>", "by": "delahantyj", "at_utc": "..."},
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
  orchard; the checks above are on the declaration itself.
- `blank: true` is exclusive with `dish_id` and `subject_count`. It is emitted
  in the subject reference as status `not_collected` with a new reason
  `declared_blank` (today's undeclared camera stays `no_dish_declared`).

## What each owner would need to change

- **Orange**: parser and writer for v2 next to v1 (v1 keeps working), the
  confirmation and spec-copy paths unchanged, subject reference schema
  version bump for `declared_blank`, tests; the orchestrator and the run spec
  path take v2 as they take v1.
- **Palette**: accept declaration v2 inside `subject_confirmation.json`
  (#442/#443 follow-up), the `declared_blank` reason in the subject reference
  (a pinned schema), and decide what of `source` it records or checks.
- **orchard (experiment-spec)**: `tools/declare.py` emits v2 with `source`
  and `blank` for blank arenas (today it omits them).
- **pancake-plate**: whether the plan bytes travel with the recording (a copy
  next to `run_spec/`) or by the sha256 store; Jeremy's decision is pending
  with you.
- **Citrus**: none for Orange's declaration; its own subject declaration v2
  comes from the same plan (citrus-agent's design).

## Open questions

1. Plan bytes in the recording: copy (e.g. `run_plan/<file_sha256>.yaml`) or
   store-first? Same question as the spec copy, which Jeremy answered with a
   copy for run specs.
2. Should Orange refuse an `orchard_run_plan` declaration whose
   `plan_hash_rule` it does not recognise, or record it as given?
3. Groups: Orange declares the source dish and the count (decided). Does
   Palette need the subject_type (individual/group) from Orange too, or only
   from Citrus?
4. The run spec route (Jeremy, 2026-11-15: the generated declaration goes
   into the run spec as `fixed.declaration`): who writes that run spec, and
   where it is committed so the spec copy names a file in git.
5. Is `operator` worth a kind, or is "no source" enough for GUI and client
   starts?
