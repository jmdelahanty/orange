# Orange/Citrus Calibration Stack Lifecycle Audit

Date: 2026-07-18

Status: read-only architecture and artifact inspection

Orange baseline: commit `5ddcc39` on branch `exp/gop-split-a16`

## Post-audit commissioning observation: 2026-07-19

A later unarmed four-camera Shadow commissioning run tested independent
camera-visible arena-square sizing. It proposed even sides of `348`, `350`,
`352`, and `354` canvas px for Cam2010093 through Cam2010096. Using the
currently persisted per-arena projected-surface scales, those enclosing arena
regions correspond to approximately `83.466`, `83.843`, `83.091`, and
`84.260 mm`.

These enclosing regions are camera-support geometry, not the physical
experimental areas. Citrus kept every experimental-area radius at `40 mm`,
with camera-specific pixel radii of approximately `166.7735`, `166.9795`,
`169.4524`, and `168.0508 px`. Each pixel radius equals its arena's current
projected-surface px/mm scale times `40 mm`. Citrus kept those radii unchanged
and rebased only each local center so its offset from the resized arena center
would remain invariant. The staged resize was rolled back and the canvas file
checksum was unchanged.

Measured opposite-edge scale differences were small: no more than `0.16%`
between top and bottom and `0.45%` between left and right. The different
maximal square sizes therefore do not by themselves demonstrate materially
unlevel cameras. Small height, focus/magnification, projector-scale, crop,
distortion, and safety-margin differences remain plausible.

The implementation now protects the semantic separation:

```text
camera-visible commissioning region may differ per camera
!=
physical experimental-area radius, which remains 40 mm per arena
```

This is a structural guarantee, not a new independent scale calibration.
Physical equality across all four projected `40 mm` radii still depends on
the accuracy of each arena's pixels/mm scale and should ultimately be backed
by immutable known-length evidence. See
`docs/orange_citrus_arena_centering_commissioning_contract.md` for the active
save and rollback contract.

## Executive Conclusion

The current calibration mathematics are mostly sound, but the live calibration
lifecycle is not yet reliable enough to make the mathematically good result the
artifact used at runtime.

For Cam2010093, the current circular-ring capture supports an excellent
camera-to-canvas homography. The fitted ring stopping short of the experimental
area boundary does not explain the smaller Citrus outline. A fit using only the
inner rings predicts both the withheld outer ring and the independently
observed 40 mm boundary accurately.

The current size discrepancy instead comes from the active Citrus homography
sidecar being inconsistent with the present camera image and Citrus geometry.
The fresh circular fit predicts the observed boundary radius to within about
`0.1 camera px`; the active transform predicts it about `56.5 camera px`, or
`2.7%`, too small.

The main architectural gap is therefore not a missing mathematical primitive.
It is the absence of a complete lifecycle that connects:

```text
capture
  -> immutable projection/configuration evidence
  -> pattern-aware detection and correspondence
  -> quality-gated candidate
  -> independent validation
  -> operator acceptance
  -> atomic promotion
  -> runtime identity and compatibility checks
  -> recording provenance
```

The repositories already contain many of these pieces, but they are split
between active legacy paths, richer diagnostic paths, and Orange artifact
writers that do not yet enforce all of their own contracts.

## Scope

This audit covered:

- Citrus calibration-pattern configuration and rendering;
- rectangular and circular point detection/correspondence;
- homography fitting, persistence, loading, and session logging;
- Orange full-resolution calibration capture and metadata persistence;
- Citrus active-projection snapshot collection;
- Orange import and inverse-projected overlays;
- dish top-rim observation and detection-gate semantics;
- plane and physical-scale conventions;
- relevant documentation and test coverage;
- the current Cam2010093 artifacts captured on 2026-07-18.

The inspection was read-only. It did not modify the Orange repository, Citrus
repository, active Citrus configuration, homography sidecar, or calibration
artifacts. The audit was documented after the Orange checkpoint commit listed
above.

## Primary Evidence

### Current circular-ring capture

```text
/home/jeremy/orange_data/calibrations/sessions/
  calsess_2026_07_18T19_32_12Z_shadow/
  artifacts/Cam2010093_arena_1/captures/
  homography_grid_2026_07_18T19_49_23Z.png
```

The capture contains the configured Citrus circular-ring pattern for
Cam2010093.

### Current Citrus configuration

```text
/home/jeremy/citrus/targets/rigs/omnifin0/shadow/shadow.json
```

Relevant `arena_1` values at inspection time:

| Field | Value |
| --- | ---: |
| Pattern mode | `circular_rings` |
| Pattern mask | `experimental_area` |
| Ring count | `4` |
| Inner ring dot count | `8` |
| Outer ring dot count | `32` |
| Inner ring radius | `40 canvas px` |
| Outer ring dot-center radius | `140 canvas px` |
| Arena center | `(261, 332) canvas px` |
| Arena size | `346 x 346 canvas px` |
| Experimental center, arena-relative | `(172, 172) canvas px` |
| Experimental radius | `166.7735138 canvas px` |
| Experimental radius | `40 mm` |
| Projector scale | `4.1693377 canvas px/mm` |
| Legacy camera scale | `53.2253342 camera px/mm` |

The outer calibration ring is therefore intentionally smaller than the
experimental area:

```text
140 / 166.7735 = 0.83946
```

It occupies about `83.95%` of the experimental-area radius. The purple
fit-ring outline should consequently appear smaller than the separately
projected experimental-area outline. The fit ring is calibration support; it
is not the configured arena boundary.

### Active Citrus sidecar

```text
/home/jeremy/citrus/targets/rigs/omnifin0/shadow/
  calibration_artifacts/homography_arena_1_2010093.yml
```

The sidecar was written on 2026-05-27 and contains:

```text
[ 0.0813233532, -0.0002894653,  78.3456062772 ]
[ 0.0000054979,  0.0820815608, 138.7450882516 ]
[-0.0000006295,  0.0000007664,   1.0000000000 ]
```

It contains a timestamp and matrix but no source capture identity,
configuration fingerprint, target-plane contract, valid domain, acceptance
status, or fit-quality fields.

## Numerical Validation

The fresh capture was processed using Citrus's existing circular-ring
detector, circular correspondence matcher, and shared homography fitter. The
analysis executable was built and run from `/tmp`; no repository or active
artifact was changed.

### Full circular fit

| Metric | Result |
| --- | ---: |
| Expected points | `81` |
| Detected/matched points | `81` |
| RANSAC inliers | `81` |
| RMS reprojection error | `0.0832577 canvas px` |
| Maximum reprojection error | `0.191142 canvas px` |

Fresh candidate, camera-native pixels to final-display canvas pixels:

```text
[ 0.0791011151, -0.0003665101,  79.4282121622 ]
[ 0.0003040341,  0.0796936713, 151.3772177870 ]
[-0.0000003342,  0.0000002206,   1.0000000000 ]
```

This matrix was not persisted or promoted.

### Held-out ring check

The fit was repeated using only the center and inner three rings: `49` points
with a maximum fitted ring radius of approximately `106.67 canvas px`. The
outer `140 canvas px` ring was withheld.

| Metric | Result |
| --- | ---: |
| Inner-only fit RMS | `0.0715777 canvas px` |
| Inner-only fit maximum | `0.138490 canvas px` |
| Withheld outer-ring RMS | `0.111465 canvas px` |
| Withheld outer-ring maximum | `0.234458 canvas px` |

The withheld-ring result shows that the transform generalizes cleanly beyond
the smaller fitted domain.

### Experimental-boundary check

The independently observed crosshair/experimental-area circle was
approximately:

```text
center = (2278.01, 2258.90) camera px
radius = 2099.63 camera px
```

The configured experimental-area center in global canvas coordinates is near
`(260, 331)`, with radius `166.7735138 canvas px`.

| Transform | Predicted camera radius for configured 40 mm boundary |
| --- | ---: |
| Fresh full circular fit | `2099.7333 px` |
| Inner-three-ring-only fit | `2099.9744 px` |
| Active May sidecar | `2043.1482 px` |
| Independently observed boundary | about `2099.63 px` |

Forward-projecting the independently observed circle gives:

| Transform | Canvas center | Canvas radius |
| --- | --- | ---: |
| Fresh circular candidate | `(258.979, 332.100)` | `166.7660 px` |
| Active May sidecar | `(263.097, 323.800)` | `171.4098 px` |

The small fresh-candidate center displacement is consistent with the
crosshair/circle being placed and measured by eye. Its radius matches the
configured experimental area. The active sidecar does not.

### Interpretation

The evidence rules out the following as the current size-error cause:

- the `140 px` calibration ring not reaching the `166.77 px` arena boundary;
- the approximately `0.2%` macro-lens scale change measured between nearby
  physical heights;
- a large unmodeled edge distortion within the currently exercised domain.

This does not prove that lens distortion or plane effects are always
negligible. It shows that they are not needed to explain this particular
Cam2010093 discrepancy. The active transform is already sufficient to explain
it.

## Architecture Findings

### 1. Critical: the active fitter does not dispatch on the rendered pattern

Cam2010093 renders circular rings, but the active Citrus
`calculate_and_save_homography(...)` implementation always calls:

```text
DetectRectangularGridPoints(...)
GenerateIdealRectangularGridPoints(...)
```

Relevant code:

```text
/home/jeremy/citrus/src/calibration/homography.cpp
/home/jeremy/citrus/src/ui/arena_view_ui.cpp
```

The UI passes rows, columns, and dot radius to this path regardless of
`calibration_pattern_mode`. It does not route circular-ring captures to the
circular detector and matcher.

The robust circular implementation already exists in:

```text
/home/jeremy/citrus/src/calibration/
  moved_diffuser_projector_coordinate_diagnostic.cpp
```

That path is explicitly labeled `diagnostic_only` with
`mutation_allowed = false`. It cannot promote the good candidate into the
active runtime slot.

Impact:

- the renderer and active fitter can disagree about the pattern contract;
- a valid circular capture cannot safely replace an obsolete active
  homography through the normal UI;
- operators can see a successful diagnostic without having a supported
  promotion path.

### 2. Critical: there is no candidate, review, and atomic promotion lifecycle

The active Citrus path writes directly to:

```text
homography_<arena>_<camera>.yml
```

Runtime loads `homography_matrix` directly from that sidecar. It does not
require or validate:

- a candidate artifact ID;
- source capture checksum and frame identifiers;
- pattern snapshot or expected points;
- config fingerprint;
- transform direction;
- target plane;
- valid spatial domain;
- residual thresholds or inlier ratio;
- independent validation results;
- operator acceptance;
- promotion generation or rollback pointer.

The shared fitter reports residuals, but any nonempty `cv::findHomography`
result is marked successful. There is no rejection threshold in the active
save path.

Persistence is also direct-to-final-path rather than temporary-write, flush,
and atomic rename. A failed or interrupted write can damage the only active
artifact.

Impact:

- a weak or incompatible fit can become active immediately;
- an older matrix can remain active after configuration changes;
- runtime cannot distinguish an accepted artifact from an arbitrary sidecar;
- rollback and audit reconstruction are unnecessarily difficult.

### 3. Critical: transform dependencies are mutable but not fingerprinted

Homography destination points depend on Citrus configuration fields including:

- arena center and size;
- camera/arena association;
- pattern mode and point layout;
- dot radius and marker configuration;
- experimental-area center;
- canvas identity and dimensions.

Those dependencies are not stored or hashed into the active sidecar. Changing
them can silently make a previously fitted matrix incompatible with the
current configuration.

The current sidecar predates the current `shadow.json` modification and is
numerically inconsistent with the new ring capture. The exact change that
invalidated it cannot be reconstructed from the sidecar.

Impact:

- the runtime has no stale-artifact detector;
- Orange overlays can faithfully display the inverse of the wrong matrix;
- visual disagreement is discovered by an operator instead of a compatibility
  gate.

### 4. High: active-projection provenance is presently unreliable

Citrus builds `projection_epoch_id` by hashing the active projection snapshot
after removing only `generated_at_utc`. The hashed payload still includes
`latest_stimulus_frame`. Identical static projected content can therefore get
a different epoch as the stimulus frame advances.

Orange constructs projection-snapshot request IDs from phase plus a fixed
operation name. Repeated single-camera or full-resolution captures reuse the
same request ID. Citrus caches responses by request ID and returns the cached
response for repeats.

Orange additionally has no default Citrus local-control socket path; it only
uses an environment override. The inspected image set consequently reports:

```text
status = unavailable
policy = warn_only_v0
```

Impact:

- a valid static pattern can appear to change during a capture;
- a later capture can receive an old cached projection snapshot;
- captures can be saved without authoritative expected-point evidence;
- current provenance cannot prove which Citrus pattern produced an image.

### 5. High: Orange permits captured-frame relabeling and contradictory state

The inspected aggregate image set is:

```text
/home/jeremy/orange_data/calibrations/sessions/
  calsess_2026_07_18T19_32_12Z_shadow/
  artifacts/Cam2010093_arena_1/image_set.json
```

Its first `crosshair_alignment` image and first `homography_grid` image have:

- the same camera frame ID: `7527`;
- the same local frame ID: `335202`;
- the same image checksum;
- different purposes and save timestamps.

The acquisition worker did obtain a fresh next frame when asked. The issue is
that the UI allowed the already captured buffer to be saved again after its
purpose metadata changed.

The same entries report:

```text
wet_or_dry = wet
dish_installed = true
open_water_surface_present = true
fill_state = recording_fill_level
```

while their operator notes say that the capture is dry and contains no water
path.

The generic aggregate writer constructs and writes JSON directly. It does not
call the semantic validator used by
`write_calibration_image_set_json_file(...)`. Session review warns about some
missing fields and parity links, but not:

- projection snapshot unavailable or changed;
- duplicate checksum or camera frame under a different purpose;
- contradictory wet/dry, dish, fill, and operator-note state;
- impossible stage/purpose combinations already covered by the standalone
  validator.

Impact:

- a physically valid image can be interpreted using the wrong acquisition
  state;
- the latest matching capture selector can silently choose a relabeled stale
  frame;
- downstream fitting has insufficient evidence to distinguish operator error
  from actual plane behavior.

### 6. High: Orange imports a matrix rather than a calibration artifact

Orange's Citrus importer reads only `homography_matrix` from the sidecar. The
in-memory `CitrusSpatialTemplateState` stores the matrix and inverse, but not:

- the sidecar path and checksum as the matrix identity;
- calibration timestamp;
- source capture;
- fit method, residuals, or inliers;
- target plane;
- valid domain;
- configuration fingerprint;
- acceptance/promotion state.

The importer also collects a homography-load error but does not surface it when
building the template state.

Impact:

- a stale active sidecar appears simply as `available`;
- Orange cannot warn that the matrix is incompatible with the imported Citrus
  configuration;
- saved Orange evidence cannot identify exactly which transform produced an
  overlay.

### 7. High: projected-surface circular fitting is trapped in a mismatched diagnostic

The moved-diffuser diagnostic selects captures by purpose and target plane, but
its projected-surface fit always uses the rectangular pattern implementation.
Its circular implementation is attached to the moved-diffuser/tank-bottom
diagnostic fit.

The current circular capture is correctly labeled `homography_grid` on
`projected_surface`, so the diagnostic projected-surface path will not use the
circular fitter that successfully fits it.

The diagnostic also obtains the Citrus config path from a linked accepted
top-rim observation rather than directly from the aggregate image set's
`rig_context.citrus_config_ref`. The current dish-free projected-surface
session has no linked top-rim observation and therefore cannot supply that
configuration through the diagnostic's current lookup path.

Impact:

- pattern type and physical diagnostic purpose are coupled unnecessarily;
- a projected-surface homography fit can depend on an unrelated top-rim link;
- the current high-quality capture is not consumable by the intended semantic
  role without new routing.

### 8. Medium: top-rim schema-v2 intent is correct, but raw and accepted evidence blur

The schema-v2 design correctly separates:

```text
accepted_inner_rim_boundary
```

from:

```text
valid_detection_region
```

The former is the operator-confirmed physical water-side inner rim. The latter
can be offset outward for bounding-box-centroid forgiveness without redefining
the physical boundary.

Two gaps remain:

1. `observed_boundary.geometry` is currently populated from
   `accepted_circle`, not the raw `detected_circle`. If the operator adjusts the
   proposal, the nominal observed boundary no longer preserves the raw fit.
   The raw values survive only under `circle_detection.detected_circle`.
2. Hough proposals are selected using geometric heuristics such as large
   radius and proximity to the image center. They are not ranked using an
   explicit water-side-inner-edge feature model, known physical diameter
   residual, or edge-family classification.

Operator confirmation therefore remains load-bearing.

The latest saved Cam2010093 top-rim observation still contains the previous
`12 px` inward erosion. The source now supports outward centroid forgiveness,
but a new artifact using that policy has not yet been saved.

### 9. Medium: position transforms, scale models, and optical planes remain incomplete

The documentation correctly distinguishes:

```text
plane_position_transform
plane_scale_model
```

The active runtime, however, still has only the projected-surface homography
and a projected-surface scale. It does not yet have an accepted transform for:

- the dish top-rim plane;
- the dish-base inner surface as a physical-world map;
- the fish observation/behavior plane;
- a shared physical-world millimetre frame.

There is also no active per-camera intrinsic/lens-distortion model in the
Orange/Citrus calibration path. The held-out ring result shows that this is not
the cause of the current Cam2010093 size error, but it remains a generalization
and uncertainty-modeling gap.

The current projected-surface scale is numerically supported by the operator's
ruler measurement, but its persisted `scale_models` entry is a legacy import
with placeholder clicked points. The calibration value is plausible; its
evidence and acceptance history are weak.

## Existing Strengths

The refinement should preserve and connect the following working pieces.

### Coordinate conventions are substantially improved

Orange correctly converts Citrus arena-relative points to global canvas
coordinates using the arena origin before applying the inverse homography.
The experimental-area overlay samples the full projected circle rather than
transforming only one center and scalar radius.

### Circular pattern fitting is strong

Citrus already has:

- deterministic circular expected-point generation;
- center-dot and orientation-marker handling;
- ring correspondence ordering;
- blob shape diagnostics;
- RANSAC homography fitting;
- reprojection error and inlier reporting;
- focused unit tests for generation and correspondence.

The new capture demonstrates that these pieces work well on real data.

### Orange captures useful frame provenance

Full-resolution captures can carry:

- local frame identifiers;
- camera frame identifiers;
- temporal averaging range;
- capture group identity;
- source-array role;
- image checksum;
- camera configuration and acquisition metadata.

The acquisition path provides a fresh next frame when explicitly requested.
The remaining gap is enforcing the relationship between a captured buffer and
the purpose under which it is later saved.

### Citrus recording logs preserve after-the-fact provenance

Citrus session logging records the numeric runtime matrix and can record the
active sidecar path, modification time, size, and checksum. Newer sidecars can
also carry fit-quality fields.

This is useful for reconstructing what a recording used. It is not yet a
pre-use compatibility or acceptance gate.

### Physical boundary and detection policy are now distinct

The schema-v2 inner-rim model preserves the key rule:

```text
physical water-side boundary != derived centroid acceptance policy
```

This allows a small outward centroid gate without silently redefining the
physical dish or the Citrus experimental area.

## Target Artifact Model

The next design should introduce an immutable plane-transform candidate with a
stable identity. A minimum conceptual shape is:

```json
{
  "schema_id": "citrus.calibration.plane_position_transform_candidate",
  "schema_version": 1,
  "artifact_id": "...",
  "created_utc": "...",
  "status": "candidate",
  "source_frame": {
    "camera_serial": "2010093",
    "camera_frame_id": 41598,
    "checksum": "fnv1a64:...",
    "path": "..."
  },
  "transform": {
    "direction": "camera_native_px_to_final_display_canvas_px",
    "target_plane": "projected_surface",
    "matrix": []
  },
  "projection_snapshot": {
    "projection_epoch_id": "...",
    "pattern_mode": "circular_rings",
    "expected_points": [],
    "content_fingerprint": "..."
  },
  "configuration_dependency": {
    "rig_id": "omnifin0",
    "canvas_id": "shadow",
    "arena_id": "arena_1",
    "camera_id": "2010093",
    "config_fingerprint": "..."
  },
  "fit": {
    "detector": "...",
    "point_count": 81,
    "inlier_count": 81,
    "rms_reprojection_error_px": 0.0833,
    "max_reprojection_error_px": 0.1911,
    "correspondences": []
  },
  "valid_domain": {
    "coordinate_space": "final_display_canvas_px",
    "geometry": {}
  },
  "scale_model_ref": {},
  "validation": {},
  "operator_review": {}
}
```

The active runtime slot should reference the accepted artifact identity rather
than being the artifact itself:

```text
active pointer
  -> immutable accepted artifact
  -> source capture and config fingerprints
```

Promotion should write the candidate first, validate it, record acceptance,
then atomically replace the active pointer. The previous accepted artifact
should remain available for rollback.

## Recommended Refinement Order

### Phase 1: make projection capture authoritative

1. Give every projection snapshot query a unique request ID.
2. Define a separate stable content fingerprint that excludes generated time
   and frame counters.
3. Configure a production default Citrus local-control socket.
4. Require the snapshot to contain the expected rig, canvas, arena, camera, and
   pattern.
5. For homography candidates, reject unavailable or changed content epochs
   unless the operator explicitly records an override.
6. Persist the expected-point list in the Orange capture artifact.

Acceptance criteria:

- repeated snapshots of an unchanged static pattern have the same content
  fingerprint;
- each query bypasses stale request-ID cache entries;
- a saved homography capture identifies the exact expected points visible in
  the image.

### Phase 2: create pattern-aware immutable candidates

1. Dispatch on the captured pattern snapshot, not a mutable current UI field.
2. Route circular projected-surface captures through the existing circular
   detector and matcher.
3. Preserve raw blobs, ordered correspondences, inlier mask, and residuals.
4. Persist the configuration dependency fingerprint and valid domain.
5. Do not write the active sidecar during candidate calculation.

Acceptance criteria:

- the current Cam2010093 image produces an immutable 81-point circular
  candidate;
- rectangular-grid behavior remains supported for rectangular patterns;
- changing a dependency creates a different candidate/config fingerprint.

### Phase 3: add validation and quality gates

Candidate validation should include:

- minimum point and inlier counts;
- RMS and maximum reprojection thresholds;
- residual distribution by ring/radius;
- outer-ring holdout or equivalent spatial holdout;
- determinant/invertibility and conditioning checks;
- configured-domain coverage;
- an independent crosshair or validation-pattern capture;
- warnings for extrapolation beyond the observed point domain;
- explicit target-plane and scale-model compatibility.

The top-rim circle must not be used as a same-plane homography truth unless its
plane relationship is modeled. It is useful physical boundary evidence, not an
automatic projected-surface validation target.

Acceptance criteria:

- the current fresh candidate passes the ring holdout and boundary check;
- the current May sidecar fails comparison against the new capture/config;
- a deliberately corrupted correspondence set cannot be promoted.

### Phase 4: add operator acceptance and atomic promotion

1. Present candidate versus current-active overlays and metrics.
2. Record operator identity/status, notes, and acceptance time.
3. Promote an immutable artifact using an atomic active-pointer update.
4. Retain the previous accepted generation for rollback.
5. Reload the promoted transform in the UI and verify the loaded identity.
6. Make runtime reject a transform whose dependency fingerprint does not match
   the current rig/canvas/arena/camera configuration.

Acceptance criteria:

- calculation alone cannot mutate runtime behavior;
- interrupted promotion leaves the previous accepted transform intact;
- runtime and Orange overlays display the same accepted artifact ID/checksum;
- rollback restores the previous artifact without recomputation.

### Phase 5: harden Orange artifact integrity

1. Route aggregate writes through the same semantic validation used by the
   standalone writer.
2. Detect duplicate camera frame IDs and checksums within an image set.
3. Require explicit confirmation before reusing one frame for another purpose.
4. Add wet/dry, dish, fill, open-water, and capture-stage consistency checks.
5. Warn or block when projection provenance is unavailable or changed.
6. Verify checksums when loading session-review images.
7. Use transactional JSON writes and coordinated manifest/index updates.

Acceptance criteria:

- a crosshair frame cannot silently become a homography-grid frame;
- contradictory dry notes and wet/dish/water fields are surfaced before save;
- aggregate and standalone writers enforce the same invariants.

### Phase 6: preserve plane separation

Keep these as distinct products:

1. projected-surface commissioning homography;
2. projected-surface physical scale model;
3. daily dish/top-rim center registration;
4. operator-confirmed physical inner-rim boundary;
5. outward bounding-box-centroid gate;
6. dish-base/fish-observation plane transform or explicit uncertainty model;
7. stimulus-safe region accepted by Citrus.

A small same-plane dish displacement should normally update daily placement,
not recompute the commissioning homography or silently change canonical dish
diameter. A meaningful radius discrepancy should trigger review or
re-commissioning.

The coordinated operator workflow and cross-program ownership for that daily
placement product are specified in
`docs/orange_citrus_guided_daily_registration_contract.md`.

## Test Gaps To Close

Existing tests cover circular point generation, circular correspondence,
artifact serialization, and spatial runtime schema behavior. The following
end-to-end invariants are not currently covered:

- renderer pattern snapshot to matching fitter dispatch;
- immutable candidate creation from an Orange capture;
- stable projection content epoch across rendered frames;
- unique Orange projection-snapshot request IDs;
- config-fingerprint mismatch rejection;
- fit-quality rejection and spatial holdout validation;
- atomic promotion and rollback;
- runtime accepted-artifact identity verification;
- duplicate-frame relabel prevention;
- aggregate writer semantic validation;
- raw versus accepted top-rim boundary preservation;
- projected-surface versus top-rim/fish-plane misuse rejection.

## Current Operational Cautions

Until the lifecycle is refined:

1. Do not treat the active Cam2010093 sidecar as valid for the current setup.
2. Do not overwrite it merely because a diagnostic fit succeeds.
3. Preserve the new ring and crosshair captures as evidence.
4. Compare the blue experimental-area outline to the physical projection; do
   not expect the purple `140 px` fit ring to reach the `166.77 px` boundary.
5. Treat unavailable Citrus projection snapshots as missing provenance, not as
   proof of a stable pattern.
6. Do not interpret top-rim Hough radius as projected-surface scale validation.
7. Save a new top-rim observation after the outward centroid-gate policy is
   ready for operational use.
8. Require explicit plane labels whenever a transform or scale is compared.

## Documentation Follow-Up

The existing dish top-rim design document contains older rounded diagnostic
values such as `166 canvas px` and `39.8145 mm`. The inspected current config is
`166.7735138 canvas px` and `40 mm`. Those passages should be updated when the
next implementation slice begins so that the design document and current
configuration do not disagree.

## Final Assessment

The stack is not failing because homography is intrinsically inadequate or
because the circular rings are too small. The current real image demonstrates
that the existing circular mathematical path can recover the configured
40 mm boundary accurately.

The stack is failing at calibration state management: the good fitter is not
connected to the active projected-surface workflow, the active artifact does
not carry its dependencies, provenance snapshots are unreliable, and neither
promotion nor runtime loading enforces calibration identity.

The highest-value next slice is therefore an immutable circular
projected-surface candidate plus independent validation and operator-reviewed
atomic promotion. Plane-specific fish mapping and lens/intrinsic refinement
remain important follow-on work, but they should not be used to explain or
mask the currently demonstrated stale-active-homography problem.
