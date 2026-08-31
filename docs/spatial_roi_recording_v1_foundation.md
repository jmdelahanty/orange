# Spatial ROI Recording v1 Foundation

**Date:** 2026-08-30

**Last updated:** 2026-08-31

**Status:** extraction foundation complete in Orange commit `c423ad5`; the
frame-contract, strict recorder-plan materializer, and bounded per-ROI runtime
foundation are implemented on
`agent/acquisition/spatial-roi-recording-v1-20260830`. The feature remains
default-off and is not connected to acquisition, the existing external
recorder supervisor/protocol, session finalization, or deployment. It does not
yet produce ROI video files.

This document is the canonical status and handoff for the detector-independent
spatial ROI path. It must not be read as claiming that the existing scalar,
YOLO-driven `Cam<serial>_crop` output is a spatial ROI stream.

## Scope

This slice establishes the detector-independent front half of continuous
camera-native spatial ROI recording. It does not change the legacy top-one crop
pipeline and does not make an ROI video.

Implemented components:

- closed configuration schema:
  `docs/schemas/orange_spatial_roi_recording_config.schema.json`;
- closed, canonical-digest-bound plan schema:
  `docs/schemas/orange_spatial_roi_recording_plan.schema.json`;
- strict config parsing, deterministic plan materialization, identity and
  geometry validation, and aggregate resource admission in
  `src/session/spatial_roi_recording_config.*`; and
- a bounded native-Mono8 CUDA batch extractor in
  `src/spatial_roi_batch_producer.*`;
- a closed, collision-checked per-ROI frame/metadata descriptor in
  `src/spatial_roi_frame_contract.*`;
- a verified-plan-only recorder contract materializer in
  `src/session/spatial_roi_recorder_contract.*`; and
- one strictly bounded worker lane per verified ROI, with shared batch/source
  lifetime and explicit terminal outcomes, in
  `src/spatial_roi_recording_runtime.*`.

The plan binds the parent recording identity/token, producer generation,
camera ID and serial, native raster, layout/materialization/registration
authority references, ordered ROI descriptors, exact encoded rasters and
padding, pool depth, and admitted pool bytes. A producer can only be
constructed from limits minted by a verified plan. Mutating those public
limits after verification is rejected.

## Batch contract

One accepted batch:

1. retains one opaque source allocation/event lease;
2. waits once for the source-ready CUDA event;
3. zero-fills each admitted output raster;
4. copies every exact camera-native ROI by device-to-device `cudaMemcpy2DAsync`
   without resize or color conversion;
5. records one completion event after every output operation; and
6. retains the source lease and output slot until completion is observed.

Pools are preallocated from the verified plan and admission is nonblocking.
Pool exhaustion, stopped admission, invalid work, and CUDA faults have distinct
statuses. If a failed asynchronous submission cannot be proven complete by a
stream synchronization, the complete pool state and source lease are
quarantined for process lifetime rather than risking use-after-free.

The runtime submits one verified-order batch, fans its immutable envelope to
independently bounded ROI lanes, and never waits for another submitter or a
lane queue. Schema v1 is always strict: any queue rejection, missing sink,
sink failure, or CUDA completion failure makes the spatial-ROI batch
incomplete. Duplicate or out-of-order recording-frame identities are rejected
before production, and fatal producer states remain latched in the runtime. A
completed lane means the handoff reached a source-safe boundary; it is not
session-finalization proof.

The new recorder contract is intentionally a separate
`orange.spatial_roi_recording.external_recorder_contract` schema. The current
`orange.external_recorder.contract` supervisor and positional v1 frame
protocol cannot carry the required ROI identity and must reject it. Do not
relabel or feed this object to that parser. The next integration must add an
ROI-aware supervisor/protocol consumer before enabling the runtime.

## Validation completed

- strict config/plan parsing, normalization, digest binding, bounds, overlap,
  naming, recording-token, admission, malformed-input, and mutation tests;
- SHAMAN-independent host geometry/identity validation;
- four simultaneous device-to-device ROI copies on an RTX A6000;
- exact decoded content pixels and explicit zero padding;
- bounded pool exhaustion/reuse and source-lease lifetime; and
- `StopAccepting()` linearization against concurrent production;
- closed frame/metadata JSON round trips, canonical stream naming, origin-
  anchored encoded geometry, and duplicate identity rejection;
- exact verified-plan-to-recorder materialization, GPU-map coverage, HEVC
  lossless/GOP-1 policy, queue-depth propagation, and odd-NV12 rejection;
- four-lane CUDA fanout, exact outstanding-capacity enforcement, queue-full,
  sink-rejection/failure, missing-sink, and re-entrant-stop behavior; and
- production Orange GUI and headless builds with the new foundation linked.

## Next slice: one-camera, four-ROI recorder integration

The next implementation slice is one end-to-end, detector-independent recorder
path for a single camera with one accepted four-region plan. It must produce
four independently addressable native-resolution lossless ROI streams while
retaining the authoritative full-frame stream. YOLO, preview, pose, packed
atlases, crop-only media policy, and detection-centered routing are out of
scope.

The schema and runtime remain general over a verified plan's admitted ROI
count so future layouts do not require a second ABI. Activation and acceptance
are nevertheless gated to one camera/four ROIs for this slice.

### A. Acquisition seam and batch submission

- [ ] At recording cadence, obtain the authoritative `WORKER_ENTRY` source
      allocation/event lease and construct one `SpatialRoiSourceView` for the
      camera; do not read pixels through YOLO or the legacy top-one crop path.
- [ ] Submit exactly one `TryProduce()` batch per eligible source frame, with
      all plan ROIs in verified order and the complete source identity copied to
      every work item.
- [x] Make runtime batch/lane admission nonblocking and expose separate busy,
      stopped, pool-empty, invalid, duplicate/out-of-order, CUDA-error,
      queue-full/queue-admission-failure, and source-quarantine outcomes.
      Acquisition-side per-frame persistence remains pending.
- [x] Release the source lease only after the batch completion fence; on an
      unprovable completion, preserve the producer quarantine behavior and never
      recycle or reuse the allocation speculatively.
- [ ] Prove that ROI work cannot delay acquisition, YOLO, display, or the
      authoritative full-frame recorder. In strict mode, an admitted ROI loss
      makes the spatial-ROI product incomplete rather than silently shortening
      its frame range.

### B. Per-ROI recorder/IPC lanes

- [x] Define and validate a closed handoff descriptor with explicit `roi_id`,
      `region_id`,
      `logical_stream_id`, plan digest, native content rectangle, encoded raster,
      and ROI-local frame index. `recording_frame_id` alone is not a sufficient
      key when four ROI descriptors share one source frame.
- [ ] Carry that descriptor on an ROI-aware supervisor/IPC protocol; the
      existing positional protocol remains deliberately unchanged.
- [x] Give each required ROI an independently bounded runtime queue and
      terminal state. A lane may fail or drop only its own sidecar; it must not
      relabel another ROI. Connecting those lanes to recorder processes and
      full-frame coexistence remains pending.
- [ ] Encode the exact Mono8 pixels with the validated lossless profile. Keep
      alignment padding explicit and zero-filled; no scaling or color
      conversion is permitted.
- [ ] Define and validate ACK/release identity as `(camera_serial, roi_id,
      recording_frame_id, roi_stream_frame_index)` or an equivalent collision-
      free key before using an existing external recorder transport.
- [ ] Keep recorder encode/mux/disk work outside the acquisition process. The
      source-safe boundary is an accepted detached copy or an explicit
      recorder `RELEASE`, never encode completion inferred by Orange.

### C. Artifacts, arming, and finalization

- [ ] Replace the scalar `recording_outputs[serial].crop` assumption with a
      collection keyed by stable ROI/logical stream identity while retaining
      compatibility aliases for the legacy top-one crop output.
- [ ] Write one frame metadata row per accepted ROI frame, including source
      identity, ROI identity, native content rectangle, encoded raster/padding,
      and submission outcome. Accepted video indices advance only on queue
      admission.
- [ ] Add per-ROI descriptors, counters, summaries, rolling/session manifests,
      portable relative paths, final status, byte sizes, packet/frame counts,
      and SHA-256 evidence. Finalization must fail closed on range or identity
      disagreement.
- [ ] Add arming preflight for camera/raster/layout/registration/codec,
      aggregate pixel rate, encoder/session count, pool/queue bytes, storage,
      and reserved free space. Invalid or over-budget plans must not start.

### D. Acceptance and safety gates

- [ ] Validate one camera/four ROI at the declared cadence with YOLO disabled
      and preview/pose disabled; compare every decoded content pixel to the
      source fixture and verify explicit padding.
- [ ] Inject one ROI queue/recorder/finalization failure and verify that the
      other ROI lanes and full-frame recording remain correctly identified and
      bounded.
- [ ] Validate counts, frame identity joins, container/keyframe correctness,
      finalization, file size, and digest evidence before increasing camera or
      ROI count.
- [ ] Repeat at four cameras only after the one-camera acceptance artifact is
      complete, then measure aggregate GPU/NVENC/storage headroom.

The following boundaries are mandatory throughout the slice:

- The configuration stays strict, versioned, optional, and default-off.
- Full-frame recording remains ingest-authoritative and required; no crop-only
  or replacement-authority mode is introduced.
- Stable spatial `roi_id`/`region_id` comes from the verified plan. Detector
  order, confidence, `track_id`, and `fish_id` must not name an ROI stream.
- No atlas/packing fallback is introduced until standalone ROI correctness is
  proven; no pose or detection fanout is coupled to ROI availability.
- Queue/pool pressure is bounded and observable. Acquisition must not wait for
  encode, mux, disk I/O, or finalization.
- Merge/deployment remains blocked until the acceptance gates above and the
  corresponding Citrus media-reference checks pass.

Authoritative full-frame recording must remain enabled until those acceptance
gates pass. The extraction tests alone do not authorize merge or deployment.
