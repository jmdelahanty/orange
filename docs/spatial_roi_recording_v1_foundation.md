# Spatial ROI Recording v1 Foundation

**Date:** 2026-08-30

**Status:** experimental extraction foundation; default off and not connected
to acquisition, recorder processes, session finalization, or deployment.

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
  `src/spatial_roi_batch_producer.*`.

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

## Validation completed

- strict config/plan parsing, normalization, digest binding, bounds, overlap,
  naming, recording-token, admission, malformed-input, and mutation tests;
- SHAMAN-independent host geometry/identity validation;
- four simultaneous device-to-device ROI copies on an RTX A6000;
- exact decoded content pixels and explicit zero padding;
- bounded pool exhaustion/reuse and source-lease lifetime; and
- `StopAccepting()` linearization against concurrent production.

## Required next slice

Before this can produce usable scientific recordings, Orange still needs:

- a recording-cadence `WORKER_ENTRY` integration that supplies the authoritative
  source allocation/event lease without blocking acquisition;
- one bounded logical recorder/IPC lane per required ROI;
- native lossless Mono8 media plus per-frame identity metadata;
- output descriptor collections, counters, summaries, portable manifests,
  rolling/session finalization, file size, and SHA-256 evidence;
- storage, codec, encoder/session, and free-space preflight at arming; and
- one-camera/four-ROI followed by four-camera load and failure validation.

Authoritative full-frame recording must remain enabled until those acceptance
gates pass. This branch must not be merged or deployed on the strength of the
extraction test alone.
