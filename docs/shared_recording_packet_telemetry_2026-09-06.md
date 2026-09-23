# Shared recording packet telemetry

Date: 2026-09-06. Implementation branch:
`agent/acquisition/timing-evidence-v1-20260906`, following timing slice `af21118`.

## Implemented

`src/recording_packet_telemetry.h` provides one dependency-light packet-write
snapshot, disjoint-output aggregation, and packet-balance rule. It replaces
duplicated fields/copy loops and completion checks in:

- `FFmpegWriter` and its container-finalization evidence, used by native
  full-frame, shared split-GOP, and moving-crop output;
- external-recorder encoder-shard, merged-output, and rolling-clip summaries;
- external frame-identity proof v2's packet accounting.

The shared fields distinguish accepted/rejected packet submissions, submission
bytes, write attempts, successfully written packets/bytes, write failures, and
the first nonzero write error. The writer still owns its existing atomic
counters; summaries use plain snapshots under their existing lifecycle/locking.
There are no new threads, allocations in counter updates, queue policies,
device synchronization, PTP calls, or source-buffer retention.

`FFmpegWriterPacketWriteStats` remains an alias for source compatibility.
Existing external JSON names (including the `mp4_*` prefixes), finalization v2
fields, null error representation, frame IDs, and verification versions are
unchanged. No new wire schema is introduced by this internal refactor.

## Meaning and lifecycle

1. A crop IPC ACK reports accepted/detached source handoff, not encoder or mux
   completion. Crop-worker counters are not substituted for recorder evidence.
2. Packet balance means no rejections/failures and accepted submissions = write
   attempts = successfully written packets. Take terminal snapshots after the
   existing drain/join. A balanced live snapshot or an empty output is not proof
   of a complete recording.
3. Container-finalization evidence additionally requires the writer failure
   latch to be clear, a successful muxer flush, trailer write, and output close.
   The existing playback-intent patch classification remains separate.
4. Frame-identity proof additionally compares packet counts with returned
   encoder identities and metadata rows. Its existing `encoded_subset` policy
   does not establish coverage of all camera acquisition frames.
5. Aggregate each disjoint clip/shard once; never add consecutive live snapshots
   of the same output. Retain individual output evidence. The aggregate first
   error follows aggregation order, not wall-clock order across shards.

Native crop writer destruction already persisted packet/finalization evidence
before this refactor. The remaining gap is propagating it, together with upstream
crop failures, to the parent recording's exact coverage decision—not inventing
a second crop mux-telemetry implementation.

## Deliberately separate / remaining work

Crop detection/blank/window/pool/queue metrics remain crop-specific. Encoder,
source-release, identity, and acquisition counters remain separate stages, even
when their healthy totals coincide. Python consumers retain independent wire
validation rather than calling into the producer's C++ implementation.

This refactor does not rename the crop worker's handoff-based `encoded_frames_`
counter, fix all crop failure-counter branches, add a media-independent master
frame writer, or enable moving-crop-only recording. Those are explicit remaining
items in Citrus's `docs/detection_crop_only_master_frame_record_plan_2026-09-06.md`.
No source coverage is inferred from packet balance alone.

## Validation

- Shared-module unit tests cover stage separation, all 256 small balance states,
  equivalence to the existing external v2 packet/frame rule, disjoint aggregation,
  first-error retention, per-output independence, reset, and flush/failure gates.
- Writer tests cover exact v2 JSON packet fields, real container lifecycle,
  injected mux failure, queue rejection, and construction failure.
- Seven focused suites passed: packet telemetry, FFmpeg writer failures,
  recording-session manifest, timing evidence, rolling metadata projection,
  acquisition-index authority, and external-recorder verification.
- External recorder built successfully. Production manifest test target rebuilt
  affected acquisition, native crop, full-frame, shared output and writer sources.
- No cameras, deployment, recording, or live performance certification involved.
