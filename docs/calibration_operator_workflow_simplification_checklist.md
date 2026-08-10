# Calibration Operator Workflow Simplification Checklist

Status: implementation checklist; current daily-alignment behavior is the
accepted baseline

Date: 2026-08-10

Implemented P0 slice on 2026-08-10:

- [x] Centralized checked composition of automatic and manual daily
      translations and used it for candidate validation and live preview.
- [x] Added regression coverage for signed manual deltas, automatic reset
      semantics, canonical-radius preservation, and integer overflow.
- [x] Removed the currently open Orange session from commissioning-finalize
      authority; Citrus now derives source sessions only from accepted active
      homography and scale pointers.
- [x] Moved release mutation and confirmation into a background worker.
- [x] Automatically reconcile and display the exact compatible active release,
      including lost-acknowledgement recovery and definitive rejection.
- [x] Keep both applications open after publication and avoid Citrus status
      polling on the GUI/render thread.
- [ ] Run the new release-publication interaction in a live Orange/Citrus GUI
      session.

## Goal

Make stable commissioning, daily dish registration, and experiment readiness
feel like one coherent operator workflow without weakening their separate
authorities.

The current daily registration interaction is the behavior to preserve:

```text
fit physical inner rim
  -> compute automatic translation
  -> show live physical projection and computed overlay
  -> optionally apply per-arena manual nudges
  -> freeze operator alignment
  -> explicitly accept and select the immutable registration
```

This work must not change the canonical experimental-area radius, refit a base
homography during daily registration, or make daily registration mandatory.

Related contracts and plans:

- `docs/rig_canvas_commissioning_release.md`
- `docs/orange_citrus_guided_daily_registration_contract.md`
- `docs/calibration_surfaces_architecture_audit_2026_07_26.md`
- `docs/commissioned_setup_packaging_implementation_checklist.md`
- `docs/calibration_transaction_lease.md`
- `docs/spatial_layout_ui_refactor_plan.md`

## Existing Foundation To Preserve

- [x] Citrus owns immutable accepted homography and projected-surface scale
      products plus atomic active pointers.
- [x] Citrus can report per-arena commissioning readiness and finalize an
      immutable rig/canvas commissioning release.
- [x] Orange exposes commissioning readiness, finalization, and optional
      `base_only` runtime controls in Spatial Layout.
- [x] Commissioning finalization uses an exact canvas checksum and an explicit
      armed acceptance.
- [x] Daily registration translates the arena and experimental area together
      without changing the canonical radius or base homography.
- [x] Daily registration preserves automatic, manual-delta, and final integer
      translations separately.
- [x] The review screen updates after every Citrus-acknowledged manual nudge.
- [x] The operator can freeze the physical alignment before accepting it.
- [x] Daily registration remains optional; `base_only` is an explicit valid
      runtime selection.
- [x] Calibration mutation is protected by the shared transaction lease.
- [x] The 2026-08-10 Shadow homography and scale sets were successfully sealed
      into a four-camera stable commissioning release, and a subsequent daily
      registration produced substantially improved physical alignment.

## Non-Negotiable Invariants

- [ ] Keep stable commissioning, daily registration, and experiment runtime
      selection as distinct lifecycle objects.
- [ ] Keep publication and runtime selection explicit operator actions.
- [ ] Never create a commissioning release merely by opening a canvas.
- [ ] Never require new captures when only refreshing a release over already
      accepted compatible members.
- [ ] Never infer that a successful fit is accepted or runtime-selected.
- [ ] Never mutate `shadow.json`, a canonical radius, arena size, homography, or
      scale as a side effect of daily registration.
- [ ] Never run compatibility validation or artifact polling in the render,
      acquisition, inference, or experiment-update hot paths.
- [ ] Preserve `base_only` operation when daily registration is skipped.
- [ ] Fail closed when an operator explicitly selects an incompatible artifact;
      do not silently substitute a different calibration.

## P0: Freeze The Working Daily-Registration Baseline

- [ ] Add a replay fixture based on the accepted 2026-08-10 Shadow
      commissioning release and a compatible daily-registration transaction.
- [ ] Test camera-center to canvas-center translation direction for all four
      cameras.
- [ ] Test that manual `+X`, `-X`, `+Y`, and `-Y` controls move the physical
      projection in the documented canvas directions.
- [ ] Test that the final translation is the automatic translation plus the
      absolute manual delta, followed by the existing integer quantization.
- [ ] Test that resetting to automatic removes only the manual delta.
- [ ] Test that freezing and accepting preserves the exact reviewed candidate.
- [ ] Test that rejection, abort, timeout, or restart leaves the previous active
      registration unchanged.
- [ ] Record expected overlay and artifact identities without treating a phone
      photograph as geometric ground truth.

Done when the current successful alignment behavior is guarded by deterministic
replay tests before UI or lifecycle refactoring begins.

## P0: Unified Calibration Authority Snapshot

- [ ] Add one Orange service that obtains and normalizes:
  - loaded rig and canvas identity;
  - current canvas checksum and projection-geometry fingerprint;
  - active homography and scale identity for every arena/camera;
  - homography-to-scale binding status;
  - active stable commissioning release and compatibility;
  - commissioning readiness and exact blocking reasons;
  - daily-registration availability and runtime selection;
  - active calibration transaction owner, if any.
- [ ] Represent unavailable Citrus, missing authority, stale authority,
      incompatible authority, ready-to-publish, and compatible-active as
      different states.
- [ ] Include exact artifact IDs, paths, checksums, target planes, and source
      sessions in the normalized result.
- [ ] Refresh this snapshot after relevant operator actions and at experiment
      arm, not from a periodic render-path loop.
- [ ] Keep the existing Citrus socket methods as the authority; do not duplicate
      compatibility logic in ImGui rendering code.
- [ ] Add pure parser/state-reduction tests for complete, partial, stale,
      unavailable, and malformed replies.

Suggested Orange boundary:

```text
calibration_authority_service
  -> immutable CalibrationAuthoritySnapshot
  -> calibration home panel, commissioning workflow, and arm preflight
```

## P0: One-Action Stable Release Refresh

The current Spatial Layout controls are a useful foundation, but finalization
still requires the operator to understand source-session assembly and manually
refresh status afterward.

- [ ] After homography or scale promotion, automatically refresh commissioning
      readiness once the promotion transaction reaches a terminal state.
- [ ] When all required members pass, show a single primary action:

  ```text
  4/4 compatible -> Publish refreshed stable release
  ```

- [ ] Preserve an explicit armed acceptance immediately before publication.
- [ ] Have Citrus expose the complete source-session set it authoritatively
      infers from the active homography and scale pointers.
- [ ] Do not inject `ui_state->calibration_session_dir` merely because that
      session happens to be open. An unrelated or incomplete open session must
      not become a commissioning member; homography and scale evidence may also
      come from separate sessions, as they did on 2026-08-10.
- [ ] Show the exact homography set, scale set, canvas checksum, rig revision,
      and source sessions before acceptance.
- [ ] Submit one compare-and-swap finalization request using the exact canvas
      checksum and expected member identities.
- [ ] Make retry idempotent across a lost acknowledgement.
- [ ] On success, automatically refresh the authority snapshot and show the new
      release ID, immutable manifest path, checksum, and `accepted` state.
- [ ] Do not close Orange or Citrus after successful publication.
- [ ] Replace ambiguous completion text with an explicit terminal result:
      `Published`, `Rejected`, `Failed`, or `Unchanged`.
- [ ] Provide `Open evidence` and `Copy release ID` actions.
- [ ] Do not offer publication for mixed candidate sets, missing acceptance
      receipts, incompatible scales, or scales bound to older homographies.

Tests:

- [ ] Homography and scale from the same Orange session.
- [ ] Homography and scale from two separate Orange sessions.
- [ ] One missing session index.
- [ ] One stale active pointer between readiness and finalization.
- [ ] Canvas checksum changes between review and finalization.
- [ ] Lost response followed by an identical retry.
- [ ] Successful publication requires no capture or camera stream.

## P0: Experiment-Arm Calibration Preflight

- [ ] Run one frozen calibration preflight before experiment arming.
- [ ] Show the exact compatible commissioning release that will be used.
- [ ] Show either the exact selected daily registration or explicit
      `base_only` mode.
- [ ] Reject a selected stale daily registration before any arena starts.
- [ ] Allow explicit `base_only` operation when no daily registration exists.
- [ ] Freeze one composition identity for Orange recording metadata and Citrus
      H5 runtime metadata.
- [ ] Ensure the validated geometry cannot change while the experiment is armed
      or active.
- [ ] Keep all status reads and release validation outside the 120 Hz rendering
      and arena-update paths.

## P1: Calibration And Commissioning Home

- [ ] Add one top-level `Calibration & Commissioning` home rather than requiring
      operators to discover controls deep in Spatial Layout.
- [ ] Show three clearly separate cards:
  1. `Stable rig/canvas commissioning`;
  2. `Today's dish registration`;
  3. `Experiment runtime selection`.
- [ ] Display a four-row arena/camera matrix with homography, scale, stable
      release, daily registration, and runtime status.
- [ ] Give every blocking state one plain-language explanation and one next
      action.
- [ ] Make `ready to publish` visually distinct from `published and active`.
- [ ] Make `accepted but not selected` visually distinct from `selected for
      runtime`.
- [ ] Put raw schema editing, manual artifact paths, and fit-tuning internals
      under `Advanced`.
- [ ] Reuse the normalized authority snapshot rather than issuing socket calls
      from the render function.
- [ ] Extract this panel from `src/spatial_layout_ui.cpp` into a focused module.

## P1: Versioned Acquisition Recipes And Applied-State Journal

- [ ] Define one versioned recipe contract for each commissioning stage.
- [ ] Resolve projector intensity from an accepted projector-intensity product
      or the recipe; do not scatter a literal value such as `76` across runners.
- [ ] Include required camera frame rate, exposure, gain, trigger mode, PTP mode,
      IR/visible illumination, filter state, scene, settling interval, averaging,
      target identity, holder state, dish state, water state, and target plane.
- [ ] Show required physical actions and wait for operator confirmation before
      acquisition.
- [ ] Snapshot actual camera, projector, and lighting state before mutation.
- [ ] Record every applied setting and acknowledgement.
- [ ] Restore the exact prior state on success, rejection, abort, and timeout.
- [ ] Persist the pre-mutation/restoration journal so abandoned transactions can
      be recovered after restart.
- [ ] Refuse capture when the actual state differs from the acknowledged recipe.
- [ ] Keep camera mutations off the GUI thread.

## P1: Acquisition-Quality Gates Before Fitting

- [ ] Calculate native-frame black level, useful dynamic range, clipped-high and
      clipped-low fractions, and local contrast before target fitting.
- [ ] Add a focus/sharpness warning suitable for the expected target features.
- [ ] Verify expected disk outside diameter, hole/feature count, 5 mm pitch, and
      25 mm orientation span before scale fitting.
- [ ] Reject excessive ellipse/circularity residual when the expected physical
      target is circular.
- [ ] Detect missing, merged, or low-contrast target features and explain which
      acquisition condition likely failed.
- [ ] Require the asymmetric orientation marker to be visible and unambiguous.
- [ ] Record diffuser/target-plane physical confirmation as operator evidence;
      do not infer flatness solely from a successful fit.
- [ ] Produce a QC overlay before enabling candidate acceptance.
- [ ] Separate hard gates from operator-confirmable warnings.
- [ ] Persist metrics, thresholds, software version, and the exact native source
      frame with the candidate.

## P1: Independent Coordinate-Orientation Proof

- [ ] Add an asymmetric projected validation scene that visibly identifies
      canvas origin, `+X`, and `+Y`.
- [ ] Capture it as a PTP-grouped four-camera evidence set.
- [ ] Compute and record transform determinant, handedness, local axis
      directions, and orientation-marker correspondence for every camera.
- [ ] Fail commissioning when a reflection, axis swap, or direction inversion is
      detected.
- [ ] Validate orientation independently from the symmetric circle/ring fit.
- [ ] Preserve a human-readable overlay showing camera pixels, arena-local
      coordinates, and global canvas coordinates.
- [ ] Bind the orientation proof and checksum into the stable release.
- [ ] Add a synthetic reflected-transform regression test that must fail.

## P1: Restart-Safe And Unsurprising Lifecycle

- [ ] Persist candidate-review state needed to resume after either application
      restarts.
- [ ] Reload and revalidate immutable scale candidates without refitting their
      source observations.
- [ ] Use unique request IDs with idempotent operation IDs for every mutation.
- [ ] Show transaction stage, last acknowledged operation, and current owner.
- [ ] On successful workflow completion, remain open and return to a stable home
      state by default.
- [ ] If an automation runner intentionally exits an application, announce that
      behavior before execution and report a normal terminal status afterward.
- [ ] Distinguish application exit, transport disconnect, rejected mutation,
      timeout, crash, and successful one-shot completion.
- [ ] Add restart/reconnect tests at capture, fit, review, promotion, release
      publication, and daily-registration acceptance boundaries.

## P2: Evidence And Authority Browser

- [ ] Browse releases, member products, receipts, source sessions, daily
      registrations, and recording-local capsules from one UI.
- [ ] Display raw evidence separately from derived overlays and accepted runtime
      products.
- [ ] Verify checksums on demand and report missing external members.
- [ ] Compare two releases or two daily registrations without activating either.
- [ ] Show which recordings reference a selected release or registration when
      that index is available.
- [ ] Add a generated calibration documentation/product status index.

## P2: Code Organization

- [ ] Move commissioning authority rendering out of
      `src/spatial_layout_ui.cpp`.
- [ ] Keep Citrus transport in
      `src/gui/spatial_layout/projection_snapshot_client.*` or a narrower
      successor transport module.
- [ ] Put status normalization, source-session derivation, and state reduction
      in non-ImGui services with focused tests.
- [ ] Keep camera/projector mutation in background workflow services.
- [ ] Keep immutable publication in one owner per product.
- [ ] Avoid introducing a second recipe registry, artifact publisher, or
      compatibility implementation.

## Validation Matrix

- [ ] Pure unit tests for authority-state normalization and UI action
      eligibility.
- [ ] Pure unit tests for source-session union and checksum/CAS request assembly.
- [ ] Replay tests using the accepted four-camera Shadow artifacts.
- [ ] Socket contract tests for success, stale pointer, malformed response,
      timeout, lost acknowledgement, and idempotent retry.
- [ ] Headless workflow test proving stable release publication without cameras.
- [ ] GUI automation test proving no application restart is required between
      promotion, publication, and daily registration.
- [ ] Four-camera PTP acquisition test for recipe application and restoration.
- [ ] Live physical validation under normal experimental optics and water state.
- [ ] Experiment-arm test for both selected daily registration and explicit
      `base_only`.
- [ ] Recording metadata/H5 comparison proving one exact calibration composition
      identity.
- [ ] Performance test proving no new periodic work enters render, acquisition,
      inference, encoding, or arena-update hot paths.

## Recommended Delivery Order

1. Freeze the working daily-registration behavior with replay tests.
2. Add the normalized authority snapshot and expose Citrus's authoritative
   source-session derivation.
3. Make stable release refresh one explicit action with automatic terminal
   refresh.
4. Add experiment-arm calibration preflight.
5. Move those controls into the Calibration & Commissioning home.
6. Introduce versioned recipes and the persistent applied-state journal.
7. Add pre-fit acquisition QC and independent orientation proof.
8. Complete restart-safe review/revalidation and the evidence browser.
9. Finish package self-containment and shared Orange/Citrus composition identity
   under the existing packaging checklist.

## Completion Criteria

This simplification is complete when a new operator can:

1. open one calibration home and understand the current authority without
   reading logs or filesystem paths;
2. acquire and promote homography/scale evidence with automatically applied and
   restored settings;
3. publish a compatible four-camera stable release in one explicit action,
   including all source sessions and without taking new images;
4. complete the existing daily alignment, including live manual correction,
   without navigating Citrus UI;
5. arm an experiment with an explicit compatible daily registration or
   `base_only` selection; and
6. recover from restart or lost acknowledgement without refitting immutable
   evidence or guessing whether an operation succeeded.
