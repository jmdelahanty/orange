# Recording Observation Context: Acquisition-Side Census

Date: 2026-08-13

Status: read-only architecture census and implementation checklist; no schema
is frozen by this document

Repositories inspected:

- Orange: `/home/jeremy/orange-gop-split-a16`, branch `exp/gop-split-a16`,
  inspected HEAD `5509e5f61c99`
- Citrus: `/home/jeremy/citrus`
- Palette consumer checkout: `/home/jeremy/palette-stimulus-v5`

Related documents:

- [Orange recording metadata](recording_metadata.md)
- [Orange recording-session manifest](recording_session_manifest_contract.md)
- [Orange recording geometry](recording_geometry_contract.md)
- [Orange YOLO spatial-mask runtime](yolo_spatial_mask_runtime_design.md)
- [Orange calibration-surface audit](calibration_surfaces_architecture_audit_2026_07_26.md)
- [Citrus H5 log](../../citrus/docs/Understanding_H5_Log.md)
- [Citrus runtime geometry H5 contract](../../citrus/docs/runtime_geometry_h5_contract.md)
- [Palette session context](../../palette-stimulus-v5/docs/session_context.md)
- [Palette recording manifest contract](../../palette-stimulus-v5/docs/recording_manifest_contract.md)
- [Palette recording-bound geometry design](../../palette-stimulus-v5/docs/recording_bound_geometry_import_and_validation_design.md)

## Purpose

Palette is designing `recording_observation_context_v1`, a digest-bound
recording-level context shared by detections, keypoints, masks, training
exports, and derived analyses. This census identifies the current
acquisition-side authorities, their exact persisted surfaces, and the gaps that
must be resolved before that context is frozen.

This document also establishes the implementation order for two related but
independent work lanes:

1. `MaskSetDefinition` and
   `larval_zebrafish_subject_anatomy_2d_v1` can proceed now.
2. `recording_observation_context_v1` should be frozen only after the
   authority, identity, lifecycle, and missing-acquisition-field decisions in
   this checklist are resolved.

The census is intentionally limited to two-dimensional observation geometry.
One context describes one recording-bound camera-to-arena observation edge.
Neither camera identity nor arena identity is unique across the collection, so
the model supports multiple arenas observed by one camera and one arena
observed by multiple cameras without defining a multi-camera or 3D
reconstruction product in v1.

## Inspection limitation

The mounted recording stores containing the requested Batman-era, Sleepyfish,
and recent acquisition bytes were not available on this host during this pass.
Historical conclusions therefore use Palette's checked-in exact-artifact
audits and current Orange/Citrus implementations and calibration artifacts.
They are not a new byte-level validation of every representative recording.

Before final acceptance, the resulting validator must still be run against:

- one new producer-native recording with a selected daily registration;
- one July 2021 Batman-era recording without recording-bound geometry; and
- the Sleepyfish recording used by the subject-mask canary.

## Executive conclusion

The acquisition stack already has sufficient digest-bound camera-native
geometry to define reliable masks for new recordings. The largest remaining
problem is not geometry math. It is assigning each overlapping persisted
surface one precise authority.

There must not be a flat rule such as "H5 wins over Orange JSON." Different
producers own different claims:

- Orange owns the acquisition recording, encoded-frame identity, applied
  camera state, and physical rim/gate evidence.
- Citrus owns the stimulus session, protocol execution, presentation mapping,
  and geometry actually selected for stimulus rendering.
- Aquatics/Zebrobot owns mutable biological source records.
- Palette owns normalized analysis ontology, compatibility adapters, recovery
  receipts, and derived publications.

When two sources that should make the same claim disagree, consumers must
record a contradiction, stale binding, or scope mismatch. They must not
silently select the more convenient value.

## Required authority model

| Claim | Authoritative persisted source | Schema/lifecycle and digest | Current gap |
| --- | --- | --- | --- |
| Orange acquisition recording identity and lifecycle | `<recording>/recording_session.json` | `orange.recording_session` v1; terminal status; no documented whole-file content digest | Must bind explicitly to the Citrus session rather than equating their IDs |
| Encoded frame identity and raw timestamps | `camera_artifacts.<serial>.metadata`, normally `external_recorder/Cam<serial>_external_meta.csv` | One row per encoded frame; manifest count/parity validation | Historical recording modes vary; use the session manifest rather than guessing a filename |
| Original requested camera configuration | `recording_snapshot.json.cameras[serial]`, also embedded in Citrus `/recording_snapshot/recording_snapshot_json` | Recording-start snapshot; no per-camera content digest | Requested state must not be presented as SDK readback |
| Resolved runtime camera configuration | `recording_snapshot.json.camera_runtime[serial].runtime` | Recording-start snapshot | Needs explicit temporal scope |
| Applied camera/sensor readback | `recording_snapshot.json.camera_runtime[serial].sensor_pipeline` | `orange.camera.sensor_pipeline_state` v1; supported/unsupported and requested/applied results | Older recordings lack it; binning is not currently explicit |
| Camera-native raster frame | `recording_snapshot.json.camera_runtime[serial].coordinate_frame` | Top-left origin, +X right, +Y down, native extent | Physical view role and gravity relation are absent |
| Citrus stimulus-session identity | Citrus H5 root attributes | `session_uuid`, start time, rig, canvas, arena, protocol, host, software | It is distinct from the Orange recording ID |
| Protocol definition and semantic identity | `/protocol_snapshot/protocol_definition_json`; when supported, `protocol_semantic_json` and `protocol_semantic_hash` | Semantic hash is SHA-256; unsupported protocols fail closed with a status payload | The mutable source filepath is provenance, not semantic identity |
| Geometry Citrus actually selected and used | `/runtime_geometry_contract/contract_json` | `citrus.session.runtime_geometry_contract` v1; immutable, checksummed component references | Physical inner-rim/gate adoption remains incomplete in Citrus runtime v1 |
| Physical water-side inner rim and outward detection gate | `<recording>/recording_geometry_contract.json`; exact H5 mirror at `/recording_geometry_contract/contract_json` and scoped view at `h5_scope_json` | `orange.recording.geometry_contract` v1; exact SHA-256 and checksummed asset manifest | H5 mirror is provenance and currently says `metadata_only_not_applied_by_citrus` |
| Presentation reflection into decoded Citrus stimulus video | `/presentation_mapping/contract_json` | `citrus.presentation_mapping` v1; checksum, extents, named transforms | Missing historical metadata must remain `legacy_unknown`; no guessed flip |
| Recording-time subject declaration | `/subject_metadata` and `/zebrobot_snapshot/snapshot_json` | Zebrobot snapshot has schema/status but no whole-snapshot digest was found | Life stage is not controlled; values can differ from later aquatics truth |
| Mutable dish, cross, and cohort record | Aquatics/Zebrobot record referenced by stable ID | External authority and revision policy | Recording must preserve the acquisition-time snapshot even after later corrections |
| Analysis ontology and corrected derived context | Palette registry/publication | Palette-owned, versioned, digest-bound | Corrections must be additive and must not rewrite immutable producer evidence |

## Exact persisted paths and interpretation

### Orange acquisition envelope

The recording folder is the portable acquisition envelope. Relevant paths are:

```text
recording_session.json
recording_snapshot.json
recording_geometry_contract.json
recording_geometry_assets/manifest.json
external_recorder/Cam<serial>_external_meta.csv
```

`recording_session.json` owns output topology and lifecycle. Consumers should
resolve the per-camera video and metadata through
`camera_artifacts.<serial>` instead of assuming a root filename. The metadata
CSV owns the encoded-frame-to-camera/host-timestamp mapping.

`recording_snapshot.json` schema version 2 carries three deliberately distinct
camera views:

```text
cameras[serial]                         original requested config
camera_runtime[serial].runtime          resolved runtime config
camera_runtime[serial].sensor_pipeline  SDK/device readback
```

A requested/applied mismatch is useful evidence. It must not be normalized by
overwriting one with the other.

The same snapshot can contain:

```text
camera_runtime[serial].coordinate_frame
camera_runtime[serial].runtime.optics
camera_runtime[serial].runtime.rig_io
calibrations[serial].dish_top_rim_observation
recording_geometry_contract
```

The direct `calibrations` projection is convenient for recording-local use,
but the complete physical geometry authority is the exact, digest-bound
`recording_geometry_contract.json` and its materialized asset manifest.

### Citrus experiment envelope

The relevant H5 surfaces are:

```text
/
/protocol_snapshot/protocol_definition_json
/protocol_snapshot/protocol_semantic_json
/protocol_snapshot/protocol_semantic_hash
/protocol_snapshot/protocol_semantic_status_json
/associated_cameras/camera_ids
/associated_cameras/camera_metadata/<camera_id>
/recording_snapshot/recording_pointer_json
/recording_snapshot/recording_snapshot_json
/runtime_geometry_contract/contract_json
/recording_geometry_contract/contract_json
/recording_geometry_contract/h5_scope_json
/presentation_mapping/contract_json
/subject_metadata
/zebrobot_snapshot/snapshot_json
```

The H5 recording snapshot is a portable copy of Orange recording-start
metadata, not an independent live camera authority. Current Citrus associated
camera metadata carries camera identity and configured FPS; observed camera
cadence remains an Orange/per-frame-stream measurement.

`/runtime_geometry_contract` is Citrus's statement of the exact homography,
scale, commissioning release, and optional daily translation resolved before
experiment activation. It is scoped to one `(rig, canvas, arena, camera)`
tuple. Cross-arena fallback is forbidden.

`/recording_geometry_contract` is an exact verified mirror of Orange physical
geometry. It is recording provenance, not evidence that Citrus applied the
physical boundary as a runtime stimulus constraint. The current linkage
explicitly records `runtime_adoption = metadata_only_not_applied_by_citrus`.

### Geometry roles that must remain separate

The following are not aliases:

```text
accepted_inner_rim_boundary
    Physical water-side boundary evidence.

valid_detection_region
    Slightly outward centroid gate used to tolerate a wall-adjacent animal.

input_mask_region
    Runtime-derived neural-network input context, which may include an
    additional explicitly recorded outset.

canonical_experimental_area
    Citrus logical stimulus geometry after the accepted homography and daily
    translation.
```

Palette's `MaskSetDefinition` should name the selected role. An offline Hough
fit may compare with the acquisition-time boundary but must never silently
replace it.

## Current normalized evidence

### Camera and sensor

The current production-like four-camera configuration declares:

- cameras `2010093` through `2010096`;
- Emergent HB-20000SBM cameras;
- `4512 x 4512` Mono8;
- `100 fps`;
- `100 us` configured exposure;
- gain `256`, representing unity digital gain in the current Emergent
  convention;
- Canon EF 100 mm f/2.8L Macro IS USM lenses;
- Hoya R72 67 mm IR long-pass filters; and
- per-camera focus and iris device values.

Standalone sensor characterization found Bit8 active with Bit8/Bit10 options,
DualADC false, gain 256, PGAGain 0, AutoGain false, LUT disabled, HCG
unsupported, gamma unsupported, and supported higher-bit-depth pixel formats.
That standalone record is capability evidence only. It must not be copied into
a recording context unless the recording's own sensor readback supports the
same claim.

Explicit acquisition gaps include:

- binning mode/readback;
- a camera mirror setting distinct from homography reflection;
- controlled physical view role such as `overhead`;
- optical-axis relation to gravity;
- stable mount/optical-path identity; and
- complete per-camera illumination identity.

### Physical boundary

Current accepted daily top-rim observations demonstrate the intended modern
evidence shape:

- native `4512 x 4512` Mono8 frame identity;
- camera serial and arena identity;
- temporal mean over 60 source frames;
- exposure, frame rate, filter, NIR, TTL, and projector state;
- accepted water-side inner-rim center/radius;
- physical `40 mm` radius;
- observed camera pixels/mm;
- outward centroid-gate radius and explicit outset;
- operator acceptance; and
- source-image and derived-artifact checksums.

Acceptance is currently distributed across the observation's accepted
boundary/operator-review fields and the accepted daily-registration product;
the observation does not expose one uniform top-level lifecycle status. The
context normalizer must interpret the existing schema explicitly rather than
assuming a generic `status` field.

### Optics and physical medium

Current sources can carry lens, focal length, configured iris/aperture, focus
device value, filter, tank design, water/refractive geometry, and the optical
state of a calibration capture.

They do not yet provide:

- one controlled `optical_path_id`;
- a calibrated physical focus distance;
- a complete recording-time fill-state guarantee;
- a frame-indexed focus-change authority; or
- a digest-bound recording-wide optics snapshot independent of the general
  recording snapshot.

Normal GUI recording locks most sensor and optics mutations, but focus remains
mutable. Until focus is locked or event-logged, it must be represented as a
recording-start snapshot that may change without an authoritative event.

### Illumination

Calibration capture artifacts record useful state such as NIR wavelength,
triggering, filters, exposure, projector black, and averaging. This is not a
complete statement of experiment illumination.

The current recording surfaces do not consistently provide:

- a controlled illumination-source identity;
- source geometry;
- spectral distribution or measured wavelength;
- configured and measured optical power;
- consistent metadata for every camera;
- verification/commissioning identity; or
- a frame-indexed baseline-illumination event stream.

Citrus protocol events own changing projected stimuli. They do not prove
physical projector output or baseline NIR illumination.

### Subject identity

Citrus queries and preserves recording-time subject fields including dish ID,
cross ID, fertilization date, genotype, parents, source aliases, species, sex,
fish count, source-dish population count, line/strain, query time, and derived
days post fertilization.

Ownership must remain explicit:

- Citrus H5 records what acquisition declared and observed at session start.
- Aquatics/Zebrobot owns mutable source records and later corrections.
- Palette owns normalized analysis ontology, publication membership, and
  versioned compatibility/correction records.

Operator-selected subject count is not the same fact as source-dish population
count. DPF may be recorded or derived, but a controlled `life_stage` must not
be inferred without a versioned biological policy.

## Temporal-scope model

Every normalized field should declare one of:

```text
recording_constant
recording_start_snapshot
interval_scoped
frame_event_stream
unknown
```

Current expected assignments include:

| Field family | Current temporal scope |
| --- | --- |
| Camera raster, configured FPS, exposure, gain, pixel format, iris | Normally recording-constant after arm; preserved as start config/readback |
| Focus | Recording-start snapshot; currently mutable and not event-logged |
| Commissioning release, homography, scale, selected daily registration | Frozen by Citrus before experiment activation |
| Encoded-frame identity and timestamps | Per-frame event rows |
| Projected stimulus state | Citrus protocol/event/tracking timelines |
| Baseline NIR illumination | Intended to be recording-constant, but current semantic evidence is incomplete |
| Subject declaration | Recording-start snapshot |

If a value can change but no frame or interval authority exists, the context
must say so. It must not upgrade a start snapshot to a recording-wide truth.

## Lifecycle and missing-value vocabulary

One overloaded `status` field cannot represent the required distinctions. The
normalized context should keep independent axes:

```text
value_state:
  known | unknown | not_applicable | historically_unavailable

evidence_state:
  available | missing | invalid | unsupported

selection_state:
  accepted | candidate | rejected | not_selected | not_applicable

temporal_scope:
  recording_constant | recording_start_snapshot |
  interval_scoped | frame_event_stream | unknown
```

Examples:

- Unsupported HCG is `value_state = not_applicable` or `unknown` according to
  the hardware contract, with `evidence_state = unsupported`; it is not a
  numeric zero.
- A July 2021 recording with no geometry field is
  `historically_unavailable`; it is not proof that no dish existed.
- A valid daily-registration candidate that was not selected is
  `evidence_state = available`, `selection_state = not_selected`.
- An Orange recording geometry mirror in H5 can be accepted physical evidence
  while separately declaring that Citrus did not apply it as runtime geometry.

## Conflict and precedence policy

Precedence is claim-specific:

1. Resolve output paths and recording lifecycle from
   `recording_session.json`.
2. Resolve encoded frame identity and raw timestamp values from the manifest's
   authoritative per-frame metadata artifact.
3. Resolve requested, runtime, and applied camera values from their separate
   `recording_snapshot.json` subtrees. Do not collapse them.
4. Resolve protocol identity and execution from Citrus H5 protocol and event
   surfaces.
5. Resolve the geometry used by Citrus from `/runtime_geometry_contract`.
6. Resolve physical rim and centroid-gate evidence from the exact
   recording-bound Orange geometry contract and verify its checksum and
   camera/arena scope.
7. Resolve presentation-video mapping only from `/presentation_mapping`.
8. Preserve H5 subject data as the recording-time declaration and apply later
   registry corrections as separate versioned records.

If two supposedly identical immutable identities, digests, dimensions,
camera IDs, or arena IDs disagree, fail the affected use closed. Classify the
failure as a contradiction, stale pointer, scope mismatch, malformed evidence,
or requested/applied difference. Do not hide it through a global source
priority.

## Historical compatibility

### Modern producer-native recording

Geometry is eligible when all of the following hold:

- the Orange recording carries a complete geometry contract;
- its exact digest verifies;
- its camera serial, native raster, rig, canvas, and arena match;
- the boundary role is explicit;
- the associated source observation and assets verify; and
- any claim about Citrus application agrees with the H5 runtime geometry
  contract.

### July 2022 recovered registration

Palette's current audit found operator-approved associations with complete
assets for selected July 2022 recordings. Those recovery receipts are valuable
but are not producer-native proof that the geometry was selected or applied at
recording time. They must remain labeled as recovery evidence and independently
validated against offline fits.

### July 2021 Batman and Sleepyfish-era recordings

When no recording-bound geometry pointer or exact source exists, classify the
boundary as `historically_unavailable`. Do not synthesize an authoritative mask
from:

- arena dimensions alone;
- a later daily registration;
- a current mutable active pointer;
- approximate pixels/mm;
- camera serial alone; or
- a visually plausible circle.

Offline Hough fits can create derived candidate masks with their own lifecycle
and comparison results. They cannot retroactively become acquisition-time
accepted geometry.

## Mask-set binding recommendation

`MaskSetDefinition` can proceed before the complete observation context is
frozen. Its first recording binding should include at least:

```text
recording_observation_context_digest
orange_recording_id
citrus_session_uuid, when present
camera_id
arena_id
camera_coordinate_frame
geometry_contract_digest
rim_observation_digest
boundary_role
derivation_rule
```

The derivation rule should preserve the physical boundary, centroid gate, and
input-context mask as separately named masks. The acquisition geometry is
immutable input evidence.

An offline Hough comparison should emit a separate result:

```text
not_evaluated | agreement | review_required | mismatch
```

Neither `agreement` nor a better numerical fit silently changes the selected
acquisition mask. Replacement requires an explicit, versioned derived
publication or recovery decision.

## Digest recommendations

Strong digest coverage already exists for physical geometry assets,
commissioning releases, homography and scale products, daily registration,
runtime geometry components, and supported protocol semantic payloads.

Future producer work should add or expose:

- a digest over the finalized `recording_session.json` semantic payload;
- a digest over the stable recording-start snapshot;
- a digest over each normalized applied-camera state;
- a digest over the recording-time Zebrobot/subject snapshot;
- controlled optical-path and illumination identities with revisions; and
- a digest over the final normalized observation-context envelope.

The design must specify canonicalization before assigning semantic JSON
digests. A digest of mutable pretty-printed bytes and a digest of canonical
semantic JSON are different products and must be named accordingly.

## Ordered implementation and decision checklist

The items below are intentionally ordered. Each decision constrains the next
one.

Checklist semantics:

- `[x]` means the decision is frozen and, where code is required, implemented
  with focused validation.
- `[ ]` means material implementation or a still-open design decision remains.
- A frozen policy whose runtime enforcement is still missing stays unchecked
  and is labeled accordingly; documentation alone is not treated as producer
  compliance.

### Current execution track: reciprocal Orange/Citrus binding

This is the immediate correctness track. It closes the mutable-pointer
time-of-check/time-of-use gap before the broader observation-context envelope
is frozen. Checkpoints 2 through 9 below can proceed incrementally, but they do
not need to delay this producer-native association chain.

#### Foundation

- [x] Implement and test the stable observation-edge identity, canonical
      digest, general edge-collection validation, and current one-arena-per-
      source-camera-stream producer restriction.
- [x] Freeze strict schemas for the Orange request, Citrus acceptance or
      rejection, and finalized-H5 receipt.
- [x] Implement and test canonical sealing and reciprocal-chain validation for
      those three records.
- [x] Correct current crop-media vocabulary and coordinate-space semantics
      while preserving historical schema-v1 interpretation.

#### Phase A: immutable request and pre-arm acceptance

- [ ] Materialize an immutable recording-start snapshot, distinct from the
      ordinary `recording_snapshot.json` that finalization may patch.
- [ ] Have Orange create one sealed binding request per observation edge from
      recording-bound inputs only.
- [ ] Persist the identities and request references/digests in the Orange
      recording-start envelope.
- [ ] Transport the exact request to Citrus before experiment activation.
- [ ] Have Citrus validate the request against its selected rig, canvas,
      arena, active camera, frozen output folder, and exact Orange geometry
      mirror.
- [ ] Generate and persist one shared Citrus experiment-group ID plus one
      per-arena session UUID.
- [ ] Embed the exact accepted request and Citrus acceptance in every matching
      H5; do not reread `latest_recording.json` for association authority.
- [ ] Enforce `required`, `optional`, and `not_applicable` arm behavior without
      relabeling a failed Citrus handshake as Orange-only.
- [ ] Add focused producer tests for acceptance, rejection, stale/mismatched
      pointers, output-path containment, and four-camera Shadow association.

#### Phase B: finalized H5 receipt and parent manifest

- [ ] After successful H5 flush and close, compute its byte size and SHA-256
      and seal the finalized receipt.
- [ ] Return the receipt to Orange through a bounded finalization handshake.
- [ ] Have Orange verify the complete request/acceptance/receipt chain before
      accepting `bound` status.
- [ ] Atomically write a parent-level observation-edge collection into
      finalized `recording_session.json`.
- [ ] Make rolling clips reference the parent observation contexts rather than
      minting or duplicating identities.
- [ ] Preserve explicit terminal `unbound` reasons for incomplete optional or
      required finalization.
- [ ] Add restart, partial-write, invalid-receipt, and multi-H5 finalization
      tests.

#### Consumer handoff

- [ ] Teach Palette to consume and verify the producer-native edge and exact H5
      receipt before treating the Citrus association as bound.
- [ ] Keep mask eligibility independent from Citrus binding eligibility: a
      valid binding does not itself prove an accepted dish mask, and an
      Orange-only recording may still carry a valid recording-bound mask.
- [ ] Run one modern Citrus-backed semantic canary and one Orange-only canary.

### Checkpoint 1: Scope and identity key

- [x] Define v1 identity as one stable recording-bound camera-to-arena
      observation edge: `(Orange recording, source-camera frame stream, qualified
      rig/canvas/arena)`.
      Citrus binding is a separate lifecycle that must not rename the edge.
- [x] Allow camera IDs and arena IDs to repeat across the context collection;
      the topology is many-to-many even when a current rig is one-to-one.
- [x] Preserve Orange recording ID in the observation identity and Citrus
      session UUID in a separate binding acceptance/final receipt.
- [x] Keep clip identity outside the observation edge so rolling clips reuse
      the same recording-level context.
- [x] Represent recording-only acquisition as binding mode `not_applicable`
      rather than fabricating a Citrus session.
- [x] Define fail-closed behavior for one source-camera frame stream mapped to
      multiple arenas or mismatched camera/arena identities at current
      producer materialization.
- [x] Define a future multi-view product as a reference to multiple existing
      observation-context IDs rather than changing the meaning of a 2D
      context.

Accepted decision: one recording-camera-arena observation edge, with one
shared recording-level parent allowed as a deduplicated storage optimization.
Many-to-many topology is supported by creating one context for each observed
edge. This decision does not itself define cross-view fusion, triangulation, or
3D reconstruction semantics.

### Checkpoint 2: Claim-specific authority registry

- [ ] Freeze the source-of-truth table in this document as executable field
      policy.
- [ ] Define requested-versus-applied camera semantics.
- [ ] Define exact disagreement classes and which downstream operations they
      block.
- [ ] Prohibit current mutable calibration pointers as substitutes for
      recording-bound evidence.
- [ ] Define how Palette records recovery authority without presenting it as
      producer-native.

### Checkpoint 3: Lifecycle and missing-value model

- [ ] Freeze independent value, evidence, selection, and temporal-scope enums
      for the complete observation context.
- [x] Define `historically_unavailable` separately from `unbound` and
      `not_applicable`.
- [x] Freeze the producer binding lifecycle and controlled terminal reasons.
- [ ] Map every existing Orange/Citrus runtime variant into the normalized
      lifecycle during materialization.
- [ ] Preserve source-native status and reason fields alongside normalized
      status.

### Checkpoint 4: Digest envelope

- [x] Decide canonical semantic JSON rules for observation identity and the
      three binding records.
- [x] Name byte-level file digests separately from semantic-payload digests in
      the binding chain.
- [ ] Define the context digest and transitive referenced-artifact manifest.
- [ ] Decide which small immutable values are copied and which larger artifacts
      remain path/digest references.
- [ ] Require portable recording-local or H5-embedded evidence for normal
      publication.

### Checkpoint 5: Time-varying settings

- [ ] Assign temporal scope to every field.
- [ ] Decide whether Orange should lock focus during recording or emit
      frame/interval-indexed focus events.
- [ ] Confirm that other camera mutations are prevented after arm in GUI and
      headless paths.
- [ ] Define the event authority for any changing baseline illumination.
- [ ] Preserve Citrus stimulus timelines separately from baseline acquisition
      illumination.

### Checkpoint 6: Camera and physical-view additions

- [ ] Add applied binning/readback to the sensor-pipeline snapshot.
- [ ] Add an explicit camera mirror/orientation readback when supported.
- [ ] Add controlled `view_role`, initially including `overhead` without
      inferring it from rig/camera identity.
- [ ] Define optical-axis/gravity orientation only if acquisition can provide
      defensible evidence.
- [ ] Decide whether mount identity belongs to rig configuration or a separate
      optical-path product.

### Checkpoint 7: Optical-path identity

- [ ] Define controlled lens, filter, aperture/iris, focus, dish, water, and
      refractive-path fields.
- [ ] Add one stable `optical_path_id` and revision.
- [ ] Distinguish configured device values from calibrated physical values.
- [ ] Decide the authoritative recording-time dish-fill-state evidence.
- [ ] Bind optional USAF/MTF/DOF characterization by reference and digest;
      never make it a mandatory geometry gate.

### Checkpoint 8: Illumination identity

- [ ] Define baseline NIR source ID, mode, spectrum/wavelength semantics,
      source geometry, configured power, and measured-power optionality.
- [ ] Persist the same controlled structure for every camera or declare a
      shared rig-level source explicitly.
- [ ] Separate configured, verified, and measured illumination facts.
- [ ] Bind illumination commissioning evidence by path and digest.
- [ ] Keep projector stimulus state under Citrus protocol/presentation
      authority.

### Checkpoint 9: Subject authority

- [ ] Digest the recording-time Zebrobot snapshot.
- [ ] Define which fields are acquisition declarations and which are registry
      references.
- [ ] Keep operator subject count distinct from source population count.
- [ ] Define a versioned policy before deriving controlled life stage from DPF.
- [ ] Preserve later aquatics/Palette corrections as additive records.

### Checkpoint 10: Schema and producer materialization

- [ ] Freeze `recording_observation_context_v1` JSON Schema and examples.
- [x] Freeze the narrower observation-identity and Orange/Citrus request,
      acceptance, and finalized-H5 receipt schemas.
- [ ] Add an Orange writer using only recording-bound inputs.
- [ ] Reference it from `recording_snapshot.json` and finalized
      `recording_session.json` with exact digests.
- [ ] Mirror the exact verified context or a scoped view into Citrus H5 when a
      Citrus-backed experiment is present.
- [ ] Keep recording-only Orange acquisitions fully valid.
- [ ] Add strict producer and consumer validators.
- [ ] Add a digest-bound acquisition-media inventory beneath each context,
      retaining distinct full and live-crop stream IDs while binding both to
      the same source-camera frame authority.
- [x] Split crop-video pixel space (`crop_frame_pixels`) from placement
      geometry space (`full_frame_pixels`) in Orange's descriptor rather
      than overloading `coordinate_space`.
- [x] Reconcile Orange's crop role `sidecar` with Palette's established
      `runtime_derived_acquisition_input` role without changing historical
      artifact bytes.

### Checkpoint 11: Historical adapters

- [ ] Create explicit modern, recovered, and legacy-negative fixtures.
- [ ] Validate the July 2022 recovery path without upgrading its authority.
- [ ] Validate Batman/Sleepyfish `historically_unavailable` behavior.
- [ ] Ensure no current pointer or heuristic fills missing historical facts.
- [ ] Record all compatibility adapters and normalized semantic digests.

### Checkpoint 12: Palette mask and publication binding

- [ ] Bind `MaskSetDefinition` to the context digest and exact boundary role.
- [ ] Implement offline Hough comparison as a separate publication.
- [ ] Require camera-native dimensions and coordinate-frame agreement.
- [ ] Bind subject-mask, keypoint, training-export, and derived-analysis
      publications to the same context.
- [ ] Run a semantic canary and return to the packed Crimson presentation
      projection only after these bindings validate.

## Acceptance tests

The eventual implementation should prove at least:

1. A new Orange-only recording produces a valid context with an explicit absent
   Citrus binding.
2. A new Citrus-backed recording binds distinct Orange and Citrus identities.
3. Requested and applied camera values survive a deliberate mismatch without
   being collapsed.
4. Camera-native dimensions, orientation, arena, and geometry digests must
   match before a mask is eligible.
5. Physical rim, centroid gate, and input-context mask remain distinct.
6. Citrus runtime geometry and Orange physical geometry can coexist without
   claiming that Citrus applied the rim mask.
7. A reflected homography does not cause a guessed camera-space flip.
8. Missing presentation metadata is `legacy_unknown` rather than a heuristic
   reflection.
9. A focus or illumination change is either event-bound or causes the relevant
   recording-wide claim to remain unproven.
10. July 2022 recovered geometry remains labeled recovered.
11. Batman and Sleepyfish legacy-negative fixtures do not receive masks from
    current calibration pointers.
12. An offline Hough mismatch creates review evidence without rewriting the
    acquisition mask.

## Current decision and next implementation slice

Checkpoint 1's observation-edge scope is accepted. The stable v1 identity is:

```text
one Orange recording
  x one source-camera frame-stream observation
  x one arena observation
```

Zero or one Citrus stimulus session is attached through the separate binding
lifecycle. It is not part of the context ID.

Camera and arena identities may repeat. For example:

```text
context(recording R, camera C1, arena A1)
context(recording R, camera C1, arena A2)  # one camera sees two arenas
context(recording R, camera C2, arena A1)  # one arena is seen by two cameras
```

Rolling clips and multiple derived publications reference these stable
recording-level contexts rather than duplicating or redefining them. A future
multi-view product for arena A1 references the C1/A1 and C2/A1 context IDs; it
does not replace their camera-native 2D evidence or masks.

The current Shadow rig is the simple one-to-one case:

```text
Orange recording session
  Cam2010093 stream <-> arena_1 <-> Citrus arena_1 H5
  Cam2010094 stream <-> arena_2 <-> Citrus arena_2 H5
  Cam2010095 stream <-> arena_3 <-> Citrus arena_3 H5
  Cam2010096 stream <-> arena_4 <-> Citrus arena_4 H5
```

This produces four observation contexts. Recording/PTP lifecycle can be shared
at the parent; native sensor state belongs to the source-camera frame stream;
dish and
subject declarations belong to the arena; homography, daily registration,
camera-native masks, visibility, and the matching Citrus H5 belong to the
camera-to-arena edge. Equivalent-looking Citrus protocol metadata is not
assumed equal; matching semantic hashes prove equality when available.

This choice prevents one arena's accepted rim, one camera's optical state, or
one Citrus H5's runtime geometry from leaking into another stream while still
allowing recording-wide subject and protocol references to be shared.

Implementation foundation added on 2026-08-13:

- `src/session/recording_observation_identity.{h,cpp}` defines and validates
  the digest-bound edge identity without materializing it into recordings;
  Citrus lifecycle state is deliberately excluded so binding cannot rename the
  context.
- `docs/schemas/orange_recording_observation_identity.schema.json` provides
  the matching strict JSON Schema.
- focused tests cover today's four-row Shadow topology, schema-level
  many-to-many topology, current producer rejection of multiple arenas per
  source-camera frame stream, multiple streams observing one arena, duplicate-edge
  rejection, rolling-clip identity reuse, cross-recording rejection, and
  digest tamper detection.

This is a narrow sub-contract, not a freeze of
`recording_observation_context_v1`. Producer association is now frozen in
`orange_citrus_recording_observation_binding_contract.md`: Orange issues one
immutable request per edge, Citrus returns an acceptance or rejection before
activation, and a complete H5 becomes authoritative only through a final
post-close receipt.

The next implementation slice is Phase A above: create the immutable
recording-start evidence and materialize the Orange request before wiring its
transport to Citrus. The full context schema, optical/illumination additions,
historical adapters, and Palette publication binding remain subsequent tracks;
they must not be represented as already materialized merely because the
narrower identity and handshake schemas exist.

Current production geometry materialization still contains maps such as
`arena_by_camera` and explicitly rejects a camera appearing under more than one
arena in a selected canvas. That behavior was not changed by this foundation.
Many-to-many is currently representable at the identity layer; acquisition
support requires a later deliberate migration from camera-keyed maps to
edge-keyed collections, with ambiguity and overlap validation.

## Parallel producer/consumer association census

On 2026-08-13, three read-only reviews independently traced Orange, Citrus,
and Palette association behavior. No repository, configuration, recording, or
calibration artifact was modified by those reviews.

All three reached the same conclusion:

```text
Strong today:
  recording <-> full and crop acquisition media streams
  camera/arena <-> physical rim and gate
  Citrus H5 <-> runtime camera/arena geometry

Missing today:
  one reciprocal, producer-native, digest-bound relation among all three
```

The desired association exists as individually trustworthy components, but no
artifact currently proves the complete edge:

```text
Orange recording
  x source-camera frame stream
  x arena
  x exact Citrus session/H5
```

### Orange findings

Orange currently knows:

- `recording_id` and recording folder at recording preparation;
- camera serial, full-frame external-recorder `stream_id`, and the separate
  crop stream ID when enabled;
- selected static rig/canvas and camera-to-arena geometry;
- recording-bound physical geometry and its SHA-256;
- exact per-frame stream identity at finalization; and
- in GUI mode, optional post-start evidence that Citrus applied a matching
  daily-registration artifact.

Orange does not currently obtain or persist:

- Citrus `session_uuid`;
- final Citrus H5 path, file size, or SHA-256;
- a Citrus experiment group ID binding all per-arena H5s;
- a reciprocal H5 receipt; or
- a final observation-edge collection.

GUI association is therefore currently:

```text
recording_id x camera_serial -> arena_id from selected static canvas
```

with optional daily-registration confirmation. Headless recording can resolve
static Citrus geometry from an explicitly configured canvas path, but it does
not query a live Citrus process or receive an H5 binding.

Orange's full-frame `stream_id` equals camera serial today. Its live crop is a
separate, first-class acquisition media stream (`<serial>_crop`) derived from
that source frame stream. The observation edge therefore names the
`source_camera_stream_id`; a recording-context media inventory must retain both
full and crop stream IDs rather than collapsing all media to serial.

Relevant one-to-one assumptions include:

- `camera_bindings()` maps one serial to one `ArenaBinding` and rejects a
  second arena in `src/citrus_recording_geometry.cpp`;
- materialized geometry assets use `cameras/Cam<serial>` paths;
- session camera artifacts and frame identity are keyed by serial; and
- artifact validation consumes `arena_by_camera`.

Repeated arenas across different streams are less constrained, but no
first-class edge collection currently represents them.

### Citrus findings

Citrus currently discovers the active Orange recording through
`latest_recording.json`, creates one `SessionInfo` and H5 per selected arena,
resolves one active camera for each arena, and writes the H5 beneath:

```text
<Orange recording folder>/citrus/<Citrus session UUID>_<protocol>.h5
```

Each modern H5 can strongly preserve:

- Citrus `session_uuid`;
- rig, canvas, arena, and active camera;
- runtime homography, scale, commissioning release, and daily translation;
- exact Orange recording-geometry mirror when available; and
- protocol definition and semantic hash when supported.

Citrus does not currently:

- persist a Citrus experiment group ID in H5;
- seal a complete Orange/Citrus binding contract in H5;
- compute a final H5 file SHA-256/size receipt; or
- return final H5 identity/path/digest to Orange.

The current completion callback contains experiment ID, terminal state,
reason, and grace timing only.

#### Pointer time-of-check/time-of-use gap

The Citrus review identified a current correctness risk:

1. Citrus resolves the output directory from the live Orange pointer.
2. Each `SessionLogger` later rereads that mutable pointer independently to
   embed Orange snapshot/geometry provenance.
3. No frozen pointer digest or equality check binds the two reads.

If Orange rotates the latest-recording pointer between these operations,
Citrus could create an H5 under recording folder A while embedding Orange
provenance from recording B. This is the highest-priority association bug to
close before claiming producer-native observation bindings.

Current Citrus runtime geometry is one active camera-to-arena edge per H5.
Additional associated camera IDs are metadata, not separate runtime geometry
authorities. Future multi-camera arenas should therefore produce multiple edge
records/H5 authorities under a shared experiment group rather than placing
several camera transforms into one v1 runtime contract.

### Palette findings

Palette already has a strong registered-dish-mask key:

```text
(rig_id, canvas_name, arena_id, camera_serial)
```

and validates:

- exact recording-bound geometry and asset digests;
- native camera dimensions and pixel convention;
- physical rim versus outward detection gate;
- daily-registration identity and selection state;
- Citrus runtime registration agreement when reading a modern H5; and
- independent recovery provenance where applicable.

This mask edge can be retained. The future observation context should become
its entry-point binding rather than replacing the normalized mask lifecycle.

Palette's current H5 discovery uses, in order, an explicit path, an existing
run attribute, a matching filename stem, or a sole H5 in the recording root;
otherwise it fails as ambiguous. That is cautious discovery but not
producer-native proof of a video/camera/H5 association.

Palette's current recording registry also stores scalar camera and arena
fields. It should remain a compatibility parent while observation contexts
become child records or immutable referenced artifacts. Arrays added to the
legacy row would not provide a real many-to-many relation.

Historical behavior remains:

- July 2021 Batman: `legacy_missing_recording_bound_mask`;
- July 2022 Batman: exact operator-approved recovery receipt, not a rewritten
  producer claim and still requiring independent fit; and
- Sleepyfish: valid inference/storage canary but no modern producer-native
  observation-context or accepted daily-rim authority.

### Complete present/missing/incomplete matrix

| Association component | Present | Incomplete or missing |
| --- | --- | --- |
| Orange recording identity | Recording ID/folder/snapshot/session | No sealed cross-artifact observation collection |
| Acquisition media streams | Full and crop recorders have distinct `stream_id`; Palette inventories both and frame mapping is strong | The future observation context must bind both to one source-camera frame authority rather than treating crop as absent or as a second arena edge |
| Camera/arena static association | Digest-bound selected canvas geometry | One-arena-per-camera maps and camera-keyed asset paths |
| Orange physical mask geometry | Strong digest-bound rim/gate and assets | Producer envelope is camera-keyed rather than edge-keyed |
| Citrus runtime edge | One H5 has one strong active camera/arena runtime contract | No frozen incoming Orange binding; no shared experiment group persisted |
| Orange-to-Citrus discovery | Mutable latest-recording pointer | Pointer is reread; no frozen request/acknowledgement digest |
| Citrus-to-Orange finalization | Terminal completion state | No H5 path, session UUID, size, checksum, or edge receipt |
| Palette mask consumption | Strong fail-closed geometry and camera-frame binding | No general observation-context ID or producer-hashed H5 binding |
| Historical association | Explicit recovery and legacy-negative states | Cannot be upgraded to producer-native evidence |

### Binding lifecycle correction

The observation identity is now stable and contains no Citrus lifecycle state.
The separate binding lifecycle must distinguish:

```text
not_applicable
requested
accepted_pending_finalization
bound
unbound
historically_unavailable
```

Controlled unbound reasons are frozen in
`orange_citrus_recording_observation_binding_contract.md`. A static Citrus
canvas reference must remain separate from a live Citrus-session binding and
must never produce `status = bound` by itself.

### Smallest safe reciprocal handshake

The combined census recommends a two-phase handshake.

#### Phase A: frozen request and Citrus acceptance before experiment arm

Orange materializes one immutable request per observation edge containing:

```text
binding_id and observation_context_id
Orange recording_id and recording folder
recording_snapshot relative path and SHA-256
camera_id and source_camera_stream_id
rig_id, canvas_name, and arena_id
recording_geometry_contract SHA-256, when available
Citrus binding mode: required | optional | not_applicable
```

Citrus validates the frozen request against its selected rig/canvas/arena,
active camera, output folder, and exact Orange geometry mirror before runtime
activation. It persists the accepted request and its digest in the H5 rather
than rereading a mutable pointer for association truth.

#### Phase B: finalized Citrus receipt after H5 closure

After flush and close, Citrus returns an immutable receipt containing:

```text
binding_id and observation_context_id
Citrus experiment group ID and session_uuid
rig/canvas/arena/camera/stream tuple
H5 recording-relative path, size, and SHA-256
session terminal status
protocol semantic hash or explicit unsupported state
runtime geometry contract digest
accepted incoming-binding digest
```

Orange verifies the receipt and atomically adds it to the finalized recording
session. The H5 stores the reciprocal Orange request. Palette can then verify:

```text
Orange recording manifest -> exact Citrus H5
Citrus H5 binding          -> exact Orange recording and edge
```

Orange-only recording remains valid with `not_applicable`. Optional or
required Citrus operation that fails to bind remains explicit `unbound` or
rejected evidence; it must not be relabeled as Orange-only.

### Revised safe implementation order

1. Keep the stable observation identity separate from binding lifecycle.
   **Complete.**
2. Freeze the request, acceptance, and final-receipt schemas, IDs,
   canonicalization, and comparison rules before wiring either producer.
   **Complete.**
3. Remove the Citrus pointer reread from association authority by passing one
   frozen request into every selected arena/session logger.
4. Persist the accepted binding and shared Citrus experiment group ID in each
   H5.
5. Compute the final H5 artifact receipt only after successful close.
6. Return receipts to Orange and write a parent-level edge collection into the
   finalized `recording_session.json`; rolling clips reference that parent.
7. Teach Palette to consume the producer-native edge and verify every referenced
   artifact before publishing masks or other observation products.
8. Separately migrate camera-keyed geometry maps and asset paths to edge-keyed
   collections before claiming live many-to-many acquisition support.
