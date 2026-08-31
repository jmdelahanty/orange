# Spatial ROI Recording v1 Foundation

**Date:** 2026-08-30

**Last updated:** 2026-08-31

**Status:** extraction foundation complete in Orange commit `c423ad5` and the
acquisition/IPC export foundation committed through `d895d60`; the current
branch also implements an exact HELLO/FRAME/ACK/RELEASE handoff owner and
explicit drain/finalize wire states. The frame contract, strict recorder-plan
materializer and plan-bound consumer parser, bounded per-ROI runtime,
acquisition ownership bridge/controller, dedicated ROI IPC-v2 grammar,
CUDA-IPC frame exporter, and adopt-only bounded Unix-socket line transport are
implemented on `agent/acquisition/spatial-roi-recording-v1-20260830`. The
feature remains default-off and has no production arming caller, socket
listener/connector, recorder process/import implementation, child supervisor,
or operational drain/finalize exchange. It is not connected to session
finalization or deployment and does not yet produce ROI video files.

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
- a recorder-side parser that accepts only the deterministic contract rebuilt
  from an independently verified plan, authoritative recording root, and
  parent-supplied GPU mapping in
  `src/session/spatial_roi_recorder_contract_parser.*`;
- one strictly bounded worker lane per verified ROI, with shared batch/source
  lifetime and explicit terminal outcomes, in
  `src/spatial_roi_recording_runtime.*`;
- an owned packed-Mono8 `WORKER_ENTRY` bridge and session-time arm/disarm/drain
  controller in `src/spatial_roi_acquisition_bridge.*` and
  `src/spatial_roi_acquisition_controller.*`;
- a separate closed JSON-line ROI IPC v2 contract in
  `src/spatial_roi_ipc_protocol.*`, including explicit drain/finalize control;
- verified-plan-only CUDA memory/event handle export in
  `src/spatial_roi_ipc_exporter.*`; and
- a bounded, one-logical-stream handoff state machine with exact HELLO feature
  negotiation and ACK/RELEASE ownership in `src/spatial_roi_ipc_handoff.*`;
  and
- a single-owner, no-reconnect Unix-domain line transport that adopts one
  already-connected endpoint, verifies peer credentials when requested,
  enforces hard read/write deadlines and bounded chunked framing, and preserves
  coalesced messages in `src/spatial_roi_unix_socket_transport.*`.

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

Each lane assigns its own positive, dense, one-based
`roi_stream_frame_index` exactly when its bounded queue admits the delivery.
Rejected or failed queue admission does not consume an index. The IPC exporter
accepts that immutable lane delivery directly; callers cannot substitute an
unrelated counter. IPC correlation includes the recording identity token,
producer generation, logical stream, recording frame, and ROI-stream frame
index, so a later recording or producer epoch cannot collide merely because a
new lane starts at one. A controller also refuses to re-arm a producer
generation that it has already used. Production plan creation must mint a new
producer generation after a process/runtime restart.

The exporter retains the batch envelope and therefore its CUDA allocation and
source lease. The handoff inserts the complete export into a bounded
correlation table before writing the first `FRAME` byte and retains it through
both accepted and rejected `ACK` until an exact `RELEASE`. Merely sending
`FRAME` or receiving `ACK` is not permission to recycle the allocation. A
timeout, EOF, partial/failed write, malformed response, identity mismatch, or
out-of-order response latches the endpoint fatal and keeps ownership
indeterminate until the future supervisor confirms recorder-process exit. If
an owner violates that lifecycle and destroys an indeterminate handoff, the
table is deliberately quarantined for process lifetime rather than releasing a
possibly live CUDA allocation.

Worker-entry final release is also fail-closed: queue-lock exceptions cannot
strand the recycle mutex, a failed camera-SDK frame return prevents wrapper
reuse, and a failed recycle-queue insertion drops the zero-reference wrapper
instead of retrying it. Saturating process/context counters distinguish camera
return failures from recycle-queue failures. Those counters are diagnostic
only in this foundation; production arming remains blocked until the supervisor
persists them into the recording/session status.

The new recorder contract is intentionally a separate
`orange.spatial_roi_recording.external_recorder_contract` schema. The current
`orange.external_recorder.contract` supervisor and positional v1 frame
protocol cannot carry the required ROI identity and must reject it. Do not
relabel or feed this object to that parser. Its dedicated consumer parser does
not trust repeated fields or an embedded digest by themselves: the candidate
must exactly equal a deterministic rebuild from the independently verified
plan, expected recording root, and expected runtime GPU mapping. The bounded
file reader opens one non-symlink regular contract file, rejects embedded-NUL
paths and duplicate JSON keys, never reads more than 16 MiB, and applies fixed
depth/event/container/string limits before DOM materialization. Artifact paths
are still only lexically validated at this stage;
the future recorder must open beneath an already-owned artifact-root directory
using descriptor-relative, no-symlink semantics rather than treating a parsed
path as authorization.

The adopt-only Unix transport is not a process supervisor. It neither binds nor
unlinks a socket path, launches a child, reconnects, retries a FRAME, nor proves
peer exit. PID validation through `SO_PEERCRED` is valid only when the recorder
connects after it has been spawned; a socketpair created before `fork()` names
the creator and is not child-exec proof. The next integration must add that
listener/connect lifecycle, CUDA import/recorder process, exact reap evidence,
and finalization supervisor before enabling the runtime. The active HELLO
capability list is exactly `cuda_ipc`,
`packed_mono8`, `ack_release`, and `terminal_error`. The v2 protocol also
defines closed `DRAIN_REQUEST`, `DRAIN_STATUS`, `FINALIZE_REQUEST`, and
`FINALIZE_STATUS` messages, but `drain_finalize` is deliberately not negotiated
and a peer advertising it is rejected. It becomes an active feature only when
the recorder supervisor can coordinate every lane and verify finalization
evidence; no production component sends or consumes those messages yet.

The version domains are deliberately distinct: the generated recorder contract
is schema v1 with mode `spatial_roi_external_recorder_v1`, while its embedded
wire transport is `orange.spatial_roi.external_recorder_ipc` version 2. The
normative closed schema for the embedded `ipc_v2` object is
`docs/schemas/orange_spatial_roi_recorder_ipc_v2.schema.json`.

The extraction pixel contract and the eventual encoder-input contract are
deliberately distinct. Extraction copies native Mono8 without resize or color
conversion. HEVC/NVENC consumes NV12, so the recorder must copy every extracted
Mono8 byte unchanged into the NV12 Y plane and fill interleaved UV with the
neutral value 128; alignment padding remains zero in Y. The contract now says
`luma_preserved_exactly=true` and `neutral_chroma_value=128` instead of the
incorrect claim that Mono8-to-NV12 performs no format conversion. That transform
and decoded-pixel proof remain pending with the recorder implementation.

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
- dense per-lane stream indexing with no gaps on rejected admission;
- acquisition bridge/controller identity, lease, exception, concurrent
  disarm, generation-reuse, and teardown behavior;
- strict ROI IPC-v2 HELLO/FRAME/ACK/RELEASE/TERMINAL_ERROR and drain/finalize
  parsing, control-state sequencing, and cross-recording/cross-generation
  identity;
- real CUDA IPC export of every ROI allocation and the shared completion
  event;
- deterministic handoff tests for exact HELLO capabilities, ACK-versus-RELEASE
  lifetime, accepted/rejected frames, mismatches, duplicates, old-index replay,
  timeout/EOF/write failure, peer-exit release, and fail-safe destructor
  quarantine; and
- exact plan/root/GPU-bound recorder contract parsing, canonical identity,
  geometry/queue/artifact mutation rejection, bounded non-symlink file reads,
  and explicit Mono8-to-NV12 declarations;
- real-kernel Unix transport tests for partial/coalesced I/O, hard zero and
  partial-write deadlines, EOF versus timeout, oversized input, peer closure,
  credentials, invalid-descriptor rejection/closure, and no reconnect; and
- production Orange GUI and headless builds with the new foundation linked.

## Two optional crop products

`ROI` must not be used as shorthand for only one kind of crop. The accepted
architecture has two independently optional products:

| Product | Pixel selection | YOLO dependency | Stable stream identity |
|---|---|---|---|
| `fixed_region` | One immutable camera-native rectangle per configured ROI/region | None; it records at the declared cadence even when inference is off, skipped, empty, or failed | recording + camera + `arena_group_id` + `region_id` + `roi_id` + plan/generation |
| `subject_follow` | One configured fixed-size output window whose camera-native source rectangle follows the selected subject | Requires a complete terminal multi-box observation and accepted region assignment | the same stable region/ROI identity; never detector-vector order or a transient track ID |

The current implementation is the `fixed_region` foundation. A fixed-region
video contains the complete compartment rectangle, not a live detection box.
This is useful when no real-time detector is desired and remains independently
reconstructable from the full-frame authority.

`subject_follow` is a later policy built on the same geometry and source-frame
contracts. Its encoded raster remains fixed for the life of a video, while
each frame records the exact moving camera-native source rectangle, selected
bbox/provenance, padding, and one of `detected`, `held`, `unlocalized`,
`ambiguous`, or `technical_failure`. Any last-position hold is bounded and
versioned. A miss must never be represented as a newly detected subject.

One region may declare more than one `roi_id`, for example
`region_r0_c0_full` (`fixed_region`) and `region_r0_c0_subject`
(`subject_follow`). `region_id` identifies the physical compartment;
`roi_id` identifies a particular pixel product. Full-frame recording remains
the ingest authority for both modes until separate crop-only acceptance.

## Geometry and metadata authority

The recording plan must reference, but must not invent, three immutable
geometry/binding layers:

1. **Physical layout:** a content-addressed global dish definition in physical
   layout coordinates. Orange's existing
   `orange.calibration.arena_layout` artifact supplies `layout_id`, outer
   geometry, stable `zone_id`, optional `zone_index`, and view-registration
   vocabulary. Grouped operation must require the index and extend the bundle
   with barriers/dead zones and the declared layout kind.
2. **Camera materialization:** the exact accepted resolution of that layout for
   one camera serial/native raster, including registration/calibration
   identities and digests, resolved camera-native region geometry, boundary
   policy/tolerance, and every fixed ROI rectangle/mask.
3. **Arena-group binding:** the session/run mapping from stable `region_id` to
   Citrus `arena_id`, the optional session-level `fish_id`, routing policy,
   detector/SHAMAN identities when used, and the ROI plan digest.

For the cross-repository contract, a Citrus `region_id` is the corresponding
canonical Orange layout `zone_id`, and `region_index` is its `zone_index`.
Neither identity may be inferred from detector order or from the current order
of a JSON array.

For commissioned rectangular grids, the convenience ordering is explicitly:

```text
coordinate_space = camera_native_pixels
coordinate_origin = top_left
x_axis = right
y_axis = down
row_base = 0
column_base = 0
region_index = row * column_count + column
ordering = row_major
```

This camera-perspective convention is valid only relative to a frozen
orientation landmark/fiducial or manually accepted commissioning view. Runtime
must not re-sort and rename regions after a camera is rotated or remounted; a
changed orientation requires a new accepted materialization. Radial layouts
must instead store sector-zero orientation, clockwise/counterclockwise
direction, and center-out radial tier explicitly. Arbitrary layouts store an
explicit stable index and have no inferred geometric ordering.

The persistence chain is mandatory:

- recording snapshot/geometry bundle: portable relative path, size, SHA-256,
  and exact copies/references for physical layout, camera materialization,
  registration, masks, and group binding;
- verified ROI plan and recorder contract: all three authority IDs/digests,
  ordered region/ROI bindings, exact rectangles, and output policy;
- every ROI frame: plan digest, stable region/ROI/arena identities, exact
  source rectangle, source acquisition identity, dense ROI index, and outcome;
- final session manifest: per-stream relative media/sidecar paths, sizes,
  SHA-256 values, counts/ranges, terminal status, and finalized receipt digest;
  and
- Citrus H5: an embedded immutable arena-group geometry/binding contract (or
  exact content-addressed copy), root/member region identities, and routing or
  media rows joined to the same materialization/plan digests.

The current plan already cryptographically binds layout, materialization, and
registration references plus ordered ROI rectangles. The acquisition and IPC
foundation repeats the plan and region/ROI identities. Geometry-bundle
materialization, Citrus H5 embedding, media manifests, and final receipt
closure remain explicit acceptance work; the present tests do not claim those
artifacts exist.

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
- [ ] Before production arming, either generation-tag reusable worker-entry
      leases or prove stale release impossible across every consumer; refcount
      alone cannot distinguish a late release from a recycled entry generation.
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
- [x] Define and validate a dedicated ROI-aware IPC-v2 grammar and a
      verified-plan CUDA-IPC FRAME exporter. The existing positional protocol
      remains deliberately unchanged.
- [x] Implement the bounded one-stream handoff owner, exact HELLO negotiation,
      strictly increasing ROI indices, and correlation-to-envelope table. Do
      not report a lane source-safe until exact `RELEASE`.
- [x] Define and validate explicit drain/finalize messages and their ordered,
      stream-bound sequence/nonce state machine.
- [x] Implement a bounded adopt-only connected Unix-socket transport and a
      strict consumer parser bound to the independently verified plan,
      authoritative recording root, and parent GPU placement.
- [ ] Implement socket listener/connect ownership, recorder CUDA
      import/process, supervisor/reap lifecycle, and production drain/finalize
      exchange.
- [x] Give each required ROI an independently bounded runtime queue and
      terminal state. A lane may fail or drop only its own sidecar; it must not
      relabel another ROI. Connecting those lanes to recorder processes and
      full-frame coexistence remains pending.
- [ ] Encode the exact Mono8 pixels with the validated lossless profile. Keep
      alignment padding explicit and zero-filled; do not scale. Map each Mono8
      byte unchanged to NV12 Y and set interleaved UV to neutral 128, then prove
      decoded luma equality.
- [x] Define and validate ACK/release identity including recording token,
      producer generation, logical stream/ROI identity,
      `recording_frame_id`, and dense `roi_stream_frame_index`.
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
- [ ] Persist worker-entry recycle, camera-return, and recycle-queue failure
      counters in recording/session status before production arming.
- [ ] Persist the complete physical-layout -> camera-materialization ->
      arena-group-binding chain and its portable path/size/SHA-256 evidence in
      the Orange geometry bundle, ROI plan/contract, final session manifest,
      and Citrus H5. Require explicit stable region index/order fields; array
      order alone is not authority.
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
- `fixed_region` and `subject_follow` are separate optional products. The
  first one-camera acceptance enables only `fixed_region`; it must work with
  YOLO disabled. A later `subject_follow` policy is keyed by stable region
  identity and records a moving source rectangle plus detection/hold state.
- No atlas/packing fallback is introduced until standalone ROI correctness is
  proven; no pose or detection fanout is coupled to ROI availability.
- Queue/pool pressure is bounded and observable. Acquisition must not wait for
  encode, mux, disk I/O, or finalization.
- Merge/deployment remains blocked until the acceptance gates above and the
  corresponding Citrus media-reference checks pass.

Authoritative full-frame recording must remain enabled until those acceptance
gates pass. The extraction tests alone do not authorize merge or deployment.
