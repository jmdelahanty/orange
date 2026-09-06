# Master-frame journal and moving-crop correspondence: first slice

Date: 2026-09-06. Branch: `agent/acquisition/master-frame-journal-v1-20260906`.
Base: `fd1459388c8451c5446e3f55b6d9287bf068703e` (timing evidence plus shared
packet telemetry). Other Orange worktrees/branches were not merged or modified.

Status: CPU library and offline metadata-correspondence utility implemented.
The follow-up [headless integration](master_frame_journal_headless_integration_2026-09-06.md)
now wires an opt-in timed headless source journal and required-product gate.
**Not a usable crop-only recording mode, not deployed**.
Both JSON records below are closed candidate schemas, not accepted replacements
for acquisition-index mapping v1, SHAMAN, Citrus v6 or Palette intake.

## Implemented components

- `src/recording_master_journal.h/.cpp`: one bounded single-producer/single-consumer
  journal per parent recording/camera/producer instance/stream generation.
- `src/recording_master_crop_coverage.h/.cpp`: streaming comparison of the journal's
  assigned-frame rows with an explicitly ordered collection of moving-crop CSVs.
- `orange_recording_master_journal`: reusable CPU-only CMake library, now linked
  by runtime and test targets; the opt-in timed headless path constructs it.
- `tools/recording_master_journal_tests.cpp`: deterministic failure/backpressure,
  identity, clip correspondence, affinity and drain tests without cameras/GPU/media.
- Candidate schemas:
  `docs/schemas/orange_recording_master_frame_journal_v1.schema.json` and
  `docs/schemas/orange_recording_moving_crop_metadata_correspondence_v1.schema.json`.

The library stores copied scalar facts, never images, WORKER_ENTRY leases or GPU
events. It does not need a full-frame encoder or its CSV. Prearm allocates the
queue, starts the writer, applies/records its requested CPU mask, and opens the
new output exclusively. Failure before readiness throws; recording must not arm.
Existing files or symlink targets are not overwritten.

## Journal identity, boundaries and meanings

Each candidate descriptor binds `recording_id`, exact `camera_serial` spelling,
`producer_instance_id`, and numeric `stream_generation`. Its CSV is
`Cam<serial>_master_frames_v1.csv`; its terminal descriptor is the same stem with
`.json`. CSV SHA-256 and size cover exact stored bytes. The writer's incremental
digest must match the closed file readback before claiming complete.

The declared observation boundary is `caller_submitted_acquisition_facts_v1`.
The follow-up wires the acquisition hooks, with live validation still pending.
This says only that **every offered fact** was durably recorded, not that every
hardware exposure was received. `status=complete` is journal completeness, not loss-free acquisition,
encoded-media completeness or scientific eligibility.

`observation_index` starts at zero and advances for every offer, including an
offer rejected by the bounded queue or fact validator. `recording_frame_id`
retains its original one-based assigned identity; zero explicitly means no ID
was assigned. Local source IDs, hardware camera IDs, crop indices and journal
indices are not aliases. Original integer camera/realtime/steady timestamps have
separate presence bits; present zero is not automatically absence. No timestamp
offset, synchronization state or exposure phase is inferred.

| Fact kind | Value | Assigned recording ID? |
| --- | --- | --- |
| Assigned received frame | 1 | Required, > 0 |
| Received frame rejected before assignment | 2 | No; keep its available source facts |
| Resource starvation before attempting receipt | 3 | No; do not invent a received frame |
| Receive error | 4 | No; do not invent an exposure or camera timestamp |
| Recording pause | 5 | No |
| Recording resume | 6 | No |

Rejection reasons distinguish worker-entry, readiness-event, detector-event and
receive failures. Pauses/resumes are observations, not synthetic frames. A
producer restart needs a new generation and new journal instance/root; it cannot
append to or silently reuse an existing seal. Sparse/repeated/reset assigned IDs
remain visible in the journal and make its dense-from-one flag false. Hardware
exposure coverage is always `not_certified` in this candidate.

## Scheduling, boundedness and failure behavior

`TrySubmit` is a single-producer API: bounded scalar copy plus lock-free atomics,
with no allocation, file I/O, hashing, mutex, notification, device call or wait.
The queue capacity is bounded to 2..65536 entries and recorded with its high-water
mark. A non-realtime writer polls at 1 ms when empty. Its I/O and hashing are
outside the acquisition caller; no new PTP polling is introduced.

The caller must resolve writer CPUs using the existing housekeeping/CPU-role
policy before arm. Explicit masks are verified and recorded. Empty masks inherit
the caller's process affinity and are explicitly labeled `inherited`; that is
useful in CPU tests, not a claim of production isolation. No isolated core is
selected automatically and no real-time scheduling priority is introduced.

After quiescing the producer, control calls `Finalize(normal_finish)`. The writer
drains all accepted facts, fsyncs/closes the CSV, and the control path hashes and
publishes the create-once descriptor. Overflow, invalid facts, partial writes,
sync/close errors, changed CSV bytes, empty journals and interrupted closure
cannot yield a complete journal. Failure retains the CSV and available terminal
evidence. An unfinalized destructor attempts an explicitly incomplete close.

Counters distinguish offered, accepted, queue-rejected, invalid, written and
discarded-after-I/O-failure facts. Queue overflow retains source-offer counts and
first/last rejected observation indices; it does not renumber surviving rows or
shrink source obligations. Live counter snapshots are observational; the terminal
snapshot is taken after producer quiescence and writer join. Calls after final
closure reject and increment live misuse telemetry but never rewrite the seal.

Finalize must not race the single producer. Hung filesystem I/O can delay control-
plane drain; there is no unsafe thread cancellation or claim of a bounded disk
latency. A future session watchdog must mark timeout/incompleteness without
freeing resources still used by the writer.

## Moving-crop clipped metadata check

`CheckMovingCropMetadataCoverage` accepts the writer's finalized record and typed
clip bindings supplied by a future recording finalizer. It is not a general
untrusted-JSON loader. It verifies the master's CSV digest, exact parent/camera
bindings, ordered unique clip IDs/indices and normalized in-root metadata paths.
It walks both timelines with constant frame-storage memory, skipping non-assigned
master observations. Every assigned master frame needs exactly one crop row.

For each row it checks original recording ID, camera/realtime timestamp scalars,
clip-local `crop_video_frame_index`, and continuous
`session_crop_video_frame_index`. Clip-local indices restart at zero; session
indices do not. A shorter final clip is valid if all assigned source frames are
covered. Missing first/interior/final rows, duplicates, excess outputs, index
resets, wrong timestamp values, partial CSV rows and changed metadata refuse.
The first profile requires the master's assigned IDs to be dense from one.

Blank crops remain actual output rows. This utility does not reinterpret detector
state or declare that a technical failure is a successful zero detection. It only
checks correspondence; inference status/blank policy remains a separate gate.

The returned candidate record binds master/crop CSV hashes and counts. Its mapping
digest hashes normalized decimal ASCII rows:
`clip_index,local_crop_index,session_crop_index,recording_frame_id,master_observation_index\n`.
The master-record digest covers `nlohmann::json::dump()` sorted compact UTF-8 JSON
without a trailing LF; CSV hashes cover stored bytes. It reports
`authority=master_and_crop_metadata_rows_only` and
`media_finalization_evaluated=false`. It never claims decoded order, encoder
success, physical timing, every exposure, or a new Palette acquisition index.

## Remaining integration checklist

- [x] Add prearm/session ownership and a stable, immutable handoff before starting
  each acquisition thread (opt-in timed headless; GUI rearm remains deferred).
- [x] Place hooks at every assigned frame, received-but-unassigned rejection,
  pre-receive starvation, receive failure and sampled pause/resume boundary.
  Pause/resume is acquisition-observed state, not a timestamped command log.
  Existing assignment remains after resource checks and is not renumbered.
- [x] Quiesce acquisition and drain the master before headless parent finalization.
  Journal failures/timeouts enter required-product completion, not just logging.
- [ ] Add explicit media selection independently of logical recording activity;
  retain full-frame-only and full-frame+moving-crop paths as first-class modes.
- [ ] Require compatible full-rate detector/crop configuration and reconcile
  rejected upstream jobs as well as recorder-received and encoded outputs.
- [ ] Reuse accepted registered-context capture deliberately from the spatial-ROI
  branch; no unrelated merge. Bind one full-camera context image and geometry.
- [ ] Call the metadata checker from native/external/rolling finalizers alongside
  existing container, returned-identity, packet and inference-state gates.
- [ ] Add a closed external loader and obtain Orange/Citrus/Palette agreement for
  the source-domain/profile. Do not weaken the existing dense v1/v6 consumer.
- [ ] Add recording snapshot/inventory references, synthetic end-to-end integration
  tests and a controlled headless crop-only run with measured queue/latency impact.
- [ ] Implement deployment/configuration only after those gates pass.

Validation command:

```bash
cmake --build /tmp/orange-timing-build-20260906 --target recording_master_journal_tests -j 4
ctest --test-dir /tmp/orange-timing-build-20260906 --output-on-failure -R '^recording_master_journal_tests$'
```

These are CPU synthetic tests, not live capture, codec, hardware throughput or
Palette acceptance tests. Full Draft 2020-12 schema-engine validation is not
claimed here; no dependencies are installed by this slice.

Validation completed for this slice:

- Release library/test build passed with the installed toolchain.
- `recording_master_journal_tests` passed 10 groups, including deterministic
  queue overflow, header/row/partial-write/fsync/close failures, changed bytes,
  unknown/invalid facts, original timestamp precision, present zero, generation
  custody, affinity, 100 terminal-drain runs and the clipped-crop refusal matrix.
- Existing packet telemetry, sampled timing, rolling projection and acquisition-
  index authority suites passed alongside it: 5 CTest suites total.
- Both candidate schema files parse as JSON; `git diff --check` passed.
- Standalone AddressSanitizer/UndefinedBehaviorSanitizer build and all 10 test
  groups passed, including leak checking outside the sandbox. The initial sandbox
  run completed the groups but LeakSanitizer could not finish under ptrace; the
  authorized outside-sandbox rerun exited successfully without sanitizer findings.
