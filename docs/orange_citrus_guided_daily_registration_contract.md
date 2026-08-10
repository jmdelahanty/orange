# Orange/Citrus Guided Daily Registration Contract

Date: 2026-07-18

Status: rim-only daily coordinator implemented; live four-camera validation
remains. Named fixture-aware workflows, grouped capture, commissioning, the
Citrus translation-only daily-registration transaction, immutable acceptance,
explicit runtime selection, and Orange/Citrus recording provenance are
implemented. The default Orange workflow now keeps the normal IR filters and
mapped experiment illumination in place, captures only the physical inner rim,
and uses the accepted commissioning homography plus Citrus's canonical
experimental-area center to compute and review the exact integer runtime
translation. Visible projected-marker capture and transient optical preview
remain optional recommissioning diagnostics, not daily gates. Restart-time state
rehydration, per-target partial retry, and a real four-camera dish run remain
before calling the workflow
production-hardened.

The real-GUI grouped-capture lifecycle now also has a calibration-specific
semantic autorun and supervised smoke runner documented in
`docs/gui_guided_capture_smoke.md`. It validates scene presentation, fresh
grouped acquisition, scene consistency, restoration, and optional image-set
persistence without simulating ImGui clicks. It does not implement daily
center fitting or registration activation.

## Decision

For a bolted, mechanically stable rig, calibration is split into two products:

```text
rare commissioning calibration
  + daily dish-placement registration
  = effective runtime geometry
```

The commissioning product owns the stable camera/projector/canvas geometry.

The stable product is now formalized as the rig-owned, per-canvas immutable
release described in `docs/rig_canvas_commissioning_release.md`. Daily
registration references that release and must not mutate its canvas snapshot,
homography members, or projected-surface scale members.
The daily product owns only the small placement translation caused by removing
and replacing a dish.

Daily registration is opt-in. A user may run from the compatible commissioned
base geometry without performing rim fitting, projected-center capture, or a
daily translation. That is a normal, recorded `base_only` mode, not a
calibration error or an override. The absence of a daily artifact must never
block recording by itself.

Selecting a daily artifact is a separate operator decision. Once selected, it
must be current and compatible; Citrus must not silently fall back from a
stale selected translation to a different translation. The UI must instead
offer the user an explicit return to `base_only` mode. This preserves both
optional use and fail-closed handling of geometry the user actually chose.

Orange is the operator-facing workflow coordinator. Citrus remains the
authority for its canvas, arena, experimental-area geometry, projection state,
base homography, and accepted runtime registration. Orange may request named
calibration components from Citrus, capture the resulting camera evidence, and
guide the operator through review without requiring routine application
switching.

For the current circular Palm-dish rig, one accepted daily translation moves
the effective arena placement and its experimental area together. The daily
workflow does not independently resize, rotate, or reshape either one.

## Motivation

The rig, camera mounts, projection surface, projector, and final display canvas
are bolted into place. Their geometry should remain stable across ordinary
days. The dish itself is removable and can return a few pixels away from its
previous center.

Rewriting a homography or canonical arena configuration to absorb that small
dish-placement shift would mix two different physical facts:

- how the fixed camera and projector relate to the fixed canvas; and
- where today's removable dish sits inside that stable system.

Keeping those facts separate makes daily work faster, preserves provenance,
and provides a clear escalation path when the rig actually has moved.

The desired operator experience is one guided workflow in Orange. Behind that
workflow, Orange and Citrus perform an explicit, recoverable transaction:

```text
Orange requests a black Citrus scene
  -> Citrus acknowledges the exact rendered content
  -> Orange captures and measures the physical rims under normal IR imaging
  -> Citrus computes a registration candidate
  -> Orange inverse-projects the unchanged canonical outline for review
  -> the operator accepts or rejects
  -> Citrus atomically activates an immutable daily artifact
  -> both applications snapshot the same artifact identity
```

## Scope

This contract covers:

- the stable-commissioning versus daily-registration boundary;
- Orange/Citrus ownership;
- a guided daily center-registration workflow;
- named Citrus calibration scenes requested by Orange;
- camera-frame and rendered-scene provenance;
- candidate, computed-review, acceptance, abort, and recovery semantics;
- immutable Orange evidence and Citrus registration artifacts;
- multi-camera and multi-arena grouping;
- runtime and recording provenance;
- conditions that stop the daily path and require recommissioning.

This contract does not yet implement:

- restart-time rehydration of an interrupted Orange wizard from its persisted
  checkpoint;
- per-camera partial retry without recapturing successful targets;
- atomic selection of the accepted Orange top-rim artifact through the
  general per-camera spatial-calibration loader. That older loader still uses
  `ORANGE_SPATIAL_CALIBRATION_ARTIFACT_<serial>` independently of this Citrus
  transaction;
- a fish-observation-plane transform;
- refraction or full 3-D modeling;
- arbitrary Orange-authored Citrus projection geometry.

## Relationship To Existing Documents

This contract narrows and coordinates the current-rig workflow described in:

- `docs/spatial_layout_contract.md`;
- `docs/dish_top_rim_observation_design.md`;
- `docs/projected_center_alignment_todo.md`;
- `docs/calibration_stack_lifecycle_audit_2026_07_18.md`;
- Citrus `docs/orange_citrus_dish_pose_registration_design.md`;
- Citrus `docs/orange_citrus_center_alignment_control_todo.md`;
- Citrus `docs/citrus_gui_automation_control.md`.

For the bolted circular-dish rig, this contract supersedes older suggestions
that the normal daily workflow directly edit the canonical Citrus
experimental-area center or radius. Daily acceptance creates a separate
runtime registration overlay. It does not rewrite the rig configuration.

The broader ownership, coordinate-frame, and immutable-artifact rules in the
existing documents remain in force.

## Calibration Products

### Physical layout layers and capture profiles

The Shadow canvas and each camera's arena assignment are stable commissioning
geometry. They are not replaced when the holder or dish is installed. The
capture stack records three distinct spatial facts:

1. `unobstructed_arena_rectangle`: the full rectangular arena assigned to one
   camera on the stable Citrus canvas, observable with the holder removed;
2. `imaging_shelf_aperture`: the camera-visible circular cutout after the
   holder is installed; and
3. the installed tank's experimental area: the physical region the animal can
   occupy, owned by the tank design and daily placement registration.

The third item is not stored as a `visibility_domain`; it already has its own
experimental-area and physical-rim products. Until the shelf aperture is
measured independently, holder-installed captures record it as a circular
visibility domain with `geometry_status = "unmeasured"`.
The circular ring point set remains in `circular_experimental_domain` and is a
subset of that camera-visible aperture; it is not relabeled as the aperture.

The implemented named profiles are:

| Profile | Fixture | Citrus scene | Homography role |
| --- | --- | --- | --- |
| `unobstructed_canvas_commissioning` | holder removed, dish absent, dry | `homography_grid` | `commissioning_reference` |
| `holder_installed_projected_surface` | holder installed, flattened operational gels, dish absent, dry | shape-appropriate support plus verification | support image: `operational_candidate`; companion images: `validation_only` |
| `wet_tank_projected_surface` | holder and dish installed, wet | `homography_rings` | `operational_candidate` or validation |
| `installed_tank_registration` | holder and dish installed | `experimental_area_center_and_outline` | daily registration evidence, not a homography rewrite |

`homography_grid` explicitly renders a full-arena unmasked rectangular grid.
Its revision-2 pattern enlarges the row-0/column-1 point so Citrus can
automatically distinguish every rotation/reflection before assigning
correspondences. The marker is part of fit validity, not an optional visual
annotation.
`homography_rings` explicitly renders circular rings centered on the Citrus
experimental area. Both produce correspondences for the same class of planar
transform; “grid” and “rings” describe visible support, not different
homography mathematics.

The holder-installed checkpoint consumes the accepted dry commissioning
homography as a read-only comparison, records the holder's visibility aperture,
fits an immutable operational candidate on the real installed projection
surface, and validates it with an independent point set. It never promotes that
candidate automatically. See
[holder_fixture_validation.md](holder_fixture_validation.md).

The `arena_outline` reference scene renders the arena rectangle plus a small
ring-with-center-dot at the configured arena center. That marker is visual
layout evidence only and remains separate from both homography correspondences
and experimental-area geometry.

### Commissioning calibration

Commissioning is rare and explicit. It owns:

- rig, canvas, display, arena, and camera identities;
- camera image dimensions and crop/orientation conventions;
- final display canvas dimensions and orientation;
- canonical arena placement and local coordinate frame;
- canonical experimental-area center, shape, and physical size;
- tank/dish physical dimensions;
- projector scale and its measurement provenance;
- camera-to-final-display homography for a named plane;
- lens model, if one is accepted later;
- pattern configuration and calibration support domain;
- fit correspondences, residuals, holdout results, and acceptance;
- any plane-specific center-bias model accepted for daily registration.

Commissioning outputs immutable artifacts and an atomic active-artifact
reference. It must never be started implicitly because a daily fit looks bad.

### Daily placement registration

Daily registration is expected after the dish is placed. It owns:

- the observed water-side inner-rim center in the camera image;
- the measured rim circle as evidence and a health diagnostic;
- a proposed placement translation in final-display canvas pixels;
- operator adjustments to that proposed translation, if any;
- a computed camera-space review of the unchanged canonical outline after the
  exact integer translation Citrus will apply;
- acceptance/rejection state and operator provenance;
- the base calibration identity against which it is valid;
- its validity interval and invalidation reason.

For the current circular rig, the accepted runtime effect is translation only.

### Runtime effective geometry

Runtime effective geometry is derived, not separately authored. It has two
explicit modes:

```text
base_only:
  effective geometry = immutable commissioning geometry

selected_daily_registration:
  effective geometry
    = immutable commissioning geometry
    + explicitly selected accepted daily placement translation
```

Recordings always reference the base input and record the mode. In
`selected_daily_registration` they also reference the daily input. They must
not snapshot only the resulting numbers while losing the identities that
produced them.

## Non-Negotiable Invariants

1. A daily registration never overwrites a base homography, canonical canvas,
   arena definition, physical dimension, projector scale, or tank definition.
2. The current circular-dish daily registration has exactly two translational
   degrees of freedom: `dx` and `dy` in final-display canvas pixels.
3. The arena and experimental area move together. Their arena-local
   relationship remains unchanged.
4. The observed daily radius is evidence and a quality check. It does not
   silently change the canonical 40 mm radius.
5. Orange never directly writes Citrus configuration or active-registration
   files.
6. Citrus never invents or relabels Orange camera evidence.
7. A rendered calibration scene is usable only after Citrus acknowledges its
   exact content and a render-readiness fence.
8. Preview is transient. Acceptance is a separate explicit operator action.
9. No geometry may be accepted or activated while an experiment is armed or
   active.
10. Failure, timeout, or cancellation restores the prior Citrus projection
    scene and Orange-controlled camera/illumination state.
11. Every active daily registration identifies the exact accepted base
    calibration and configuration fingerprint it depends on.
12. A base-identity mismatch invalidates the daily registration; it is never
    repaired by copying the old translation forward.
13. Every semantic mutation is idempotent and auditable.
14. The daily workflow stops rather than expanding itself into a hidden base
    recalibration.
15. Daily registration is optional. Its absence is represented as
    `base_only` / `not_performed` and is never, by itself, an experiment-start
    blocker.
16. A stale or invalid daily artifact blocks only
    `selected_daily_registration`. Continuing requires an explicit operator
    choice to return to `base_only`; Citrus never silently substitutes or
    reuses a translation.

## Ownership

| Concern | Authority | Responsibilities |
| --- | --- | --- |
| Camera acquisition | Orange | Fresh frames, camera IDs, local and device frame IDs, timestamps, image checksums |
| Physical rim observation | Orange | Hough proposal, raw detected circle, operator-confirmed water-side inner edge, camera-space quality |
| Guided operator UI | Orange | Step sequencing, instructions, previews, warnings, accept/reject controls |
| Projection rendering | Citrus | Named component rendering from Citrus-owned geometry, readiness fences, rendered-content snapshots |
| Canvas and arena definitions | Citrus | Canonical layout identity and geometry |
| Base homography selection | Citrus | Accepted artifact selection, compatibility validation, mapping into Citrus coordinates |
| Registration proposal | Citrus | Authoritative conversion of Orange evidence into a candidate daily translation |
| Registration activation | Citrus | Validation, immutable artifact write, atomic active-pointer update, rollback |
| Physical interventions | Operator | Dish placement, water state, and water-side inner-rim feature confirmation |
| Recording provenance | Both | Snapshot the same base and daily artifact identities and checksums |

Orange may display an independently calculated translation as a diagnostic,
but Citrus computes the authoritative proposal because Citrus owns the active
homography, arena embedding, and runtime apply semantics.

## Geometry Contract

### Coordinate frames

The workflow uses named frames; unqualified `x`, `y`, `center`, `radius`, or
`delta` fields are invalid.

- `camera_native_px`: the Orange camera sensor raster, origin at the top left,
  +X right, +Y down.
- `final_display_canvas_px`: the logical Citrus full-canvas composition buffer,
  origin at the top left, +X right, +Y down. Presentation to the GLFW window
  and physical projector is a separate mapping and may include reflection.
- `arena_relative_canvas_px`: coordinates local to the canonical arena, origin
  at the arena top left, +X right, +Y down.
- `dish_top_rim`: physical plane of the water-side inner rim observation.
- `projected_surface`: plane named by the accepted base homography.
- `fish_observation_plane`: future explicit product; absent in V1.

The commissioning homography direction is always
`camera_native_px_to_final_display_canvas_px`. For rectangular-grid revision 2,
point ordering is row-major in the destination frame: `(0,0)` is the grid
origin, the enlarged `(0,1)` point establishes +X, and `(1,0)` establishes +Y.
Orange requires Citrus's orientation-validation receipt and matching coordinate
contract before a fit can pass its workflow validator.

`final_display_canvas_px` describes Citrus's logical composition indices; it
does not claim matching signs in the GLFW window, projector raster, projector
enclosure, camera sensor, or laboratory. The established presentation path,
OS rotation, projector firmware/keystone, and physical mounting are separate
transforms. A camera keeps top-left/right/down sensor coordinates; the fitted
homography accounts for the complete effective optical relationship, including
reflection.

Every immutable candidate makes this relationship visual. Its camera overlay
draws the camera raster basis and the detected `G00 -> G01` logical +X and
`G00 -> G10` logical +Y vectors, while its receipt records the corresponding
camera, arena-local, and global canvas coordinates and signed determinant. A
candidate-set layout image separately shows the full logical canvas, each
arena's global placement/local origin, and the experimental-area geometry.
These evidence products document the existing mapping; they do not change the
render or presentation path.

### Canonical and effective centers

Let:

- `A0` be the canonical arena origin in `final_display_canvas_px`;
- `E0` be the canonical experimental-area center in
  `arena_relative_canvas_px`;
- `d` be the accepted daily placement translation in
  `final_display_canvas_px`.

The current-rig apply rule is:

```text
effective_arena_origin_canvas = A0 + d
effective_experimental_center_canvas = A0 + d + E0
effective_experimental_center_arena_relative = E0
```

This apply target is named `arena_placement_translation`. It moves the entire
arena-local scene while preserving every arena-local coordinate and size.

An alternative future target,
`experimental_area_center_translation_arena_relative`, is a different
operation. It must use a different schema value and may not be combined with
`arena_placement_translation` in one V1 registration. The current bolted rig
uses `arena_placement_translation` because the removable dish placement, not
the canonical layout, is the daily variable.

### Center proposal

Orange supplies `C_rim_camera`, the operator-confirmed water-side inner-rim
center. Citrus uses the accepted commissioning mapping
`H_camera_to_display` and its canonical global experimental-area center
`C0_experimental_display` to form the candidate:

```text
C_rim_display = H_camera_to_display(C_rim_camera)
d_requested = C_rim_display - C0_experimental_display
d_applied = round_to_integer_canvas_pixels(d_requested)
C_effective_display = C0_experimental_display + d_applied
```

Orange independently inverse-projects `C_effective_display` and the full
unchanged canonical boundary through the exact accepted canvas-to-camera
matrix. The UI records the integer-rounding residual, camera-space center
residual, predicted radius range, radial RMS error, and maximum boundary amount
outside the fitted physical rim. The center residual is a gate; radius and
containment values are QC and operator-review evidence, never inputs to a
resize.

If commissioning supplies a named plane-specific center-bias model, Citrus
applies it explicitly and records the model identity:

```text
d_candidate_corrected = center_bias_model(C_rim_camera, d_requested)
```

Without that model, V1 records the `dish_top_rim` versus `projected_surface`
plane mismatch as an uncertainty and requires review of the computed
camera-space outline. It must not claim fish-plane accuracy.

### Radius policy

Let `R_observed_camera` be the accepted daily rim radius and `R0_mm` be the
canonical physical radius.

- Orange persists `R_observed_camera` and the raw Hough proposal.
- Citrus may map samples around the observed rim through the base homography
  and report a fitted canvas radius as a diagnostic.
- Citrus compares the diagnostic with the canonical radius and scale model.
- The daily candidate preserves `R0_mm` and the canonical canvas radius.
- A large mismatch blocks or escalates; it never auto-resizes the arena.
- Orange's outward centroid-forgiveness gate remains a separate detection
  policy and does not alter the physical rim or Citrus experimental area.

### Circular rotation policy

Rotation is undefined for a circle and is not estimated in V1. If a future
non-circular arena needs daily pose, it receives a new schema version with an
explicit rigid-pose model and orientation evidence.

## Operator Experience

The normal workflow is launched and completed in Orange. The operator should
not need to navigate Citrus UI panels.

The only expected pauses are physical steps that software cannot perform:

- confirm the correct dish and water state;
- confirm the proposed water-side inner-rim feature;
- approve or reject the computed automatic canonical-outline overlay;
- visually align the Citrus-projected outline and center to the physical dish.

The normal daily path does not ask the operator to remove camera filters or
change illumination. It explicitly records the runtime IR filter state and
retains the mapped pulse/strobe path used for experiments. Citrus does show the
translated experimental-area outline and center to the human operator after
the automatic fit. That visible review is not a camera calibration capture and
does not claim camera-plane validation.

## Guided Daily Workflow

This workflow begins only after the operator chooses **Perform Daily Dish
Registration**. Skipping or cancelling it before activation leaves the session
in `base_only` mode and does not change general experiment readiness.

### Step 0: start and acquire the calibration lease

Orange asks Citrus to begin a registration transaction for an explicit set of
rig, canvas, arena, and camera identities.

Citrus:

- verifies that no experiment is armed or active;
- verifies that no conflicting calibration transaction owns the canvas;
- snapshots the current projection state for restoration;
- acquires a time-limited calibration lease;
- returns the active base calibration identities and current effective
  registration state.

Orange snapshots its current camera, exposure, illumination, streaming, and
capture state.

### Step 1: preflight the stable base

Orange and Citrus verify:

- selected rig/canvas/arena/camera identities;
- camera image dimensions and orientation;
- active base calibration status and checksum;
- Citrus configuration fingerprint;
- tank/dish design identity and canonical dimensions: 80 mm inner/usable
  diameter, 90 mm outer diameter, and 5 mm radial wall thickness;
- current date/session registration policy;
- control-socket and projection-snapshot health.

A failed base compatibility check returns `requires_recommissioning`. The
daily workflow does not offer a button that silently refits or promotes a
homography.

### Step 2: capture the physical rim

Orange requests the Citrus `black_reference` scene. Citrus acknowledges the
actual scene revision and readiness fence. Orange then captures a fresh frame
after that fence.

Orange:

- detects a circular proposal;
- displays the raw proposal over the full-resolution image;
- asks the operator to confirm that it follows the water-side inner edge;
- stores the raw detected circle separately from any operator adjustment;
- stores the accepted physical rim and outward centroid gate separately.

The frame cannot be reused from an earlier purpose without an explicit,
persisted operator override.

### Step 3: create the rim-only candidate

Orange submits each immutable rim observation with
`alignment_basis = commissioned_homography_and_canonical_experimental_center`.
No projected-marker coordinates or marker artifact are required. Citrus
validates the accepted commissioning release and exact active homography,
maps the fitted rim center into the logical canvas, subtracts the canonical
global experimental-area center, and rounds once to the integer canvas
translation used by runtime.

Citrus returns:

- base and proposed effective centers;
- requested and applied `d_candidate` in `final_display_canvas_px`;
- integer-rounding residual;
- the same shift in millimetres when a valid scale model exists;
- plane assumptions and bias-model identity;
- blocking errors and warnings;
- a candidate ID and checksum.

No runtime geometry changes in this step.

### Step 4: compute and persist camera-space review evidence

Orange verifies the candidate file checksum and exact arena/camera/homography
identity. It then applies the candidate's exact integer translation to the
canonical experimental-area center and inverse-projects the full unchanged
boundary through the accepted commissioning matrix. Orange writes a
checksummed overlay and immutable per-target geometry-review JSON containing:

- physical inner-rim evidence;
- corrected center and unchanged canonical outline;
- center residual in camera pixels;
- predicted camera-space radius minimum, mean, and maximum;
- radial RMS error and maximum amount outside the fitted rim;
- the proposed translation in canvas pixels and millimetres;
- homography identity, plane assumptions, and source-frame provenance.

No new camera capture is taken. The visible-marker preview APIs remain
available for an optional independent diagnostic. A future bounded manual
center nudge must create a new candidate revision; it may not edit the existing
candidate in place.

### Step 5: validate

Automated validation checks:

- the inverse-projected effective center against the observed rim center;
- computed outline containment against the accepted physical boundary;
- center residual in camera pixels;
- candidate translation magnitude;
- preserved canonical radius;
- source rim-frame freshness and uniqueness;
- Citrus black-scene fingerprint and readiness;
- base/config identity stability throughout the transaction.

For the current plane model, outline-to-rim comparison is validation evidence,
not a claim that a projected-surface homography is a fish-plane transform.

### Step 6: accept or reject

The operator accepts or rejects from Orange.

On accept, Orange sends an idempotent acceptance operation naming the exact
candidate checksum and expected Citrus state revision. Citrus revalidates all
preconditions and writes an immutable accepted daily-registration artifact.
Acceptance deliberately does not change runtime selection. Orange then sends a
second explicitly armed request selecting that exact path and SHA-256; Citrus
atomically publishes and applies the runtime-selection pointer.

On rejection, Citrus discards the candidate. The workflow's explicit
measurement baseline remains `base_only`; no rejected candidate is selected
and no previously selected daily translation is silently restored.

An acceptance acknowledgement is complete only when it includes the accepted
artifact ID, checksum, active-pointer revision, and effective geometry summary.

### Step 7: restore and close

Citrus restores the prior non-calibration projection scene or a defined safe
black scene. The normal rim-only coordinator has not changed the camera filter
or experiment illumination path, so no physical optical-restoration checkbox
is required. Citrus's accept/reject/abort acknowledgement is not treated as
terminal until the transaction is inactive.

Only after successful restoration does Citrus release the calibration lease
and Orange mark the transaction complete.

If restoration cannot be verified, experiment start remains blocked.

### Step 8: bind future recordings

Orange and Citrus add the accepted daily-registration reference to subsequent
recording/session snapshots only when the operator selects
`selected_daily_registration`. Both references must resolve to the same
artifact ID and checksum. In `base_only`, both applications instead record
`daily_registration_status = "not_performed"` or
`"available_not_selected"` and no daily artifact identity.

## Citrus Calibration Scene Contract

Orange requests named components or recipes. It does not send arbitrary pixels,
centers, radii, shaders, or drawing commands.

### Allowed V1 components

| Component ID | Meaning |
| --- | --- |
| `background.black` | Safe black calibration background |
| `marker.arena_center` | Marker at the selected effective arena center |
| `marker.experimental_area_center` | Marker at the selected effective experimental-area center |
| `outline.arena_bounds` | Citrus-owned effective arena bounds |
| `outline.experimental_area` | Citrus-owned canonical-size experimental-area outline |
| `pattern.circular_homography_rings` | Commissioning/verification pattern, not a daily boundary |
| `marker.orientation` | Citrus-owned orientation marker for commissioning diagnostics |

### Named V1 recipes

The scene socket exposes these concrete recipe
IDs: `black_reference`, `arena_outline`,
`experimental_area_center_and_outline`, `homography_grid`,
`homography_rings`, and `verification_dots`. Daily candidate preview reuses
`experimental_area_center_and_outline` with a candidate-only transient runtime
translation; it does not add a second geometry authoring path.

| Recipe ID | Components | Normal use |
| --- | --- | --- |
| `black_reference` | `background.black` | Physical rim and difference-image reference |
| `base_center_crosshair` | black plus experimental center marker | Observe current projected center |
| `base_center_and_experimental_outline` | black, center marker, experimental outline | Review current effective geometry |
| `candidate_center_and_experimental_outline` | black, candidate center marker, candidate experimental outline | Daily candidate validation |
| `commissioning_homography_rings` | circular rings and orientation marker | Explicit commissioning or base-health workflow only |
| `restore_previous_scene` | transaction's saved prior scene | Transaction cleanup |

Citrus resolves every recipe from the selected base configuration and, when
applicable, the named registration candidate. Orange cannot override the
resolved geometry inside the scene request.

### Scene readiness

A successful scene response includes at least:

```json
{
  "transaction_id": "regtxn_...",
  "scene_id": "scene_...",
  "recipe_id": "base_center_crosshair",
  "scene_revision": 12,
  "content_fingerprint": "sha256:...",
  "visible_from_render_revision": 88421,
  "resolved_geometry": {
    "coordinate_space": "final_display_canvas_px",
    "experimental_area_center_px": {"x": 433.0, "y": 504.0},
    "experimental_area_radius_px": 166.7735
  },
  "base_calibration_ref": {
    "artifact_id": "...",
    "sha256": "..."
  },
  "registration_ref": null
}
```

`content_fingerprint` hashes semantic rendered content. It excludes render
frame counters, wall time, and other values that change while the scene remains
visually identical.

Orange captures only after `visible_from_render_revision` and stores the scene
response beside the image. If Citrus cannot expose a reliable render fence,
Orange waits for an explicit stable-scene acknowledgement plus a configured
settling interval and records that weaker synchronization mode.

## Transaction State Machine

The normative states are:

```text
idle
  -> lease_acquired
  -> preflight_complete
  -> rim_observed
  -> candidate_ready
  -> computed_geometry_review_complete
  -> accepted | rejected
  -> restoring
  -> complete
```

`aborting -> restoring -> aborted` is reachable from every state after
`lease_acquired`.

Rules:

- state transitions use optimistic `expected_state_revision` checks;
- only one transaction may control a rig/canvas projection at a time;
- one transaction may include multiple arena/camera targets;
- every mutating operation is idempotent;
- retries return the prior result for the same `operation_id`;
- a repeated `request_id` is never used for a new semantic request;
- the lease has a heartbeat and a bounded timeout;
- loss of the coordinator triggers safe restoration, not acceptance;
- no experiment can arm while a calibration preview or unverified restore is
  active.

## Local-Control Methods

The implemented Citrus Unix-domain local-control methods are:

Read-only:

- `daily_registration_status`
- `active_projection_snapshot`

Mutating and idempotent:

- `begin_daily_registration`
- `create_daily_registration_candidate`
- `preview_daily_registration_candidate`
- `restore_daily_registration_preview`
- `abort_daily_registration`
- `accept_daily_registration`
- `reject_daily_registration`
- `select_daily_registration_runtime_mode`

These use the existing `citrus.local_control.request` version-1 envelope. Every
mutation includes a unique request ID and stable semantic operation ID. The
transaction mutations also include `transaction_id`; runtime selection is a
separate per-canvas operation:

```json
{
  "schema_id": "citrus.local_control.request",
  "schema_version": 1,
  "request_id": "uuid-per-attempt",
  "operation_id": "uuid-per-semantic-mutation",
  "method": "begin_daily_registration",
  "params": {
    "transaction_id": "regtxn_...",
    "targets": [{"arena_id": "arena_1", "camera_id": "2010093"}]
  }
}
```

Read-only requests may omit `operation_id`. Mutating requests may not.

Every response identifies the request, transaction, resulting state revision,
active experiment state, base identities, and structured errors/warnings.

## Orange Evidence Artifact

Orange writes one immutable transaction bundle under its calibration session:

```text
<session>/guided_registrations/<transaction_id>/
  manifest.json
  transaction.json
  targets/<camera_serial>_<arena_id>/
    rim_only_geometry_review.json
    rim_only_geometry_review_overlay.png
```

`transaction.json` is rewritten atomically as a recovery/audit checkpoint;
`manifest.json` is published only at successful completion. The bundle
references, rather than copies or relabels, the session's existing Orange
image-set and schema-v2 dish-top-rim artifacts. The observation JSON files
carry source-image and overlay checksums. The transaction record includes:

- original frame IDs, timestamps, image sizes, and checksums;
- distinct capture purpose for every frame;
- any explicit duplicate-frame override;
- Citrus scene responses and content fingerprints;
- camera, filter, illumination, dish, and water-state metadata;
- raw detected rim and operator-accepted rim separately;
- exact requested/applied translation and integer-rounding residual;
- accepted homography identity and inverse-projected canonical outline;
- computed center/radius/containment QC;
- Citrus candidate and Orange geometry-review results;
- operator acceptance/rejection and warnings acknowledged;
- Citrus accepted artifact acknowledgement, when accepted;
- restoration outcome.

## Citrus Accepted Daily Registration Artifact

Citrus writes an immutable artifact similar to:

```json
{
  "schema_id": "citrus.daily_arena_placement_registration",
  "schema_version": 1,
  "registration_id": "dailyreg_...",
  "status": "accepted",
  "created_at_utc": "2026-07-18T20:05:00Z",
  "validity": {
    "policy": "local_operating_day",
    "local_date": "2026-07-18",
    "invalidated_at_utc": null,
    "invalidation_reason": null
  },
  "target": {
    "rig_id": "omnifin0",
    "canvas_id": "shadow",
    "arena_id": "arena_1",
    "camera_serial": "2010093"
  },
  "base_calibration_ref": {
    "artifact_id": "basecal_...",
    "sha256": "...",
    "configuration_fingerprint": "sha256:...",
    "position_transform_plane": "projected_surface"
  },
  "apply": {
    "target": "arena_placement_translation",
    "coordinate_space": "final_display_canvas_px",
    "translation_px": {"dx": 1.3, "dy": -11.2},
    "translation_mm": {"dx": 0.31, "dy": -2.69},
    "canonical_geometry_preserved": true
  },
  "canonical_experimental_area": {
    "shape": "circle",
    "radius_mm": 40.0,
    "radius_changed": false
  },
  "evidence": {
    "orange_transaction_artifact_ref": {
      "artifact_id": "...",
      "sha256": "..."
    },
    "rim_observation_ref": {"artifact_id": "...", "sha256": "..."},
    "geometry_review_observation_sha256": "...",
    "geometry_review_overlay_sha256": "..."
  },
  "plane_model": {
    "observed_boundary_plane": "dish_top_rim",
    "position_mapping_homography_plane": "projected_surface",
    "center_bias_model_ref": null,
    "fish_plane_claimed": false
  },
  "quality": {
    "center_residual_camera_px": 0.8,
    "observed_radius_diagnostic_mm": 40.1,
    "warnings": ["plane_specific_center_bias_model_unavailable"]
  },
  "operator_decision": {
    "decision": "accepted",
    "source_application": "orange",
    "accepted_at_utc": "2026-07-18T20:04:58Z"
  }
}
```

Activation updates an active-reference file atomically. The reference contains
the artifact ID, checksum, base identity, and pointer revision. It does not
contain an independently editable copy of the geometry.

## Quality Gates

### Blocking conditions

The daily transaction cannot accept when any of these is true:

- experiment is armed or active;
- rig/canvas/arena/camera identity is ambiguous or changed mid-transaction;
- active base calibration is missing, unaccepted, or incompatible;
- camera dimensions, crop, orientation, or selected configuration differ from
  the base contract;
- the Citrus scene lacks a stable content fingerprint or readiness evidence;
- required Orange frames are stale, missing, corrupt, or accidentally reused;
- the water-side inner rim was not explicitly confirmed;
- computed effective-center residual is outside its quality bound;
- center shift exceeds the hard holder/displacement bound;
- observed radius differs enough to suggest the wrong rim, wrong dish, changed
  scale, or changed imaging geometry;
- candidate geometry would be clipped or leave the valid arena domain;
- base identity or configuration fingerprint changes before acceptance;
- accepted artifact persistence or atomic activation fails;
- prior projection state cannot be restored.

### Warnings requiring acknowledgement

The workflow may permit explicit accept-with-warning for:

- marginal but reviewable rim confidence;
- a center shift above the normal bound but below the hard bound;
- small observed-radius drift within a commissioning-defined tolerance;
- missing plane-specific center-bias model;
- bounded manual center nudge;
- weaker time-based black-scene settling when a render fence is unavailable.

Thresholds belong to the commissioned rig profile and must be recorded in the
candidate. They must not be hidden constants in Orange UI code.

## Multi-Camera And Multi-Arena Coordination

A transaction may include multiple targets, but evidence and decisions remain
identifiable per camera/arena pair.

- Citrus renders one coherent black-scene revision across the requested arenas.
- Orange may issue a grouped capture after that common scene fence.
- Each target gets its own rim observation, candidate, computed geometry
  review, quality result, and accepted registration target.
- A transaction manifest records the set and the common scene revisions.
- Failed targets can be retried without recapturing successful targets when
  their source scene and base revisions remain valid.
- A daily-registration target set becomes a readiness requirement only after
  the operator explicitly selects `selected_daily_registration` for that set.
  `base_only` has no daily target-set requirement.
- Activation of a required multi-arena set is atomic at the registration-set
  pointer level, so an experiment cannot start with an accidental half-old,
  half-new set.
- A deliberately scoped Cam2010093-only workflow is valid only when the
  transaction and readiness policy explicitly select Cam2010093. It does not
  imply that the other cameras had rim observations or geometry reviews.

## Concurrency, Failure, And Recovery

### Orange failure

If Orange disconnects or stops heartbeating, Citrus:

1. refuses acceptance;
2. restores the pre-transaction scene or safe black;
3. clears the transient candidate;
4. marks the transaction aborted after the lease timeout;
5. leaves the previous active registration untouched.

### Citrus failure

If Citrus restarts, Orange marks the transaction interrupted. On reconnect it
queries transaction and active-pointer state before retrying anything. An
accept whose artifact was written but whose acknowledgement was lost returns
the same result when retried with the same `operation_id`.

### Partial artifact failure

An artifact that is written but not activated remains a non-active candidate or
orphan for audit. It is never inferred active from its timestamp or filename.

### Restoration failure

Any unverified software or physical-state restoration is a blocking condition.
The UI names the remaining intervention rather than marking the transaction
complete.

## Daily Validity And Invalidation

The default policy is one accepted registration per local operating day and
base-calibration identity. A rig profile may choose a shorter session scope.

A registration is invalidated by:

- a different base calibration or configuration fingerprint;
- camera resolution, crop, orientation, lens, focus, or mount change;
- projector/display mode or canvas change;
- platform, diffuser, holder, or projected-surface height change;
- dish replacement after registration;
- explicit operator clear;
- expiration under the configured daily/session policy;
- repeated validation residuals above the commissioned bound.

Starting an experiment with no current accepted registration is valid in
`base_only` mode and requires no override. Orange and Citrus record that mode
and `daily_registration_status = "not_performed"`. Reusing yesterday's
translation must never happen silently; selecting any existing translation is
an explicit action and requires current compatibility validation.

## Runtime And Recording Integration

Citrus runtime resolves:

```text
accepted base calibration
  + explicit runtime registration mode
  + selected accepted daily registration (only when that mode requests one)
  -> effective arena placement
  -> effective experimental-area geometry
```

The accepted Orange top-rim artifact is the intended source for Orange's
camera-space physical boundary, and the outward centroid gate remains a
separate Orange detection policy. At recording start Orange now resolves the
exact selected registration, validates its accepted schema-v2 rim observation,
and writes the full physical boundary and outward centroid gate directly to
`recording_snapshot.json.calibrations[serial].dish_top_rim_observation`.
It also materializes the exact selection, registration, candidate,
observation, manifest, image set, and both mask exports into the recording.
This path does not depend on the older
`ORANGE_SPATIAL_CALIBRATION_ARTIFACT_<serial>` hook and does not silently enable
the mask in Orange's live detector.

Each recording snapshot and Citrus H5/session snapshot includes:

- base calibration artifact ID and checksum;
- Citrus configuration fingerprint;
- runtime registration mode (`base_only` or
  `selected_daily_registration`);
- daily registration status (`not_performed`, `available_not_selected`, or
  `selected_valid`);
- daily registration artifact ID and checksum when selected, otherwise null;
- Orange transaction/evidence artifact ID and checksum when selected,
  otherwise null;
- effective arena origin and experimental center;
- canonical experimental radius and physical units;
- Orange physical rim and centroid-gate references;
- plane IDs, bias-model identity, warnings, and override state;
- transaction and operator-decision provenance.

Orange and Citrus should refuse or prominently flag a recording snapshot in
which their runtime registration modes disagree, or in which selected daily
registration IDs or checksums disagree. Matching `base_only` snapshots with no
daily artifact are valid.

## Recommissioning Escalation

The guided daily path returns `requires_recommissioning` when evidence suggests
that the fixed geometry is no longer fixed. Examples include:

- circular-ring or independent holdout verification fails;
- active homography/configuration fingerprint is obsolete;
- center shifts repeatedly exceed the physical holder's daily range;
- observed diameter changes beyond tolerance;
- camera image geometry or projection canvas changes;
- the projected preview has non-translational residuals;
- residual direction/magnitude depends strongly on position;
- mount, lens, projector, platform, diffuser, or holder was disturbed.

Recommissioning is a separate guided recipe with separate authority,
artifacts, quality gates, acceptance, and promotion. It may reuse the same
scene-control transport, but never the daily registration mutation.

For the current Cam2010093 evidence, the known obsolete active homography must
be replaced through the commissioning candidate/review/promotion lifecycle
before this daily workflow can be treated as production-safe. The daily
translation layer is not a workaround for that base inconsistency.

## Implementation Slices

### Implemented dry commissioning handoff (2026-07-19)

The holder-removed commissioning utility can now continue from an accepted
arena-center/size transaction into a rectangular homography capture without
restarting either program:

```text
center and optionally resize every arena
  -> commit the verified Citrus canvas
  -> re-import that committed geometry into Orange
  -> request Citrus homography_grid at the committed arena placements
  -> acquire one PTP-grouped all-camera capture
  -> append it to the same Orange calibration session
  -> fit immutable per-camera candidates in Citrus
  -> require full-fit plus disjoint perimeter-holdout quality gates
  -> reject safely, or atomically promote when separately armed
```

The projected point set is centered on the committed arena placement, but the
homography remains a full camera-native-pixel to final-display-canvas mapping.
Fitting and review cannot move the arena or experimental area. Promotion is a
separate persistent mutation guarded by the committed canvas checksum and a
configuration fingerprint.

Run the combined workflow with:

```bash
scripts/run_gui_arena_centering_commissioning.sh \
  --execute \
  --save-verified-centers \
  --fit-homographies
```

That form saves candidates and review overlays but leaves active homographies
unchanged. Add `--accept-homographies` only when automatic promotion after all
quality gates is intended. Arena resizing also requires
`--save-verified-layout` before homography fitting is permitted.

Orange's Spatial Layout UI also supports delayed review. Under **Homography
Candidate Review And Promotion**, select the saved `candidate_set.json`, ask
Citrus to revalidate it, refresh until the persisted-set revalidation passes,
inspect the per-camera metrics and the detection/reprojection/coordinate-frame
files listed for each camera, then explicitly attest that all evidence was
reviewed. Only that attestation enables **Promote Reviewed Set As Canvas
Authority**. Merely selecting or revalidating a set cannot change the active
homographies; **Release Without Promotion** closes the live review transaction
without changing them.

Promotion makes the active JSON pointer—not the legacy matrix-only YAML—the
authority for that arena/camera on the current canvas. Orange's Citrus canvas
import now requires and verifies that pointer, the current canvas checksum, the
recomputed arena/camera/plane configuration fingerprint, candidate JSON and
YAML checksums, direction and target-plane semantics, and fit/holdout,
orientation, and source-photometry gates. It imports the matrix only when all
checks pass and displays the candidate-set identity, acceptance time, quality,
and pointer path. A legacy YAML without an accepted pointer is reported as
`legacy_unverified` and is not used to seed registration.
After Citrus reports a committed promotion, refreshing the review status makes
Orange re-import that canvas automatically and requires the selected camera's
active pointer to pass the authoritative checks before reporting success.

Candidate artifacts preserve the source Orange session/capture group and image
checksum, exact Citrus projected points, plane/direction, config fingerprint,
correspondences, residuals, holdout measurements, and before/after overlays.
Runtime rejects missing or stale active pointers. The temporary compatibility
escape hatch is `CITRUS_ALLOW_LEGACY_HOMOGRAPHY=1`; it must not be used to
accept new commissioning work.

Implemented foundation as of 2026-07-21:

- Citrus exposes the five named V1 calibration scenes through its Unix-domain
  local-control socket, with unique requests, idempotent mutations, semantic
  content fingerprints, multi-arena display fences, transaction conflict
  checks, prior-scene restoration, and experiment interlocks.
- Orange exposes an intentional expected-camera set and an automatic
  purpose-to-scene recipe mapping, requests one Citrus scene transaction,
  waits for the exact common presentation fence, then uses the existing
  per-camera `SpatialSnapshotWorker` path to acquire one shared capture group.
- Orange samples one shared pre/post Citrus scene identity, records complete,
  partial, failed, or `invalid_scene` group membership, waits for the restore
  fence, and persists the scene, membership, and restoration provenance on
  every completed camera artifact.
- Orange's Spatial Layout UI now adds an opt-in **Guided Daily Dish
  Registration** coordinator. It explicitly returns to commissioned
  `base_only` geometry for measurement, acquires the Citrus daily lease,
  captures and reviews the rims, detects all selected cameras concurrently,
  submits checksummed rim-only observations, verifies the returned candidate
  and homography identities, inverse-projects the unchanged canonical outline,
  persists computed geometry overlays, applies automatic center-residual and
  operator outline-review gates, then separately accepts and selects the exact
  immutable registration. The normal path keeps installed IR filters and the
  mapped experiment pulse illumination in place.
- Generic daily scenes are allowed only for the owning daily transaction and
  for `black_reference`. The older
  `experimental_area_center_and_outline` and candidate-preview scenes remain
  optional optical diagnostics with distinct authority; they are not required
  by the rim-only path. Commissioning recipes remain outside the daily lease.
- Abort and reject are acknowledgement-driven: Orange does not report a safe
  terminal state until Citrus reports the transaction inactive with the exact
  terminal operation and confirms that its preview restoration completed.

### Slice 1: stable Citrus scene control

- unique request IDs and idempotent operation IDs;
- configured default local-control socket;
- named V1 recipes;
- semantic content fingerprints that exclude frame counters;
- render-readiness fence and scene snapshots;
- transaction lease, safe restore, and experiment-state interlock.

### Slice 2: Orange guided rim-only capture — implemented, live validation pending

- one-window wizard and atomically checkpointed state;
- fresh PTP-grouped black/rim captures under the normal runtime IR path;
- no daily physical filter or illumination transition;
- scene-snapshot linkage;
- duplicate frame and contradictory physical-state rejection;
- immutable transaction bundle.

The current checkpoint is an audit/recovery record, not yet automatic
restart-time UI rehydration. A restarted application must abort/restart the
transaction until the persisted resume API is added.

### Slice 3: Citrus candidate and computed Orange review — implemented backend

- accepted base-artifact lookup and compatibility checks;
- rim-center versus canonical-center translation computation;
- one-time integer canvas rounding with an explicit residual;
- derived-canvas projection-authority resolution;
- Orange inverse-projected center/radius/containment/plane diagnostics;
- optional transient candidate scene retained for independent diagnostics;
- bounded candidate revision for manual nudge.

### Step 4: physical visual alignment and derived candidate

The automatic candidate must first pass the existing homography and integer-
translation QC. Citrus then presents that automatic translation as a transient
experimental-area outline and center crosshair. Orange pauses at an explicit
manual-alignment stage.

For every arena, Orange shows three outlines over the accepted rim capture:

- commissioned base geometry;
- automatic rim-registration geometry; and
- the current operator-adjusted geometry.

Orange supplies coarse and fine integer controls (`±5` and `±1` final-display
canvas pixels). Each request carries the complete absolute offset set relative
to the automatic candidate, so retrying a request is idempotent and never
accumulates a delta twice. Citrus updates only the transient preview. It does
not edit the canvas JSON, commissioned homography, scale, arena size,
experimental-area local center, or radius.

The operator confirms every arena independently and explicitly arms the freeze
action. Citrus then writes a new immutable candidate that contains:

- the automatic candidate path and SHA-256;
- automatic applied translation;
- manual X/Y delta;
- final applied translation;
- per-arena operator confirmation; and
- an explicit `operator_visual_alignment` evidence classification.

The original automatic candidate remains unchanged. Orange regenerates the
camera-space overlay for the exact derived candidate, but its residual against
the rim-plane prediction is diagnostic rather than an acceptance gate. Using
the projected-surface homography as that final gate would recreate the known
plane mismatch that the physical review is intended to measure. Acceptance
and runtime selection remain separate explicit actions bound to the derived
candidate checksum.

### Slice 4: acceptance and runtime overlay — implemented backend

- immutable accepted daily-registration artifact;
- atomic per-canvas runtime-selection pointer and explicit `base_only` return;
- arena-placement translation at the runtime geometry boundary;
- recording/H5 provenance;
- daily expiry and invalidation;
- Orange `recording_snapshot.json` and Citrus H5 mode/status/artifact snapshot.
- exact recording-local schema-v2 rim observation and mask-export package for
  each participating selected camera;
- direct camera-native physical-rim and outward centroid-gate geometry in the
  recording snapshot, with an exact Citrus runtime-identity comparison.

### Slice 5: multi-arena production hardening

- grouped scene fences and captures;
- partial retry;
- atomic required-target registration sets;
- end-to-end crash/retry tests;
- GUI and headless recording readiness enforcement.

### Post-stack terminology and UI pass

Do not rename isolated pieces while the complete calibration procedure set is
still evolving. Once the commissioning, scale, installed-fixture, daily
registration, validation, and provenance slices have been exercised as one
end-to-end operator workflow, rename the current operator-facing “arena
centering” workflow to **Canvas/Arena Commissioning** and reserve **Daily Dish
Registration** for the translation-only daily product. Update UI labels, help
text, launcher descriptions, and operator documentation in the same pass.
Existing socket methods and automation variables may remain as compatibility
aliases; their commissioning semantics must not change.

The first four slices deliberately follow repair of the base homography
candidate lifecycle documented in the calibration-stack audit. Scene control
and artifact schemas can be built in parallel with that repair, but a stale
base must not be blessed by a successful daily UI demo.

## Acceptance Criteria

The design is implemented only when all of the following are demonstrated:

1. An operator completes a normal daily registration from Orange without
   navigating Citrus UI.
2. Citrus renders only named, Citrus-resolved calibration scenes.
3. Every Orange capture identifies a fresh camera frame and the exact Citrus
   scene revision/content fingerprint.
4. The daily accepted artifact changes only the effective two-dimensional
   placement translation.
5. Canonical canvas, arena-local geometry, 40 mm radius, scale, and base
   homography files remain byte-for-byte unchanged.
6. Review, reject, abort, timeout, and application crash leave the previous
   active registration unchanged.
7. Accept is idempotent across lost acknowledgements.
8. An experiment cannot arm during preview or failed restoration.
9. A base/config identity change invalidates the active daily registration.
10. Orange and Citrus recording snapshots contain matching base and daily
    artifact IDs/checksums.
11. Radius mismatch and non-translational residuals escalate rather than
    resizing or refitting the daily model.
12. A Cam2010093-only run never claims completion for cameras that did not
    capture and confirm a rim observation.
13. The automatic candidate remains immutable and independently addressable
    after manual alignment.
14. Every final target records automatic, manual, and final integer
    translations, and the final runtime selection references the exact derived
    candidate.
15. Live review shows base, automatic, and current manual outlines while the
    physical Citrus projection updates after each acknowledged nudge.

## Proposed Decisions To Confirm Before Coding

The contract recommends these concrete choices:

1. Use one `arena_placement_translation` per circular arena, with the
   arena-local experimental center unchanged.
2. Keep the normal operator workflow entirely in Orange; Citrus supplies the
   authoritative control and persistence service.
3. Allow Orange to request only named Citrus-owned calibration components and
   recipes, not arbitrary geometry.
4. Treat daily radius measurements as health evidence, never an automatic size
   update.
5. Default accepted registrations to the local operating day and require an
   explicit override to reuse one outside its validity policy.
6. Require visual review of the computed camera-space canonical outline until
   a plane-specific center-bias model has been commissioned; keep projected
   marker capture as an optional independent diagnostic.
7. Repair and promote a valid base homography before enabling production daily
   registration.

These choices minimize operator back-and-forth while retaining a strict
authority boundary and a recoverable calibration history.
