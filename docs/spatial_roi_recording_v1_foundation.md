# Spatial ROI Recording v1 Foundation

**Date:** 2026-08-30

**Last updated:** 2026-09-02

**Status:** the extraction, acquisition/IPC handoff, transport, and
recorder-owned CUDA detach foundations are committed through `22ab8b1` on
`agent/acquisition/spatial-roi-recording-v1-20260830`. The reviewed Gate 1
one-output versioned HEVC encoder, descriptor-authorized artifact root/bundle,
recorder evidence writer, and shared NVENC/FFmpeg failure hardening are
committed as `157ce0f`. The current uncommitted work now adds the camera-level
four-stream recorder executable, exact-PID child supervisor, producer
coordinator, headless session owner, optional default-off `orange_client`
acquisition/finalization wiring, strict schema-v3 session snapshots,
content-addressed finalized receipts, atomic terminal manifest publication,
and a read-only offline acceptance verifier, together with focused host test
targets. The integrated source path and full release targets build, and the
real driver passed legacy lossless/GOP-1 plus 51-frame P1/GOP-25 encode/decode
tests on 2026-09-02. Live-camera, four-output headless artifact validation has
not been completed for this tree. No accepted Orange run currently certifies
spatial-ROI video files.

The dependency-ordered completion authority is
`docs/spatial_roi_headless_completion_checklist_2026-08-31.md`. The component
inventory later in this document supplies design detail but must not be used to
declare the headless feature ready. The deferred modularization design for the
large headless translation unit is
`docs/orange_headless_client_modularization_design_2026-09-01.md`; it does not
authorize refactoring during this spatial-ROI feature.

Before any production consumer was armed, the closed configuration and plan
documents advanced to schema v3 to bind explicit long-run frame, media, and
evidence budgets plus one of three immutable encoder profiles. Runtime parsing
retains the unpublished schema-v2 lossless form and the earlier P1/GOP-1
profile without changing either identity. The dedicated recorder contract is
now schema v5, with mode
`spatial_roi_external_recorder_v5`, and binds encoder/writer limits, evidence
artifacts, authenticated detach-pool budgets, and a versioned nonzero
reserved-free-space policy. The unpublished v4 lossless contract remains an
explicit legacy parser path. There is intentionally no
v1 compatibility parser for these unpublished, default-off contracts. The
embedded wire protocol remains IPC v2 and is a separate version domain.

This document is the canonical status and handoff for the detector-independent
spatial ROI path. It must not be read as claiming that the existing scalar,
YOLO-driven `Cam<serial>_crop` output is a spatial ROI stream.

## Scope

This slice establishes the detector-independent front half of continuous
camera-native spatial ROI recording. The optional headless caller now arms the
new path in source. The one-output real CUDA/NVENC tests pass and create only
temporary test artifacts; live-camera, four-output headless artifact
validation remains pending.

The full-frame router/recorder is a first-class supported product, not a legacy
compatibility path. It remains valid by itself and remains the full-context
ingest authority when fixed-region or future subject-follow products are added.
Spatial ROI is optional and additive. Crop-only recording is a distinct future
mode with its own acceptance requirements; it is never inferred by disabling
or omitting the full-frame product.

The first-class full-frame and top-one crop protocols and artifact identities
remain unchanged. Full-frame recording remains the ingest/context authority
and a supported product on its own; spatial ROI is additive. Spatial ROI
descriptor mode alone uses the compact keyframe-summary contract described
below; the established full-frame pathname mode retains its existing
keyframe-list schema. Full FFmpeg/NVENC regression and a live headless artifact
run remain acceptance gates for the integrated path.

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
- a bounded, single-owner, versioned HEVC/NVENC/MP4 core in
  `src/spatial_roi_lossless_encoder.*`, including ordered per-frame results,
  the legacy lossless/GOP-1 profile, P1/VBR-Q20 GOP-1 and GOP-25 profiles,
  explicit AQ-off/temporal-AQ-off/lookahead-off/lookahead-depth-zero controls,
  descriptor-backed metadata, terminal writer evidence, exact sidecar checks,
  and terminal sealing; and
- a bounded JSONL evidence writer and finalized-manifest validator in
  `src/spatial_roi_recorder_evidence.*`; and
- a strict, one-shot filesystem `AF_UNIX` listener in
  `src/spatial_roi_unix_socket_listener.*` that refuses pre-existing paths,
  applies mode `0600`, accepts with a bounded deadline, verifies the configured
  peer credentials through the adopted transport, and conditionally removes
  only its recorded socket identity. The listener carries IPC-v2 control and
  CUDA-handle descriptors; video pixels are not serialized through the socket;
- a recording-token-derived, mode-0700 private socket runtime directory with
  retained descriptor/device/inode authority in
  `src/spatial_roi_socket_runtime_directory.*`, plus a bounded, one-attempt
  producer connector in `src/spatial_roi_unix_socket_connector.*`;
- an authenticated one-camera/four-fixed-region contract projection and
  plan-ordered camera owner in
  `src/session/spatial_roi_recorder_camera_contract.*` and
  `src/spatial_roi_camera_recorder.*`, with one plan-ordered stream-core vector
  in `src/spatial_roi_camera_recorder_stream_core.*`;
- a recorder-side per-stream IPC session in
  `src/spatial_roi_recorder_ipc_session.*` that owns bounded HELLO,
  FRAME/ACK/RELEASE, EOF, terminal-error, dispatch, and source-ownership
  truth without owning the listener, CUDA resources, or encoder;
- a bounded transport/encoder frame journal and bridge in
  `src/spatial_roi_recorder_frame_journal.*` and
  `src/spatial_roi_recorder_frame_journal_bridge.*` that join lifecycle
  outcomes with asynchronous encoder results without retaining CUDA handles
  or doing blocking evidence I/O in the encoder callback;
- closed, non-certifying terminal candidate sidecars in
  `src/spatial_roi_recorder_terminal_sidecars.*`, with the finalized evidence
  manifest as the sole commit marker; and
- a descriptor-bound video-sanity probe/result capability in
  `src/spatial_roi_recorder_video_sanity.*` and evidence-v2 finalization
  validation in `src/spatial_roi_recorder_evidence.*`. Complete publication
  requires the probe-produced capability; offline validation is consistency
  checking unless the descriptor-bound probe is rerun;
- a bounded exact-PID recorder child supervisor in
  `src/spatial_roi_camera_recorder_process.*` and the dedicated
  `spatial_roi_camera_recorder` executable in
  `tools/spatial_roi_camera_recorder.cpp`. It authenticates the plan/root/GPU
  mapping before one no-shell launch, separates socket-bound from recorder
  ready, drains bounded JSONL lifecycle output, and escalates SIGTERM/SIGKILL
  with exact reap evidence;
- a headless one-camera session owner in
  `src/spatial_roi_headless_camera_session.*`, plus optional default-off
  preparation, acquisition dispatch, snapshot, manifest, Finish/Abort wiring
  in `src/orange_headless_client.cpp`. This keeps the existing full-frame
  recorder first-class and additive; it is not a live acceptance result. ROI
  output and session-state transitions use one atomic recording-snapshot
  update. A preparation failure is persisted only after the plan, recorder
  contract, and their exact artifact receipts are authenticated; earlier
  failures are reported without manufacturing scientific identity. Full-frame
  finalization remains independently gated by its own start, drain, frame,
  packet, and path evidence when an ROI makes the aggregate session
  incomplete; and
- a closed finalized-session receipt in
  `src/spatial_roi_finalized_session_receipt.*`, strict schema-v3 session
  validation in `src/session/spatial_roi_session_snapshot.*`, and retained
  recording/artifact-root authority in
  `src/spatial_roi_session_authority_store.*`. Completion requires exact
  plan-ordered stream receipts, dense identity ranges, successful lifecycle
  counters, twelve positive-size content-addressed artifacts per stream, and
  matching ready/terminal storage preflight. The terminal recording-session
  manifest is published by same-directory atomic replacement; and
- a read-only acceptance verifier in
  `tools/validate_spatial_roi_recording.py` that opens only declared artifacts
  descriptor-relatively, rejects link/path substitution, validates all 48
  artifact sizes and SHA-256 values, and can require decoded media/raster/frame
  proof through `ffprobe`.

The plan binds the parent recording identity/token, producer generation,
camera ID and serial, native raster, layout/materialization/registration
authority references, ordered ROI descriptors, exact encoded rasters and
padding, pool depth, admitted pool bytes, and explicit per-stream frame,
media-byte, and evidence-byte limits with checked aggregate admission. The v4
recorder contract additionally binds per-stream detach-pool frame/byte
budgets, their checked aggregate byte total, and the storage-preflight policy;
these are not inferred by the recorder from untrusted runtime inputs. The v2
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
zero_based_contiguous: true}` and the selected immutable GOP policy. GOP-1
uses `{name: "all_frames_idr", keyframe_frames: N,
non_keyframe_frames: 0, satisfied: true}`; the active P1/VBR-Q20/GOP-25
profile uses `fixed_gop_25_idr` and requires IDRs at exact zero-based GOP
boundaries. Per-frame evidence proves every keyframe decision without adding
an unbounded sidecar list. The unbounded keyframe-index list remains part of the
established first-class full-frame pathname mode. Complete output also requires a size-matching container
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
only in this foundation; production acceptance remains blocked until the
supervisor persists them into the recording/session status.

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
are still only lexically validated by the parser. The recorder therefore opens
them beneath an already-owned artifact-root directory using
descriptor-relative, no-symlink semantics rather than treating a parsed path
as authorization. The current Gate 2/3 implementation composes the
private runtime directory, listener, producer connector, camera-level owner,
child executable, and exact-PID supervisor; live child/artifact validation is
still pending.

The adopt-only Unix transport is not a process supervisor. It neither binds nor
unlinks a socket path, launches a child, reconnects, retries a FRAME, nor proves
peer exit. PID validation through `SO_PEERCRED` is valid only when the recorder
connects after it has been spawned; a socketpair created before `fork()` names
the creator and is not child-exec proof. The current integration supplies that
listener/connect lifecycle, recorder executable/encoder, exact reap evidence,
and bounded finalization supervisor. The active HELLO
capability list is exactly `cuda_ipc`,
`packed_mono8`, `ack_release`, and `terminal_error`. The v2 protocol also
defines closed `DRAIN_REQUEST`, `DRAIN_STATUS`, `FINALIZE_REQUEST`, and
`FINALIZE_STATUS` messages, but `drain_finalize` is deliberately not negotiated
and a peer advertising it is rejected. It becomes an active feature only when
the recorder supervisor can coordinate every lane and verify finalization
evidence; no production component sends or consumes those messages yet.

The version domains are deliberately distinct: the generated recorder contract
is schema v5 with mode `spatial_roi_external_recorder_v5`, while its embedded
wire transport is `orange.spatial_roi.external_recorder_ipc` version 2. The
normative closed schema for the embedded `ipc_v2` object is
`docs/schemas/orange_spatial_roi_recorder_ipc_v2.schema.json`.

The v4 contract's `storage_preflight_policy` is a separate closed policy
object with a nonzero `reserved_free_bytes` (currently 500,000,000,000). The
recorder performs a live `fstatvfs` against the retained artifact-root fd
before readiness and requires available block-derived bytes to cover the
authenticated aggregate media and evidence budgets plus that reserve. The
versioned result, including exact root device/inode, raw blocks, capacity,
available bytes, budgets, required bytes, and pass/fail, is carried in every
recorder lifecycle snapshot. The camera owner also rejects a missing,
unchecked, failed, or contract-mismatched attached result before it can enter
`ready`. This is host implementation evidence, not a live CUDA/NVENC
acceptance result.

The extraction pixel contract and the eventual encoder-input contract are
deliberately distinct. Extraction copies native Mono8 without resize or color
conversion. HEVC/NVENC consumes NV12, so the recorder must copy every extracted
Mono8 byte unchanged into the NV12 Y plane and fill interleaved UV with the
neutral value 128; alignment padding remains zero in Y. The contract now says
`neutral_chroma_value=128`, with `luma_preserved_exactly=true` only for the
legacy lossless profile. The recorder always copies the source bytes exactly
into encoder-input Y before encoding; the active P1/VBR-Q20/GOP-25 media is
lossy and does not claim decoded-Y identity. That transform is implemented and
covered by host seams in recorder-owned device-storage code. A non-skipped
CUDA/NVENC/decode run for the current integrated tree passed on the host for
both legacy lossless/GOP-1 and 51-frame P1/GOP-25 output. The production
recorder target now consumes this core, but no live-camera headless artifact
run has certified it.

## Thread topology, CPU isolation, and jitter budget

The fixed-region product deliberately uses one camera-level recorder process,
not one process per ROI. For a four-ROI camera, the current steady-state thread
topology is:

- in the Orange acquisition process, the existing camera acquisition thread
  performs one bounded, nonblocking batch submission; four bounded ROI-lane
  workers wait for the batch completion event and independently drive their
  authenticated IPC endpoints;
- in the camera-level recorder child, four IPC session threads own
  `FRAME`/`ACK`/`RELEASE`, four encoder-owner threads own CUDA/NVENC submission,
  and four FFmpeg writer threads own packet/mux output; and
- the recorder executable's existing control thread owns readiness, heartbeat,
  EOF polling, evidence-journal draining, and finalization. The frame journal
  has no thread of its own.

The encoder-result and transport-outcome callbacks may only stage bounded
records. They must not perform blocking evidence I/O. During live acquisition,
the recorder control thread must continuously retire the complete, dense
journal prefix. Deferring all evidence draining until EOF turns
`max_pending_entries` into an accidental recording-length bound instead of an
in-flight-work bound. The 2026-09-01 first live attempt exposed exactly that
failure at 64 pending entries; the control-thread drain and a 128-frame
bounded-journal regression are the corrective slice.

Process isolation remains the primary hot-path boundary: ROI CUDA detach,
NVENC, mux, evidence writes, video probes, and finalization stay outside the
Orange acquisition process. A separate process does not, however, imply CPU
affinity. As of 2026-09-01:

- Citrus explicitly pins stimulus render to CPU 1 and arena update to CPU 2;
  its arena workers use CPUs 24-27 and Shaman readers start at CPU 20;
- Orange explicitly pins the four YOLO workers to CPUs 6, 8, 10, and 12;
- the running kernel isolates CPUs
  `1,2,6,8,10,12,38,40,42,44` with matching `isolcpus`, `nohz_full`, and
  `rcu_nocbs` parameters; Orange's four YOLO physical-core sibling pairs are
  covered, while Citrus CPUs 1 and 2 have non-isolated SMT siblings 33 and 34;
  and
- the four producer lanes in Orange still use inherited `SCHED_OTHER`
  placement; and
- the camera-level recorder now has an optional multi-core affinity envelope.
  When configured, its control thread applies and exactly reads back the mask
  before any IPC-session, encoder-owner, or writer thread is created, so those
  child threads inherit the envelope. When absent, all child roles retain the
  prior inherited `SCHED_OTHER` behavior. They are not individually pinned.

The first affinity implementation is optional, versioned, and observable. The
headless parent resolves
`ORANGE_SPATIAL_ROI_RECORDER_CPU_AFFINITY_CAM_<camera_serial>` before the global
`ORANGE_SPATIAL_ROI_RECORDER_CPU_AFFINITY` fallback. It strictly parses and
canonicalizes a bounded Linux CPU list, passes the value and authority-source
name through fixed child arguments, and fails before recorder construction if
the requested and effective masks differ. The child emits the closed
`orange.spatial_roi_recorder.scheduling` v1 object in ready, heartbeat, and
terminal lifecycle records. The supervisor validates its exact shape and
requires complete-terminal scheduling evidence to equal ready evidence. The
record distinguishes affinity syscall success, exact-mask verification, and a
successful versus unavailable kernel-isolation observation.

The sudo benchmark wrapper source exposes the bounded
`--spatial-roi-recorder-cpu-affinity` option. Installing that updated source at
`/usr/local/bin/orange-local-benchmark` remains a deliberate root-owned
deployment step; an older installed wrapper continues to run the safe default-
inherit profile.

New threads inherit a process mask, so a camera-recorder process envelope is
the preferred first control; per-thread pinning should be introduced only when
measurement shows a benefit. On the current 64-logical-CPU host, recorder
allocation must exclude complete physical pairs used by housekeeping, Citrus,
and Orange: `0-2,6,8,10,12,20-27,32-34,38,40,42,44,52-59`. Candidate complete
pair pools include `13-19,45-51` and `28-31,60-63`, but neither is exclusively
reserved today. A run must inspect current workload and IRQ placement before
selecting one. Do not enable `SCHED_FIFO` by default.

The live performance gate must persist enough evidence to distinguish CPU
scheduling jitter from GPU/NVENC, IPC, disk, or queue pressure:

- requested/effective process and thread CPU masks, scheduler policy, and
  thread role/name;
- camera frame gaps, `GetFrame` errors, acquisition cadence, YOLO queue wait,
  and detect-latency percentiles;
- per-lane extraction and IPC queue high-water, detach-pool pressure, encoder
  queue high-water, and FFmpeg writer queue high-water;
- evidence-journal pending high-water, drain batch size, drain duration, and
  overflow count;
- per-role CPU time and voluntary/involuntary context switches when available;
  and
- GPU/NVENC utilization, storage throughput, encoded frame parity, and
  finalization duration.

CPU affinity is a deployment/performance control, not scientific identity.
Its requested and effective state belongs in immutable runtime metadata and
acceptance evidence, while ROI geometry, recording identity, and frame
correlation remain unchanged.

The current implementation uses Linux `cpu_set_t` and therefore intentionally
bounds CPU indices to `CPU_SETSIZE` (1024 on this build), comfortably above
this rig's 64 logical CPUs.

## Validation completed and current evidence boundary

The items below are host/focused validation or previously established
foundations unless explicitly described as pending. They do not certify a live
one-camera/four-ROI headless recording. The integrated release build and
one-output CUDA/NVENC tests pass; live separate-process camera/IPC/four-output
artifact validation remains required before acceptance.

- strict config/plan parsing, normalization, digest binding, bounds, overlap,
  naming, recording-token, admission, malformed-input, and mutation tests;
- SHAMAN-independent host geometry/identity validation;
- four simultaneous device-to-device ROI copies on an RTX A6000;
- bounded pool exhaustion/reuse and source-lease lifetime; and
- `StopAccepting()` linearization against concurrent production;
- closed frame/metadata JSON round trips, canonical stream naming, origin-
  anchored encoded geometry, and duplicate identity rejection;
- exact verified-plan-to-recorder materialization, GPU-map coverage, immutable
  HEVC lossless/GOP-1 and P1/VBR-Q20/GOP-25 policies, queue-depth propagation,
  and odd-NV12 rejection;
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
- host encoder/artifact-authority, recorder-contract, process-supervisor,
  camera-owner, headless-session, and journal/sidecar/video-sanity tests are
  registered and exercised by the available release test tree;
- real-kernel Unix transport, listener, connector, and peer-credential checks
  pass on the host outside the restricted sandbox (the sandbox itself returns
  `EPERM` for those operations);
- authenticated v4 recorder-contract parsing with detach-pool and storage-policy
  binding,
  alongside the unchanged strict v2 config/plan and IPC-v2 domains;
- one-camera/four-stream contract projection and plan-ordered owner lifecycle
  tests, including aggregate readiness, EOF, partial failure, drain, and
  finalization state; and
- recorder IPC-session protocol tests, bounded frame-journal/bridge joins,
  non-certifying terminal sidecar validation, and descriptor-bound video
  sanity/evidence-v2 checks for profile, cadence, PTS, duration, samples,
  dimensions, size, and SHA-256;
- deterministic fake-child/exact-PID supervisor and headless-session lifecycle
  tests, including bounded teardown and malformed lifecycle JSON;
- strict CPU-list parsing, default-inherit behavior, explicit exact readback,
  child-thread inheritance, closed scheduling-v1 validation, and ready/terminal
  scheduling-parity rejection; and
- production Orange GUI/headless and recorder targets are build-registered,
  with the current source path wired for optional one-camera/four-ROI
  operation. The integrated release rebuild and one-output CUDA/NVENC media
  run pass. A live separate-process CUDA IPC import/detach run and live
  four-output headless artifact run remain pending.

The earlier isolated recorder-detach result is not current integrated-tree
acceptance. Cross-GPU use is preflighted for unified addressing, IPC-event
support, and recorder-to-source peer access, but positive/negative multi-GPU
topology tests remain an explicit acceptance gate before enabling such a
placement.

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
foundation repeats the plan and region/ROI identities. Media descriptors and
final receipt closure are implemented in the host session path. Complete
physical geometry-bundle materialization and Citrus H5 embedding remain
explicit cross-repository acceptance work; the present tests do not claim
those artifacts exist in a live recording.

## Detailed one-camera/four-ROI component inventory

The stable gate order and acceptance evidence are maintained in
`docs/spatial_roi_headless_completion_checklist_2026-08-31.md`. The checklist
below is a detailed component inventory retained with the foundation design;
completed library seams here do not imply that a later headless gate is
complete.

The current integration slice is one end-to-end, detector-independent recorder
path for a single camera with one accepted four-region plan. Its source path is
intended to produce four independently addressable native-resolution ROI
streams using the selected immutable encoder profile. The active diagnostic
profile is P1/VBR-Q20/GOP-25; the legacy lossless profile remains supported.
All three immutable profiles explicitly bind `aq=false`,
`temporal_aq=false`, `lookahead=false`, and `lookahead_depth=0`. These are
effective encoder controls, not advisory experiment-spec settings: schema-v3
config/plan, recorder-contract v5, runtime NVENC overrides, session/evidence
artifacts, and MP4 metadata all preserve the same values. Legacy schema-v2
config/plan and contract-v4 files omit those fields on wire and are parsed with
the historical off/off/off/zero inference.
Combined mode retains the authoritative full-frame stream, while the separate
fixed-ROI-only policy retains a registered context image instead. Live artifact
validation remains pending. YOLO, preview, pose, packed atlases, and
detection-centered routing are out of scope.

The schema and runtime remain general over a verified plan's admitted ROI
count so future layouts do not require a second ABI. Activation and acceptance
are nevertheless gated to one camera/four ROIs for this slice.

### A. Acquisition seam and batch submission

- [x] At recording cadence, obtain the authoritative `WORKER_ENTRY` source
      allocation/event lease and construct one `SpatialRoiSourceView` for the
      camera; do not read pixels through YOLO or the legacy top-one crop path.
- [x] Submit exactly one `TryProduce()` batch per eligible source frame, with
      all plan ROIs in verified order and the complete source identity copied to
      every work item.
- [x] Make runtime batch/lane admission nonblocking and expose separate busy,
      stopped, pool-empty, invalid, duplicate/out-of-order, CUDA-error,
      queue-full/queue-admission-failure, and source-quarantine outcomes.
      Acquisition-side per-frame persistence beyond the bounded journal remains
      pending.
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

#### CUDA transport selection

IPC-v2 deliberately uses the supported CUDA Runtime IPC family:
`cudaIpcGetMemHandle`/`cudaIpcOpenMemHandle` for the producer-owned
`cudaMalloc` ROI allocation and an interprocess CUDA event for readiness. The
contract-bound, peer-credential-checked producer proves the original allocation
is device memory on its declared `source_gpu_id` before exporting the handle.
After a cross-GPU import, the recorder treats
`cudaPointerGetAttributes().device` as mapping evidence rather
than source authority because released NVIDIA drivers can report the importing
GPU for this legacy allocation mapping. It still requires a device-accessible
alias, an ordinal belonging to the declared source/recorder pair, verified peer
access, the exact allocation base/range, the contract-bound descriptor from the
checked producer peer, event completion, and a completed detach copy before
source release.

CUDA Virtual Memory Management is a deferred IPC-v3 transport option, not a
prerequisite for fixed-ROI acceptance. Adopting it would replace the producer
pool allocator and import lifecycle, add allocation-granularity and device
capability checks, and require POSIX shareable file descriptors to be passed
with `SCM_RIGHTS` rather than serialized into the current line protocol. It
would not remove the need for an interprocess synchronization contract.

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
      quarantine checks. The camera-level executable now consumes this core;
      live-camera four-output validation remains pending (the one-output
      CUDA/NVENC tests pass).
- [x] Implement the strict one-shot recorder-side socket-listener library and
      its focused lifecycle/credential tests. The restricted sandbox returns
      `EPERM`, while the corresponding host tests pass outside it.
- [x] Implement the recording-token-derived private socket runtime-directory
      authority and bounded producer connector. The directory retains its
      descriptor/device/inode identity; the connector makes one exact,
      peer-credential-checked attempt and never retries or launches a child.
- [x] Implement the authenticated one-camera/four-fixed-region contract view
      and plan-ordered camera-owner orchestration seam. It constructs four
      injected stream cores and coordinates readiness, EOF, stop-admission,
      drain, finalization, and aggregate failure; it does not bind sockets,
      import CUDA, own encoders, or constitute a process.
- [x] Implement the recorder-side per-stream IPC session, bounded frame
      journal, and journal bridge. These join transport outcomes to encoder
      results while keeping CUDA handles out of retained evidence and blocking
      evidence I/O out of encoder callbacks.
- [x] Implement closed non-certifying terminal candidate sidecars and the
      descriptor-bound video-sanity result/capability consumed by evidence-v2
      finalization. Offline checks remain consistency-only unless the probe is
      rerun; the finalized manifest remains the commit marker.
- [x] Integrate the camera-level listener/connect owner into a production
      recorder executable and bounded supervisor/reap lifecycle.
- [ ] Enable the operational wire-level drain/finalize exchange; the current
      executable uses producer EOF followed by recorder-local drain/finalize
      and intentionally does not negotiate `drain_finalize` yet.
- [x] Give each required ROI an independently bounded runtime queue and
      terminal state. A lane may fail or drop only its own sidecar; it must not
      relabel another ROI. The process and full-frame coexistence composition
      now exists; live four-output validation remains pending.
- [x] Copy the exact Mono8 source into the selected encoder profile. Keep
      alignment padding explicit and zero-filled; do not scale. Map each Mono8
      byte unchanged to NV12 Y and set interleaved UV to neutral 128 (the
      recorder-owned transform is complete). Exact decoded-video luma is a
      requirement only for the legacy lossless profile; the active
      P1/VBR-Q20/GOP-25 profile instead requires decode, cadence, dimensions,
      keyframe placement, and quality sanity. A live finalized-container proof
      remains pending.
- [x] Define and validate ACK/release identity including recording token,
      producer generation, logical stream/ROI identity,
      `recording_frame_id`, and dense `roi_stream_frame_index`.
- [x] Keep recorder encode/mux/disk work outside the acquisition process. The
      source-safe boundary is an accepted detached copy or an explicit
      recorder `RELEASE`, never encode completion inferred by Orange.

### C. Artifacts, arming, and finalization

- [x] Replace the scalar `recording_outputs[serial].crop` assumption with a
      collection keyed by stable ROI/logical stream identity while retaining
      compatibility aliases for the legacy top-one crop output.
- [x] Write one bounded frame-journal/evidence row per accepted ROI frame,
      including source
      identity, ROI identity, native content rectangle, encoded raster/padding,
      and submission outcome. Accepted video indices advance only on queue
      admission.
- [x] Add per-ROI descriptors, counters, summaries, session snapshots,
      portable relative paths, and final status. The output index keys each
      spatial stream by stable logical identity.
- [x] Add final size, packet/frame, SHA-256, and finalization receipts to
      the session manifest, and fail closed on any range or identity
      disagreement. Live media still must exercise and certify this path.
- [ ] Persist worker-entry recycle, camera-return, and recycle-queue failure
      counters in recording/session status before production arming.
- [ ] Persist the complete physical-layout -> camera-materialization ->
      arena-group-binding chain and its portable path/size/SHA-256 evidence in
      the Orange geometry bundle, ROI plan/contract, final session manifest,
      and Citrus H5. Require explicit stable region index/order fields; array
      order alone is not authority.
- [x] Add plan-time arming preflight for camera/raster/layout/registration/
      codec, aggregate pixel rate, encoder/session count, pool/queue bytes, and
      authenticated storage estimates. Invalid or over-budget plans do not
      start.
- [x] Add the descriptor-bound runtime reserved-free-space/storage preflight;
      validate it with deterministic host seams. Live CUDA/NVENC/headless
      acceptance and complete physical geometry-bundle/Citrus H5 receipt
      closure remain pending.

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
