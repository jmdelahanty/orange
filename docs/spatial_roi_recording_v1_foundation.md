# Spatial ROI Recording v1 Foundation

**Date:** 2026-08-30

**Last updated:** 2026-08-31

**Status:** the extraction, acquisition/IPC handoff, transport, and
recorder-owned CUDA detach foundations are committed through `22ab8b1` on
`agent/acquisition/spatial-roi-recording-v1-20260830`. The working tree also
contains an uncommitted one-output lossless encoder, descriptor-authorized
artifact root/bundle, recorder evidence writer, and shared NVENC/FFmpeg
failure hardening. That library/test media slice has the real-driver
acceptance result recorded below, but has not been committed. The feature
remains default-off and still has no production arming caller, socket
listener, camera-level recorder executable, child supervisor, operational
drain/finalize exchange, or headless-runner integration. No Orange application
path currently produces spatial-ROI video files.

The dependency-ordered completion authority is
`docs/spatial_roi_headless_completion_checklist_2026-08-31.md`. The component
inventory later in this document supplies design detail but must not be used to
declare the headless feature ready.

Before any production consumer was armed, the closed configuration and plan
documents advanced to schema v2 to bind explicit long-run frame, media, and
evidence budgets, and the dedicated recorder contract advanced to schema v2 to
bind encoder/writer limits and evidence artifacts. There is intentionally no
v1 compatibility parser for these unpublished, default-off contracts. The
embedded wire protocol remains IPC v2 and is a separate version domain.

This document is the canonical status and handoff for the detector-independent
spatial ROI path. It must not be read as claiming that the existing scalar,
YOLO-driven `Cam<serial>_crop` output is a spatial ROI stream.

## Scope

This slice establishes the detector-independent front half of continuous
camera-native spatial ROI recording. It does not change the legacy top-one crop
pipeline or arm a production ROI video; the one-output encoder test creates
only a temporary acceptance artifact.

No production spatial-ROI caller exists, and the legacy full-frame/top-one
crop protocols and artifact identities remain unchanged. Descriptor mode alone
uses the compact keyframe-summary contract described below; legacy pathname
mode retains its existing keyframe-list schema. Full FFmpeg/NVENC regression is
a commit gate for the shared writer hardening.

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
  coalesced messages in `src/spatial_roi_unix_socket_transport.*`; and
- a bounded synchronous, one-owner CUDA detach pool in
  `src/spatial_roi_recorder_cuda_detach.*` that binds the exact stream,
  fixed-region geometry, source/recorder GPUs and shard, enforces an aggregate
  allocation budget and one active caller, bounds source-event/copy waits,
  validates the imported allocation, and materializes packed Mono8 plus NV12
  with byte-identical Y and neutral UV=128;
- a descriptor-relative artifact-root authority in
  `src/spatial_roi_recorder_artifact_root.*`, plus a move-only encoder bundle
  retaining one shared root and exactly four read-write artifact descriptors;
- a bounded, single-owner lossless HEVC/NVENC/MP4 core in
  `src/spatial_roi_lossless_encoder.*`, including ordered per-frame results,
  descriptor-backed metadata, terminal writer evidence, exact sidecar checks,
  and terminal sealing; and
- a bounded JSONL evidence writer and finalized-manifest validator in
  `src/spatial_roi_recorder_evidence.*`.

The plan binds the parent recording identity/token, producer generation,
camera ID and serial, native raster, layout/materialization/registration
authority references, ordered ROI descriptors, exact encoded rasters and
padding, pool depth, admitted pool bytes, and explicit per-stream frame,
media-byte, and evidence-byte limits with checked aggregate admission. The v2
defaults are **4,000,000 frames**, **128 GiB of media**, and **4 GiB of
aggregate evidence** per stream. The frame and evidence defaults are also
implementation ceilings. Media is configurable up to
`9223372036854775807` bytes, the signed `off_t` ceiling. Whatever values the
verified plan selects are hard authenticated budgets—not a duration or an
estimate of HEVC compression—and are charged against aggregate admission. A
producer can only be constructed from limits minted by a verified plan.
Mutating those public limits after verification is rejected.

The one-output encoder has no arbitrary output or metadata pathname authority.
For each stream, the contract-authorized artifact directory is
`<recording_root>/external_spatial_roi_recorder`, and the encoder receives a
move-only bundle holding exactly these four retained read-write regular-file
handles (all from that same root identity, with distinct inodes and exact
contract-relative names):

```text
Cam<camera_serial>_spatial_roi_<roi_id>.mp4
Cam<camera_serial>_spatial_roi_<roi_id>_meta.csv
Cam<camera_serial>_spatial_roi_<roi_id>_keyframe.json
Cam<camera_serial>_spatial_roi_<roi_id>.mp4.finalization.json
```

The shared artifact root and handles are checked against the allow-list and
current directory bindings before use and are sealed only after writer
quiescence. Metadata is written through the held descriptor with an
authenticated frame-derived ceiling. Terminal keyframe and finalization JSON
are read through the held descriptors and are not reopened by pathname. The
encoder locally bounds both reads at 16 MiB; final evidence validation applies
a stricter 16 KiB bound to the constant-size keyframe summary. The descriptor
mode summary is an exact closed object with schema
`orange.spatial_roi_keyframe_summary` version 1, `terminal=true`, HEVC codec,
the contract FPS, and a positive total frame count `N`. It proves the exact
dense sequence with `{first: 0, last: N - 1,
zero_based_contiguous: true}` and the GOP-1 policy with
`{name: "all_frames_idr", keyframe_frames: N, non_keyframe_frames: 0,
satisfied: true}`. The unbounded keyframe-index list remains only in legacy
pathname mode. Complete output also requires a size-matching container
finalization sidecar.

The broader recorder contract authorizes twelve exact products: the encoder's
video, metadata, keyframe summary, and container-finalization descriptors plus
perf, summary, status, video-sanity, recorder-log, transport, evidence-JSONL,
and evidence-manifest artifacts. Every non-video product—including evidence
JSONL and its manifest—is charged against one aggregate evidence budget;
video alone is charged to the media budget. Finalization, summary, status,
video-sanity, and manifest JSON each have a 16 MiB read ceiling. Metadata and
perf CSV bounds are frame-derived. Evidence JSONL is limited by the
authenticated aggregate evidence budget, one MiB per line, and the
4,000,000-frame ceiling; log and transport inputs have their own bounded
grammars and count against that same aggregate budget.

A successful immutable terminal snapshot requires every admitted frame to
complete: public enqueue attempts are exactly partitioned into enqueued and
rejected attempts, successful finalization has no rejection or queue overflow,
dequeue/copy/source-release/result counts match, and the writer's
`video_size_limit_failures` is zero.

Recovery APIs deliberately require the same retained
`SpatialRoiRecorderArtifactRoot`; there is no pathname-only recovery overload.
The evidence JSONL/manifest pair can be adopted exactly between its two
no-replace publication steps while that authority survives. Process-boundary
recovery is not yet claimed: Gate 2/3 must inherit or pass the root directory
descriptor together with its expected `(device, inode)` identity, or fail
closed under a new recording identity. Partial MP4, metadata, keyframe, and
finalization artifacts are non-certifying residue in Gate 1 and are neither
resumed nor adopted.

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
lane queue. Configuration/plan schema v2 is always strict: any queue rejection, missing sink,
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
listener/connect lifecycle, recorder executable/encoder, exact reap evidence,
and finalization supervisor before enabling the runtime. The active HELLO
capability list is exactly `cuda_ipc`,
`packed_mono8`, `ack_release`, and `terminal_error`. The v2 protocol also
defines closed `DRAIN_REQUEST`, `DRAIN_STATUS`, `FINALIZE_REQUEST`, and
`FINALIZE_STATUS` messages, but `drain_finalize` is deliberately not negotiated
and a peer advertising it is rejected. It becomes an active feature only when
the recorder supervisor can coordinate every lane and verify finalization
evidence; no production component sends or consumes those messages yet.

The version domains are deliberately distinct: the generated recorder contract
is schema v2 with mode `spatial_roi_external_recorder_v2`, while its embedded
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
is implemented and byte-verified in recorder-owned device storage. The
outside-sandbox real-driver acceptance for the working-tree encoder is
**PASS**: device `0`, `256x256`, `3/3` frames decoded, elapsed `1001 ms` on
the final hardened encoder/writer tree.
The local sandbox has no CUDA device and therefore only exercises the host
portion; no production target consumes this core yet.

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
- host encoder/artifact-authority rejection, bounded frame/media admission,
  descriptor-backed metadata, strict terminal writer/result accounting,
  exact four-artifact binding/sealing, and immutable terminal snapshots; and
- outside-sandbox CUDA/NVENC/container acceptance on device `0`: **PASS**,
  `3/3` decoded frames at `256x256`, elapsed `1001 ms`;
- real-kernel Unix transport tests for partial/coalesced I/O, hard zero and
  partial-write deadlines, EOF versus timeout, oversized input, peer closure,
  credentials, invalid-descriptor rejection/closure, and no reconnect; and
- a real separate-process CUDA IPC import/detach test that holds the producer
  allocation through recorder exit, verifies exact Mono8 and NV12 Y bytes,
  verifies UV=128, exercises bounded slot reuse/rejection/quarantine, proves a
  deliberately pending producer event reaches its hard deadline, rejects a
  concurrent caller immediately, and only then permits producer release; and
- production Orange GUI and headless builds with the committed Gate 0
  foundation; the Gate 1 encoder/evidence sources remain library/test targets
  until the dedicated recorder executable is added.

The recorder detach acceptance above is same-GPU on the installed Linux/CUDA
driver. Cross-GPU use is preflighted for unified addressing, IPC-event support,
and recorder-to-source peer access, but a positive/negative multi-GPU topology
test remains an explicit acceptance gate before enabling such a placement.

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

## Detailed one-camera/four-ROI component inventory

The stable gate order and acceptance evidence are maintained in
`docs/spatial_roi_headless_completion_checklist_2026-08-31.md`. The checklist
below is a detailed component inventory retained with the foundation design;
completed library seams here do not imply that a later headless gate is
complete.

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
- [x] Implement recorder-side CUDA import and bounded detach into
      recorder-owned packed Mono8/NV12 storage, with exact plan geometry,
      source/recorder GPU, shard, allocation extent, pool-budget, timeout, and
      quarantine checks. This is a library/test seam, not yet a recorder
      process.
- [ ] Implement socket listener/connect ownership, recorder executable,
      supervisor/reap lifecycle, and production drain/finalize exchange.
- [x] Give each required ROI an independently bounded runtime queue and
      terminal state. A lane may fail or drop only its own sidecar; it must not
      relabel another ROI. Connecting those lanes to recorder processes and
      full-frame coexistence remains pending.
- [x] Encode the exact Mono8 pixels with the validated lossless profile. Keep
      alignment padding explicit and zero-filled; do not scale. Map each Mono8
      byte unchanged to NV12 Y and set interleaved UV to neutral 128 (the
      recorder-owned transform is complete), then prove decoded-video luma
      equality through NVENC and the finalized container. The one-output
      library/test seam passes this proof on the real driver; production
      recorder wiring remains pending.
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
