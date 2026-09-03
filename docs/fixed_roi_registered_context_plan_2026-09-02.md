# Fixed-ROI Recording With Registered Scene Context

Date: 2026-09-02

Status: implementation in progress; opt-in and default-off. The release build,
focused unit tests, static specification validation, and headless parse/resolve
dry run pass. A camera-independent real CUDA/NVENC test also passed 51 active
P1/GOP-25 frames with IDRs at 0, 25, and 50. Live camera 2010093 has now
sustained four 2256x2256 streams at 100 FPS through capture, detach, encode,
mux, and decoder setup. Final content acceptance remains pending because the
2026-09-02 source scene was knowingly unilluminated and correctly failed the
fixed luma-sanity threshold.

## Outcome

Add an opt-in recording product that retains continuous fixed-region ROI
videos and one recording-bound, camera-native registered scene context instead
of retaining a second continuous full-frame video.

This is a third first-class media policy. It does not replace either existing
full-frame mode:

| Media policy | Continuous full frame | Continuous fixed ROIs | Registered context |
| --- | --- | --- | --- |
| `full_frame_only` | required | disabled | not retained |
| `full_frame_and_fixed_rois` | required | required | not retained |
| `fixed_rois_with_registered_context` | omitted by policy | required | required |

Media policy and recorder backend remain separate decisions. In particular,
omitting a full-frame product must not be represented by pretending that a
full-frame recorder failed or by relabeling ROI media as a raw full-frame
recording.

## Current Implementation Status

The first executable slice now exists on this branch:

- the closed `fixed_rois_with_registered_context` policy is parsed, resolved,
  conflict-checked, and persisted separately from recorder backend selection;
- headless acquisition uses `immediate_recycle` for the primary full-raster
  frame after ROI handoff, preserving camera participation and recording-frame
  identity without starting a continuous full-frame encoder or writer;
- one retained acquisition frame is copied off the hot path and published as
  exact native Mono8 bytes plus a closed, SHA-256-bound context descriptor;
- the context descriptor binds the recording, camera, source frame, resolved
  camera configuration, spatial authorities, and an explicit capture
  declaration;
- each configured fixed-ROI stream (four in the current diagnostic) retains
  its normal independent encoder, journal, receipt, and finalization evidence;
- recording snapshots and the finalized session manifest explicitly omit the
  full-frame product by policy instead of inventing a failed or placeholder
  `Cam*.mp4` product; and
- ROI-only acquisition identity is derived from authenticated finalized ROI
  receipt coverage. The historical full-frame acquisition mapping is recorded
  as `not_applicable_by_media_policy`.

Registration acceptance is never inferred merely from a layout ID or digest.
The capture declaration must explicitly choose
`accepted_for_experiment` or `diagnostic_not_physical_acceptance`. The current
2010093 diagnostic specification uses the latter and therefore cannot be
misrepresented as an experimentally accepted registration.

Still outstanding are the deterministic reconstruction/availability-mask
tool, a process-level no-full-frame lifecycle test, illuminated live content
acceptance, and concurrent four-camera validation. The offline acceptance
verifier now checks the ROI-only policy,
context bytes/digest/authority, exact ROI bindings, and profile-derived GOP
evidence. The current
`immediate_recycle` accounting also reuses the primary routed-frame counter;
an explicit `immediate_recycled_frames` telemetry counter remains a useful
nonblocking follow-up.

### Cached raw-detach refactor

The 2026-09-02 recorder refactor changes the RELEASE-critical path without
changing the wire schema, media policy, geometry, or profile identity:

- each recorder stream opens a bounded, session-scoped cache of authenticated
  CUDA IPC memory/event pairs instead of opening and closing both handles for
  every frame;
- cache cardinality is bounded by the pre-admitted detach-slot count, handle
  collisions fail closed, and cache cleanup is required after encoder drain;
- detach waits for the producer event, copies only packed Mono8 into a
  recorder-owned slot, waits for that copy, and can then return RELEASE;
- the encoder owner copies that Mono8 directly into the Y plane of a
  pre-neutralized NVENC input surface. The generic recorder-owned NV12 input
  path remains supported;
- the existing per-slot NV12 scratch allocation remains reserved but unused in
  this slice so the published v1 resource/budget contract is not silently
  changed; and
- producer batch-pool allocations/events remain alive after transport EOF and
  are released only after the recorder child is definitively reaped. An
  unreaped abnormal child causes bounded fail-closed retention rather than a
  use-after-free beneath cached mappings.

The detach counters now distinguish unique imports from cache hits/misses and
record cache cardinality/cleanup. They are covered by focused real-driver
tests, but they are not yet projected into the terminal per-stream perf
sidecar; that metadata extension remains required before scaled acceptance.

## Registered Context Semantics

The context is not an image of an empty rig. It represents the registered
production scene after:

- the dishes, holders, water, and other fixed apparatus are in their intended
  positions;
- daily layout/materialization/registration has been accepted;
- the production camera raster, pixel format, exposure, gain, focus, and lens
  state are active; and
- the production NIR illumination is active and stable.

The current cameras see the NIR acquisition scene. Citrus projector content is
not visible in this camera modality, so projector state does not require a new
context image. The LEDs and rig are expected not to change during a recording;
such a change is an experimental fault rather than a supported dynamic context
state.

The first implementation captures one exact native-resolution Mono8 source
frame per camera. A later version may add a bounded burst or a deterministic
median, but it must preserve the original source frames and declare the
derivation. Capturing before subject introduction is preferred because it
avoids a static subject image in unobserved areas. If a subject is present, the
context metadata must say so rather than silently claiming an empty background.

The context validity is bound to the recording and the exact accepted daily
registration authorities. It becomes invalid if camera configuration or any
layout/materialization/registration digest changes. Reuse across recordings is
deferred; v1 captures and binds a fresh context for each recording.

## Data Flow

```text
one camera-native acquisition frame stream
  +-- one-shot registered context capture (one native Mono8 frame)
  +-- fixed ROI extraction for every admitted recording frame
        +-- one independently finalized video/evidence stream per ROI
```

No continuous full-frame encoder, writer, split-GOP router, or full-frame media
artifact is started for `fixed_rois_with_registered_context`. Camera
participation, recording/session identity, frame identity, timing, analytics,
and ROI extraction remain active.

For the current four diagnostic quadrants, the ROI content rectangles tile the
4512x4512 camera raster. They can therefore be mosaicked directly when all four
frame identities are present. For smaller or non-tiling physical regions, the
registered context supplies only the static pixels outside retained ROI
content.

### Current artifact layout

The recording root contains the recording/session snapshots and the context:

```text
registered_scene_context.mono8
registered_scene_context.json
external_spatial_roi_recorder/
  Cam<camera_serial>_spatial_roi_<roi_id>.mp4
  Cam<camera_serial>_spatial_roi_<roi_id>_meta.csv
  Cam<camera_serial>_spatial_roi_<roi_id>_keyframe.json
  Cam<camera_serial>_spatial_roi_<roi_id>_perf.csv
  Cam<camera_serial>_spatial_roi_<roi_id>_summary.json
  Cam<camera_serial>_spatial_roi_<roi_id>_status.json
  Cam<camera_serial>_spatial_roi_<roi_id>_video_sanity.json
  Cam<camera_serial>_spatial_roi_<roi_id>.mp4.finalization.json
  Cam<camera_serial>_spatial_roi_<roi_id>_transport.jsonl
  Cam<camera_serial>_spatial_roi_<roi_id>_evidence.jsonl
  Cam<camera_serial>_spatial_roi_<roi_id>_evidence_manifest.json
  Cam<camera_serial>_spatial_roi_<roi_id>_recorder.log
```

There is no root-level `Cam<serial>.mp4`, full-frame metadata CSV, or synthetic
full-frame output descriptor in this policy. The exact configured `roi_id`,
camera serial, relative paths, geometry, encoder profile, and artifact receipts
are authenticated by the resolved plan/contract and finalized session
metadata; filenames are not used as the sole identity proof.

## Per-Experiment Analytics And Future Subject Crops

For grouped Citrus experiments, the fixed region is the preferred independently
processable scientific unit. Each region video is independently decodable and
can be queued to downstream tracking, pose, segmentation, or behavioral
analysis without decoding an unrelated 20 MP camera frame. The camera recording
session remains the parent acquisition unit: it proves the shared camera state,
frame timeline, registration, and aggregate completion across all region
products.

This division is intentional:

- `region_id` identifies the stable physical compartment and Citrus experiment;
- `roi_id` identifies a particular pixel product for that region;
- `recording_frame_id` and the recording identity join sibling region products
  back into the parent camera timeline; and
- the geometry/materialization/registration digests place every local pixel in
  camera-native and physical coordinates for batching or later visualization.

The current 2010093 fixed-region diagnostic uses P1/VBR Q20/GOP-25. It is not
lossless. Cropping one of those decoded videos and encoding the crop losslessly
prevents another generation of loss, but cannot restore pixel detail already
changed by the fixed-region encode.

That named profile also fixes AQ off, temporal AQ off, lookahead off, and
lookahead depth zero. The values are repeated through config/plan, contract,
session/evidence records, and MP4 metadata and are applied as explicit NVENC
overrides. They are not inherited from the P1/VBR preset defaults.

GOP-1 was retained as an immutable compatibility and diagnostic profile because
an all-IDR stream makes every frame independently seekable and simplified the
first media/evidence proof. It is not the active long-running ROI policy.
GOP-25 permits inter-frame compression while keeping deterministic IDRs at
zero-based frames 0, 25, 50, and so on. At 100 FPS this bounds the GOP and random-
access interval to 250 ms. Every ROI file remains independently decodable; only
the frames inside that file are interdependent within each bounded GOP.

The current C++ type and filenames still contain `lossless_encoder` because the
first profile was lossless. That source-level name is historical; the closed
profile identity and recorded metadata are authoritative. Renaming the mature
API is deferred separately from changing the on-disk media contract.

A future `subject_follow` product therefore branches from the original acquired
Mono8 frame before the fixed-region P1 encode. It is separately optional per
region and owns an independently decodable lossless encoder stream. Each frame
must bind the stable `region_id`/subject-product `roi_id`, pixel-source frame,
routing observation, selected detection, exact moving camera-native source
rectangle, padding, hold/expiry state, and terminal crop outcome. Detector
vector order and a transient technical track ID never name the scientific
subject stream.

“Direct from source” describes pixel provenance, not automatic same-frame
detection correspondence. The first bounded mode remains `latest_causal`: the
latest accepted region target applies only to subsequent acquired frames and
records its observation age. Exact detection-frame crops require an explicit
bounded source-frame cache and a later `buffered_exact` policy.

The fixed-region stream remains the preferred primary record because it retains
local behavioral context and continues through `not_scheduled`, pending,
zero-detection, and detector-failure states. A subject-follow stream is an
analytics-acceleration product, not a substitute that allows detection misses
to erase the experiment. Enabling both products increases encoder-session and
storage demand, so it requires its own closed media policy, resource admission,
telemetry, and finalization evidence rather than being an implicit side effect
of fixed-region recording.

## Context Artifact Contract

The v1 context product must bind at least:

- schema ID/version and role `registered_scene_context`;
- recording ID, session ID, recording identity token, and producer generation;
- camera ID, serial, native raster, coordinate space, and `mono8` pixel format;
- local frame ID, camera frame ID, recording frame ID, camera timestamp, and
  host/system timestamp;
- exact layout, materialization, and registration IDs and SHA-256 digests;
- the applied camera-configuration digest;
- assertions that the production NIR illumination and registered rig state are
  fixed for the recording;
- subject-presence declaration;
- artifact relative path, byte size, and SHA-256; and
- capture/finalization status and failure reason.

The pixel artifact is an exact, lossless native Mono8 frame. A viewable PNG or
PGM may be derived later, but the derived file must not replace the exact source
artifact or its digest.

## Reconstruction Contract

Offline reconstruction is deterministic:

1. Create the camera-native canvas from the registered context.
2. Join every required ROI by exact source recording-frame identity, not merely
   decoder frame number or wall-clock proximity.
3. Copy only the ROI's declared encoded content rectangle into its declared
   camera-native content rectangle; padding is never pasted into the scene.
4. Mark every output pixel/frame as contemporaneous ROI content or static
   registered context.
5. Mark missing ROI observations explicitly. Never conceal a drop by leaving
   the context visible without an availability indication.

The result is `synthetic_registered_context_composite`. It is a visualization,
not a recovered sensor full frame, and is ineligible as raw full-frame evidence
for detection, motion analysis, or claims about pixels outside retained ROIs.

## Completion And Failure Semantics

The ROI-only recording completes only when:

- the context artifact is present, exact-size, hashed, and bound to the same
  recording/camera/registration authorities;
- every required fixed ROI stream reached clean EOF and finalized;
- every admitted source frame has an explicit per-ROI terminal outcome;
- all required ROI media/evidence cardinalities and frame identities agree;
- no queue, IPC, detach, encoder, writer, or journal overflow occurred; and
- the session manifest declares that continuous full-frame media was omitted by
  the selected policy, not missing because of failure.

A context failure or required ROI failure makes the aggregate recording
incomplete. No empty placeholder context or full-frame descriptor is minted.

## Performance Position

Four 2256x2256 ROI streams at 100 FPS represent approximately one
4512x4512-at-100-FPS pixel workload in aggregate, plus independent-session
overhead. Removing the continuous full-frame product avoids approximately the
second full-raster encode workload. The one-time native context copy/write is
negligible relative to a continuous 100 FPS encoder, but its latency and bytes
must still be reported.

Per-stream encoder telemetry and per-GPU NVENC utilization remain required for
scaled configurations. Explicit runtime GPU placement is operational metadata,
not part of the scientific geometry policy.

### 2026-09-02 two-GPU throughput evidence

Camera 2010093 was run at 4512x4512 Mono8 and 100 FPS with four continuous
2256x2256 P1/VBR-Q20/GOP-25 ROI streams balanced two each across its production
PIX pair, GPUs 3 and 4. This is distribution of independent ROI videos; an ROI
is not itself split-GOP-sharded. The topology is two encoder GPUs assigned to
each camera, with that camera's four ROI streams placed 2+2 across its pair;
it is not two encoder GPUs shared by all four cameras.

Artifact root:

```text
/home/jeremy/orange_data/exp/unsorted/2010093_spatial_roi_cached_raw_detach_two_gpu_pair_100fps_v1/run_0001__codec_hevc__preset_p1__tuning_ll__rc_vbr__q_20__gop_25__aq_off__tempaq_off__lookahead_off__lookdepth_0__imappx_258
```

- acquisition received 601 frames at 99.849 FPS with zero camera frame-ID
  gaps, get-frame errors, preprocess drops, and encode failures;
- every ROI MP4 contains exactly 600 frames at 2256x2256 and 100 FPS, and every
  ROI metadata CSV contains exactly 600 rows;
- steady encoder utilization was approximately 58% on each GPU (observed peak
  58%); steady SM utilization was approximately 14% on source GPU 3 and 20% on
  helper GPU 4; and
- the descriptor-bound decoder accepted the files after its allocation guard
  was corrected to allow only the bounded HEVC/FFmpeg coded-raster alignment
  envelope while retaining exact 2256x2256 visible-raster checks.

The registered source context had mean luma 2.01/255 because the acquisition
lights were not powered. The same per-quadrant values appeared in the ROI
videos, which supports pixel-path correctness but deliberately fails the
content-validity gate. Dark content also understates bitrate/disk pressure, so
this result supports the two-GPUs-per-camera topology but does not replace an
illuminated run, longer soak, or concurrent four-camera acceptance.

At equal quadrants, two ROI streams per GPU have the same average pixel rate as
the established two-way full-frame split-GOP workload:

```text
2 * 2256 * 2256 * 100 = 1,017,907,200 luma pixels/s/GPU
4512 * 4512 * 50      = 1,017,907,200 luma pixels/s/GPU
```

The proposed four-camera placement therefore preserves the existing disjoint
PIX pairs: 2010093 -> 3/4, 2010094 -> 1/2, 2010095 -> 7/8, and 2010096 -> 5/6.
It still requires an illuminated four-camera validation because independent
session overhead, content-dependent bitrate, writer/storage pressure, CPU
threading, and simultaneous acquisition traffic are not proven by one camera.

## Implementation Checklist

### Contract and configuration

- [x] Add a closed, versioned media-policy schema with the three policies above.
- [x] Preserve existing behavior when no new policy is configured.
- [x] Require enabled fixed ROI configuration for
      `fixed_rois_with_registered_context`.
- [x] Reject full-frame-only controls that conflict with ROI-only policy.
- [x] Persist resolved media policy in run, session, and completion metadata.

### Registered context

- [x] Add a closed registered-context descriptor and receipt schema.
- [x] Add a bounded one-shot native Mono8 capture path after an explicit
      registration-authority declaration and before the recording can
      complete; production use requires `accepted_for_experiment`, while a
      diagnostic capture remains explicitly non-accepted.
- [x] Use descriptor-bound artifact creation and exact size/SHA-256 evidence.
- [x] Bind the source frame and all daily registration/camera authorities.
- [x] Expose capture latency, bytes, and terminal status outside the acquisition
      hot path.

### ROI-only lifecycle

- [x] Keep camera participation and recording-frame identities active without a
      continuous full-frame sink.
- [x] Start and require the spatial ROI camera recorder without starting the
      external full-frame recorder.
- [x] Remove combined-storage assumptions from ROI-only preflight while keeping
      ROI storage reservation strict.
- [x] Stop, drain, and finalize every ROI stream normally.
- [x] Cache authenticated CUDA IPC imports for the bounded recorder session,
      make completed raw Mono8 copy the source-RELEASE boundary, and close the
      cache only after encoder drain.
- [x] Retain producer CUDA pool/export resources until definitive recorder
      reap on normal and abort paths.
- [x] Do not require or invent full-frame media artifacts in ROI-only completion.

### Evidence and reconstruction

- [x] Extend session outputs and finalized receipts with context authority and
      explicit full-frame omission-by-policy.
- [x] Extend the offline validator with ROI-only policy, context checksum, and
      ROI/context identity checks.
- [ ] Add a deterministic reconstruction tool that joins exact frame identities
      and emits an availability mask/sidecar.

### Optional direct-from-source subject crops

- [ ] Add a separate closed `subject_follow` media/product policy; do not
      mutate a `fixed_region` stream into a moving crop.
- [ ] Crop from the original acquired Mono8 source before the fixed-region
      lossy encode and use an independently finalized lossless encoder.
- [ ] Bind stable region/product identity, source frame, routing observation,
      selected detection, moving rectangle, padding, hold/expiry state, and
      terminal outcome for every declared cadence position.
- [ ] Keep fixed-region recording detector-independent and require explicit
      storage/GPU/queue admission when both products are enabled.

### Validation

- [x] Unit-test closed media-policy parsing and conflict rejection.
- [x] Unit-test context descriptor validation, exact bytes, digest substitution,
      and authority mismatch rejection.
- [ ] Unit-test ROI-only startup/finalization without a full-frame child.
- [x] Dry-run the 2010093 four-quadrant P1 profile with explicit ROI GPU mapping.
- [x] Run the 2010093 four-quadrant throughput path at 100 FPS with two ROI
      streams per member of GPU pair 3/4; prove four 600-frame videos/metadata
      streams and zero acquisition/encode drops. Content acceptance is not
      claimed because illumination was intentionally unavailable.
- [ ] Live-run one camera at 100 FPS and prove: context complete, four ROI
      streams complete, no continuous full-frame process/artifact, no drops,
      bounded queue/latency telemetry, and deterministic mosaic reconstruction.
- [ ] Repeat with powered production illumination, then run all four cameras
      concurrently on their disjoint two-GPU PIX pairs.
- [ ] Persist cache/import, detach/raw-copy, encoder-owner copy, queue
      high-water, and latency counters in the versioned per-stream terminal
      performance metadata.
- [x] Preserve regression coverage for full-frame-only and combined modes.

## Existing Components To Reuse Carefully

- `pre_encoder_reference_capture` already demonstrates bounded reference-frame
  copying and evidence, but it is benchmark-oriented, captures prepared NV12,
  and is unavailable with external IPC. Its implementation concepts may be
  reused; it is not itself the registered context product.
- Spatial ROI config/plan/contract already binds fixed camera-native geometry,
  recording identity, exact ROI artifacts, and per-stream GPU placement.
- Spatial ROI frame journals and operational snapshots already provide the
  required drop/queue accounting foundation.
- The continuous full-frame external recorder remains a supported first-class
  product and must not be described as legacy.
