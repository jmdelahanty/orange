# Crop-Only Recording And Context Reconstruction Design

Date: 2026-08-21

Status: proposed design contract and implementation checklist; no production
detection-driven moving-crop `crop_only` profile is promoted by this document

The detector-independent fixed spatial-ROI path that began with the 2026-08-31
foundation is now implemented as a separate, default-off product (see
[`spatial_roi_recording_v1_foundation.md`](spatial_roi_recording_v1_foundation.md)).
It has a descriptor-bound recorder, stable per-region identities, an additive
multi-output session inventory, and two fixed-region policies: one retains the
full frame and one intentionally replaces continuous full-frame media with a
registered context artifact. That implemented fixed-ROI-only policy is not the
detection-driven, moving-subject `crop_only` product specified here. Fixed ROI
media also cannot recover omitted full-frame pixels. The moving-crop product
remains explicitly unpromoted until its own subject-coverage, context,
media-policy, identity, and finalization evidence are implemented.

## Purpose

Define a production-safe Orange acquisition mode that retains detected subject
crops without retaining a continuous full-frame video, while also preserving a
small, recording-bound set of native-resolution context images that can later be
used to place each crop back into its full camera view.

The intended result is a substantial storage reduction for long recordings
without losing:

- exact crop pixels;
- per-frame crop placement in native camera coordinates;
- camera and recording identity;
- the dish, holder, water, illumination, and optical context needed for human
  review; or
- explicit evidence about frames for which no subject crop was available.

The reconstructed full-context view is a synthetic visualization product. It
is not an acquired full-frame recording and must never be presented as one.

## Related Contracts And Existing Foundations

- [`output_artifacts_contract.md`](output_artifacts_contract.md) defines the
  current crop MP4 and `Cam<serial>_crop_meta.csv` artifacts.
- [`recording_output_unification_plan.md`](recording_output_unification_plan.md)
  defines shared full-frame and crop output descriptors.
- [`crop_recording_lifecycle_fix_plan.md`](crop_recording_lifecycle_fix_plan.md)
  covers crop encoder, queue, pool, and finalization correctness.
- [`recording_geometry_contract.md`](recording_geometry_contract.md) defines
  recording-bound daily registration, dish-mask, homography, scale, and tank
  geometry assets.
- [`orange_standalone_daily_physical_registration_plan.md`](orange_standalone_daily_physical_registration_plan.md)
  separates Orange-owned camera-space dish registration from optional
  Citrus-owned projection registration.
- [`orange_citrus_recording_observation_binding_contract.md`](orange_citrus_recording_observation_binding_contract.md)
  defines the producer-native recording/camera/arena observation edge.
- [`calibration_image_set_schema.md`](calibration_image_set_schema.md) defines
  structured calibration captures. Context images reuse its provenance
  principles but are a distinct recording product.
- [`pre_encoder_reference_capture_plan.md`](pre_encoder_reference_capture_plan.md)
  defines a bounded codec benchmark tap. It is not the context-image product
  specified here.

## Current State

Orange already has most of the pixel and geometry plumbing:

- `CropProducerWorker` selects a YOLO-centered square crop.
- `CropAndEncodeWorker` writes crop video and one metadata row per encoded crop
  frame.
- Crop metadata carries recording, local, and camera frame identities; camera
  and host timestamps; the selected detection rectangle; and the actual
  clamped crop rectangle in `full_frame_pixels`.
- No-detection frames are represented explicitly as black crop frames with
  `crop_state = blank_no_detection` and invalid crop geometry.
- `recording_geometry_assets` can preserve the accepted daily rim observation,
  masks, arena geometry, homography, scale, and optionally their source images.
- `ORANGE_GUI_DIAGNOSTIC_NO_FULL_FRAME=1` maps the full-frame sink to
  `immediate_recycle`, allowing crop and analytics artifacts to exist without a
  full-frame MP4.

That is not yet a production detection-driven moving-crop mode:

- the GUI and startup validator still describe Crop+Encode as requiring
  full-frame Record selection;
- the legacy moving-crop path has no first-class `crop_only` media policy;
  the implemented fixed-ROI media policies do not implicitly promote it;
- moving-crop failures currently have optional-sidecar semantics because the
  full-frame master remains authoritative for that path;
- the recording-bound context-image set required by this moving-crop design
  does not yet have a stable recording-output contract;
- daily-registration images are calibration evidence and are not guaranteed to
  match the production optical state needed for visual reconstruction; and
- existing validators do not prove that moving crops plus background evidence
  are a complete `crop_only` payload.

## Terminology

### Canonical acquisition source stream

The physical camera's frame stream. Its current
`source_camera_stream_id` is the camera serial under
`camera_serial_unique_source_frame_stream_within_recording_v1`.

Crop-only recording does not create a second source stream. Crop MP4s, pose
inputs, encoder shards, and reconstructed videos are derived products of that
one source stream.

### Retained observation media

The media bytes deliberately preserved for a recording. In crop-only mode the
crop video is the primary retained observation subset, but it remains derived
from the canonical acquisition source stream. It is not renamed
`ingest_authoritative` and does not imply that unretained full-frame pixels can
be recovered.

### Context image set

A small, immutable, recording-bound set of native-resolution camera images
captured under declared physical and optical conditions. It includes source
images and may include a deterministic derived background image.

### Synthetic context composite

A full-raster visualization formed by placing one recorded crop over a declared
context background at the crop rectangle recorded for that frame.

It is not a recovered sensor frame. Pixels outside the crop come from a
different capture time, and no-detection frames contain no contemporaneous
subject pixels.

## Core Decisions

### 1. Media policy is separate from camera participation and sink backend

Add a first-class, persisted media policy. Recommended v1 values are:

| Policy | Full-frame media | Crop media | Context image set |
| --- | --- | --- | --- |
| `full_frame_only` | required | disabled | optional |
| `full_frame_and_crop` | required | required | optional |
| `crop_only` | omitted by policy | required | required for reconstructable status |

The media policy answers **what evidence must be retained**. The sink backend
answers **how a required output is written**, for example `external_ipc` or
`in_process`. These must not be encoded in the same field.

The existing per-camera `record` selection continues to mean that the camera
participates in the recording session and receives recording-frame identities.
It must no longer be interpreted as an unconditional request for a full-frame
MP4.

An app-level default may select one policy for all participating cameras, but
the normalized recording snapshot must persist the resolved policy per
observation edge. This keeps the schema compatible with future multiple-camera
per-arena or multiple-arena per-camera acquisition without changing the current
one-camera/one-arena restriction.

For a future multi-compartment dish, one camera still produces one canonical
source acquisition stream. Compartment crops are region-scoped derived media,
not additional source streams. Each physical region may bind one-to-one to a
sibling Citrus Arena, but neither the region nor the whole dish contains nested
Arenas. See
[Multi-compartment physical-layout detection routing](multi_compartment_physical_layout_detection_routing_design.md).

### 2. Crop-only is explicit and opt-in

Ordinary `full_frame_only` and `full_frame_and_crop` recording must remain
usable without context-image capture.

Selecting `crop_only` is an explicit acceptance that full-frame subject pixels
outside recorded crops will not be retained. The UI must state this before
arming and must show the resolved policy while recording.

The current `ORANGE_GUI_DIAGNOSTIC_NO_FULL_FRAME=1` path remains diagnostic and
must not silently become a production crop-only profile. Historical recordings
made through that flag are not retroactively crop-only compliant.

### 3. Crop media is required and fail-closed in crop-only mode

In `full_frame_and_crop`, crop output may retain its current optional-sidecar
failure semantics.

In `crop_only`, the crop output is required retained media. The recording must
finalize as `incomplete` if any of the following occurs:

- crop video missing or undecodable;
- crop metadata missing;
- encoded crop frame and metadata row counts differ;
- a crop frame is dropped by queue, pool, IPC, encoder, or writer pressure;
- crop frame identities are duplicated or reordered;
- a required recorder fails to drain or finalize;
- crop rectangle semantics are missing or incompatible; or
- context-image evidence required by the selected profile is missing or fails
  checksum validation.

Model no-detections are not transport drops. They remain explicit scientific
coverage gaps and are counted separately.

### 4. Context images are a recording product, not incidental calibration files

Define a new schema:

```text
schema_id = orange.recording.context_image_set
schema_version = 1
```

The product may reference compatible daily-registration evidence, but it must
not silently reinterpret a calibration capture as a reconstruction background.
Compatibility must be explicit and verified.

### 5. Reconstruction is deterministic and visibly synthetic

The normative reconstruction uses a hard rectangular replacement:

1. Decode the context background into `camera_native_px`.
2. Decode crop video frame `i`.
3. Read crop metadata row `i`.
4. If `crop_rect_valid = 1`, replace the exact background rectangle named by
   `crop_x,crop_y,crop_w,crop_h` with the decoded crop pixels.
5. If `crop_rect_valid = 0`, retain the background and mark the subject
   observation unavailable for that frame.

No feathering, interpolation, relighting, or content-aware fill is part of the
scientific reconstruction. A viewer may offer a separate cosmetic rendering,
but it must be labeled and must not replace the deterministic composite.

Every reconstructed frame should have an aligned availability state or mask:

- `recorded_crop_present`
- `no_detection_no_subject_pixels`
- `crop_transport_missing`
- `context_background_unavailable`

The output role is `synthetic_context_composite`. It is ineligible as a raw
full-frame source for detection, background subtraction, or claims about pixels
outside the recorded crop.

## Context Image Roles

### Required: `empty_arena_background_source`

A short burst of full-resolution frames captured with:

- the same camera serial and native raster as the recording;
- the same camera mount and lens state;
- the same exposure, gain, ADC/pixel format, binning, and orientation;
- the production filter state;
- the production illumination state;
- the dish, holder, water/fill, diffuser, and optical path in their recording
  configuration;
- an explicitly declared projector/stimulus state; and
- no subject present, confirmed by the operator.

The capture must use the live production acquisition configuration. It must not
invoke calibration preflight logic that changes exposure, frame rate, camera
triggering, filters, or illumination.

Recommended v1 default: 17 grouped source frames per camera. An odd count makes
the per-pixel median deterministic without defining an even-sample tie rule.
The count should remain configurable within a bounded range.

### Required: `empty_arena_background_derived`

A native-size `mono8` background computed as the per-pixel median of the
declared source image set.

The derived artifact must record:

- algorithm ID and version;
- source image identities and ordered SHA-256 digests;
- source count;
- input and output dtype;
- image dimensions and coordinate space; and
- output SHA-256.

Source frames must remain preserved. The derived median is not a replacement
for its evidence.

### Required: `recording_start_context`

At least one full-resolution frame captured after the subject is introduced
and immediately before recording begins, without changing the production
camera/illumination state.

This frame documents the actual starting scene. It is not an empty background
and must not be selected automatically for reconstruction.

### Optional: `recording_end_context`

One or more full-resolution frames captured after recording stops but before
the dish, camera, lens, filters, or illumination are changed. This documents
gross end-state changes and helps diagnose a displaced dish or altered optical
path.

### Optional future roles

- background sets for multiple stable projector states;
- an operator-approved replacement background;
- a subject-present context burst;
- a runtime background refresh after explicit review.

These roles require explicit identities. A background must never be selected
merely because it is the newest image near the recording time.

## Capture Synchronization And Freshness

Context capture should reuse Orange's grouped acquisition infrastructure but
must introduce a production-state capture recipe rather than a calibration
recipe.

Requirements:

- Capture all participating cameras as one grouped operation when PTP is
  enabled.
- Preserve per-image `recording_frame_id` when assigned, local frame ID,
  camera frame ID, camera timestamp, and host receive timestamp.
- Record the PTP/runtime synchronization evidence available for the group.
- Require a fresh frame after the context-capture request; do not relabel an
  earlier preview frame.
- Record the active camera stream epoch and camera configuration digest.
- Reject mixed native dimensions or pixel formats within one per-camera image
  set.
- Never pause, restart, or reconfigure a production stream merely to save the
  context image unless the workflow explicitly reports that lifecycle.

PTP simultaneity is useful evidence across cameras, even though an empty static
background does not depend on exact simultaneity.

## Proposed Artifact Layout

```text
<recording_folder>/
  recording_context_images/
    manifest.json
    cameras/
      Cam<serial>/
        context_image_set.json
        empty_arena_background/
          source_000.png
          source_001.png
          ...
          source_016.png
          median.png
        recording_start_context/
          source_000.png
        recording_end_context/
          source_000.png                 # optional
```

PNG is the recommended v1 container for native `mono8` context images because
it is lossless, common, and self-describing. The schema, rather than the file
extension, remains authoritative for pixel format and coordinate semantics.

`manifest.json` is the immutable recording-level collection. Each per-camera
`context_image_set.json` is bound to exactly one observation edge and lists
every file with relative path, role, byte count, and SHA-256.

Paths must be recording-relative, normalized, remain inside the recording
folder, and be rejected on symlink traversal during validation.

## Minimum Context Image Schema

Each per-camera image-set payload should include at least:

```json
{
  "schema_id": "orange.recording.context_image_set",
  "schema_version": 1,
  "status": "complete",
  "context_image_set_id": "...",
  "recording_id": "...",
  "observation_context_id": "...",
  "camera_id": "2010093",
  "source_camera_stream_id": "2010093",
  "arena_id": "arena_1",
  "coordinate_frame": {
    "id": "camera_native_px",
    "origin": "top_left",
    "x_axis": "right",
    "y_axis": "down",
    "width_px": 4512,
    "height_px": 4512
  },
  "capture_configuration": {
    "camera_config_digest": "sha256:...",
    "stream_epoch_id": "...",
    "pixel_format": "Mono8",
    "exposure_us": 50.0,
    "gain": 256,
    "filter_state": "installed",
    "illumination_state": "production",
    "projector_state": "declared_state_id"
  },
  "physical_state": {
    "dish_installed": true,
    "water_present": true,
    "holder_installed": true,
    "subject_present": false,
    "subject_presence_authority": "operator_confirmed"
  },
  "images": [],
  "derived_backgrounds": [],
  "geometry_binding": {
    "recording_geometry_manifest_sha256": "sha256:...",
    "dish_mask_artifact_sha256": "sha256:..."
  }
}
```

Unknown values remain explicit `null`/`unknown`; they must not be filled from
filename conventions or temporal proximity.

## Recording Manifest Integration

`recording_snapshot.json` and `recording_session.json` should expose:

- resolved per-observation-edge media policy;
- full-frame output status;
- crop output descriptor and required/optional state;
- context-image collection relative path and SHA-256;
- selected background ID and SHA-256;
- reconstruction eligibility status; and
- crop coverage and transport-completeness summaries.

Recommended crop-only descriptor semantics:

```json
{
  "media_policy": "crop_only",
  "source_stream_materialization": "selective_roi_only",
  "outputs": {
    "full": {
      "status": "omitted_by_policy",
      "required": false
    },
    "crop": {
      "status": "completed",
      "required": true,
      "role": "primary_retained_observation_subset",
      "lineage": "derived_from_canonical_acquisition_source"
    },
    "context_images": {
      "status": "complete",
      "required": true
    }
  },
  "reconstruction": {
    "status": "eligible",
    "product_role": "synthetic_context_composite"
  }
}
```

Do not omit the full output descriptor entirely. An explicit
`omitted_by_policy` state distinguishes intentional crop-only acquisition from a
lost or failed full-frame recorder.

## Crop Timeline And Coverage

The existing crop video and metadata shape is suitable for deterministic
placement when terminal parity is proven:

- one metadata row per encoded crop frame;
- `crop_video_frame_index` binds the row to the crop MP4 frame;
- `recording_frame_id` joins crop, detection, pose, and acquisition telemetry;
- `crop_x,y,w,h` define the actual clamped source rectangle; and
- `crop_rect_coordinate_space = full_frame_pixels` with
  `xywh_top_left` semantics defines placement.

Crop-only finalization must publish two separate summaries:

### Transport completeness

- frames offered;
- frames accepted;
- frames encoded;
- metadata rows;
- queue/pool/IPC/encode/write drops;
- first/last recording frame ID; and
- duplicate, reordering, or unexpected-gap counts.

Any transport loss makes the crop-only recording incomplete.

### Subject-observation coverage

- frames with detections;
- explicit no-detection frames;
- detection coverage fraction;
- longest consecutive no-detection interval;
- crop states and counts; and
- any future fallback/held-ROI states.

No-detection coverage may be scientifically undesirable, but it is not a
recorder transport failure. Recording policy may define a warning or acceptance
threshold without conflating it with artifact corruption.

## Miss Handling

V1 preserves the current explicit blank-frame policy:

- no detection produces a black crop frame;
- metadata records `has_detection = 0`, `blank_frame = 1`, and
  `crop_rect_valid = 0`; and
- reconstruction uses only the background and records
  `no_detection_no_subject_pixels`.

The workflow does not require a full-resolution sentinel frame every time the
detector misses. The subject is physically confined to the arena, and frequent
sentinel capture would undermine the storage and independence goals of
crop-only acquisition.

A later, separately validated continuity policy may record a crop at the last
valid or tracker-predicted ROI while `has_detection = 0`. Such a frame must use
a distinct state such as `held_last_valid_roi` and must not be mislabeled as a
model detection. This can reduce correlated detector/crop loss without
reintroducing full-frame media.

## Daily Registration Compatibility

Daily physical dish registration and daily projection registration are separate
products:

- Orange owns the camera-native physical inner-rim observation, dish mask, and
  centroid gate. This workflow must work without Citrus or a selected canvas.
- Citrus owns the optional translation that aligns its canonical projected
  arena and experimental area with today's physical dish.

When no projection canvas participates, projection registration is
`not_applicable` with reason `no_active_projection_canvas`; it is not missing
calibration. Crop-only reconstruction and live dish-mask gating depend on the
Orange physical product, not on a synthetic Citrus canvas selection.

One compatible production-state empty-dish source burst may feed both the
schema-v2 physical rim observation and the context-image background set. They
remain independently derived, accepted, checksummed artifacts. The full
implementation sequence and requiredness matrix are defined in
[`orange_standalone_daily_physical_registration_plan.md`](orange_standalone_daily_physical_registration_plan.md).

The accepted daily rim observation remains the geometry authority for the dish
mask and registered arena placement. Its source frame may be reused as an
`empty_arena_background_source` only when all of these are proven:

- exact camera and source stream identity match;
- native dimensions, pixel format, orientation, and binning match;
- camera mount/lens state is declared compatible;
- exposure, gain, filters, illumination, and projector state are compatible
  with the intended reconstruction background;
- dish, holder, water, and diffuser state match;
- the operator confirmed no subject was present;
- source bytes and declared checksums validate; and
- the recording context manifest explicitly selects that source.

Otherwise, keep the daily image as calibration evidence and capture a new
recording-context background. No heuristic fallback to the newest daily image
is allowed.

## Dish Mask Relationship

The recording-bound dish mask and context background serve different roles:

- the dish mask defines valid camera-space subject occupancy and model-input
  gating;
- the background supplies visual camera context outside the retained crop.

The context-image set should bind the selected dish-mask artifact by identity
and digest. Reconstruction may optionally display the boundary or mask invalid
regions, but must not silently alter crop pixels based on that mask.

## UI Workflow

Recommended guided flow:

1. Select participating cameras, YOLO, and crop recording.
2. Select `crop_only` media policy.
3. Orange displays the irreversible evidence warning: continuous full-frame
   pixels will not be retained.
4. With holder, dishes, water, filters, illumination, and projector in their
   production state and no subject present, the operator confirms
   **Capture Empty Backgrounds**.
5. Orange captures the grouped source bursts, derives median backgrounds, and
   validates image sets.
6. The operator introduces subjects without moving cameras or dishes.
7. Orange captures the grouped recording-start context.
8. Pre-arm validation checks geometry binding, context evidence, crop recorder
   readiness, storage, and media policy.
9. Recording starts. The UI shows `CROP ONLY` prominently.
10. Stop/drain/finalize requires crop media and metadata parity.
11. Optional end-context capture occurs before the physical setup changes.
12. The UI reports transport completeness and detection coverage separately.

The normal full-frame workflow does not require these steps unless the operator
asks for context images.

## Reconstruction Tool Contract

Add a read-only offline tool before enabling the production profile. It should:

- resolve paths only through `recording_session.json` and the context manifest;
- validate all referenced sizes and SHA-256 digests;
- validate crop MP4/CSV parity;
- decode the selected background and crop video;
- reconstruct a requested frame range deterministically;
- write an output manifest describing every source artifact and digest;
- emit a per-frame availability table;
- optionally write a preview MP4 or individual PNGs; and
- refuse to call the result a full-frame source or master.

The tool must not infer crop placement from detection boxes when canonical
`crop_x,y,w,h` exists.

## Validation Requirements

### Context-image validation

- schema ID/version/status valid;
- all paths relative, contained, and free of symlink traversal;
- byte counts and SHA-256 digests match;
- camera, source stream, observation context, arena, and recording identities
  match the parent recording;
- native raster and coordinate descriptor match the camera runtime snapshot;
- source frames have unique fresh frame identities;
- camera config digest and stream epoch are compatible;
- physical and optical state fields are present or explicitly unknown;
- background derivation reproduces the declared digest; and
- selected background belongs to the same validated image set.

### Crop-only recording validation

- resolved media policy is `crop_only`;
- full output is `omitted_by_policy`, not missing or failed;
- crop output and context images are required and complete;
- crop MP4 decodes at declared size/cadence;
- first keyframe and container metadata pass existing crop gates;
- crop MP4 frames equal metadata rows and terminal encoded count;
- zero queue, pool, IPC, encoder, writer, and finalization drops;
- recording frame IDs and crop video indices follow the declared continuity
  rules;
- all crop rectangles lie within the native raster;
- recording geometry and dish-mask bindings validate; and
- transport and detection-coverage summaries are present.

### Reconstruction validation

- known synthetic fixtures prove exact hard-placement behavior at all four
  image corners and along edges;
- blank/no-detection frames leave the background unchanged and emit the correct
  availability state;
- reflected homographies have no effect on camera-native crop placement;
- a crop reconstruction never applies Citrus presentation reflection;
- malformed dimensions, paths, digests, or row/frame parity fail closed; and
- output manifests clearly declare `synthetic_context_composite`.

## Historical Compatibility

- Existing recordings with full-frame video remain unchanged.
- Existing crop sidecars remain valid derived outputs under their historical
  contracts.
- A historical recording lacking a context-image set is
  `reconstruction_context_unavailable`; it is not silently paired with a nearby
  calibration image.
- A diagnostic no-full-frame recording may be inspected, but is not promoted
  to compliant crop-only status unless every required artifact and identity can
  be proven under a versioned migration procedure.
- Existing daily-registration and geometry assets are never rewritten.

## Non-Goals For V1

- Reconstructing the original full-frame sensor image.
- Saving full-resolution sentinel frames for every detector miss.
- Multi-subject crop sets.
- Multiple independently moving crops per camera frame.
- Background subtraction or replacement as a detector input.
- Cosmetic inpainting outside the crop.
- Treating reconstructed context frames as training truth outside the recorded
  crop.
- Changing Orange/Citrus coordinate conventions or applying presentation
  reflections to camera-native crop placement.

## Implementation Checklist

### Phase 0: Freeze schemas and terminology

- [ ] Add `orange.recording.context_image_set` schema v1.
- [ ] Add a recording-level context image collection schema.
- [ ] Define the persisted media-policy field and allowed values.
- [ ] Define `omitted_by_policy`, `required`, and reconstruction status values.
- [ ] Define transport-completeness and subject-coverage summaries.
- [ ] Update recording output and observation-binding documentation.

### Phase 1: Separate participation from retained output policy

- [ ] Add media-policy config parsing and validation.
- [ ] Persist the resolved policy per observation edge.
- [ ] Change GUI wording so camera participation does not imply a full MP4.
- [ ] Keep sink backend selection independent from media policy.
- [ ] Make the full-frame descriptor explicit `omitted_by_policy` in crop-only
      mode.
- [ ] Keep existing full-frame defaults unchanged.

### Phase 2: Context capture and derivation

- [ ] Add a production-state grouped context-capture recipe.
- [ ] Capture fresh native-resolution source bursts without calibration-state
      camera reconfiguration.
- [ ] Preserve source frame identities, timestamps, config digest, and stream
      epoch.
- [ ] Write immutable per-camera image sets and collection manifest.
- [ ] Implement deterministic `median_u8` background derivation.
- [ ] Add recording-start context capture.
- [ ] Add optional recording-end context capture.
- [ ] Apply safe file ownership and atomic publication conventions.

### Phase 3: Crop-only lifecycle semantics

- [ ] Make crop output required when policy is `crop_only`.
- [ ] Make crop transport drops/finalization failures fail the recording.
- [ ] Keep no-detection frames distinct from transport loss.
- [ ] Persist detection coverage and longest miss interval.
- [ ] Update external crop recorder supervision and finalization.
- [ ] Update storage preflight using crop bitrate plus bounded context-image
      storage rather than full-frame bitrate.

### Phase 4: Manifest and geometry integration

- [ ] Bind context image sets to `observation_context_id`.
- [ ] Bind the selected recording geometry and dish-mask digests.
- [ ] Project context-image references into `recording_snapshot.json` and
      `recording_session.json`.
- [ ] Add explicit compatible reuse of daily-registration source evidence.
- [ ] Reject heuristic or temporal-nearest background selection.

### Phase 5: Reconstruction and validation tooling

- [ ] Implement the read-only deterministic reconstruction tool.
- [ ] Emit per-frame availability records and an output provenance manifest.
- [ ] Add context-image and crop-only session validators.
- [ ] Add synthetic corner/edge/no-detection fixtures.
- [ ] Add path containment, symlink, digest, and dimension failure tests.
- [ ] Add historical `reconstruction_context_unavailable` coverage.

### Phase 6: Live acceptance

- [ ] One-camera short run with a detectable subject.
- [ ] Four-camera PTP grouped context capture.
- [ ] Four-camera crop-only recording with zero acquisition and crop transport
      drops.
- [ ] Decode and reconstruct sampled frames from every camera.
- [ ] Visually confirm crop placement against recording-start context.
- [ ] Verify daily mask, crop rectangle, and background share the same native
      camera coordinate frame.
- [ ] Measure storage rate and crop recorder queue pressure.
- [ ] Document detection miss coverage separately from recorder completeness.
- [ ] Promote a production profile only after these gates pass.

## Recommended First Implementation Slice

Implement the contract and capture product before changing the recording
backend:

1. add the context-image schemas and immutable writer;
2. add production-state grouped empty-background and start-context capture;
3. add validation and recording-manifest references; and
4. implement the offline reconstructor against existing crop artifacts.

This proves that the retained crop plus context evidence is sufficient and
honestly represented. Only then should Orange promote `crop_only` from the
existing diagnostic no-full-frame machinery into a production media policy.
