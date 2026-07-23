# Commissioned Setup Packaging Implementation Checklist

Status: design accepted; the non-active schema, candidate packager, standalone
validator, Shadow migration-report slice, and a recording/H5 bridge through the
existing recording-geometry contract are implemented. General registry
promotion, package-native runtime loading, shared arm-time composition identity,
and GUI package selection remain open.

This checklist turns the existing rig/canvas commissioning release, optional
daily registration, Orange recording geometry contract, and Citrus H5 runtime
geometry snapshot into one coherent lifecycle:

```text
immutable commissioned rig setup
  + immutable experiment-canvas binding
  + explicit base_only or immutable daily-registration set
  -> immutable per-recording calibration capsule
```

The package work must remain optional for operating the rig. Missing optional
calibration produces explicit metadata status; it does not prevent recording.
Once an operator explicitly selects a commissioned or daily artifact, however,
that selected artifact must validate or fail closed rather than silently falling
back to a different artifact.

Related contracts:

- `docs/rig_canvas_commissioning_release.md`
- `docs/recording_geometry_contract.md`
- `docs/orange_citrus_guided_daily_registration_contract.md`
- `docs/spatial_layout_contract.md`
- `docs/dish_top_rim_observation_design.md`

## Existing Foundation

- [x] Citrus has an immutable accepted per-canvas commissioning release and an
  atomic active pointer.
- [x] Commissioning binds each camera/arena to accepted homography and
  projected-surface scale products and their acceptance receipts.
- [x] Derived canvases can inherit stable projection geometry while retaining
  ownership of their experimental-area semantics.
- [x] Citrus has an immutable daily-registration artifact plus an independent
  `base_only` / `selected_daily_registration` selection pointer.
- [x] Daily registration checksum-binds the exact Orange rim-observation JSON.
- [x] Orange rim observations distinguish the raw Hough proposal, accepted
  water-side inner rim, and outward centroid-forgiveness gate.
- [x] Orange writes an immutable `recording_geometry_contract.json` and can
  atomically materialize exact source bytes into
  `recording_geometry_assets/`.
- [x] Citrus mirrors the Orange recording contract into H5 and separately
  records the geometry it actually applied.
- [ ] The commissioning release is self-contained. Today it copies the rig and
  canvas snapshots but leaves most accepted products at absolute external
  paths.
- [ ] A selected daily registration packages its Orange rim observations and
  derived masks at the calibration-authority layer. The Citrus registration
  itself still retains path/checksum references; recording start now resolves
  and copies those exact compact members into the recording-local bundle.
- [x] Recording start resolves the exact selected daily rim evidence into the
  per-camera recording geometry contract and direct recording-snapshot mask,
  then materializes its compact files. This does not implicitly activate the
  older environment-selected Orange spatial-calibration loader or live YOLO
  gating.
- [ ] Orange and Citrus verify one shared, frozen calibration-composition digest
  before experiment arm.

## Invariants To Freeze Before Coding

- [x] Name and version the four schemas:
  `commissioned_rig_setup`, `experiment_canvas_binding`,
  `daily_registration_set`, and `recording_calibration_capsule`.
- [x] Declare one writer/authority for each product:
  Citrus seals commissioned setups and runtime registrations; Orange owns
  camera observations and seals recording capsules.
- [x] Use package-relative paths as resolvable identities. Absolute source paths
  may be retained only as non-authoritative provenance.
- [x] Require SHA-256, byte count, semantic role, target plane, camera, arena,
  and required/optional classification for every manifest member.
- [x] Define canonical JSON hashing and package-ID derivation so both programs
  calculate the same digest.
- [x] Keep the canonical physical experimental dimensions separate from the
  daily observed radius. Daily registration may translate but never silently
  resize the canonical area.
- [x] Keep `dish_mask.outer_geometry` as the physical inner rim and
  `dish_mask.valid_geometry` as the separately derived centroid gate.
- [x] Record coordinate origin, axis directions, pixel-center convention,
  boundary-inclusion rule, camera-native raster, and target plane with every
  runtime geometry.
- [x] Prohibit cross-camera and cross-arena fallback.
- [ ] Prohibit mutation of calibration packages while an experiment is armed or
  active.

## 1. Registry And Storage Contract

- [ ] Add a configurable neutral registry root, initially defaulting to:

  ```text
  /home/jeremy/orange_data/calibrations/registry/rigs/<rig_id>/
  ```

- [ ] Define this initial layout:

  ```text
  commissions/<commission_id>/
  active_commission.json
  canvases/<canvas_id>/bindings/<binding_id>/
  canvases/<canvas_id>/active_binding.json
  canvases/<canvas_id>/daily_registrations/<registration_id>/
  canvases/<canvas_id>/daily_registration_runtime_selection.json
  ```

- [ ] Retain backwards-compatible readers for current Citrus
  `calibration_artifacts/` paths during migration.
- [ ] Publish packages through a sibling staging directory, flush files and
  directories, then atomically rename.
- [ ] Apply the same safe ownership/chown policy used by Orange recording
  outputs.
- [ ] Never overwrite an immutable package directory. An identical retry may
  reuse it only after every checksum and request identity validates.
- [x] Add a standalone package validator that requires no live Orange or Citrus
  process.

## 2. Commissioned Rig Setup Package

- [ ] Extend commissioning finalization to materialize, rather than merely
  reference, the accepted products.
- [ ] Always include compact runtime-critical artifacts:
  - rig and authority-canvas snapshots;
  - display/canvas raster, refresh mode, and coordinate conventions;
  - camera identities, native rasters, crops/orientations, and configuration
    fingerprints;
  - active homography selection receipts, accepted candidates, matrix YAML,
    candidate-set manifest, and numeric QC;
  - active projected-surface scale selection receipts, accepted candidates,
    Orange source observations, target definitions, homography bindings, and
    numeric QC;
  - tank/dish definitions and physical dimensions;
  - source Orange `session.json` and `session_index.json` files;
  - operator acceptance and software/build provenance.
- [ ] Include the operational fixture contract explicitly:
  shelf/holder identity, diffuser or gel state, projection-surface plane,
  holder-validation report, and accepted overlays.
- [ ] Include one accepted review overlay per camera/product as required review
  evidence.
- [ ] Classify full-resolution source captures as archive evidence and define a
  configurable include policy.
- [ ] Do not include rejected candidate histories in the active package; retain
  them in their original calibration sessions.
- [ ] Seal an inventory manifest over every file and write an acceptance
  receipt for the final package digest.
- [ ] Change the active commissioning pointer to select the sealed package while
  preserving a version-1 release compatibility path.

## 3. Experiment Canvas Binding

- [ ] Create an immutable binding containing the exact selected canvas config,
  experimental-area geometry and units, tank selection, and camera/arena map.
- [ ] Bind it to one commissioned setup ID and checksum.
- [ ] Validate canvas dimensions, camera native rasters, arena identities, and
  projection-authority inheritance before acceptance.
- [ ] Ensure an inherited canvas receives only the stable arena placement,
  homography, and projected-surface scale from its authority package.
- [ ] Ensure the selected canvas remains the owner of experimental-area shape,
  dimensions, and tank semantics.
- [ ] Add an atomic active-binding pointer and explicit rollback/revalidation
  path.

## 4. Daily Registration Package

Recording-local bridge implemented 2026-07-22: the existing accepted Citrus
selection is resolved without `latest` lookup, bound to the active
commissioning/homography identity, and its exact compact Orange/Citrus source
files are copied into each new recording. This is the recording/capsule slice;
it does not by itself replace the still-planned promoted package registry.

- [x] On recording with a selected accepted daily registration, copy and verify
  each exact Orange source
  artifact rather than retaining only an absolute path:
  - `observation.json`;
  - observation `manifest.json` and `image_set.json`;
  - `exports/spatial_dish_mask_runtime_v1.json`;
  - `exports/palette_dish_mask_v2.json`;
  - accepted inner-rim and valid-detection-region overlays;
  - source frame according to the configured evidence policy.
- [x] Include the Citrus candidate, accepted registration, verification result,
  and selection receipt in the recording-local bundle.
- [x] Bind the recording-local product to the exact commissioned setup, canvas binding,
  homography, camera/arena identities, native raster, and validity interval.
- [ ] Preserve the raw Hough proposal separately from the operator-accepted
  physical boundary.
- [x] Materialize `outer_geometry` from the accepted water-side inner rim.
- [x] Materialize `valid_geometry` from the accepted outward centroid gate.
- [ ] Record the outset in pixels and physical units and assert that it did not
  modify canonical Citrus projection geometry.
- [ ] Keep acceptance separate from runtime selection.
- [x] Disallow timestamp-based or `latest` selection in recording resolution.
- [ ] Add restart-time rehydration and validation of the selected daily package.

## 5. Recording Calibration Capsule

- [ ] Extend `recording_geometry_contract` to version 2 with an explicit
  composition block containing setup, binding, and daily-mode identities.
- [ ] At arm time, resolve exactly one composition for only the participating
  cameras.
- [ ] In `base_only`, record an explicit null daily package and commissioned
  effective geometry.
- [ ] In `selected_daily_registration`, require the exact selected registration
  and package checksums to match Citrus runtime state.
- [x] Reuse the existing Orange exact-byte materializer for all compact package
  members.
- [x] Always copy runtime-critical compact artifacts into the recording.
- [x] Copy accepted daily rim observation and mask-runtime exports for every
  participating registered camera.
- [ ] Copy large commissioning captures only under an explicit portable/full-
  evidence policy; prefer reflink with verified-copy fallback.
- [ ] Add the capsule path, checksum, package IDs, and asset-manifest checksum to
  `recording_snapshot.json`, `recording_session.json`, and rolling clip indices.
- [x] Preserve non-blocking statuses for `not_configured`, `partial`, and
  `base_only` workflows.
- [ ] Treat an explicitly selected but invalid setup/registration as a start
  error; never silently substitute another package.

## 6. Orange/Citrus Arm-Time Handshake

- [ ] Add a read-only Citrus method that resolves the in-memory effective
  geometry and returns its setup/binding/daily composition digest.
- [ ] Have Orange materialize and verify the recording capsule, then return its
  digest to Citrus.
- [ ] Require Citrus to confirm that the capsule digest matches the geometry it
  will use before arming.
- [ ] Freeze that resolved geometry in memory for the experiment lifecycle.
- [ ] Keep all registry/pointer validation outside the render, arena-update,
  acquisition, and detection hot paths.
- [ ] Block commissioning or daily-selection mutation while armed/active.
- [ ] Record an end-of-session verification that the frozen runtime identity was
  not replaced.

## 7. H5 Contract

- [ ] Continue writing the exact authoritative bytes under:

  ```text
  /recording_geometry_contract/contract_json
  /recording_geometry_contract/h5_scope_json
  /runtime_geometry_contract/contract_json
  ```

- [ ] Include the setup ID/checksum, binding ID/checksum, daily mode,
  registration ID/checksum, and capsule checksum in the H5 scope.
- [x] Include the exact camera-scoped mask-runtime export and accepted rim
  observation when daily registration is selected, through the authoritative
  Orange contract mirror.
- [ ] Populate convenience datasets/attributes for homography, scale, effective
  translation, physical rim circle, and valid centroid-gate circle.
- [ ] Bind every convenience value to the authoritative contract checksum; do
  not allow it to become an independently editable authority.
- [ ] Make Citrus `/runtime_geometry_contract` echo the same composition digest
  and the translation it actually applied.
- [ ] Record a structured mismatch status and reject arming when an explicitly
  selected runtime composition disagrees.
- [ ] Keep images out of H5; store relative recording-asset paths and checksums.

## 8. Downstream Dish-Mask Gate

- [ ] Define the normative gate as floating-point bounding-box-centroid
  containment in `valid_geometry`, unless an analysis explicitly selects a
  different policy.
- [ ] Define circle boundary inclusion exactly:
  `(x-cx)^2 + (y-cy)^2 <= r^2` in camera-native pixels.
- [ ] Preserve `outer_geometry` for physical interpretation and QC.
- [ ] Provide a shared loader/validator for Orange outputs and Citrus H5 scopes.
- [ ] Reject camera-raster, camera-serial, arena, coordinate-space, or checksum
  mismatch rather than applying a mask approximately.
- [ ] Allow downstream pipelines to set `require_valid_dish_mask=true` without
  making the mask mandatory for all recording workflows.
- [ ] Keep live YOLO pre-gating as a separate opt-in implementation from
  downstream filtering.

## 9. Migration And UI

- [x] Build a read-only migration report for the currently promoted Shadow
  release.
- [x] Build a packaging tool that converts that release to a sealed version-2
  package without activating it.
- [x] Compare every copied member checksum with the version-1 release.
- [ ] Review the package inventory and fixture evidence in Orange.
- [ ] Activate only through a separately armed promotion step.
- [ ] Add registry-based rig/canvas/package selection to Orange so operators do
  not need to browse raw Citrus repository folders.
- [ ] Display active setup, binding, daily mode, expiration, mask availability,
  and validation state before recording.
- [ ] Add explicit `base_only` selection and selected-registration rollback.
- [ ] Document portable export, backup, restore, and retention policy.

## Automated Acceptance Matrix

- [ ] Package validates after original candidate/session paths are made
  unavailable in a test fixture.
- [ ] Corrupting any required member fails package validation.
- [ ] Cross-camera or cross-arena substitution is rejected.
- [ ] An inherited canvas shares the commissioned projection package but retains
  its own experimental-area definition.
- [ ] A `base_only` recording succeeds without any daily artifact.
- [ ] A selected daily recording contains the exact source observation SHA-256
  and matching `valid_geometry` in Orange outputs and Citrus H5.
- [ ] Changing active pointers after recording does not change the recorded
  capsule.
- [ ] Orange/Citrus composition mismatch prevents arm when the composition was
  explicitly selected.
- [x] Missing optional package metadata remains non-blocking and is represented
  explicitly.
- [ ] Four-camera recording materializes only the four exact camera/arena
  members with no fallback.
- [ ] Rolling recordings reference one stable capsule across every clip.
- [ ] Safe ownership is correct for every package and recording-local asset.
- [ ] No registry reads or checksum walks occur in the steady render/update
  path.

## First Implementation Slice

Do this before changing runtime loading:

- [x] Write the version-2 package schemas and canonical-hash tests.
- [x] Extract/reuse Orange's exact-byte staging and manifest primitives where
  practical.
- [x] Implement a standalone validator.
- [x] Package the current accepted Shadow release into a non-active candidate.
- [x] Produce an inventory/diff report against the current release.
- [x] Review missing holder/fixture evidence and package completeness.
- [x] Do not activate or modify current runtime pointers in this slice.

First-slice evidence and remaining gaps are documented in
`docs/calibration_package_v2.md`. The current Shadow candidate is intentionally
`incomplete` and rejected by the validator because its version-1 release does
not checksum-bind one authoritative fixture manifest, holder-validation
evidence, or exact Orange/Citrus build provenance. No timestamp-based inference
or promotion was performed.

## Definition Of Done

This workstream is complete when a recording can be moved away from both
repositories and the central registry while still containing enough verified,
camera-scoped compact data to reconstruct the exact commissioned transform,
scale, canvas/tank geometry, selected daily translation, physical inner rim,
and downstream centroid gate used for that recording. Citrus H5 and Orange
recording metadata must identify the same capsule checksum, and ordinary
`base_only` recording must remain available when daily registration is skipped.
