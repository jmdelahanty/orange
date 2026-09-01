# Spatial ROI Headless Completion Checklist

**Date:** 2026-08-31

**Scope:** Orange fixed-region spatial ROI recording from the current committed
foundation to a supervised one-camera/four-ROI headless acceptance run.

**Current checkpoint:** Gate 0 is committed through `22ab8b1`. The reviewed
Gate 1 one-output media/evidence library and test slice is committed as
`157ce0f`, with the real-driver acceptance result recorded below. Gates 2-6 are
not implemented. No current Orange application path can arm this feature or
produce spatial-ROI videos; there is not yet an end-to-end recorder executable.

This is the authoritative execution-order checklist for the first headless
fixed-region slice. The detailed contracts and rationale remain in
`docs/spatial_roi_recording_v1_foundation.md`. A box is checked only when the
code, tests, and named completion evidence all exist; a library or test seam by
itself does not make a later integration gate complete.

## Scope freeze

- [x] The first accepted product is `fixed_region`: four immutable,
      camera-native rectangles from one camera.
- [x] Each ROI is an independently decodable, native-resolution, lossless
      video with its own metadata and finalization evidence.
- [x] The existing full-frame external recorder remains enabled and remains
      the ingest authority.
- [x] Fixed-region production must work with YOLO, pose, and preview disabled.
- [x] Configuration is closed, versioned, optional, and default-off.
- [x] The first ROI recorder topology is one child process per camera owning a
      vector of ROI output cores, not one process per ROI.
- [x] The existing full-frame recorder child remains separate for the first
      acceptance slice.
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

**Status:** the one-output library/test slice and its real-driver acceptance
are complete and reviewed in commit `157ce0f`. Production recorder integration
and the end-to-end executable remain pending.

- [x] Implement one bounded, single-owner lossless NVENC/MP4 output core.
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
      without a skip and prove decoded dimensions, frame count, timestamps,
      keyframes, exact Y bytes, neutral chroma, and complete container
      finalization. Outside-sandbox acceptance: **PASS**, device `0`,
      `256x256`, `3/3` frames decoded, elapsed `1001 ms` on the final hardened
      encoder/writer tree.
- [x] Rebuild `orange`, `orange_client`, and all affected tests; run the full
      spatial-ROI and FFmpeg/NVENC regression set plus `git diff --check`.
- [x] Commit the reviewed media/evidence slice and update this checkpoint with
      its commit and test evidence: `157ce0f`.

**Gate 1 evidence:** the focused encoder target builds and its host portion
passes the artifact-authority, bounded-admission, profile, and terminal-truth
checks. The real-driver acceptance above is the non-skipped CUDA/NVENC/decode
result. The local sandbox run has no CUDA device and therefore reports only
the expected GPU-stage skip; it does not replace the outside-sandbox result.
An immutable successful terminal snapshot additionally requires
`enqueue_attempted == enqueued`, zero rejected admission and queue overflows,
matching dequeue/copy/source-release/result counts, and zero writer media-size
limit failures.
The end-to-end recorder executable is still required before this feature is
merge-ready. The final host regression passed 17 sandbox-compatible tests plus
the expected sandbox-limited Unix-socket test; that socket test then passed
outside the sandbox with real `SO_TYPE`/peer-credential access.

The v2 defaults are 4,000,000 frames, 128 GiB media, and 4 GiB aggregate
evidence per stream. The frame and evidence defaults are also implementation
ceilings. Media is configurable up to `9223372036854775807` bytes, the signed
`off_t` ceiling. Whatever limits the verified plan selects are authenticated,
enforced, and charged against aggregate admission. The encoder's
descriptor-authorized exact artifact set is rooted at
`<recording_root>/external_spatial_roi_recorder` and retains read-write
handles for `Cam<camera_serial>_spatial_roi_<roi_id>.mp4`,
`Cam<camera_serial>_spatial_roi_<roi_id>_meta.csv`,
`Cam<camera_serial>_spatial_roi_<roi_id>_keyframe.json`, and
`Cam<camera_serial>_spatial_roi_<roi_id>.mp4.finalization.json`; it accepts
no arbitrary output or metadata pathname.

## Gate 2: camera-level ROI recorder executable

**Status:** not implemented.

- [ ] Add a dedicated `spatial_roi_recorder` production target.
- [ ] Accept exactly one authenticated camera recorder contract plus retained
      descriptor authority and authenticated expected artifact-root identity
      per process.
- [ ] Define process-boundary recovery by retaining/passing the root directory
      descriptor and expected `(device, inode)` identity, or fail closed under
      a new recording identity.
- [ ] Define the disposition of partial MP4, metadata, keyframe, and
      finalization artifacts after child failure. Gate 1 neither resumes nor
      adopts partial media.
- [ ] Own one logical IPC-v2 handoff state per configured ROI while accepting
      all of that camera's ROI endpoints in one child process.
- [ ] Bind/listen on all planned ROI endpoints in that one child; do not spawn
      a child for each endpoint.
- [ ] Construct the exact verified-plan-ordered vector of output cores. Do not
      infer ROI identity from connection order, detector order, or filenames.
- [ ] Detach each accepted ROI frame before ACK/RELEASE, then enqueue it only to
      its matching output core.
- [ ] Bound aggregate encoder sessions, pixel rate, detach pools, queues,
      device memory, writer memory, storage estimates, and reserved free space
      before reporting ready.
- [ ] Publish readiness, heartbeat, per-ROI counters, first failure, and
      process-wide terminal state.
- [ ] Implement ordered stop-admission, drain, encoder flush, mux close,
      artifact sealing, and final status across all required ROI outputs.
- [ ] Treat an uncertain CUDA source lifetime as terminal: stop intake and
      retain quarantine until the supervisor proves process exit.
- [ ] Keep one ROI's identity and evidence independent when another ROI fails;
      nevertheless mark the required four-ROI recording incomplete.
- [ ] Add synthetic multi-endpoint tests for shuffled connection order,
      pressure in one lane, one encoder failure, drain, finalize, EOF, timeout,
      root-path replacement, partial exclusive media artifacts, and restart.

**Gate completion evidence:** a standalone synthetic driver launches one
recorder child and produces four correctly named, independently decodable,
finalized ROI videos and four validated evidence manifests.

## Gate 3: ROI child supervision and exact lifecycle

**Status:** not implemented.

- [ ] Materialize a supervisor plan only from the independently verified ROI
      plan, recording identity, camera mapping, and owned recording root.
- [ ] Launch exactly one ROI recorder child per enabled camera, with explicit
      PID, contract, endpoint, log, and status ownership.
- [ ] Establish each logical ROI connection and verify post-spawn peer
      credentials before arming acquisition.
- [ ] Activate `drain_finalize` capability negotiation only when both sides
      implement the complete operational exchange; update protocol, parser,
      handoff, and evidence validation together.
- [ ] Require every planned ROI endpoint and output core to report ready before
      the camera-level ROI feature becomes active.
- [ ] Apply bounded readiness, operation, drain, finalize, and exit deadlines.
- [ ] On timeout or protocol uncertainty, stop new acquisition submissions,
      terminate the exact child, reap the exact PID, and only then resolve
      quarantined source ownership.
- [ ] Persist child exit status, signal, timeout, and per-ROI terminal status in
      recording/session status.
- [ ] Start and stop the new ROI child without modifying the existing
      full-frame recorder's protocol or artifact identities.
- [ ] Add fake-child and real-child supervisor tests, including early exit,
      never-ready, partial readiness, hung drain, failed finalize, and clean
      reap.

**Gate completion evidence:** a supervisor integration test proves one child
per camera, four ready ROI products, bounded shutdown, and exact reap under
both success and injected failure.

## Gate 4: production acquisition and headless arming

**Status:** not implemented.

- [ ] Add the optional strict spatial-ROI configuration to the headless
      experiment-spec/session preparation path using the existing schema and
      verified-plan builder.
- [ ] Keep spatial ROI additive to the existing full-frame sink; do not encode
      it as another value of `recording_sink_mode`.
- [ ] Resolve the authoritative physical layout, camera materialization,
      camera serial/native raster, ordered ROI identities, recording token,
      producer generation, and GPU placement before launching the child.
- [ ] Persist the verified plan and exact recorder contract under the owned
      recording root.
- [ ] Arm the acquisition controller only after the full-frame path and every
      required ROI recorder endpoint are ready.
- [ ] Submit one retained authoritative camera source per recording cadence to
      the existing batch extractor from the real acquisition loop.
- [ ] Include the ROI consumer in acquisition dispatch/reference accounting
      before selecting direct versus copied source ownership, then call the
      controller exactly once while the base source reference is held.
- [ ] Generation-tag reusable acquisition entries, or provide an equivalent
      proof that a stale ROI release cannot act on a recycled entry.
- [ ] Keep acquisition nonblocking with explicit bounded outcomes; never wait
      for encode, mux, disk, or finalization on the camera thread.
- [ ] Make fixed-region submission independent of YOLO scheduling and results.
- [ ] Stop admission, drain every ROI lane, perform the IPC drain/finalize
      exchange, and disarm on normal stop, error, signal, or duration limit.
- [ ] Surface configuration, plan, resource, recorder, queue, and finalization
      failures in headless status with no silent fallback to a different ROI
      product.
- [ ] Preserve current behavior byte-for-byte when the feature is absent or
      disabled.
- [ ] Add a dry-run plan/status view that lists the camera child and all four
      output products without opening a camera or creating media.

**Gate completion evidence:** the headless program can dry-run the exact plan
and can enter/leave a real armed four-ROI session through one supervised child
without using a test-only producer.

## Gate 5: session artifacts and fail-closed finalization

**Status:** not implemented.

- [ ] Replace scalar spatial-crop assumptions with a collection keyed by
      camera serial plus stable logical `roi_id`; retain the unrelated legacy
      top-one crop compatibility field.
- [ ] Fix output JSON materialization so several products with the same
      `output_kind` cannot overwrite one another; key spatial outputs by stable
      logical stream/ROI identity.
- [ ] Persist one frame row per admitted ROI frame with complete source
      identity, dense ROI index, content/padding rectangles, detach and
      ACK/RELEASE states, encode outcome, and exact packet mapping.
- [ ] Persist the physical-layout -> camera-materialization -> ArenaGroup/ROI
      binding chain and its relative path, size, and SHA-256.
- [ ] Add every required ROI video, keyframe map, metadata/evidence stream,
      status, media-sanity result, and finalization sidecar to the session
      manifest using exact relative paths, sizes, and SHA-256 values.
- [ ] Require matching counts/ranges, plan digest, recording token, camera,
      region/ROI identity, writer health, child exit, and finalized receipt
      before the recording can be complete.
- [ ] Make the recording finalizer reject a missing, substituted, failed,
      truncated, or unvalidated required ROI while preserving evidence from
      the other lanes.
- [ ] Extend the verifier to consume only manifest-declared ROI products and
      reject fallback discovery by filename.

**Gate completion evidence:** a completed headless recording is accepted only
when the full-frame authority and all four required ROI products have closed,
content-addressed evidence; every injected omission is rejected.

## Gate 6: one-camera/four-fixed-ROI headless acceptance

**Status:** not implemented.

- [ ] Add a versioned one-camera/four-region headless experiment specification
      and a runner that is a dry run unless explicitly given `--execute`.
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
      recording.
- [ ] Exercise normal duration stop, operator/signal stop, recorder early exit,
      and producer/recorder restart boundaries.
- [ ] Measure acquisition latency, GPU copy/encode load, queue high-water,
      storage rate, and finalization duration with all five videos enabled.
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
