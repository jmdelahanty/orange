# Parent recording contexts (Citrus transfer-v2)

Date: 2026-09-24. Code: `src/recording_context.{h,cpp}`; gate in
`orange::session::write_recording_session_manifest`.

Citrus's transfer-v2 intake refuses a recording unless `recording_session.json`
carries `recording_contexts`: one closed seven-field
`citrus.parent_recording_context` (version 1) per camera parent, keyed by the
exact camera serial, with exactly the recording's camera membership. The values
are the operator's statement of what the recording is; Orange never infers
them from files, detectors, or whether Citrus is running.

## Configuration

GUI: app config `recording.contexts`. Headless: experiment spec
`fixed.recording_contexts`. Same shape:

```json
"contexts": {
  "schema_version": 1,
  "default": {
    "recording_type": "behavior",
    "recording_subtype": "dish_freeswim",
    "behavior_mode": "free",
    "recording_intent": "recording_only",
    "data_origin": "acquired"
  },
  "cameras": {
    "2010094": {
      "recording_type": "behavior",
      "recording_subtype": "dish_stimulus",
      "behavior_mode": "free",
      "recording_intent": "stimulus_experiment",
      "data_origin": "acquired"
    }
  }
}
```

- Entries carry exactly the five configurable fields; `schema_id` and
  `schema_version` are added by Orange on emission.
- `recording_type` and `recording_subtype`: non-empty, at most 1024
  characters, no control characters, no surrounding whitespace.
- `behavior_mode`: `free | embedded | none`; `recording_intent`:
  `stimulus_experiment | recording_only`; `data_origin`: `acquired | synthetic`.
- A serial takes its own `cameras` entry, else `default`. A recording camera
  with neither refuses the start. Configured serials that are not recording
  are ignored. Unknown fields anywhere refuse the config.
- Absent block: no `recording_contexts` is emitted and the validators warn
  (Citrus will refuse the transfer). This keeps older runs and diagnostics
  unchanged.

## Lifecycle

1. Record start resolves the config against the recording cameras (GUI:
   `prepare_recording_run_start`; headless: the prearm block before the seal)
   and writes the emitted map to `session.recording_contexts` in
   `recording_snapshot.json`, which is then sealed as
   `recording_snapshot_start.json` before acquisition.
2. Every manifest write (single clip, rolling, external-recorder rebuilds,
   post-hoc refreshes) passes through the common writer, whose gate reads the
   sealed block, requires its membership to equal `cameras`, and copies it
   verbatim to `recording_contexts`. A manifest that already holds a different
   block, or a block without a frozen source, is refused. Clip manifests are
   not touched.
3. Intent versus Citrus binding: `recording_only` forces the observation
   binding mode to `not_applicable` (default env `optional`), and refuses the
   start if `ORANGE_CITRUS_OBSERVATION_BINDING_MODE=required` was set
   explicitly. `stimulus_experiment` leaves the mode as configured. Carrying
   the context to Citrus before capture (binding request v2) is separate
   joint work.

## Validation

- `scripts/validate_gui_ptp_recording.py`: presence (warn when absent),
  closed schema, exact membership, identity with the frozen snapshot block.
- `scripts/verify_timed_recording.py`: same checks when the block is present.
- Unit tests: `tools/recording_context_tests.cpp` (parser, resolution, gate,
  intent coupling) and `tools/recording_session_manifest_tests.cpp`
  (`recording_contexts_flow_through_parent_writers_and_refresh`).
