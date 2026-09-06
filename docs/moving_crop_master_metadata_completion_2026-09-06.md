# Moving-crop master metadata completion — 2026-09-06

Status: implemented on `agent/acquisition/master-frame-journal-v1-20260906`,
following `8c6de2a6d49e6969f6a50df3cb48bee951e4444d`. CPU validation and production
build results are recorded below. No live recording or deployment is claimed.

This completes the **headless metadata-coverage gate**, not the crop-only media
selector. Continuous full-frame encoding is still required by the current
headless recording workflow. Existing single-subject moving/lossless crops
remain first-class; no changes to their placement, size, encoder preset, blank
image generation, full-frame split-GOP routing, or PTP sampling were made.

## Activation and lifecycle

When a timed experiment explicitly enables `fixed.master_frame_journal` and
moving crops, the initial supported validation combination requires:

- `fixed.crop_recording.mode = external_ipc`;
- `fixed.recording_sink_mode = external_ipc` and a supervised external contract;
- real YOLO with `decimate = 1`, and no synthetic detection override.

This restriction applies to the **new journal-plus-crop verification profile**,
not to established native crop or synthetic diagnostic modes without the journal.
The real worker already writes detector events; `fixed.yolo_event_log` is a
synthetic test producer and must not be enabled alongside it.

Before camera threads start, `recording_snapshot_start.json` freezes
`session.moving_crop_master_coverage`:

```json
{"schema_version":1,"required":true,"profile":"external_moving_crop_full_rate_v1"}
```

Its camera/parent/producer set is the immutable master-journal camera set. After
the acquisition threads, detector logger, crop workers and recorder processes
have stopped, finalization checks every camera and publishes its receipt. A
missing stream, worker shutdown exception, recorder stop failure or metadata
mismatch fails the run. The parent manifest gate independently requires every
receipt and rechecks its referenced bytes before allowing `completed` status.
The immutable start remains authoritative if the mutable snapshot loses fields.

All parsing, projection, hashing and file publication happen on the control
thread after shutdown. No acquisition image leases, hot-path I/O, additional
worker threads, PTP reads or device synchronization were introduced.

## Exact checks and output files

The root `Cam<SERIAL>_crop_meta.csv` remains unchanged. For a rolling stream,
the finalized external recorder summary supplies the ordered clip IDs, zero-based
indices, inclusive recording-frame ranges and frame counts. Ranges must cover
all assigned master frames contiguously, including the final partial clip.
No nominal-FPS or wall-clock boundary is substituted for actual recorder ranges.

New create-once files are:

```text
Cam<SERIAL>_crop_clip_0_meta_v1.csv
Cam<SERIAL>_crop_clip_1_meta_v1.csv
...
Cam<SERIAL>_moving_crop_metadata_v1.json
```

The projections preserve the original columns and session index. Only
`crop_video_frame_index` restarts at zero per clip. The receipt binds each
projection explicitly to the recorder's clip ID/index and exact summary bytes.
For a nonrolling stream the existing root crop CSV is used directly; `single`
is a collection-local label, not a new global recording/media identity.

The streaming checks require:

- A complete master journal with dense assigned recording IDs from one.
- Exactly one crop row per assigned master frame, including the first and last.
- Matching recording, original local and original camera frame IDs, and exact
  integer camera/host timestamps (no tolerance or nearest-time join).
- One ordered real detector terminal event per crop, with matching parent,
  camera, original counters and timestamps.
- `detections` paired with a detected crop, or successful `zero_detections`
  paired with an explicit blank crop. Failed, timed-out, pending, unscheduled,
  synthetic, missing, duplicate or contradictory events cannot certify coverage.
- Recorder input/output counts matching the master obligation, no skipped or
  dropped inputs, and consistent ordered rolling ranges/packet counts.
- Exact path/size/SHA-256 references for the master descriptor/CSV, original
  crop CSV, detector JSONL, recorder summary and every clip projection.

Paths must be normalized and contained under the recording root; symlink inputs
are refused. Inputs are hashed before/after processing. Publication uses staged
files, fsync, create-once linking and directory sync; existing files are never
overwritten. A failure can leave previously completed projections for diagnosis,
but no successful collection receipt is produced. Automatic overwrite/retry is
deliberately not supported. Bounds: 16 MiB JSON documents, 64 KiB input lines,
4096 clips, O(1) frame storage and O(clips) collection metadata. This is an
in-process finalizer for an exclusively owned stopped recording, not a hostile
concurrent-filesystem ingestion service or a generic JSON-schema engine.

## What the receipt does not prove

`orange.recording.moving_crop_metadata_completion` v1 explicitly declares
`authority = master_crop_and_detector_metadata_only` and
`media_finalization_evaluated = false`. Hashing an external summary and checking
its ranges/counts does **not** replace returned-frame identity validation,
packet-write balance, MP4 finalization, decodability, or video hashes. Those
existing recorder checks remain separate; the new gate is necessary, not
sufficient, for a successful media product. In particular it does not yet join
each recorder-returned metadata row to each projected crop row.

The existing master CSV and crop correspondence v1 field names are preserved.
New closed schemas:

- `docs/schemas/orange_recording_moving_crop_metadata_completion_v1.schema.json`
- `docs/schemas/orange_moving_crop_master_coverage_evidence_v1.schema.json`

No Citrus v6 wire change, dense acquisition-index redefinition, transfer-admission
change, GUI journal integration or claim of Palette acceptance is included.

## Verification and remaining implementation

- [x] Five CPU test groups: whole/rolling/partial-clip success; biological blank
  versus technical failure; original counter/timestamp mismatch; missing/duplicate
  rows and tails; recorder range/count failures; source tampering, symlink and
  existing-output refusal; immutable-start parent failure propagation.
- [x] Nineteen camera-free spec admission cases, including full-rate whole and
  rolling profiles and refusal of decimation/native/unsupervised/synthetic variants.
- [x] GUI `orange` and headless `orange_client` production builds passed.
- [x] Eleven focused CTest suites passed: moving-crop completion, master journal,
  acquisition integration, packet telemetry, timing evidence, rolling projection,
  acquisition index authority, crop rolling sidecars, session manifest, GUI
  finalizer and session status.
- [x] ASan/UBSan test build passed all five groups with leak checking outside
  sandbox tracing. A rejected-receipt initializer exception leak found during
  validation was fixed before the passing rerun.
- [x] New schema documents parse and `git diff --check` passes. A full JSON Schema
  engine is not installed; schema-engine validation is not claimed.
- [ ] Decouple logical recording from full-frame media selection and external
  crop supervision from the full-frame supervisor.
- [x] Reuse the minimal registered context-capture components deliberately. The
  next opt-in headless slice introduces a distinct whole-camera context v2
  descriptor without inventing fixed-region identities; see
  [the implementation and remaining gates](registered_context_headless_capture_2026-09-06.md).
- [ ] Integrate per-output returned-identity/container/media evidence into the
  independent master-to-crop completion contract, including native writer failure.
- [ ] Enable and validate registered-context plus moving-crops-only recording;
  update the versioned parent/transfer/downstream consumers for that authority.

Implementations: `src/recording_master_crop_coverage.{h,cpp}` owns the CPU work;
the headless client supplies stopped stream membership and failure accounting;
`src/session/recording_session.cpp` applies the required parent gate.
