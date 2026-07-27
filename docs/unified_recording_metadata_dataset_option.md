# Unified Recording Metadata Dataset Option

Status: deferred architecture option; do not implement yet.

This document records a possible future consolidation of Orange recording
metadata and profiling artifacts. It is not a current runtime contract, does
not authorize changing or deleting existing artifacts, and does not make Zarr
an Orange output. The option should remain deferred until the geometry and
analytics library contracts below support multi-arena views and genuinely
one-to-many analytics results without lossy flattening.

## Motivation

A production-like recording can currently contain separate per-camera files
for frame metadata, YOLO events and performance, pose events and performance,
crop metadata and performance, pipeline samples, acquisition diagnostics,
external-recorder routing/detach/encode telemetry, PTP evidence, keyframes, and
session state. Those files are useful producer-local crash evidence, but a
consumer must understand many schemas and repeat the same frame, clock, camera,
and artifact joins.

The consolidation option would provide one canonical, normalized metadata
dataset per finalized recording while retaining MP4 media and the small JSON
files needed for discovery, start-time provenance, recovery, and human-readable
diagnostics.

The primary goal is not merely fewer files. The goal is one unambiguous data
model for:

- frame identity and clock assignments;
- cameras, arenas, geometries, and coordinate transforms;
- zero, one, or many detections per frame;
- zero, one, or many arena memberships per detection;
- crop selection and exact source-detection provenance;
- zero, one, or many pose instances and their source relationships;
- per-stage timing and recorder outcomes;
- session events, artifacts, and immutable provenance.

## Why This Is Deferred

Consolidating the present schemas now would risk making current limitations
look permanent. In particular:

- `src/citrus_recording_geometry.cpp::camera_bindings(...)` currently rejects
  a selected canvas that maps one camera to more than one arena.
- Recording geometry schema version 1 exposes one `arena_id` under each
  `cameras[serial]` entry and the materialized asset index uses scalar
  `arena_by_camera` values.
- YOLO audit rows already carry a `detections[]` array, but their `index` is
  only position within that emitted result. There is no stable detection
  identity contract shared by YOLO, spatial-mask decisions, crop rows, pose
  rows, tracking, and downstream consumers.
- Crop metadata describes the selected detection and one produced crop. It is
  not a general one-to-many detection-to-crop relation.
- Pose rows can carry multiple decoded pose candidates, but the current worker
  is driven by one selected crop and does not provide stable relational IDs
  linking every pose instance to an exact detection, arena membership, crop,
  and track.

A consolidated schema designed around one arena per camera or one selected
detection per frame would be difficult to evolve safely. The library-level
relationships must be correct first.

## Required Readiness Gates

Do not begin implementation until all of these gates are satisfied by runtime
types, versioned schemas, and tests rather than only by narrative plans.

### 1. Multi-arena-per-camera geometry

- One camera can reference zero, one, or many arenas in one recording.
- The primary identity is a camera/arena pair, not a scalar
  `arena_by_camera[camera]` value.
- Every arena geometry has a stable `arena_id`, `geometry_id`, schema version,
  coordinate space, target plane, source artifact, and content fingerprint.
- Camera-to-arena transforms and their direction are explicit. Consumers never
  infer a transform from camera or arena ordering.
- Multiple geometries for the same camera can coexist without overwriting one
  another.
- Geometry validity can be bound to the whole recording or to explicit frame
  intervals when a runtime geometry change is supported.
- Overlapping arenas are represented honestly. A point or detection may match
  zero, one, or multiple arenas unless a separately recorded policy resolves
  the ambiguity.
- Accepted physical boundaries, valid-detection regions, projection geometry,
  and runtime gating policies remain distinct semantic objects.
- Recording-local geometry materialization and validation cover every required
  camera/arena pair and reject cross-camera or cross-arena substitution.

### 2. Multiple detections and detection-set identity

- A terminal analytics result owns a stable `detection_set_id` for one source
  frame, model invocation, and processing stage.
- Every detection has a stable `detection_id` within the recording, plus its
  original model/raw index where applicable.
- Raw model candidates, postprocessed detections, spatial-mask decisions,
  downstream-selected detections, and synthetic diagnostics are distinguishable
  rather than silently sharing one index namespace.
- Zero detections is a valid, explicit terminal result and is distinct from
  analytics failure or a dropped/missing result.
- Labels, confidence, boxes, keypoints, coordinate spaces, model identity, and
  synthetic/production validity are preserved for every detection.
- A detection can have zero, one, or many arena-membership records. Membership
  records carry the geometry/policy version, containment result, signed
  boundary distance where meaningful, and any ambiguity-resolution decision.
- Track identity is nullable and independent from detection identity; the data
  model does not assume one fish or one track.

### 3. Crop and pose relationships

- Every crop has a stable `crop_id` and an explicit source policy.
- A crop can reference the exact source detection, track, arena, or synthetic
  request that produced it; absence of a source is represented explicitly.
- The model supports zero, one, or many crops from a frame without adding
  numbered columns.
- Every pose inference has a stable `pose_result_id`, references its exact
  crop/input, and records terminal success, no-result, failure, or drop state.
- Every decoded pose candidate has a stable `pose_instance_id`; keypoints are
  related to that instance and retain skeleton/keypoint identity.
- Detection-to-crop-to-pose provenance remains valid when multiple detections,
  multiple arenas, multiple crops, or multiple pose candidates exist.

### 4. Stable frame and clock identity

- `(session_id, camera_serial, recording_frame_id)` is a unique recorded-frame
  key, with explicit nullable mappings to local, SDK camera, IPC, video-frame,
  and clip-local indexes.
- Rolling clips and external recorder routing preserve the same session-level
  frame identity without renumbering it into a conflicting namespace.
- Camera and host timestamps use stable clock IDs and the finalized
  `timestamp_clock_contract`; no consolidated table guesses an epoch.
- Missing frame metadata, analytics results, encoded packets, and telemetry
  samples are distinguishable from legitimate zero-valued results.

### 5. Terminal outcome and durability contracts

- Every expected producer reports a terminal state and row/drop/failure counts.
- Media and metadata writer failures make the final session status incomplete
  or failed rather than silently producing a successful consolidated dataset.
- Consolidation validates row counts, key uniqueness, foreign keys, frame
  ranges, clip continuity, clock assignments, and source fingerprints.
- The consolidated artifact is committed atomically and is never advertised as
  canonical before validation succeeds.
- The session manifest records the consolidation tool/version, source artifact
  fingerprints, validation result, and any retained raw diagnostics.

## Candidate Logical Model

The future representation should be relational even if its physical format is
columnar. It should avoid both a single extremely wide frame table and an
untyped metric-name/value store.

Candidate tables:

| Table | Grain and purpose |
| --- | --- |
| `sessions` | One finalized recording session and its lifecycle outcome |
| `cameras` | One camera participating in the session |
| `frames` | One recorded source frame per camera |
| `clock_domains` | One immutable clock descriptor |
| `clock_evidence` | PTP/readback evidence supporting a clock classification |
| `arenas` | One arena identity in the recording geometry snapshot |
| `camera_arena_geometries` | One versioned camera/arena geometry and transform |
| `geometry_frame_intervals` | Optional frame validity intervals for geometry versions |
| `detection_sets` | One terminal model/stage result for a source frame |
| `detections` | One detection belonging to a detection set |
| `detection_arena_memberships` | Many-to-many detection/arena decisions |
| `tracks` | Stable optional track identities and lifecycle bounds |
| `detection_track_assignments` | Versioned detection-to-track decisions |
| `crops` | One produced or terminally rejected crop request |
| `crop_sources` | Detection/track/arena/synthetic provenance for a crop |
| `pose_results` | One terminal pose inference for a crop/input |
| `pose_instances` | One decoded pose candidate |
| `pose_keypoints` | One identified keypoint belonging to a pose instance |
| `frame_stage_timings` | Typed stage/queue timings keyed to a frame and producer |
| `pipeline_samples` | Lower-rate queue, throughput, and resource samples |
| `recorder_frames` | Detach, route, shard, encode, packet, and release outcomes |
| `session_events` | Start, stop, rollover, control, failure, and recovery events |
| `artifacts` | Media/metadata paths, roles, byte counts, and fingerprints |

The `frames` table should be the central join point and should contain, at
minimum:

```text
session_id
camera_serial
recording_frame_id
local_frame_id
camera_frame_id
ipc_frame_id
camera_timestamp_ns
host_receive_timestamp_posix_ns
camera_clock_id
host_clock_id
clip_id
video_frame_index
```

Not every value is available in every recording mode. Nullability must mean
"not available/not applicable," never an implicit zero.

## Candidate Physical Representations

The logical contract should be chosen before its storage engine.

### SQLite bundle

Advantages:

- one portable file per recording;
- typed relational tables, foreign keys, indexes, transactions, and views;
- simple Python/Palette access;
- atomic finalization is straightforward;
- no need to force heterogeneous data into one table.

Risks:

- row-oriented storage may be larger or slower for long, column-heavy profiling
  scans;
- introducing database writes on acquisition/inference threads would risk
  hot-path contention and is not recommended.

### Parquet dataset

Advantages:

- efficient compression and columnar analytics for long recordings;
- natural partitioning by logical table and optionally camera;
- good interoperability with Python analytics tools.

Risks:

- a dataset remains a directory of files rather than one file;
- adding Arrow/Parquet directly to Orange would be a significant dependency;
- atomic multi-table finalization and schema validation need an explicit
  dataset manifest.

### DuckDB bundle

Advantages:

- one database file with strong analytical and columnar behavior;
- direct SQL over normalized tables and Parquet if needed.

Risks:

- another runtime/tooling dependency to qualify;
- less ubiquitous as an embedded production dependency than SQLite.

No choice is made here. Orange should not produce Zarr as part of this option.
Palette may convert the finalized logical dataset into its own Zarr layout.

## Safe Migration Shape

The first implementation, if authorized after the readiness gates pass, should
be a finalization-time compactor rather than a new hot-path writer:

1. Keep existing CSV/JSON/JSONL emitters as crash-recoverable producer spools.
2. At recording finalization, read only artifacts listed by
   `recording_session.json` and its recording-output descriptors.
3. Normalize them into a temporary consolidated dataset.
4. Validate counts, identities, relationships, clocks, geometry references,
   and source fingerprints.
5. Atomically publish the consolidated artifact.
6. Add its path, schema, status, and fingerprint to
   `recording_session.json`.
7. Keep existing artifacts during a compatibility period. Later, optionally
   move noncanonical producer files under `diagnostics/raw/` or make selected
   profiling spools opt-in only after recovery requirements are understood.

Do not initially write a shared database from acquisition, YOLO, pose, crop, or
recorder threads. That would couple the 100-fps hot path to storage locking and
failure behavior before the consolidation contract has been validated.

## What Remains Separate

- MP4 and other durable media remain media files.
- `recording_snapshot.json` remains the start-time configuration/provenance
  snapshot with its currently documented later status and artifact-pointer
  updates; consolidation must not silently redefine its lifecycle.
- `recording_session.json` remains the discovery entry point and final status /
  artifact index.
- Recorder logs and failure traces remain human-readable diagnostics.
- Process handoff contracts may remain separate recovery artifacts even if
  their finalized facts are also normalized into the dataset.
- Palette's Zarr output remains downstream and outside the Orange recording
  artifact contract.

## Minimum Validation Matrix Before Adoption

The consolidation must be tested against at least:

- one camera observing one arena;
- one camera observing multiple disjoint arenas;
- one camera observing overlapping arenas with explicit multi-membership;
- multiple cameras observing one shared arena;
- zero, one, and multiple detections in a frame;
- multiple detections producing multiple crops and pose inputs;
- multiple pose candidates and explicit no-result/failure outcomes;
- tracking enabled and disabled;
- in-process and external-IPC full-frame recording;
- crop recording enabled and disabled;
- single-clip and rolling-clip sessions;
- synchronized PTP/TAI and device-defined camera clocks;
- producer drops, missing optional rows, writer failure, interrupted
  finalization, and successful recovery from retained spools.

Every case must verify that no arena, detection, crop, pose, frame, clock, or
artifact relationship is recovered from ordering or filename conventions when
an explicit ID should exist.

## Decision Point

Revisit this option only after the readiness gates are demonstrably complete.
At that point, choose the physical representation using measured production
recordings and decide:

- maximum supported recording duration and metadata volume;
- required query patterns for Palette and other consumers;
- raw-spool retention and crash-recovery policy;
- whether the consolidated dataset is authoritative or a reproducible cache;
- dependency and operational cost of SQLite, Parquet/Arrow, or DuckDB;
- compatibility and rollback requirements for existing validators.

Until that decision, the artifact files documented in
`output_artifacts_contract.md` remain the current runtime contract.
