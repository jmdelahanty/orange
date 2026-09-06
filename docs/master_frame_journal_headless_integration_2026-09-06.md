# Headless acquisition journal integration

Date: 2026-09-06. Branch: `agent/acquisition/master-frame-journal-v1-20260906`.
Foundation: `4c50e7771c5e67e4b21bb61ade54fe68029a04a7`.

This slice wires the independent journal into **opt-in timed headless recordings**.
It does not enable moving-crop-only recording, change an encoder profile, change
crop geometry, add fixed-region crops, or promote a new Palette/Citrus mapping.
Installed binaries/configs and other Orange worktrees are unchanged.

## Scope and admission

An experiment spec may contain `fixed.master_frame_journal` with:

- `schema_version`: exactly integer `1`;
- `enabled`: boolean, default off when the object is absent;
- `queue_capacity`: integer 2..65536, default 4096;
- `writer_cpu_ids`: unique nonnegative CPU IDs, **required and nonempty when enabled**.

Unknown fields and malformed types refuse. Select housekeeping CPUs from the
actual launch configuration, not acquisition, Citrus rendering, or other reserved
latency-sensitive roles. This slice verifies that the requested writer mask is
applied, but does not infer CPU roles or certify an isolation policy. It does not
require a wrapper allowlist change: this is a parsed experiment-spec field, not a
new exported environment variable. The installed wrapper's binary-path policy
still applies; the new isolated build has not been installed there.

The initial CLI admission requires a timed recording (`record_for_seconds > 0`),
not stream-only. Both existing native and supervised external recording paths
share the acquisition hooks. Rolling clips keep one parent journal; crop-enabled
and full-frame-only configurations use the same source record. No GUI opt-in or
live reattachment/restart is provided in this slice.

Closed candidate schemas:

- `docs/schemas/orange_master_acquisition_config_v1.schema.json`;
- `docs/schemas/orange_master_acquisition_evidence_v1.schema.json`;
- the existing journal descriptor and crop-metadata correspondence v1 schemas.

The evidence schema covers the emitted required-product prearm block and the
parent completion block. This is not a new acquisition-index mapping schema.

## Ownership, admission and stop

`MasterAcquisitionSet` prepares every selected camera's writer before creating
camera threads. It is stored in `CameraControl` and never replaced while threads
run. Acquisition resolves its pointer once, not per frame. The owner remains
alive until after all camera threads join. The prearm block records camera and
parent IDs, producer-instance UUID, generation, exact expected descriptor names,
queue capacity and requested writer CPUs. It enters `recording_snapshot.json`
before `recording_snapshot_start.json` is sealed; runs.json carries the config.

Each acquisition-loop iteration has an RAII admission scope. In the opt-in path,
that scope's active decision governs both full-frame submission eligibility and
recording-frame ID assignment. An iteration admitted before stop may finish; a
later iteration cannot record even if it sampled an old `record_video=true`.
The existing shared recording flag is now atomic. Membership ownership requires
no per-frame shared-pointer copy/refcount, lock, allocation, file I/O or device
operation. The scope itself uses lock-free atomic admission/quiescence flags.

Control requests source stop **before** requesting encoder drain or clearing the
parent folder. A two-second control-side wait allows admitted iterations to
finish. Timeout latches incompleteness, and the run cannot be considered drained
while source admission remains outstanding. The journal is sealed only after
camera join and before native/external parent finalization. Exceptions unwind
the iteration scope; interrupted/thread-failed shutdown seals incomplete.

This does not bound filesystem flush latency or unsafe camera-driver shutdown.
There is no forced thread cancellation. A hung writer can still delay control
finalization; a future watchdog must preserve live resources and fail completion.

## Exactly what is observed

The runtime profile is `headless_acquisition_loop_v1`; the descriptor retains
`caller_submitted_acquisition_facts_v1` as its library-level boundary.

- Warmup/stream-only iterations do not create recording facts.
- Every existing assigned `recording_frame_id` is offered immediately at its
  assignment site, before preprocessing, detection/crop dispatch or encoding.
- Both YOLO-event resource rejection branches retain the received source IDs
  and original timestamps without inventing a recording ID.
- Pre-receive entry/event starvation and receive errors create distinct facts,
  without synthetic frame identities or inferred exposure timestamps.
- The initial active observation and later sampled inactive/active transitions
  emit resume/pause facts. These are **acquisition-observed transitions**, not a
  timestamped GUI command log; a toggle entirely between iterations is not
  reconstructed. Final stop is a lifecycle boundary, not an invented pause row.
- Clip rollover alone emits neither pause nor a new master recording identity.

Original embedded camera timestamps and existing host steady/realtime values
are copied. A failed existing `clock_gettime` leaves realtime absent rather than
an uninitialized scalar. No new per-frame camera/PTP read or latch is introduced.
Hardware-exposure coverage remains `not_certified`.

Overflow retains independent offered/source counts, rejection indices and queue
high-water evidence. The writer never holds an image lease, and source does not
block on journal disk backpressure. CSV/descriptor creation follows the existing
`ScopedFsuid` recording-owner policy under the privileged runner.

## Required-product completion

After camera join, all journals are drained/sealed, including when another camera
fails. Journal incompleteness or seal failure makes the headless run return failure.
The common parent writer reads the immutable prearm declaration (the mutable copy
is only a failed-prearm fallback), verifies expected per-camera descriptor names,
parent/producer/generation identity, complete status, durable-write counters and
CSV byte size/SHA-256. The parent stores exact descriptor/CSV references under
`master_frame_journal`. Missing, failed, symlinked, changed or contradictory required
evidence downgrades a would-be completed parent to failed. Nonterminal parents
record pending. Non-opted-in recordings acquire no new required product.

This is a producer-side completion gate for its declared artifacts, not the deferred
general untrusted external loader or a substitute for JSON Schema-engine validation.
The closed new schemas remain candidate contracts pending downstream agreement.
Journal-complete means durable offered source facts; it does not mean lossless
hardware acquisition, encoded-media success, dense IDs, crop parity or physical
timing certification. The follow-up
[moving-crop metadata completion slice](moving_crop_master_metadata_completion_2026-09-06.md)
now wires the crop correspondence utility into stopped headless finalization;
encoded-media certification and crop-only media selection remain separate.

## Validation and remaining work

`recording_master_acquisition_tests` exercises strict configuration, warmup,
sampled pause/resume, source/drop distinctions, unchanged parent numbering at a
simulated clip boundary, 100 stop-during-frame handshakes, exceptional scope exit,
timeout failure, two-camera required-product completion, missing/changed/symlinked
evidence, contradictory counts/identity, interruption and non-opted-in behavior.
It uses synthetic scalar frames, no camera or CUDA calls.

- [x] Prearm immutable ownership and headless acquisition hooks.
- [x] Source-stop acknowledgement before media drain; seal after camera join.
- [x] Required-product declaration and shared parent failure propagation.
- [x] Versioned strict opt-in configuration and candidate evidence schemas.
- [ ] Live headless validation of native/external/rolling paths and source-fact
  coverage; CPU tests/builds are not a hardware performance result.
- [ ] General GUI pause/rearm lifecycle and explicit timestamped control events.
- [ ] Media-policy separation: full-frame-only, full-frame plus moving crops,
  registered context plus moving crops without continuous full-frame encoding.
- [ ] Reuse accepted context capture from the ROI branch without unrelated merge.
- [x] Wire master-to-moving-crop metadata coverage and real inference-status
  checks into the opt-in headless external-crop finalizer.
- [x] Enforce full-rate detector/crop admission for that metadata profile; reject
  missing crop rows and source tails against the independent master.
- [ ] Join final recorder-returned identities and container/media checks into
  the master-to-crop proof, including native writer failure propagation.
- [ ] Reviewed external loader, consumer/profile agreement and transfer inventory.
- [ ] Controlled moving-crop-only headless run, including measured scheduling,
  queue occupancy, drops and latency; then consider deployment/defaults.

See [the foundation contract](master_frame_journal_first_slice_2026-09-06.md) and
Citrus `docs/detection_crop_only_master_frame_record_plan_2026-09-06.md`.

Validation of the acquisition slice (`8c6de2a`) in the isolated
`/tmp/orange-timing-build-20260906` build (follow-up results are in the link above):

- GUI `orange` and headless `orange_client` builds passed with the installed toolchain.
- Nine focused CTest suites passed: journal, acquisition integration, packet
  telemetry, sampled timing evidence, rolling metadata projection, acquisition
  index authority, session manifests, GUI finalizer and session status.
- `tools/test_headless_master_journal_spec.py --orange-client <isolated binary>`
  passed 12 admission cases using only `--validate-experiment-spec`; it verifies
  that validation never enters recording/output creation. Its CPU 0 is a parser
  fixture, not a recommended live writer placement.
- AddressSanitizer/UndefinedBehaviorSanitizer integration build and six groups
  passed outside the sandbox, including leak checking. The initial sandbox run
  passed the groups but LeakSanitizer could not finish under ptrace.
- New schemas parse; ECMA regex checks reject trailing newlines; `git diff --check`
  passed. A full JSON Schema engine remains unavailable and is not claimed.
- No cameras, live encoding, PTP services, installed wrappers or runtime configs
  were changed or exercised. No hardware performance or downstream acceptance
  claim follows from these checks.
