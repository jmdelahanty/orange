# Spatial ROI Headless Completion Checklist

**Date:** 2026-08-31

**Last updated:** 2026-09-02

**Scope:** Orange fixed-region spatial ROI recording from the current committed
foundation to a supervised one-camera/four-ROI headless acceptance run.

**Current checkpoint:** Gate 0 is committed through `22ab8b1`. The reviewed
Gate 1 one-output media/evidence library and test slice is committed as
`157ce0f`. On 2026-09-02, the current integrated tree passed the real
CUDA/NVENC/decode checks for both legacy lossless/GOP-1 and 51-frame
P1/VBR-Q20/GOP-25 output. The current uncommitted work now includes the
camera-level four-stream recorder executable, bounded exact-PID child
supervisor, producer coordinator, headless session owner, optional
`orange_client` acquisition/finalization wiring, finalized session receipts,
strict snapshot/manifest closure, and a read-only offline acceptance verifier.
CMake registers the recorder, core-link, process-supervisor, headless-session,
and component test targets. The implementation and host seams are therefore
present and build-targeted. The four Unix transport/listener/connector/process
tests pass on the host outside the restricted sandbox. No live-camera,
four-output headless artifact run is claimed. The remaining gates below
distinguish implemented host behavior from pending live artifact evidence and
the deliberately deferred operational protocol details.
The versioned camera-2010096 diagnostic plumbing smoke spec and its
repo-relative dry-run runner are now present and orange_client-validated; the
first combined smoke uses the supervised external full-frame sink so summed
storage admission is available. They do not constitute physical geometry or
live artifact acceptance.

This is the authoritative execution-order checklist for the first headless
fixed-region slice. The detailed contracts and rationale remain in
`docs/spatial_roi_recording_v1_foundation.md`. A box is checked only when the
code, tests, and named completion evidence all exist; a library or test seam by
itself does not make a later live-artifact gate complete. The deferred
translation-unit extraction plan is recorded in
`docs/orange_headless_client_modularization_design_2026-09-01.md`; it is not
part of this feature's implementation scope.

## Scope freeze

- [x] The first accepted product is `fixed_region`: four immutable,
      camera-native rectangles from one camera.
- [x] Each ROI is an independently decodable, native-resolution video using an
      explicit immutable encoder profile, with its own metadata and
      finalization evidence. The active diagnostic profile is
      P1/VBR-Q20/GOP-25; legacy lossless/GOP-1 remains separately supported.
- [x] The full-frame external recorder remains a first-class supported product,
      may run on its own, and remains the ingest/context authority when ROI
      products are also enabled.
- [x] Fixed-region production must work with YOLO, pose, and preview disabled.
- [x] Configuration is closed, versioned, optional, and default-off.
- [x] The first ROI recorder topology is one child process per camera owning a
      vector of ROI output cores, not one process per ROI.
- [x] The full-frame recorder child remains separate and fully supported for
      the first acceptance slice; spatial ROI is an additive product, not a
      replacement or compatibility fallback.
- [x] Packed/lossless atlas output is out of scope. Do not add an atlas schema,
      fallback, or implementation in this slice.
- [x] `subject_follow`, crop-only authority, per-region pose, and live
      multi-detection routing are later gates and cannot delay fixed-region
      acceptance.
- [x] Merge and deployment remain blocked until the acceptance gates below
      pass.

The target process topology for one camera is:

```text
headless runner
  -> orange_client acquisition/session process
       -> existing full-frame recorder child
            -> N split-GOP encoder sessions -> one full-frame video
       -> new spatial_roi_recorder child
            -> fixed ROI 0 output core -> one video + evidence
            -> fixed ROI 1 output core -> one video + evidence
            -> fixed ROI 2 output core -> one video + evidence
            -> fixed ROI 3 output core -> one video + evidence
```

An output core is an in-process encoder/mux/evidence object. It is not a
process. The v1 checklist plans a dedicated `spatial_roi_recorder` executable
so the ROI-v2 protocol cannot be confused with the existing positional
full-frame protocol. Shared CUDA, NVENC, FFmpeg, finalization, and utility
libraries should be reused.

## Gate 0: committed extraction and ownership foundation

**Status:** complete.

- [x] Strict closed configuration and verified-plan schemas.
- [x] Deterministic plan materialization, canonical digest, geometry identity,
      and aggregate resource admission.
- [x] One bounded CUDA batch extraction from a retained camera source into all
      configured fixed ROIs without resize or lossy conversion.
- [x] Independent bounded ROI lanes with dense, lane-owned stream indices.
- [x] Acquisition bridge and arm/disarm/drain controller library.
- [x] ROI-specific frame descriptor, CUDA IPC exporter, and closed IPC-v2
      grammar.
- [x] Exact HELLO/FRAME/ACK/RELEASE ownership and terminal
      drain/finalize message state machines.
- [x] Bounded adopt-only Unix transport.
- [x] Recorder-owned CUDA detach into packed Mono8 and NV12, including timeout
      quarantine and source-release safety.
- [x] Real-driver tests for extraction, ownership, IPC export, and detach.

**Completion evidence:** Orange commits `c423ad5` through `22ab8b1` on
`agent/acquisition/spatial-roi-recording-v1-20260830`.

## Gate 1: stabilize one-output media and evidence core

**Status:** the one-output library/test slice is complete and reviewed in
commit `157ce0f`; the current integrated tree's own real-driver result passed
on 2026-09-02. Descriptor-bound video-sanity/evidence-v2,
camera-level recorder, and headless integration are present in the current
uncommitted work. A live-camera, four-output headless result remains pending.

- [x] Implement one bounded, single-owner, versioned NVENC/MP4 output core.
- [x] Preserve a dense media timeline separately from sparse source recording
      identities.
- [x] Implement strict writer failure latching, idempotent encoder EOS, CUDA
      context-stack safety, and destination quarantine after an uncertain
      copy.
- [x] Implement streaming JSONL frame evidence and a finalized artifact
      manifest without whole-recording memory growth.
- [x] Add a per-frame encode-completion result/callback that supplies packet,
      timestamp, keyframe, and byte-count to the matching
      `SpatialRoiRecorderFrameEvidence`; the immutable terminal writer
      snapshot is consumed alongside those results, so enqueue/global
      statistics are insufficient.
- [x] Bind media/evidence paths and every detach, encode, writer, and storage
      budget into the authenticated strict recorder contract.
- [x] Create media and sidecar files descriptor-relatively beneath the owned
      artifact root with no symlink traversal; the encoder receives no caller
      pathname authority and consumes only retained authorized descriptors.
- [x] Require an independently authenticated parsed recorder contract,
      authoritative recording root, runtime GPU mapping, and exact expected
      artifact map before evidence can open.
- [x] Validate the exact required artifact set and paths. Compare the real
      video size with the finalization sidecar, validate keyframe structure,
      and require bounded media-sanity evidence.
- [x] Use stable descriptor-relative file descriptors for hash and parse;
      eliminate pathname-reopen, mutable-staging-name, root-inode-swap, and
      same-size mutation races.
- [x] Validate and publish descriptor-relatively while retaining the original
      `SpatialRoiRecorderArtifactRoot` authority.
- [x] Publish evidence JSONL and its manifest with no-replace, descriptor-bound
      operations and support exact adoption between those two publication
      steps.
- [x] Enforce exact descriptor geometry/profile, one-based external output
      index semantics, ACK/RELEASE truth tables, nonencoded packet rules, and
      checked cumulative counters.
- [x] Consume an authoritative terminal encoder/writer failure snapshot rather
      than a caller-provided packet-write boolean.
- [x] Reject duplicate JSON keys while retaining streaming, bounded-memory
      validation.
- [x] Canonicalize finalization inputs so an identical retry is idempotent
      regardless of artifact order or an empty-versus-`complete` reason.
- [x] Run `spatial_roi_lossless_encoder_tests` on the real CUDA/NVENC driver
      without a skip. For the active P1/GOP-25 profile prove decoded
      dimensions, frame count, timestamps, exact IDR cadence, media sanity,
      and complete container finalization; retain exact Y/neutral-chroma proof
      for the legacy lossless profile. The 2026-09-02 host run passed the
      legacy three-frame lossless proof and a 51-frame P1/GOP-25 proof with
      IDRs exactly at zero-based frames 0, 25, and 50.
- [x] Rebuild `orange`, `orange_client`, and all affected tests against the
      current integrated tree; run the full spatial-ROI and FFmpeg/NVENC
      regression set plus `git diff --check`. The 2026-09-02 integrated build
      passed; 46 unrestricted focused tests passed in the sandbox and the four
      AF_UNIX/process tests passed on the host, in addition to the real-driver
      encoder test and 62 offline-verifier tests.
- [x] Commit the reviewed media/evidence slice and update this checkpoint with
      its commit and test evidence: `157ce0f`.

**Gate 1 evidence:** the focused encoder target and host portion cover the
artifact-authority, bounded-admission, profile, and terminal-truth checks. A
non-skipped CUDA/NVENC/decode run for the current integrated tree passed on
2026-09-02 for both the legacy lossless/GOP-1 profile and the active
P1/VBR-Q20/GOP-25 profile. This proves the encoder path, not a live camera or
four-stream headless recording.
An immutable successful terminal snapshot additionally requires
`enqueue_attempted == enqueued`, zero rejected admission and queue overflows,
matching dequeue/copy/source-release/result counts, and zero writer media-size
limit failures.
The end-to-end recorder executable is now present, but it is not merge-ready
until its current integrated tree is rebuilt and the live child/artifact gates
pass. The historical Gate 1 host regression and any earlier outside-sandbox
socket result do not substitute for that current integrated validation.

The active configuration and materialized plan are strict schema v3 and bind
one of three complete immutable encoder profiles. The unpublished schema-v2
lossless configuration/plan remains an explicit legacy parser path. The recorder
contract is now schema v5 with mode
`spatial_roi_external_recorder_v5`; its embedded wire IPC remains version 2.
The unpublished v4 lossless contract remains an explicit legacy parser path.
Schema v5 retains the v4 authenticated detach-pool budgets and versioned,
nonzero reserved-free-space policy while adding the complete selected-profile
projection. Each v5 profile explicitly carries effective `aq=false`,
`temporal_aq=false`, `lookahead=false`, and `lookahead_depth=0`; the runtime
NVENC configuration and retained metadata must match those values. Legacy v4
omits these fields and retains the historical off/off/off/zero inference. The
default bounds are 4,000,000 frames,
128 GiB media, and 4 GiB aggregate evidence per stream. The frame and evidence
defaults are also implementation ceilings. Media is configurable up to
`9223372036854775807` bytes, the signed `off_t` ceiling. Whatever limits the
verified plan selects are authenticated, enforced, and charged against
aggregate admission. The encoder's
descriptor-authorized exact artifact set is rooted at
`<recording_root>/external_spatial_roi_recorder` and retains read-write
handles for `Cam<camera_serial>_spatial_roi_<roi_id>.mp4`,
`Cam<camera_serial>_spatial_roi_<roi_id>_meta.csv`,
`Cam<camera_serial>_spatial_roi_<roi_id>_keyframe.json`, and
`Cam<camera_serial>_spatial_roi_<roi_id>.mp4.finalization.json`; it accepts
no arbitrary output or metadata pathname.

The v5 `storage_preflight_policy` (retained from v4) is itself closed and versioned. Its
`reserved_free_bytes` value is explicit and nonzero (currently 500,000,000,000
bytes). Before the child emits `ready`, it queries `fstatvfs` on the retained
artifact-root descriptor and requires available block-derived bytes to cover
`aggregate_bounds.max_media_bytes_total +
aggregate_bounds.max_evidence_bytes_total + reserved_free_bytes`. The ready,
heartbeat, and terminal JSONL payloads retain the policy, exact artifact-root
device/inode, raw block observation, capacity/available bytes, each budget,
required bytes, and pass/fail result. The camera owner rejects readiness when
that attached result is absent, unchecked, failed, or does not match the
authenticated policy and aggregate budgets. This is host-side evidence only;
it does not certify a live CUDA/NVENC recording.

## Gate 2: camera-level ROI recorder executable

**Status:** implemented as a bounded, four-stream production executable and
core library in the current uncommitted worktree. The executable and focused
host tests are CMake-registered/build-targeted. A live child with four socket
connections, CUDA/NVENC output, and finalized artifacts has not been accepted.

- [x] Add the strict, one-shot `AF_UNIX` listener primitive with exact path,
      mode `0600`, pre-existing-path refusal, bounded accept, peer-credential
      propagation, retained private-parent descriptor, and inode-aware cleanup.
      The executable consumes this seam. The restricted sandbox cannot create
      these sockets, but the transport, listener, connector, and process tests
      pass on the host outside it.
- [x] Create the recording-token-derived, euid-owned mode-0700 socket runtime
      directory with retained descriptor/device/inode authority and exact
      plan-derived stream leaves. Cleanup is fail-closed and scoped to that
      directory.
- [x] Add the bounded, single-attempt producer connector for an exact
      authenticated socket path, including socket ownership/mode and
      post-connect peer-credential checks. It does not retry, reconnect,
      unlink, or launch a process.
- [x] Add the authenticated one-camera/four-fixed-region contract view and
      deterministic plan-ordered camera-owner library. Its injected stream
      cores cover aggregate readiness, EOF, stop-admission, drain, finalize,
      per-stream failure, and process-wide fail-closed state; the production
      executable composes this owner with the concrete stream cores.
- [x] Add the recorder-side per-stream IPC session seam with bounded HELLO,
      FRAME, ACK/RELEASE, EOF, terminal-error, dispatch, and ownership truth;
      it owns neither the listener nor CUDA/encoder resources.
- [x] Add the bounded frame journal and transport bridge that joins retained
      transport outcomes to asynchronous encoder results without retaining
      CUDA handles or performing blocking evidence I/O in the encoder callback.
- [x] Add closed, non-certifying terminal candidate sidecars and a finalized
      manifest commit-marker rule. Partial sidecar sets remain non-resumable
      residue and do not certify a recording.
- [x] Add descriptor-bound video-sanity results and evidence-v2 validation:
      the initial complete path requires the non-forgeable probe capability,
      while offline manifest validation is consistency-only unless the probe
      is rerun. Validate cadence, PTS, duration, profile, decoded dimensions,
      samples, size, and SHA-256 under the retained artifact authority.
- [x] Add a dedicated `spatial_roi_recorder` production target and core-link
      test target.
- [x] Accept exactly one authenticated camera recorder contract plus retained
      descriptor authority and authenticated expected artifact-root identity
      per process.
- [ ] Rebuild and run the complete current target outside the sandbox; the
      available sandbox cannot establish the required Unix credential/listener
      behavior and is not live artifact evidence.
- [ ] Define process-boundary recovery by retaining/passing the root directory
      descriptor and expected `(device, inode)` identity, or fail closed under
      a new recording identity.
- [ ] Define the disposition of partial MP4, metadata, keyframe, and
      finalization artifacts after child failure. Gate 1 neither resumes nor
      adopts partial media.
- [x] Own one logical IPC-v2 handoff state per configured ROI while accepting
      all of that camera's ROI endpoints in one child process.
- [x] Bind/listen on all planned ROI endpoints in that one child; do not spawn
      a child for each endpoint.
- [x] Construct the exact verified-plan-ordered vector of output cores. Do not
      infer ROI identity from connection order, detector order, or filenames.
- [x] Detach each accepted ROI frame before ACK/RELEASE, then enqueue it only to
      its matching output core.
- [x] Bound aggregate encoder sessions, pixel rate, detach pools, queues,
      device memory, writer memory, and authenticated storage estimates before
      reporting ready.
- [x] Add the descriptor-bound live reserved-free-space/storage preflight
      before reporting ready; live-camera four-output headless acceptance
      remains pending.
- [x] Publish readiness, heartbeat, per-ROI counters, first failure, and
      process-wide terminal state.
- [x] Implement ordered stop-admission, drain, encoder flush, mux close,
      artifact sealing, and final status across all required ROI outputs.
- [x] Treat an uncertain CUDA source lifetime as terminal: stop intake and
      retain quarantine until the supervisor proves process exit.
- [x] Keep one ROI's identity and evidence independent when another ROI fails;
      nevertheless mark the required four-ROI recording incomplete.
- [x] Add deterministic host tests for plan order, aggregate readiness/EOF,
      one-lane failure, drain/finalize ordering, and bounded lifecycle teardown.
- [ ] Add an integrated multi-endpoint synthetic/live test for shuffled
      connection order, pressure in one lane, root-path replacement, partial
      exclusive media artifacts, and restart.

**Gate completion evidence:** the executable/core and deterministic host seams
exist, but Gate 2 is not accepted until a standalone driver launches one
recorder child and produces four correctly named, independently decodable,
finalized ROI videos and four validated evidence manifests on the live
CUDA/NVENC/headless path.

## Gate 3: ROI child supervision and exact lifecycle

**Status:** the bounded process supervisor and headless session lifecycle are
implemented and have deterministic fake-child/factory seams. The live
four-socket/IPC/CUDA child lifecycle and clean artifact acceptance remain
pending.

- [x] Materialize a supervisor plan only from the independently verified ROI
      plan, recording identity, camera mapping, and owned recording root.
- [x] Launch exactly one ROI recorder child per enabled camera, with explicit
      PID, contract, endpoint, log, and status ownership.
- [x] Establish each logical ROI connection and verify post-spawn peer
      credentials before arming acquisition.
- [ ] Activate `drain_finalize` capability negotiation only when both sides
      implement the complete operational exchange; update protocol, parser,
      handoff, and evidence validation together.
- [x] Require every planned ROI endpoint and output core to report ready before
      the camera-level ROI feature becomes active.
- [x] Apply bounded readiness, operation, EOF, finalize, and exit deadlines.
- [x] On timeout or protocol uncertainty, stop new acquisition submissions,
      terminate the exact child, reap the exact PID, and only then resolve
      quarantined source ownership.
- [x] Persist child exit status, signal, timeout, and per-ROI terminal status in
      recording/session status.
- [x] Start and stop the new ROI child without modifying the existing
      full-frame recorder's protocol or artifact identities.
- [x] Add fake-child supervisor tests, including early exit/never-ready,
      bounded escalation, malformed/overdeep/duplicate lifecycle JSON, and
      exact reap.
- [ ] Add a live-child supervisor test covering partial readiness, hung drain,
      failed finalize, clean reap, and four finalized artifacts.

**Gate completion evidence:** host tests prove the bounded fake-child/session
ordering and exact reap behavior. Gate 3 remains open until a live-child
supervisor integration test proves one child per camera, four ready ROI
products, bounded shutdown, and exact reap under both success and injected
failure.

## Gate 4: production acquisition and headless arming

**Status:** the optional default-off headless path is implemented for the
first one-camera/four-fixed-ROI slice and is wired alongside the existing
full-frame recorder. Host/build seams exist, but no live armed headless run or
ROI artifact set is accepted yet.

- [x] Add the optional strict spatial-ROI configuration to the headless
      experiment-spec/session preparation path using the existing schema and
      verified-plan builder.
- [x] Keep spatial ROI additive to the existing full-frame sink; do not encode
      it as another value of `recording_sink_mode`.
- [x] Resolve the authoritative physical layout, camera materialization,
      camera serial/native raster, ordered ROI identities, recording token,
      producer generation, and GPU placement before launching the child.
- [x] Persist the verified plan and exact recorder contract under the owned
      recording root.
- [x] Arm the acquisition controller only after the full-frame path and every
      required ROI recorder endpoint are ready.
- [x] Submit one retained authoritative camera source per recording cadence to
      the existing batch extractor from the real acquisition loop.
- [x] Include the ROI consumer in acquisition dispatch/reference accounting
      before selecting direct versus copied source ownership, then call the
      controller exactly once while the base source reference is held.
- [ ] Generation-tag reusable acquisition entries, or provide an equivalent
      proof that a stale ROI release cannot act on a recycled entry.
- [x] Keep acquisition nonblocking with explicit bounded outcomes; never wait
      for encode, mux, disk, or finalization on the camera thread.
- [x] Make fixed-region submission independent of YOLO scheduling and results.
- [x] Stop admission, drain every ROI lane, and disarm on normal stop, error,
      signal, or duration limit. The current child observes producer EOF before
      local drain/finalize; operational wire-level `DRAIN`/`FINALIZE`
      negotiation remains pending in Gate 3.
- [x] Surface configuration, plan, resource, recorder, queue, and finalization
      failures in headless status with no silent fallback to a different ROI
      product.
- [x] Publish ROI output state and `session.spatial_roi_recording` in one
      recording-snapshot transaction. If preparation fails after the plan,
      contract, and their exact byte receipts are authenticated, retain a
      bounded non-certifying preparation-failure record; do not fabricate a
      recording identity for earlier failures.
- [x] Preserve current behavior byte-for-byte when the feature is absent or
      disabled.
- [x] Keep the full-frame router, protocol, recorder child, artifact names,
      lifecycle, and finalization evidence first-class and independent; the
      spatial-ROI controller may observe the same retained acquisition source
      but must not become an authority, replacement, or fallback for the
      full-frame route.
- [x] Support the combined sink matrix without changing full-frame-only
      behavior. The first combined product requires
      `recording_sink_mode=external_ipc` with an enabled, supervised external
      recorder contract. The external full-frame child and the spatial ROI
      child are independently supervised and their finalized products are
      joined in one terminal manifest after both have stopped. The existing
      `recording_sink_mode=real` path remains first-class for full-frame-only
      runs and is deferred for combined storage admission until it has an
      equally authenticated summed capacity gate.
- [x] In the combined external-IPC product, materialize the full-frame
      recorder's configured artifact leaf names below the active recording root
      and publish them as normalized recording-root-relative paths. Require
      nonempty regular full-frame video, metadata, and keyframe files; a
      contract whose finalized artifacts cannot be safely bound to that root
      fails closed rather than publishing absolute or escaping paths.
- [x] Before arming the spatial child, run one parent-level storage admission
      over the external full-frame plan plus the authenticated aggregate ROI
      media/evidence bound. Require one healthy shared output filesystem and
      persist `combined_storage_preflight.json`; each child still retains its
      own authenticated preflight and lifecycle checks.
- [x] Cover the explicit product matrix: full-frame-only remains supported;
      full-frame plus fixed-region ROI is the first combined product; future
      subject-follow products remain additive. Crop-only authority is not
      implied by any ROI configuration in this slice.
- [x] Add a dry-run plan/status view that lists the camera child and all four
      ROI products alongside the first-class full-frame product without
      opening a camera or creating media. The
      versioned diagnostic spec
      `experiment_specs/2010096_spatial_roi_diagnostic_plumbing_100fps_gpu5_v1.json`
      and runner `scripts/run_spatial_roi_diagnostic_2010096.sh` validate and
      print this plan by default; `--execute` is required for hardware/media.
      The selected camera is serial 2010096 with runtime/Shaman `camera_id: 3`
      in the normal four-camera rig inventory (2010093..2010096 → 0..3).
      The 3-second/1-second-warmup smoke retains full-frame recording and uses
      the durable local acquisition root only when explicitly executed.
      Its closed/versioned camera-native layout, materialization, and
      registration authorities are checked in under
      `experiment_specs/spatial_roi_diagnostic_authority/`, with exact-byte
      SHA-256 references and `diagnostic_not_physical_acceptance` status.

**Gate completion evidence:** the source path can prepare and enter/leave the
armed lifecycle through one supervised child without a test-only producer.
Gate 4 remains open until the dry-run view and a live headless run produce the
full-frame plus four-ROI artifact set with the required evidence.

## Gate 5: session artifacts and fail-closed finalization

**Status:** the additive collection-valued ROI output descriptors, snapshot
updates, frame journal, content-addressed finalized receipts, atomic terminal
manifest publication, and offline acceptance verifier are implemented in the
current worktree. Physical geometry-bundle/Citrus closure and live fail-closed
artifact acceptance remain pending.

- [x] Replace scalar spatial-crop assumptions with a collection keyed by
      camera serial plus stable logical `roi_id`; retain the unrelated legacy
      top-one crop compatibility field.
- [x] Fix output JSON materialization so several products with the same
      `output_kind` cannot overwrite one another; key spatial outputs by stable
      logical stream/ROI identity.
- [x] Keep full-frame completion independent from the aggregate ROI result:
      it requires its own non-empty frame/packet/path evidence plus successful
      start and drain, and remains finalized when only an ROI product fails.
- [x] Persist one bounded frame-journal/evidence row per admitted ROI frame
      with complete source identity, dense ROI index, content/padding
      rectangles, detach and ACK/RELEASE states, encode outcome, and exact
      packet mapping.
- [ ] Prove those rows and all required outputs through a live finalized
      artifact set; host journal tests do not establish media acceptance.
- [ ] Persist the physical-layout -> camera-materialization -> ArenaGroup/ROI
      binding chain and its relative path, size, and SHA-256.
- [x] Add every required ROI video, keyframe map, metadata/evidence stream,
      status, media-sanity result, and finalization sidecar to the session
      manifest using exact relative paths, sizes, and SHA-256 values.
- [x] Require matching counts/ranges, plan digest, recording token, camera,
      region/ROI identity, writer health, child exit, and finalized receipt
      before the recording can be complete.
- [x] Make the recording finalizer reject a missing, substituted, failed,
      truncated, or unvalidated required ROI while preserving evidence from
      the other lanes.
- [x] Extend the verifier to consume only manifest-declared ROI products and
      reject fallback discovery by filename.
- [x] Authenticate the exact normalized config, canonical verified plan, and
      recorder contract; bind the plan's resolved four-ROI camera geometry,
      order, padding, sockets, artifacts, GPU assignments, per-stream limits,
      and recomputed aggregate limits to every terminal descriptor.
- [x] Recompute and enforce all nine verified-plan admission metrics and every
      configured ceiling; require one camera-level `arena_group_id`, reject
      disabled-overlap rectangle intersections, and enforce the authenticated
      per-stream frame ceiling in terminal receipts.
- [x] Validate the complete closed schema-v4 process/IPC authority, including
      exact stream fields, ACK/RELEASE ownership, queue/detach/writer bounds,
      non-rolling control, child wrapper/raw-payload agreement, nonzero child
      transcript evidence, and latest-event equality with terminal completion.
- [x] Require the exact production evidence-manifest schema and authenticated
      binding rather than accepting an arbitrary self-digesting JSON object;
      recompute its finalization-request and finalized-receipt digests and
      require clean encoder terminal state plus exact counts/ranges/artifacts.
- [x] Require identical source-frame coverage across all four ROI lanes and
      the first-class full stream, with dense full-frame metadata proof.
- [x] Enforce global path/inode disjointness across authority, ROI, and
      full-frame video/metadata/keyframe artifacts, and require each declared
      full-frame artifact to exist as a unique nonempty regular file.
- [x] Require terminal full-frame authority to agree across the v3 index,
      schema-2 manifest/snapshot compatibility index, snapshot encoder view,
      `camera_artifacts`, and the single-clip artifact/output projections;
      reject any stale or substituted duplicate.
- [x] Couple the manifest/single-clip roots and session identity to the opened
      folder; validate full-frame backend/container/coordinate/packet
      provenance and accept the established in-process three-column metadata
      format by treating `frame_id` as recording identity when no explicit
      `recording_frame_id` alias exists.
- [x] Require the exact checked/passed storage-preflight result in both ready
      and terminal lifecycle evidence and bind it to the finalized receipt's
      recording/artifact-root identities and authenticated aggregate budgets.
- [x] Publish `recording_snapshot.json` and the terminal
      `recording_session.json` through same-directory atomic replacement; only
      a fully validated complete session may carry the complete commit marker.

**Gate completion evidence:** a completed headless recording is accepted only
when the full-frame authority and all four required ROI products have closed,
content-addressed evidence; every injected omission is rejected.

## Gate 6: one-camera/four-fixed-ROI headless acceptance

**Status:** pending live acceptance. The first integrated 2010093 attempt on
2026-09-01 proved the full-frame path and reached all four ROI encoders, but it
is intentionally rejected: the full-frame recorder finalized 401 frames while
the four ROI lanes stopped at 17-18 frames after the evidence journal reached
its 64-entry pending bound. The recorder control loop had deferred journal
draining until EOF. Continuous owner-thread draining and a 128-frame bounded
journal regression now cover that defect; a new live run is still required.

- [x] Add a versioned one-camera/four-region headless experiment specification
      and a runner that is a dry run unless explicitly given `--execute`.
      The camera-perspective quadrant rectangles are diagnostic
      plumbing/encoder-validation geometry only, not accepted physical
      compartment geometry; the spec keeps full-frame recording enabled and
      disables YOLO, pose, and display. The live run remains pending below.
- [ ] Run a full-frame-only control and prove the established router's
      protocol, artifacts, frame accounting, and finalization evidence are
      unchanged when the optional ROI configuration is absent and disabled.
- [ ] Run first with YOLO, pose, and preview disabled while retaining the
      existing full-frame external recorder.
- [ ] Verify one full-frame output and exactly four manifest-declared ROI
      videos with the planned identities and dimensions.
- [ ] Decode all ROI videos and verify frame/packet counts, dense media
      indices, source recording-frame joins, timestamps, keyframes, padding,
      media sanity, finalization status, and SHA-256 receipts.
- [ ] Require zero camera gaps, source-ownership violations, incomplete ROI
      batches, queue drops, encoder failures, mux failures, and unresolved
      children.
- [ ] Inject a failure in one ROI and prove bounded shutdown, preserved
      identities/evidence for the other outputs, and an incomplete aggregate
      recording without relabeling or corrupting the separately finalized
      full-frame product.
- [ ] Exercise normal duration stop, operator/signal stop, recorder early exit,
      and producer/recorder restart boundaries.
- [ ] Measure acquisition latency, GPU copy/encode load, queue high-water,
      storage rate, and finalization duration with all five videos enabled.
- [x] Inventory the steady-state thread topology. One camera uses four
      producer lane workers plus, in one camera-level child, four IPC-session,
      four encoder-owner, four FFmpeg-writer, and one control thread. The
      evidence journal does not own a thread, and the continuous-drain fix
      reuses the existing child control/polling thread.
- [x] Reconcile the current Orange/Citrus CPU allocation and kernel state.
      Citrus pins render/update to CPUs 1/2, Shaman readers from 20, and arena
      workers to 24-27; Orange pins YOLO to 6/8/10/12. The kernel currently
      isolates `1,2,6,8,10,12,38,40,42,44`. New ROI threads are
      process-isolated but not explicitly pinned.
- [x] Add an optional, default-inherit camera-recorder multi-core mask with
      strict parsing, pre-worker application, exact effective-mask readback,
      closed v1 lifecycle metadata, and ready/terminal parity validation.
      Keep `SCHED_OTHER`. The headless env resolution and bounded sudo-wrapper
      source option are wired; installing the updated root-owned wrapper is a
      separate deployment action.
- [ ] Add coordinated-profile admission that rejects overlap with Citrus,
      YOLO, their SMT siblings, housekeeping, and another recorder allocation.
      The generic child deliberately validates syntax/effectiveness without
      embedding this rig's CPU topology.
- [ ] Persist thread roles/names, scheduling policy, per-lane/encoder/writer
      queue high-water, journal pending high-water and drain latency, and
      per-role CPU/context-switch evidence needed to diagnose scheduling
      jitter without adding blocking work to acquisition or callbacks.
- [ ] Compare inherited scheduling with the optional recorder cpuset in a
      controlled one-camera run before changing the default or expanding
      kernel-isolated CPUs. Require unchanged camera/detect hot-path tails and
      exact full-frame/four-ROI parity.
- [ ] Record the accepted artifact paths, configuration/plan digests, hardware,
      build commits, and measured headroom in a dated validation report.

**Fixed-region v1 completion:** Gate 6 passes without waivers. Only then may a
four-camera scale run be planned; a successful one-camera run does not by
itself authorize deployment.

## Gate 7: later subject-follow product

**Status:** deliberately deferred until fixed-region Gate 6 passes.

- [ ] Validate a detector engine with bounded capacity sufficient for every
      occupied region; the currently deployed top-1 engine is insufficient.
- [ ] Require complete, nontruncated terminal multi-box observations and
      deterministic accepted region assignment.
- [ ] Define a versioned moving-window policy with fixed encoded dimensions,
      clamping/padding, bounded hold, and explicit `detected`, `held`,
      `unlocalized`, `ambiguous`, and `technical_failure` outcomes.
- [ ] Bind every moving source rectangle and selection decision to acquisition,
      detector, geometry, region, and ROI identity.
- [ ] Allow `subject_follow` independently of `fixed_region`; recording both is
      explicit and charged against encoder, memory, and storage admission.
- [ ] Reuse the camera-level ROI child and output-core substrate without
      introducing one process per subject.
- [ ] Add a separate headless acceptance run with shuffled detections, misses,
      false positives, boundary ambiguity, holds, and restart.

Atlas output remains out of scope for this gate as well.

## Cross-repository completion boundary

The Orange fixed-region headless acceptance does not require live
multi-detection routing. Full grouped-experiment readiness additionally
requires:

- a validated multi-output detector instead of the deployed top-1 engine;
- Orange SHAMAN completeness and restart semantics;
- Citrus shared-reader, deterministic region routing, and per-region protocol
  validation; and
- Citrus H5 references to the exact finalized Orange ROI media and geometry
  receipts.

Those dependencies are tracked in the Citrus ArenaGroup documents. They must
not be used to postpone the detector-independent fixed-region headless slice.
