# Headless Palette-ready recording (spec runner)

Date: 2026-10-06. Spec: `experiment_specs/fourcam_palette_recording_only_rolling_crops_shadow.json`.
Rules it satisfies: Palette's intake matrix (agent-contracts PR 53, comment
6026342060, palette main 7c081aa3) and Citrus's transfer sealer
(`scripts/check_recording_transfer_source.sh`).

## What the headless runner now writes that the GUI already did

| Block / artifact | Headless before | Now |
| --- | --- | --- |
| Recorder media location | separate `external_recorder_contract.artifact_root` under `~/orange_data/external_recorder` (refused by Palette: media outside `recording_folder`) | a **relative** `artifact_root` (`"external_recorder"`) and relative stream file names are resolved under the run's recording folder (`resolve_headless_recorder_contract_paths`); absolute values are left as they were. The crop recorder was already under the folder. |
| `models[serial].detect/.pose`, `crop_outputs` in the snapshot | absent (Citrus's capacity preflight requires the models declaration) | written with the GUI writers (`update_gui_detect_model_snapshots`, `..._crop_output_snapshots`, `..._pose_model_snapshots`), which read the camera config and the `ORANGE_POSE_*` environment the client exports from the spec |
| `session.effective_configuration` | absent | captured after the subject references (no app config in headless runs; the spec is the configuration; the environment with sources is sealed) |
| Citrus canvas | env only (`ORANGE_CITRUS_RECORDING_CANVAS_CONFIG_PATH`, not forwarded by the benchmark wrapper) | spec key `fixed.citrus_recording_canvas_config_path` (exported when the variable is absent; env wins) |
| Strict validator | refused the headless producer | `validate_gui_ptp_recording.py` accepts `orange_headless_external_ipc` |

Still GUI-only: `citrus_runtime_geometry` (Palette ignores it; a bound
headless session would need it only for Citrus, which reads its own copy).

## The spec

The validated production shape (owner push, 16 slots, extra output delay 8,
native local input, INT8 min-max detect, 192 px pose, moving crops at 384 px,
PTP gate) with:

- `recording_control.record_for_seconds 120`, `clip_seconds 30`: a rolling
  session of four clips (`mode = rolling_clips`, one parent per camera).
- `recording_contexts.default`: behavior / free / **recording_only** /
  acquired, no subtype (context v2). `recording_only` maps the Citrus binding
  mode to `not_applicable`, so no `recording_observation_bindings/` is written
  and the sealer accepts the folder.
- `zebrobot.base_url` and `subject_references`: `subject_count 1` for every
  camera, a dish only on 2010093 (`19220_1`). **Edit the dish ids and counts
  per camera before every run**; a camera without a dish is declared
  `not_collected / no_dish_declared` and Palette admits it.
- `citrus_recording_canvas_config_path`: the Shadow canvas
  `~/citrus/targets/rigs/omnifin0/shadow/shadow.json` (geometry contract).
- `external_recorder_contract.artifact_root = "external_recorder"` with
  relative stream file names; `scripts/run_detect_latency_spec.sh` leaves a
  relative root unstamped (the recording folder is unique per run).

## Run and hand over

```bash
# 1. pre-flight (cameras, PTP stack, NIC counters) as in AGENTS.md; after a reboot
#    recreate /tmp/orange_recorder_config_a16 from ~/orange_data/config/local/100_cam4_ptp_fourcam
scripts/run_detect_latency_spec.sh --orange-client <tree>/targets/release/orange_client \
    fourcam_palette_recording_only_rolling_crops_shadow
# 2. Orange checks
scripts/validate_gui_ptp_recording.py --latest-complete        # strict validator (now accepts headless)
scripts/verify_timed_recording.py <folder>
# 3. will it seal? (read-only, hashes all media)
scripts/check_recording_transfer_source.sh <folder>
# 4. the operator's transfer seals the envelope and moves the folder to the
#    group's staging (as the domain user; dry run first):
#    Use an explicit, unique destination name (the default dest-parent would
#    reuse the generic run_0001__codec... name; unique_run_id specs make the
#    source folder unique too):
~/citrus/scripts/transfer_as_domain_user.sh --no-dest-parent 'delahantyj@hhmi.org' <folder> \
    johnsonlab-staging /groups/johnson/johnsonlab/jeremy/staging/<experiment folder name> --dry-run
~/citrus/scripts/transfer_as_domain_user.sh --no-dest-parent 'delahantyj@hhmi.org' <folder> \
    johnsonlab-staging /groups/johnson/johnsonlab/jeremy/staging/<experiment folder name> --verify quick
#    -> the folder with _citrus_transfer_complete.json and _citrus_transfer/snapshot.json
# 5. since 2026-10-07 (Palette main eb0285b1): live staging is watched by the v2
#    poller every 5 min; a sealed delivery is imported (LSF) and registered (on
#    delahantyj-ws1) automatically. Tell Palette the path so they can watch it
#    through; the first bundle (2026-10-06 run 5) went through a manual trial
#    import on an isolated registry first.
```

Palette's own read-only checkers (need the sealed transfer):
`scripts/py -m fisheye.utils.inspect_recording_transfer <staging>` and
`scripts/py -m fisheye.utils.run_citrus_session_import <staging> --dest-root <new>`.

## Not yet proven

The spec validates and the client builds; no rig run has produced a folder
from it yet. The first run is the real test of the relative recorder root,
the rolling clip manifest under Palette's rules and the subject lookup from
a headless process.
