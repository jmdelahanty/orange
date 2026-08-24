# Orange Standalone Daily Physical Registration Plan

Date: 2026-08-21

Status: implementation in progress. Standalone schema-v2 persistence, grouped
camera-only capture/review, and explicit per-camera active selection are
implemented. Recording pre-arm consumption remains a later phase.

## Goal

Make daily registration of a removable dish a first-class Orange workflow that
does not require a loaded Citrus canvas, a running projector, or a Citrus
control transaction.

The workflow establishes where the subject can physically exist in today's
camera view. It produces camera-native, immutable evidence for dish masks,
detector-input gating, crop-only context binding, offline analysis, and
recording provenance.

Projection alignment remains a separate, optional Citrus product. When Citrus
is active, it may consume the accepted Orange physical registration to align
the projected arena with the dish. When no projection is used, that second
product is explicitly not applicable.

## Product Boundary

### Orange-owned daily physical dish registration

Inputs:

- native camera frames captured in the production optical state;
- camera identity, raster, configuration, and stream epoch;
- declared holder, dish, water, diffuser, filters, illumination, and subject
  state; and
- a raw circle proposal plus operator review.

Outputs:

- the raw detected inner-rim proposal;
- the operator-accepted physical inner-rim circle;
- the derived outward centroid-forgiveness gate;
- the camera-native dish mask and fit-quality evidence;
- source and review images with checksums; and
- an immutable accepted artifact and explicit recording selection.

The physical product does not require or modify a Citrus canvas, projector
homography, arena origin, canonical arena size, experimental-area radius, or
projected stimulus.

### Citrus-owned daily projection registration

Inputs:

- an accepted, compatible Orange physical registration;
- a selected Citrus rig, canvas, arena, and camera mapping;
- an accepted commissioning homography; and
- Citrus's canonical arena and experimental-area geometry.

Outputs:

- the small runtime translation that aligns canonical projected geometry with
  today's dish placement;
- inverse-projected review evidence; and
- an immutable Citrus candidate, acceptance, and runtime selection.

This product never changes the physical inner-rim observation. It remains
optional when Citrus is available: commissioned `base_only` operation is a
valid explicit choice.

## Applicability And Recording Policy

The two products must have independent persisted statuses.

Recommended physical-registration statuses:

- `selected`: an accepted artifact is selected for this recording;
- `available_not_selected`: a compatible accepted artifact exists but was not
  selected;
- `not_performed`: the setup uses a dish but no daily physical fit was made;
- `not_applicable`: the setup has no physical dish boundary; and
- `invalid`: a selected artifact failed identity, raster, state, checksum, or
  freshness validation.

Recommended projection-registration statuses:

- `selected`;
- `available_not_selected`;
- `base_only`;
- `not_applicable` with a reason such as `no_active_projection_canvas`; and
- `invalid` for a selected but incompatible artifact.

`not_applicable` is not an error and must not be represented as missing
calibration.

| Recording use | Physical registration | Projection registration |
| --- | --- | --- |
| Ordinary full-frame video | recommended, non-blocking | not applicable when no projector is used |
| Full-frame plus crop | recommended, non-blocking unless a geometry-dependent feature is selected | independent |
| Live dish-mask/model-input gating | required and selected | independent |
| Crop-only with reconstructable context | required and selected | independent |
| Citrus-backed experiment | recommended; required only by an explicitly selected mask policy | optional `base_only` or selected translation |
| No dish or no bounded physical arena | not applicable | independently determined by projection setup |

If a user selects a feature whose correctness depends on the dish boundary,
Orange must fail pre-arm when the physical artifact is absent or invalid. It
must not make physical registration mandatory merely because a camera is being
recorded.

## Shared Capture, Separate Derived Artifacts

For the common empty-dish production setup, one fresh grouped source burst may
serve as evidence for both:

1. the daily physical rim observation; and
2. the recording context-image background set.

The source frames may be shared by identity and digest. The derived products
remain distinct:

- `orange.calibration.dish_top_rim_observation` schema v2 describes the raw
  proposal, accepted physical boundary, and valid centroid gate;
- `orange.recording.context_image_set` schema v1 describes the ordered source
  images and deterministic median background.

Neither product silently stands in for the other. Reuse requires matching
camera, raster, stream epoch, camera configuration, optical state, physical
state, projector state, subject absence, and source checksums.

## Implemented Slice (2026-08-23)

The first standalone persistence slice is now implemented:

- the Spatial Layout UI labels the action **Save Physical Dish Registration**
  and no longer requires a matching Citrus template for that action;
- UI enablement and save-job preparation share one fail-closed preflight for
  capture camera identity, native raster, full-resolution role, operator target
  confirmation, accepted circle, raw Hough evidence, and writer state;
- a loaded Citrus template for another camera is not copied into the physical
  artifact or used to name its camera/arena aggregate;
- schema-v2 observation JSON now identifies the independent
  `daily_physical_dish_registration` and `daily_projection_registration`
  products;
- a no-canvas artifact records projection registration as
  `not_applicable/no_active_projection_canvas` rather than pending Citrus
  acceptance; and
- focused tests prove no-Citrus persistence plus mismatch and raster failure
  behavior.

This initial manual/semi-manual path remains available through the existing
capture, Hough review, metadata confirmation, and persistence panels.

## Implemented Grouped Workflow And Selection (2026-08-23)

Orange now also provides a guided **Standalone Daily Physical Dish
Registration** workflow that:

- operates without a Citrus socket, loaded canvas, or projector transaction;
- uses the existing selected-camera scope and requires every participating
  camera to be open, streaming, and backed by a snapshot worker;
- obtains a fresh native-resolution grouped temporal-mean capture from every
  selected camera while preserving production NIR/filter conditions;
- records the observed projector state as `off`, `not_in_use`, or
  `external_static` instead of inventing a Citrus scene;
- rejects missing frame identities, zero camera/system timestamps,
  non-native/full-resolution sources, incomplete groups, and a configurable
  cross-camera timestamp span above the default 1 ms limit;
- runs the Hough proposals concurrently, then shows raw magenta and
  operator-adjustable accepted orange fits for every camera;
- writes one accepted schema-v2 top-rim observation per camera and publishes a
  grouped manifest only after every exact observation exists and re-hashes;
  and
- leaves the new evidence `available_not_selected` until the operator performs
  a separate selection action.

The adjacent **Physical Registration Selection** surface discovers accepted
schema-v2 observations for the selected camera, displays compatible and
incompatible evidence, and writes an atomic pointer at:

```text
<calibration-base>/active/physical_dish_registration/Cam<serial>.json
```

The pointer uses schema
`orange.calibration.active_physical_registration_pointer` v1 and binds the
exact artifact ID, absolute observation path, SHA-256 digest, camera serial,
native raster, pixel format, coordinate space, and selection time. Selection
re-opens and hashes the observation immediately before publication. Restart
resolution performs the same identity, schema, camera, raster, pixel-format,
operator-acceptance, containment, and digest checks. Clearing publishes an
explicit `cleared` pointer and never deletes immutable evidence.

This active pointer is not yet a recording authority. Recording pre-arm must
consume and revalidate it in Phase E/F before a mask-dependent recording can
claim that this selection was used.

## Current Gaps

1. The Citrus-backed `src/gui/spatial_layout/daily_registration_workflow.cpp`
   still begins by selecting
   a Citrus runtime mode and acquiring a Citrus lease. Capture, rim fitting,
   persistence, projection translation, and runtime selection are presented as
   one transaction; it has not yet been refactored to call the standalone core
   and then optionally start projection registration.
2. Recording geometry supports `orange_only` and `not_configured`, but does not
   expose physical and projection registration as independently resolved
   products.
3. The active physical pointer is not yet consumed by stream start or recording
   pre-arm, so dependent features cannot rely on it yet.
4. The standalone review shows raw and accepted boundaries, while the derived
   centroid gate is currently preserved in saved review artifacts rather than
   drawn as a third live review circle.
5. A production-state context background is not yet a stable recording
   artifact, although its source burst can often be shared with the rim fit.

## Implementation Checklist

### Phase A: Freeze product and status contracts

- [ ] Use `daily_physical_dish_registration` and
      `daily_projection_registration` consistently in schemas, UI, and logs.
- [ ] Freeze independent status enums, reason codes, and lifecycle transitions.
- [ ] Define `not_applicable/no_active_projection_canvas` as a successful
      applicability result, not a warning.
- [ ] Define which recording features require a selected physical artifact and
      which merely recommend one.
- [ ] Preserve `base_only` as a valid Citrus runtime choice.
- [ ] Define freshness and compatibility fields: camera serial, native raster,
      stream epoch, camera-config digest, physical state, capture time, and an
      optional validity deadline.
- [ ] Update the recording geometry, spatial layout, and guided Orange/Citrus
      contracts with the split ownership model.

Exit criterion: a validator can decide applicability, selected authority, and
requiredness without checking whether Citrus happened to be reachable.

### Phase B: Extract the Orange-only workflow core

- [x] Create a focused, shared physical-registration save-preflight module
      rather than adding this policy to `orange.cpp`.
- [x] Extract the remaining reusable physical workflow state rather than
      adding it to the monolithic UI translation unit.
- [x] Extract reusable states for preflight, grouped capture, circle proposal,
      operator adjustment, acceptance, persistence, abort, and retry.
- [x] Keep Citrus lease, scene request, homography mapping, candidate creation,
      and runtime selection outside the Orange-only state machine.
- [x] Allow capture with Citrus absent and persist a versioned projector state
      such as `off`, `not_in_use`, or `external_static`.
- [x] Require cameras streaming, recording idle, and capture/save workers
      drained.
- [x] Preserve grouped capture for multiple participating cameras and reject
      groups outside the configured timestamp-span bound. This proves grouped
      timestamp coherence, not independent PTP lock or grandmaster health.
- [x] Require a fresh post-request full-resolution frame for every camera;
      never relabel a stale preview frame.
- [x] Reuse production NIR/TTL optical state without forcing filter removal or
      visible-light calibration exposure settings.

Exit criterion: Orange reaches an operator-reviewable rim proposal on every
selected camera while Citrus is stopped and no canvas is selected.

### Phase C: Decouple physical persistence from Citrus

- [x] Remove `citrus_template_matches_selected_camera` from eligibility to save
      a standalone camera-native top-rim observation.
- [x] Retain Citrus-template matching only for Citrus-linked projection or
      imported-layout products.
- [x] Replace that gate with camera identity, native raster, full-resolution
      role, accepted target, valid circle, and writer-idle checks.
- [x] Preserve raw Hough geometry and operator-accepted geometry separately in
      the schema-v2 artifact.
- [x] Derive the outward centroid gate from the accepted boundary and persist
      the exact policy and value.
- [x] Write source, raw-fit, accepted-fit, and valid-gate overlays with stable
      roles and declared FNV-1a checksums; selection additionally binds the
      observation and completion manifest with SHA-256.
- [x] Completion-mark each per-camera artifact only after its required files
      exist, then publish the all-camera grouped transaction manifest
      atomically with safe ownership handling.
- [x] Reject missing or mismatched capture-camera identities, non-native
      rasters, and downsampled source roles.
- [x] Add fail-closed fresh-frame, nonzero timestamp, completion-manifest, and
      source/review checksum revalidation gates to the standalone grouped
      flow and selector.
- [ ] Add structured physical-state contradiction gates beyond the explicit
      operator preparation confirmation.

Exit criterion: a complete accepted schema-v2 artifact saves, reloads, and
validates without a Citrus config or socket.

### Phase D: Add first-class Orange selection and review

- [x] Add a per-camera accepted-artifact selector to the spatial registration
      UI; environment variables must not be required for normal operation.
- [x] Show artifact ID, time, camera/raster identity, physical state, accepted
      circle, centroid outset, compatibility, validity, and
      selected/not-selected state.
- [ ] Show raw Hough, accepted physical rim, and centroid gate in live and
      saved-image review surfaces.
- [x] Keep operator acceptance and runtime selection as separate actions.
- [x] Support clearing a selection without deleting immutable evidence.
- [x] Persist selection in an atomic Orange active pointer or session
      assignment with rollback-safe replacement.
- [x] Revalidate the pointer target at recording pre-arm. Stream-start preview
      revalidation remains a UI follow-up; runtime activation never trusts the
      preview cache.
- [ ] Keep `ORANGE_SPATIAL_CALIBRATION_ARTIFACT_<serial>` as an explicit
      compatibility path below an intentional UI/session selection.

Exit criterion: restarting Orange restores or clearly requests the intended
physical selection without relying on Citrus.

### Phase E: Split recording-bound metadata

- [x] Add independent `physical_registration` and
      `projection_registration` members per observation edge.
- [x] Embed the physical circle, centroid gate, dish mask, coordinate
      descriptor, artifact identity, schema, digest, and status in
      `recording_geometry_contract.json`.
- [x] Copy compact immutable physical evidence into
      `recording_geometry_assets` with recording-relative paths and SHA-256.
- [x] Project resolved references into `recording_snapshot.json`. Direct
      `recording_session.json` projection remains a follow-up; the session
      already references the recording geometry contract through the sealed
      snapshot.
- [x] Record projection registration as `not_applicable` when no Citrus canvas
      participates; never synthesize a canvas identity.
- [x] When Citrus participates, bind its selected or `base_only` result without
      changing the Orange physical artifact.
- [x] Define precedence and contradiction handling among explicit recording
      selection, Orange active pointer, environment compatibility hook,
      Citrus selection, and unselected nearby artifacts.
- [x] Embed only participating observation edges, not every configured camera
      or arena.

Exit criterion: a recording made with Citrus stopped contains a digest-bound
camera-space dish mask and explicitly not-applicable projection status.

### Phase F: Feature-dependent pre-arm validation

- [x] Keep ordinary full-frame recording non-blocking for `not_performed` or
      `available_not_selected` physical registration.
- [x] Require a valid selected physical artifact for live neural-network input
      masking.
- [ ] Require it for production crop-only when the profile declares a
      registered mask or reconstructable context.
- [x] Fail closed for a selected artifact that is invalid, stale under its
      policy, or incompatible with the runtime camera snapshot.
- [x] Distinguish `missing_required`, `invalid_selected`, `not_performed`, and
      `not_applicable` in UI and machine-readable errors.
- [x] Never silently fall back from an invalid selection to an older or merely
      nearby artifact.
- [ ] Offer an explicit route to disable the dependent feature or return a
      Citrus experiment to `base_only` where scientifically valid.

Exit criterion: requiredness follows the selected feature, not Citrus
availability or camera streaming alone.

### Phase G: Share source capture with recording context

- [ ] Add the production-state grouped context capture defined in
      `crop_only_recording_and_context_reconstruction_design.md`.
- [ ] Allow one ordered source burst to be referenced by both physical-rim and
      context-image products.
- [ ] Keep distinct schemas, derived artifacts, digests, and acceptance states.
- [ ] Compute the deterministic median only after validating all source images.
- [ ] Require explicit subject absence and compatible dish, water, holder,
      filter, illumination, and projector state for background eligibility.
- [ ] Capture a separate subject-present start context after subjects enter.
- [ ] Reject heuristic reuse based only on capture time or filename.

Exit criterion: crop-only pre-arm resolves both its dish mask and background
evidence from one preparation capture without conflating the two artifacts.

### Phase H: Preserve the optional Citrus handoff

- [ ] Make the guided workflow call the shared Orange physical core first and
      start a separate projection transaction only when requested.
- [ ] Pass the accepted observation identity and digest to Citrus, never an
      untracked UI circle.
- [ ] Keep Citrus authoritative for camera-to-canvas mapping, integer
      translation, candidate QC, acceptance, and runtime selection.
- [ ] Verify projection acceptance never rewrites the Orange rim,
      commissioning homography, canonical radius, or stable canvas geometry.
- [ ] Keep live manual nudge/review in the projection step, not as a mutation
      of the camera-native physical fit.
- [ ] On Citrus cancellation, timeout, or absence, retain the complete Orange
      physical artifact and report projection status independently.

Exit criterion: the current successful alignment flow remains available, but
its failure cannot erase or invalidate a valid physical fit.

### Phase I: Automated and live validation

- [ ] Unit-test statuses and feature-dependent requiredness.
- [ ] Unit-test physical artifacts with no Citrus fields present.
- [ ] Test that a mismatched Citrus template does not block physical
      persistence but does block a linked projection operation.
- [ ] Test raw-versus-accepted geometry and outward-gate derivation.
- [x] Test active-pointer atomicity, restart reload, clear, stale selection,
      and rollback.
- [ ] Test manifest precedence and contradiction failures.
- [ ] Test one- and four-camera grouped capture with fresh-frame and
      timestamp-span checks; add a separate PTP-evidence gate if a workflow
      needs to claim PTP synchronization rather than grouped coherence.
- [ ] Run an automated Orange GUI flow with Citrus stopped and projector off.
- [ ] Run the same capture with Citrus active, complete projection
      registration, and compare both independently persisted products.
- [ ] Make a full-frame recording with registration omitted and verify success.
- [ ] Make a masked-input or crop-only recording with registration selected and
      verify pre-arm, embedded geometry, copied evidence, and mask loading.
- [ ] Verify Palette resolves the camera-native mask without a Citrus H5 or
      canvas reference.

Exit criterion: all policy combinations report truthful statuses, with no fake
Citrus canvas required to preserve a physical dish boundary.

## Recommended First Coding Slice

Implement Phases A through C first:

1. freeze the split status and applicability contract;
2. extract the physical capture/fit/persist core from the guided transaction;
3. remove the Citrus-template dependency from standalone top-rim persistence;
4. add focused no-Citrus unit and workflow tests; and
5. prove the accepted artifact reloads with its raw fit, accepted rim, and
   outward centroid gate intact.

This adds standalone scientific value without changing recording defaults,
crop-only retention, or Citrus runtime geometry. First-class selection and
recording integration follow after the artifact lifecycle is stable.
