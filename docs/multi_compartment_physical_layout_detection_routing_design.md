# Multi-Compartment Physical-Layout Detection Routing Design

Date: 2026-08-30

Status: design for operator and consumer review; no schema, runtime behavior,
or recording default is frozen by this document

## Purpose

Define how Orange can assign multiple detections from one physical camera to
fixed, barrier-separated compartments while preserving the original detection
evidence and remaining useful when Citrus is not running.

The motivating setup is one camera observing one dish with four physical
compartments and one fish per compartment. The design deliberately supports an
arbitrary finite set of fixed regions rather than hard-coding four quadrants.

## Normative Hierarchy

The whole dish is a **multi-compartment physical layout**. It is not a Citrus
Arena and it does not contain nested Citrus Arenas.

For the current profile, the hierarchy is:

```text
one physical camera
└── one canonical source acquisition stream
    └── one registered multi-compartment physical layout
        ├── Orange region 1 <-> Citrus Arena 1 <-> declared fish 1
        ├── Orange region 2 <-> Citrus Arena 2 <-> declared fish 2
        ├── Orange region 3 <-> Citrus Arena 3 <-> declared fish 3
        └── Orange region 4 <-> Citrus Arena 4 <-> declared fish 4
```

The arrows are explicit recording/runtime bindings, not identity equivalence.
The current Citrus-backed profile binds exactly one Orange region to exactly
one Citrus Arena for the run. A region does not contain multiple Arenas, and an
Arena does not contain other Arenas.

A Citrus canvas/session may contain the four Arenas as siblings. Their shared
parent is the Citrus canvas/session, not an outer dish Arena. Their shared
acquisition evidence is the one Orange camera stream and registered physical
layout.

When Citrus is absent, the camera, source stream, physical layout, and Orange
regions still exist. There are no Citrus Arena bindings. Scientific fish
identity remains separately declared in either case.

This document does not authorize crop-only recording, scientific subject-ID
assignment, a generic multi-object tracker, or multiple Citrus readers on one
Shaman queue.

Related contracts:

- [YOLO event log](yolo_event_log_jsonl_contract.md)
- [YOLO spatial-mask runtime](yolo_spatial_mask_runtime_design.md)
- [Shaman-v2 live state](shaman_v2_live_state_contract.md)
- [Standalone daily physical registration](orange_standalone_daily_physical_registration_plan.md)
- [Recording geometry](recording_geometry_contract.md)
- [Crop-only recording and context reconstruction](crop_only_recording_and_context_reconstruction_design.md)
- [Recording observation context census](recording_observation_context_acquisition_census_2026_08_13.md)

## Executive Decision

Implement a staged hybrid:

1. Orange retains and records the complete detector result in camera-native
   coordinates.
2. A new deterministic region-assignment layer annotates detections with a
   stable physical-compartment `region_id`.
3. A separate, optional per-region continuity layer may choose one current
   observation for each region when the declared experiment guarantees one fish
   per compartment.
4. Orange records the layout, assignments, alternatives, and continuity
   decisions as derived evidence.
5. `region_id`, tracker-local identity, Citrus Arena identity, and scientific
   `fish_id` remain distinct.
6. Full-frame recording remains the required validation master in the first
   implementation. Per-region crop streams and crop-only retention are later
   stages.

For the barrier-separated, one-fish-per-compartment case, a generic
SORT/ByteTrack-style tracker is not required initially. Fixed physical
membership is a stronger and more reproducible identity signal than appearance
association. A generic tracker becomes relevant only if multiple animals may
share one compartment, barriers are absent, or region membership can change.

## Current-State Boundary

Orange already provides the essential source evidence:

- a canonical acquisition stream per physical camera;
- detector results in top-left-origin, `+X` right, `+Y` down camera pixels;
- complete persisted YOLO event rows, including frame identity and original
  camera/host timestamps;
- an optional accepted outer-dish boundary, outward centroid gate, and fused
  network-input mask; and
- Shaman-v2 complete latest-state snapshots for one live consumer.

Orange does not currently provide:

- meaningful production multi-object `track_id` values;
- multiple internal region geometries;
- stable compartment assignments;
- four independent live crop/pose streams from one camera; or
- safe fan-out from one destructive Shaman queue to multiple Citrus Arenas.

The current single-object velocity and crop/pose paths must not be relabeled as
multi-fish tracking. The current outer-dish mask must not be reinterpreted as an
internal compartment layout.

## Terminology And Identity

| Term | Meaning | Explicit non-meaning |
| --- | --- | --- |
| source acquisition stream | One physical camera's canonical frame sequence within a recording | A crop, encoder shard, or compartment |
| detection | One model output associated with one source frame | A persistent fish identity |
| `source_detection_index` | Position in the original decoded detection vector for that frame | A stable cross-frame ID |
| multi-compartment physical layout | One physical dish/holder design and its registered camera observation | A Citrus Arena or a source stream |
| region | One fixed physical compartment in a selected physical layout | A container for Citrus Arenas or a fish identity |
| `region_id` | Stable identifier within one versioned layout | `fish_id` or `track_id` |
| region observation | A detection assigned to a region on one frame | A guaranteed biological observation |
| region continuity state | Optional short-lived estimate for one region | A generic multi-object track |
| Citrus Arena | One Citrus stimulus/runtime unit explicitly bound one-to-one to a region in this profile | The whole dish, a nested Arena, or automatically identical to a region |
| scientific `fish_id` | Subject identity supplied by experiment/aquatics metadata | Something Orange infers from image order |

One source camera remains one canonical acquisition stream. Four derived crop
videos would be four region-scoped media artifacts, not four new source
streams.

## Responsibility Boundary

### Orange owns

- source-camera and recording-frame identity;
- complete detector output and terminal inference status;
- camera-native physical-registration evidence;
- the selected multi-compartment-layout snapshot used for the recording;
- deterministic point-to-region assignment;
- optional per-region continuity as an explicitly derived runtime product;
- region-scoped crop media when that feature is later implemented; and
- all counters, ambiguity states, failures, and persisted provenance.

### Citrus owns

- stimulus Arenas and their logical coordinate frames;
- mapping an explicitly bound Orange `region_id` to a Citrus Arena;
- protocol execution and scientific stimulus behavior; and
- binding a Citrus Arena to a declared subject/fish identity.

Citrus must eventually read one immutable camera snapshot and fan it out to all
participating Arenas. It must not instantiate four destructive readers for one
Shaman queue.

### Palette owns

- offline validation and reproducible re-assignment from frozen evidence;
- comparison of acquisition-time geometry with an offline Hough or segmentation
  fit;
- normalized analysis ontology and derived mask publications; and
- reporting contradictions without rewriting acquisition evidence.

### Aquatics or experiment metadata owns

- biological subject identity, strain, life stage, cohort, and intended
  compartment occupancy.

## Required Geometry Products

Two products are needed. They must not be conflated.

### 1. Multi-compartment physical-layout definition

The stable physical design should describe:

- a versioned `layout_id` and content digest;
- dish or holder design identity;
- a declared ordering rule (`camera_row_major_v1` for a well grid) and, for
  apparatus classes that need one, a dish-local metric coordinate frame with
  its orientation marker semantics;
- the expected outer boundary;
- one non-overlapping polygon per stable `region_id`;
- physical barrier polygons or explicit dead zones;
- containment and boundary-tolerance rules;
- the declared maximum subject count per region; and
- provenance for the measured or manufactured dimensions.

The preferred authoring space is dish-local millimetres. Region IDs should be
semantic and stable, for example `compartment_north_west`, rather than derived
from detection-vector order.

### 2. Recording-selected registered layout

The recording-time product should bind the design to one exact camera view:

- camera serial and canonical acquisition-stream identity;
- native raster and camera coordinate descriptor;
- selected physical-registration artifact and digest;
- selected layout definition and digest;
- the transform or derivation used to form camera-pixel region polygons;
- camera-pixel outer boundary, region polygons, barriers, and dead zones;
- ordering rule, recorded ordering margins, per-region fit residuals, and
  their acceptance state (or orientation evidence for the barrier-partitioned
  class);
- fit residuals, containment checks, and review-overlay digests;
- accepted/candidate/rejected/invalid lifecycle state; and
- a recording-start immutable snapshot and digest.

The hot path consumes only the frozen camera-pixel geometry. It performs no
JSON parsing, Hough fitting, filesystem access, or calibration mutation.

## Region Ordering Rule

Stable region names require a declared, verifiable ordering rule. The rule
depends on the apparatus class, and the recording-selected registered layout
must record which rule it used. Two classes are recognized. This section
supersedes the earlier statement that regions must never be named from the
camera's top-left; that prohibition now applies only to the
barrier-partitioned class. It matches Citrus
`docs/arena_group_registered_geometry_detection_crop_decision_2026-09-03.md`
section 10.1.

### Well grid (current apparatus)

The multi-region holder is one acrylic plate with four cut-outs, each seating
one dish, fixed under one camera. The plate, camera, and projector together
form one Citrus rig configuration. Reorienting or relocating the plate, camera,
or projector is by definition a different rig configuration and therefore a
different registered layout; it is not a rotation to be measured within one.
Because the regions are spatially separated circles, they are distinguishable
by position alone, and region identity is camera-perspective by design:

- each region is an independently fitted circle in camera-native pixels, using
  the existing dish top-rim observation once per well;
- regions are ordered row-major from the camera's top-left with index base 0:
  centres are grouped into rows by `y`, then ordered by `x` within each row;
- the registered layout records `ordering_rule = camera_row_major_v1`, the
  ordering margins below, and the resolved region-index-to-ID table.

The ordering is valid only when the sort is unambiguous. Registration must
compute and record:

- row margin: the smallest vertical distance between adjacent row centroids
  minus the largest vertical scatter within any row; and
- column margin: the smallest horizontal distance between adjacent column
  centroids minus the largest horizontal scatter within any column.

Both margins must exceed a declared threshold, recommended to be at least one
region radius. The margin is measured between region centres, not between
region edges, so wells may sit close together or touch. For four 40 mm wells
at a 42 mm centre pitch on a machined plate, the row margin is about 42 mm
minus sub-millimetre scatter, against a 20 mm threshold. The threshold
therefore bounds plate tilt relative to the camera rows rather than well
spacing: with the pitch near one diameter, a 20 mm threshold rejects tilt
beyond roughly 24 degrees, far past any tilt that would survive rig
configuration acceptance. If either margin fails, the layout state is
`ordering_unresolved` and Orange may show unnamed geometric regions for
diagnostic review but must not publish stable semantic `region_id`
assignments.

Registration must also match the physical design's expected well count,
diameter in pixels (from the physical inner diameter and camera pixels per
millimetre), and centre spacing within declared tolerances.

The smallest safe V1 for this class needs no layout-to-camera transform:

```text
one accepted rim fit per well in camera pixels
    + declared dead-zone inset per well
    + camera_row_major_v1 ordering with recorded margins
    -> frozen recording camera-pixel region geometry
```

### Barrier-partitioned dish (out of scope for V1)

One dish split by internal barriers has regions that are not separable by
position. A circular rim fit determines centre and radius, but not rotation,
and rectangular geometry alone can still be 180-degree ambiguous. A successful
outer-rim fit is not evidence that orientation was resolved. This class must
declare a different ordering rule backed by one of:

1. **Mechanically keyed orientation.** The holder and multi-compartment dish
   fit in exactly one accepted orientation. Commissioned camera-pixel region
   polygons may then be translated, and only if explicitly allowed, uniformly
   scaled by the daily rim fit. Rotation must remain fixed by the fixture and
   declared as such; any detected rotation beyond a configured tolerance
   invalidates the selection rather than being silently ignored.
2. **Asymmetric physical marker.** Daily registration detects or asks the
   operator to confirm a marker that defines dish-local `+X` and `+Y`.
3. **Internal-barrier fit.** Daily registration detects the barrier centre and
   axes, with an asymmetric feature resolving 90- or 180-degree ambiguity.

If none is available, the layout state is `orientation_unresolved` with the
same publication restriction as `ordering_unresolved`. This class must not
reuse `camera_row_major_v1`.

## Detection And Assignment Pipeline

```text
camera frame
    -> detector preprocess and inference
    -> decoded/NMS detection vector with stable per-frame source indices
    -> outer-dish mask decision (when selected)
    -> deterministic region assignment for every decoded detection
    -> persist complete detections and assignment decisions
    -> optional one-observation-per-region continuity/selection
    -> optional live publication, crop, and pose consumers
```

The original detection vector is immutable for this derived operation:

- do not reorder it;
- do not remove detections from the persisted YOLO authority;
- do not replace its coordinates;
- do not populate `track_id` with `region_id`; and
- retain the original `source_detection_index` in every derived decision.

The existing outer spatial policy may prevent an outside-dish detection from
reaching live downstream consumers. Its decoded detection and rejection
evidence must remain persisted. Region assignment should record that outer-mask
state rather than making the detection disappear from the audit trail.

## Stateless Region Assignment

V1 uses bounding-box centroid in camera-native pixels as the representative
point. This matches the existing outer-dish centroid policy and works for all
detections before multi-object pose is available.

Each detection receives exactly one assignment state:

- `inside_region`: representative point is inside exactly one eligible region;
- `barrier_or_dead_zone`: point is in an explicitly excluded area;
- `ambiguous_boundary`: point is within a configured boundary tolerance or
  more than one eligible region;
- `outside_physical_layout_regions`: inside the source raster but outside every
  region;
- `outside_valid_detection_region`: rejected by the selected outer-dish gate;
- `geometry_unavailable`: no valid frozen layout exists; or
- `invalid_detection_geometry`: non-finite or malformed box/point.

The router must not use nearest-centre assignment to force a barrier or
ambiguous point into a compartment. Scientific ambiguity is data, not an
exception to hide.

Polygon boundary behavior, numeric tolerance, and representative-point formula
must be versioned. Iteration order over polygons must not change the result.

## Optional Per-Region Continuity

Region assignment is stateless. Continuity is a separate stateful layer.

For a layout declaring at most one fish per region, a first continuity policy
may select among multiple same-region candidates using:

1. validity under the outer-dish and region rules;
2. distance from the last accepted representative point;
3. a configured maximum speed/displacement gate using elapsed camera time;
4. confidence as a tie-breaker; and
5. deterministic `source_detection_index` as the final tie-breaker.

Every candidate and association cost remains recorded. If no candidate is
clearly defensible, the state is `ambiguous`; the policy must not guess.

Suggested region-state lifecycle:

- `uninitialized`
- `observed`
- `held_after_miss`
- `ambiguous`
- `unavailable_inference`
- `expired`

Hold and expiry are measured from camera timestamps or another declared
monotonic time authority, not a fixed number of frames. This keeps behavior
explicit across 30, 100, and 700 fps recordings.

## Inference Terminal-State Semantics

The continuity layer must distinguish model coverage from biological absence.

| Detection status | Meaning for region continuity |
| --- | --- |
| `disabled` | No inference was configured; do not count a biological miss |
| `not_scheduled` | Decimation/policy skipped this frame; do not age miss state |
| `pending` | Nonterminal; make no observation decision |
| `detections` | Terminal; evaluate candidates for every region |
| `zero_detections` | Terminal successful inference; age per-region miss state |
| `failed` | Technical unavailability; record separately and apply an explicit hold policy |

An enqueue failure must eventually produce a terminal technical failure rather
than leaving an indefinitely pending state.

## Persisted Assignment Product

The first implementation should append a per-camera JSONL sidecar, provisionally:

```text
Cam<serial>_region_assignments.jsonl
```

Provisional schema identity:

```text
schema_id = orange.analytics.compartment_region_assignment
schema_version = 1
```

Each terminal-inference row should carry:

- recording/session identity and per-slot recording token;
- camera serial, runtime camera ID, source stream ID, and native extent;
- local, camera, state, source, and recording frame identities as applicable;
- raw camera and host timestamps plus declared clock semantics;
- detector terminal status and model identity;
- selected outer-mask policy identity and result;
- selected registered-layout ID and digest;
- assignment algorithm ID/version and representative-point rule;
- one record per source detection, including original box, class, confidence,
  `source_detection_index`, assignment state, and optional `region_id`;
- optional per-region selection/continuity result and alternatives;
- truncation, invalid-geometry, writer, and queue counters; and
- event sequence plus clean finalization/count proof.

The sidecar is derived evidence, not a replacement for
`Cam<serial>_yolo_events.jsonl`. Finalization should prove that every terminal
YOLO row in scope has one corresponding assignment row, unless the recording
declares assignment disabled.

Large observations should eventually use a columnar consumer format, but JSONL
is the smallest auditable first slice and matches the existing append-only YOLO
authority.

## Live Publication And Shaman-v2

The current Shaman-v2 ring is bounded, best-effort, destructive, and designed
for one Orange writer plus one Citrus reader. Four Arenas must not independently
pop the same camera queue because they would divide observations rather than
receive identical state.

The Orange-only first slice does not require a Shaman ABI change. It records
assignments and may drive Orange-owned previews/crops locally.

Before Citrus consumes four regions live, Citrus needs:

```text
one CameraTrackingSource per physical camera
    -> one Shaman-v2 reader
    -> one validated immutable latest snapshot
    -> deterministic fan-out to every region-bound sibling Arena
```

A later coordinated ABI may carry region annotations, but it must preserve:

- the complete original object vector;
- explicit source and recording identity;
- one-reader queue semantics;
- source and transmitted object counts;
- a fail-closed truncation indicator; and
- `region_id` as a distinct field from `track_id` and `fish_id`.

The current fixed maximum of 64 live objects must not silently turn a truncated
slot into a claimed complete observation.

## Orange-Only Recording Behavior

Orange-only region tracking is a valid product. It must not require:

- a Citrus process or socket;
- a Citrus canvas, Arena, homography, or projector;
- a stimulus protocol; or
- a Citrus H5 file.

It does require, when explicitly selected:

- a compatible accepted physical dish registration;
- a compatible accepted multi-compartment physical layout;
- a declared ordering rule with recorded margins above threshold, or resolved
  orientation authority for the barrier-partitioned class;
- a frozen recording-bound layout snapshot; and
- detector terminal-event persistence.

Ordinary full-frame recording remains usable when none of these exists. The UI
should expose `off`, `audit`, and eventually `enforce/live` modes. If the user
explicitly requests region assignment and its geometry is missing, stale,
contradictory, or ordering-unresolved, pre-arm must fail closed or require
the user to turn the feature off. It must not silently select a nearby artifact.

## Crop And Pose Implications

The first release retains a full-frame master. This permits direct comparison
between online assignments and offline Palette replay before evidence is ever
discarded.

Later, four region crop streams should be defined as derived artifacts with:

- parent source-stream and recording identities;
- `region_id` and layout digest;
- one crop frame or explicit availability state for every required source
  frame;
- exact crop rectangle in camera-native pixels;
- source detection and continuity-decision identity;
- encoded-frame ACK/write/count parity; and
- explicit no-detection, ambiguous, technical-failure, and transport-loss
  states.

The current crop/pose path selects one highest-confidence detection and cannot
stand in for this design. Multi-region crop/pose requires independent bounded
per-region work and result identities.

`crop_only` remains deferred until the existing crop-only context contract is
satisfied, including recording-bound context images, deterministic
reconstruction, complete availability metadata, and fail-closed transport
validation.

## Failure And Optionality Rules

1. A normal recording never requires compartment routing by default.
2. A requested compartment-routing feature fails pre-arm for invalid required
   geometry or unresolved ordering.
3. A recording freezes one exact registered-layout digest; geometry is not
   polled or mutated on the hot path.
4. Detected dish movement during a recording invalidates the derived routing
   product. It does not silently translate the layout.
5. Ambiguous detections remain ambiguous.
6. Technical inference failures are not biological misses.
7. Assignment-sidecar failure makes the requested assignment product
   incomplete; it does not rewrite the raw recording as though assignment had
   never been requested.
8. Crop-only or live-Citrus modes may impose stricter failure behavior, but
   those policies must be explicit and separately versioned.

## Performance Requirements

Point-in-polygon tests for a small fixed set of regions are negligible relative
to detector inference, but the implementation must still remain bounded:

- preprocess immutable polygon edge data once at arm;
- perform no allocation proportional to recording duration;
- perform no filesystem or JSON work on camera/YOLO hot threads;
- send append-only rows through a bounded writer queue;
- never block acquisition, detector submission, or full-frame recorder ACKs;
- expose queue high-water, drops, write failures, and final row parity; and
- benchmark four cameras at the production rate with positive detections.

Per-region video encoding is not assumed cheap. It requires a separate
throughput and storage matrix before promotion.

## Validation Plan

### Pure deterministic geometry tests

- one detection strictly inside every compartment;
- region polygons in nontrivial order;
- points exactly on and within tolerance of barriers;
- points outside the outer dish and outside every region;
- malformed/non-finite boxes;
- reflected or rotated camera mappings;
- well-grid ordering margins above and below threshold, including plate tilt
  approaching 45 degrees;
- wrong well count, diameter, or spacing against the physical design;
- 90- and 180-degree orientation ambiguity (barrier-partitioned class);
- keyed-fixture translation within bounds and beyond bounds;
- identical replay producing byte-equivalent assignments; and
- stable result under polygon container iteration changes.

### Detection and continuity tests

- no detections;
- one detection per region;
- multiple candidates in one region;
- duplicate boxes around one fish;
- low-confidence false positives;
- missing detection followed by reacquisition;
- `not_scheduled`, `pending`, `zero_detections`, and `failed` sequences;
- delayed/stale semantic results;
- source frame-ID gaps and intentional rate decimation;
- camera/Orange restart and continuity-generation reset; and
- elapsed-time behavior at 30, 100, and 700 fps.

### Recording tests

- feature off with no layout: ordinary recording succeeds;
- audit mode with accepted layout: no downstream behavior changes;
- requested enforcement with missing/stale/unoriented layout: pre-arm fails;
- full-frame plus assignments: YOLO/assignment terminal-row parity;
- writer failure: product finalizes incomplete with the full-frame master intact;
- exact layout and geometry assets copied into the portable recording envelope;
- Palette offline replay matches online assignments; and
- offline Hough disagreement is reported without replacing acquisition truth.

### Later live/Citrus tests

- one camera reader fans the same immutable snapshot to four Arenas;
- no Arena divides or drains another Arena's observations;
- each `region_id` binds to exactly one configured Arena for the run;
- Citrus `fish_id` remains separate and explicit;
- Shaman truncation cannot masquerade as a complete state; and
- process restart changes generation and invalidates stale continuity.

## Implementation Checklist

### Phase A: Resolve and freeze the geometry contract

- [ ] Declare the apparatus class of every supported fixture: well grid
      (`camera_row_major_v1`) or barrier-partitioned.
- [ ] For the well grid, define the ordering-margin threshold and the expected
      well count, diameter, and spacing tolerances.
- [ ] For any barrier-partitioned fixture, choose a mechanical key, asymmetric
      marker, or barrier-fitting method and define its ambiguity/failure
      states.
- [ ] Define dish-local axes, units, polygon winding, boundary tolerance, and
      stable `region_id` naming.
- [ ] Decide whether daily scale adjustment is allowed or whether only
      translation is accepted for keyed fixtures.
- [ ] Define layout-definition and registered-layout schema IDs, lifecycle,
      digests, and selection precedence.
- [ ] Define a recording-bound layout snapshot and compatibility rules.

### Phase B: Implement pure stateless assignment

- [ ] Add a small module independent of `orange.cpp` and GUI code.
- [ ] Consume immutable camera-pixel polygons and indexed detections.
- [ ] Return explicit inside/barrier/ambiguous/outside/invalid states.
- [ ] Preserve the original detection vector and source indices.
- [ ] Add deterministic synthetic tests, including reflections and ambiguity.

### Phase C: Persist and review assignments

- [ ] Add the append-only per-camera assignment writer and bounded queue.
- [ ] Bind rows to YOLO terminal events, recording token, layout digest, and
      source frame identity.
- [ ] Add count/parity/failure proofs to recording finalization.
- [ ] Copy exact layout/registration assets into the recording envelope.
- [ ] Add a GUI overlay showing regions, barriers, representative points,
      assignments, and ambiguity without changing detector output.
- [ ] Add `off` and `audit` modes before any enforcement mode.

### Phase D: Add optional per-region continuity

- [ ] Freeze the one-fish-per-region declaration and policy version.
- [ ] Implement bounded elapsed-time motion gating and explicit ambiguity.
- [ ] Record all candidates, costs, state transitions, holds, and expirations.
- [ ] Reset continuity on recording/camera generation changes.
- [ ] Do not populate production `track_id` until a separate tracker contract
      exists.

### Phase E: Validate Orange-only operation

- [ ] Run full-frame plus assignment recording with Citrus stopped.
- [ ] Compare online results with deterministic Palette replay.
- [ ] Validate wall/barrier cases with real fish.
- [ ] Measure hot-path latency, writer pressure, and zero-drop behavior.
- [ ] Promote only after the portable recording envelope is independently
      consumable.

### Phase F: Add region-scoped crop and pose products

- [ ] Generalize the single-object crop selector into explicit per-region work.
- [ ] Define region-scoped derived stream IDs and availability rows.
- [ ] Preserve source-stream identity and exact frame joins.
- [ ] Validate encoder ACK/write/count parity for every region stream.
- [ ] Keep the full-frame master during this validation phase.
- [ ] Revisit crop-only only after context-image and reconstruction gates pass.

### Phase G: Coordinate Citrus fan-out

- [ ] Implement one shared camera reader/source in Citrus.
- [ ] Fan immutable state to all bound Arenas without multiple queue consumers.
- [ ] Add explicit `region_id -> arena_id` binding with digest/provenance.
- [ ] Keep scientific subject identity separately declared.
- [ ] Add coordinated live ABI fields only if local Citrus routing cannot
      consume the frozen layout safely.

## Questions For Review Before Phase A Is Frozen

1. Resolved 2026-09-03: the current fixture is a well grid, four dishes seated
   in a fixed acrylic plate under one camera, ordered by `camera_row_major_v1`.
   Any reorientation is a new rig configuration. Marker or barrier detection
   is required only if a barrier-partitioned dish is later introduced.
2. Is the one-fish-per-compartment condition a hard acquisition invariant for
   this profile?
3. What physical design artifact should own the region polygons and barrier
   widths: a CAD drawing, a measured target, or an operator-authored layout?
4. Should the first release permit bounded daily radius scaling, or only centre
   translation of commissioned polygons?
5. How wide should the explicit barrier/dead zone be in physical units, and
   should it include a configurable fish-centroid forgiveness band?
6. Is the first user-facing product `audit` plus persisted assignments, or must
   live per-region crops be available immediately?
7. When more than one candidate appears in a compartment, should the first
   continuity policy prefer nearest previous position, highest confidence, or
   refuse selection until reviewed?
8. Which consumer should own the explicit `region_id -> fish_id` binding when
   Citrus is absent: the Orange experiment specification, an aquatics record,
   or Palette's normalized recording context?

## Recommended First Review Outcome

Approve only Phases A through C initially: a digest-bound multi-compartment
physical layout, deterministic assignment, append-only evidence, and a visual
audit overlay.
Keep the full-frame master and leave continuity, crop-only, pose fan-out, and
Citrus multi-Arena consumption disabled until real recordings demonstrate that
the geometry and ambiguity policy are stable.
