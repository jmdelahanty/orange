# Synthetic recording bundle (production writers, no camera)

Date: 2026-09-26. Tool: `tools/synthetic_recording_bundle.cpp`, CMake target
`synthetic_recording_bundle` (built on demand, like the manifest tests); test:
`tools/synthetic_recording_bundle_tests.py`.

## Why

Palette rejected a Citrus-generated synthetic transfer because its Orange
geometry bundle was incomplete: the fixture hand-wrote a minimal
`recording_snapshot_start.json` and a stub `recording_geometry_contract.json`,
had no `recording_snapshot.json` and no `recording_geometry_assets/`. That is a
gap in the synthetic fixture, not in Orange's production writer. The agreed
split is: Orange generates both snapshots, the geometry contract and its
checksummed assets with its actual writers from clearly labelled synthetic
inputs; Citrus consumes them for the unified H5, then rebuilds its receipt and
transfer envelope.

## What the tool runs

Exactly the production functions a recording start runs, in the same order:

1. `write_recording_snapshot` → `recording_snapshot.json` (camera blocks,
   `camera_runtime`, `source_camera_streams`, `shaman_v2_camera_identity`,
   `recording_outputs`, `sync`, `gpu_inventory`, `source_version`).
2. `update_gui_recording_geometry_contract` →
   `recording_geometry_contract.json` and `recording_geometry_assets/`
   (`manifest.json` plus exact-byte copies of every referenced observation,
   pointer, candidate, tank design and daily-registration file, each with its
   SHA-256 and verified declared checksum); the snapshot receives the
   contract's digest reference and `calibrations[serial].dish_top_rim_observation`.
3. `update_recording_snapshot_session_artifacts` → frozen
   `session.recording_contexts` (always `data_origin = synthetic`) and
   `session.synthetic_bundle` (the label).
4. `seal_immutable_recording_start_snapshot` → create-once, read-only
   `recording_snapshot_start.json`; the mutable snapshot gains
   `immutable_recording_start_snapshot` and keeps being enriched.
5. For a `stimulus_experiment` intent with binding mode `required|optional`:
   `materialize_recording_observation_binding_requests` →
   `recording_observation_bindings/request_collection.json` and
   `requests/<context>.json` (request v1 or v2 per `--request-version`),
   referenced from the mutable snapshot only. These are Orange's sealed binding
   inputs; Citrus answers them (acceptance batch) before H5 generation. No
   pre-arm transport runs.
6. `--manifest`: `build_single_clip_recording_session_manifest` +
   `write_recording_session_manifest` → `recording_session.json` carrying the
   frozen contexts and the geometry-contract reference, with conspicuously
   labelled plain-text placeholder media (`Cam<serial>.mp4` is text, never
   decodable video).

Nothing opens a camera, GPU stream, recorder or Citrus socket. The tool
refuses a non-empty output folder: finalized evidence is never patched.

## Synthetic inputs

- `--fixture-rig <dir>` writes a self-contained synthetic Citrus rig tree
  (`synthetic-rig` / `synthetic-canvas`): one arena per camera, a local
  commissioning release with an accepted homography (candidate + YAML) and
  projected-surface scale (candidate + observation) per camera, one tank
  design, and an accepted daily registration (valid until 2099) with a
  schema-v2 dish top-rim observation and its compact exports per camera. Every
  file carries `synthetic_input`. With four cameras the resolved bundle has 48
  compact files: 11 per camera, 3 daily-registration files, 1 tank design.
- `--canvas <citrus_canvas.json>` uses an existing canvas tree instead (for
  example the real `omnifin0/shadow`, whose geometry is real while the frames
  are synthetic; the label records which).
- Camera blocks come from `--camera-config <serial>=<json>` or, by default, a
  generated `synthetic_inputs/camera_configs/<serial>.json` marked
  `synthetic_input`.
- `ORANGE_CALIBRATION_BASE_DIR` is pointed at `synthetic_inputs/calibrations`
  (or `--calibration-base`) so no host physical registration leaks in; the
  spatial-artifact and guided-capture canvas environment variables are cleared.

## Labelling

- `synthetic_bundle.json` at the root (`orange.synthetic_recording_bundle` v1):
  label text, generator and Orange source version, inputs with checksums,
  the frozen contexts, exact checksums of the four produced artifacts,
  geometry/asset status, request summary, and the snapshot distinction.
- `SYNTHETIC_BUNDLE_README.txt`.
- `session.synthetic_bundle` inside both snapshots (it is sealed).
- `data_origin = synthetic` in every recording context; the tool cannot emit
  `acquired`.
- Placeholder media are plain text and say so.

## Usage

```bash
cmake --build targets/release --target synthetic_recording_bundle -j 16
targets/release/synthetic_recording_bundle \
  --out /tmp/orange_synthetic_bundle_20260926 \
  --fixture-rig /tmp/orange_synthetic_rig_20260926 \
  --camera 2010093 --camera 2010094 --camera 2010095 --camera 2010096 \
  --intent stimulus_experiment --recording-subtype omit --behavior-mode embedded \
  --binding-mode required --request-version 2 --manifest
```

`--recording-subtype omit` (or absent) emits `citrus.parent_recording_context`
version 2 without a subtype; an explicit subtype emits version 1.
`--intent recording_only` produces no binding requests (mode
`not_applicable`). `--copy-images` also copies image evidence (none in the
fixture rig). Timestamps and `source_version` differ between runs by design;
reproducibility means the same inputs produce the same structure, statuses,
file set and declared-checksum verification, not byte-identical snapshots.

## Validation

`tools/synthetic_recording_bundle_tests.py` (ctest `synthetic_recording_bundle_tests`)
runs three shapes (bound subtype-free v2 with manifest, recording-only v1,
bound v1) and checks: read-only sealed start referenced by digest from the
mutable snapshot; contexts frozen identically in both; contract `resolved`
with every camera resolved and referenced by exact digest from both
snapshots; asset manifest `complete`, every file present, exact-byte,
checksum-verified, with the expected per-camera roles; snapshot rim entries
pointing at the recording-local observation; requests present only for bound
sessions with the requested version and the sealed digests; the manifest
carrying contexts and the geometry reference; rerun refused.

For the cross-repository handoff, Citrus's own admission script
(`recording_transfer_v2.py --validate-source-only`) and Palette intake are the
external checks; this tool makes Orange's side complete and honest.

Admission results on 2026-09-26 (Citrus script at cff3687, four-camera fixture
rig, `/home/jeremy/orange_data/synthetic_fixtures/20260926/`):

- `bound_v1_subtype`: refused with "bound recording lacks finalized
  observation collection". Expected: the bundle stops at Orange's sealed
  requests; the acceptance batch, H5 and finalized collection are Citrus's
  step before transfer.
- `bound_v2_no_subtype`: refused with "unsupported parent recording context
  fields" by that pre-revision script; Citrus's review branch accepts context
  v2.
- `recording_only_v1`: passes the frame-map checks (production metadata
  columns, monotonic synthetic timestamps) and is refused at
  `Cam<serial>.mp4.finalization.json`. That is by design: the tool does not
  fabricate an `orange.video_container_finalization` claim for a text
  placeholder. Recording-only single/rolling fixtures come from real
  recordings with real media; the synthetic bundle is for geometry, snapshots
  and binding inputs.
