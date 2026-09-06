# Producer timing evidence: first implementation slice

Date: 2026-09-06. Owner: Orange; consumers: Citrus and Palette.

Implementation branch: `agent/acquisition/timing-evidence-v1-20260906`, based on
`5cf21a9f9de0a86f58009722a39da3708f000403` (the current a16 acquisition line).
This does not merge the separate spatial-ROI branch, deploy binaries, change
camera configuration, or certify live acquisition. No camera runs are part of
this slice.

Coordination baseline: agent-contracts
`orange-palette/producer_timing_provenance_audit_2026-09-06.md` and Palette's
response in PR #48. The rolling profile below is an **opt-in candidate for
consumer review**, not a relaxation of the accepted acquisition mapping v1.

## Implemented checklist

- [x] Native in-process and shared split-GOP CSV writers append
  `recording_frame_id` equal to their existing `frame_id`. The original three
  columns retain their positions and meanings; external writer format is unchanged.
- [x] Native dense whole-recording metadata seals through the existing strict
  `orange.recording.acquisition_index_mapping` v1 producer and consumer.
- [x] Candidate finalization-only rolling projection preserves exact native CSV
  rows, timestamps and parent IDs, with hashed references to each clip's CSV.
- [x] Strictly reject incomplete clips, gaps, resets, alias mismatch, malformed
  integer timestamps, partial rows, mismatched headers and declared packet parity.
- [x] Reset sampled PTP recording IDs at session change; distinguish failed reads
  from the last successful value; bound observation storage and report omissions.
- [x] Close sampled evidence when recording stops even if streaming continues;
  do not close during an explicitly preserved rolling-session pause.
- [x] Require finalized summary and no reported sample failures/omissions for
  positive camera-clock inference. Classification rule identity advances to v3.
- [x] Correct the host timestamp's sampling-event description.
- [x] Add standalone and production-manifest regression tests, including a
  failed retry with stale parent metadata which must remain unsealed.

## Frame IDs and rolling projection

Native CSV header:

```text
frame_id,timestamp,timestamp_sys,recording_frame_id
```

Both ID columns refer to the parent recording sequence for that camera. Neither
is the camera hardware ID, stream-local frame counter, or clip-local output index.
Integer nanoseconds are emitted without floating-point conversion.

Default behavior is unchanged for rolling recordings. To opt in for controlled
validation, the **manifest-writing Orange process** must receive
`ORANGE_ROLLING_ACQUISITION_PROJECTION_V1=1`. This slice does not widen installed
sudo wrappers or add a GUI/spec control. Do not assume that setting the variable
in an ordinary shell forwards it through a privileged launcher.

At completed, drained parent finalization, the adapter checks each camera's
ordered clips. All clip indices must be consecutive from zero, IDs unique,
parent identity equal, drains complete, no rollover pending, and only the last
clip marked final. Metadata and video paths must resolve inside both the recording
and the declared clip directory. Every CSV must have the same header and retain
the two ID aliases and integer timestamp columns.

The adapter copies one header and each original data row into:

```text
<recording>/rolling_metadata_<sha256-hex>.csv
```

It requires parent IDs `1..N` without renumbering. A partial *duration* final clip
is supported if its rows are complete and the clip drained. Dropped-ID subsets,
blank outputs, empty clips and interrupted recordings are not made dense by
inventing rows. Their recordings can remain valid, but this profile stays unsealed.

The existing top-level camera metadata reference points to this aggregate; there
is no fictitious parent video. Existing closed acquisition mapping v1 keys and
the rule `source_acquisition_frame_index = recording_frame_id - 1` are unchanged.
The sibling `rolling_metadata_projection` records ordered source clip IDs, CSV
relative paths/sizes/SHA-256, video relative paths, frame ranges, and the parent
and clip-local output origins. Its closed candidate schema is
`docs/schemas/orange_rolling_metadata_projection.schema.json`.

The sibling digest is SHA-256 over compact, sorted-key UTF-8 JSON without a
trailing newline, prefixed `sha256:`. CSV hashes cover exact bytes, including
line endings. Mathematical ranges, clip order, source membership and hashes must
also be checked; JSON Schema alone cannot establish these relationships.

Files are staged with unique names and published by a no-overwrite hard link;
identical content-addressed files may be reused after hash verification. Input
size/mtime are checked around reading. A later-camera failure may leave an
unreferenced completed aggregate from an earlier camera, but never a partial
aggregate referenced by a newly sealed mapping. Previously sealed mapping
generations are preserved; this is not a historical repair/migration tool.

This is **metadata ordering with declared packet-count parity**, not an independent
proof of decoded-video frame order. Video paths here are references, not video
byte hashes. Native frame-identity authority remains `producer_declared`.
Full closed source/video inventory and returned-encoder-identity parity are
separate work. Palette must review the candidate schema and whole/rolling
equivalence before treating this as a supported ingestion profile.

## Sampled PTP evidence is not physical stimulus timing

The existing bounded PTP enum-read cadence and camera latch mechanism are retained.
There is no new per-frame camera polling, no new timing-service thread, and no
additional per-frame latch. Bookkeeping consumes existing read results.

`ptp_readback_observations` compresses equal sampled states into runs. Each run
retains first/last local and recording IDs, UTC labels, monotonic sample times
and count. Before this recording's first assigned frame, its recording ID is
zero even if a previous recording had tens of thousands of frames.
`ptp_readback_coverage` v1 declares:

- `semantics = sampled_states_not_continuous_lock`;
- attempts and separate mode/status read failures;
- largest observed inter-sample gap in nanoseconds;
- omitted observation count and a 1,024-state-run storage limit;
- the implementation-specific `std::chrono::steady_clock` epoch, **not an asserted
  cross-process or PTP clock transform**.

A read failure is `null` for that sample, never an old value presented as a fresh
success. Omissions and failures prevent positive classification under rule v3.
Older evidence lacking the new coverage object is still interpreted using its
existing evidence checks, but is not upgraded to the new coverage guarantee.
Finalization is evidence closure, not a claim of continuously observed lock.
If session-manifest finalization precedes acquisition-summary closure, classification
remains conservative; a sealed contract is not silently rewritten later.

The corrected host event is
`after_EVT_CameraGetFrame_and_optional_inline_PTP_check_before_fanout`.
It is a host observation, not exposure time. PTP supports camera-clock
interpretation; the photodiode measures projected light. Exposure-aligned physical
timing still needs photodiode and camera trigger/exposure evidence in a measured
clock relationship. Nothing in this slice substitutes frame IDs or clock labels
for that measurement.

## Validation and remaining work

Targeted build and tests:

```sh
cmake --build <build> --target recording_session_manifest_tests \
  recording_timing_evidence_tests rolling_metadata_projection_tests \
  acquisition_index_authority_tests -j8
ctest --test-dir <build> --output-on-failure \
  -R '^(recording_session_manifest_tests|recording_timing_evidence_tests|rolling_metadata_projection_tests|acquisition_index_authority_tests)$'
```

The manifest test links the production acquisition/session/native writer sources.
Rolling fixtures deliberately use placeholder video files and declared packet
counts: they test metadata finalization, not NVENC, decoding or live performance.
`rolling_metadata_projection_tests --emit-fixture` emits the candidate JSON record
to stdout for schema checks while running the same tests.

Results on 2026-09-06: the four Orange CTest executables above passed after a
fresh isolated build. The unchanged Citrus CPU executable also passed all 282
tests, including finalized mapping and one-to-many v6 fixtures. The candidate
JSON Schema parses successfully; full Draft 2020-12 schema validation remains
pending (no `jsonschema` package is installed in the checked Python environments).
No dependency installation is needed to build or run the implemented C++ path.

Still open:

- [ ] Palette approval/consumer tests for the candidate projection profile.
- [ ] Coordinated native/external whole and rolling real-media fixtures and
  Citrus finalization checks; preserve one-to-many stimulus rows and target IDs.
- [ ] Versioned sparse/subset/blank-output mappings if needed, without weakening v1.
- [ ] Complete sampled coverage bounds, grandmaster transitions and explicit
  unknown exposure/precision semantics; off-hot-path lifecycle design as needed.
- [ ] Citrus raw completion failure status, measured session-clock transforms,
  build provenance, closed source inventory and transfer-generation receipts.
- [ ] Photodiode/camera exposure validation before physical-onset claims.

The corresponding broad P0/P1 items in the shared audit remain partially open.
