# July 31 Rolling Recording Failure

- Date investigated: 2026-08-04
- Incident recording: `2026_07_31_18_05_12`
- Status: root cause confirmed; GOP-authority, bounded-buffer, and
  single-write rolling fixes implemented; live multi-shard soak still pending
- Safety status: do not use the affected rolling full-frame recorder for
  another long acquisition until the fail-fast and bounded-memory gates below
  pass

## Outcome

The intended overnight four-camera recording did not stop because its duration
elapsed and did not run out of disk space. The external recorder processes grew
without bound until the Linux kernel invoked the global OOM killer.

The immediate cause was a split GOP authority:

- the camera configs, external-recorder contract, supervisor plan, and recorder
  CLI declared `gop = 25`;
- the descriptors actually sent by the live Orange recording ingress used
  30-frame GOPs; and
- the external recorder grouped submitted frames using the descriptor GOP, but
  independently grouped returned encoded packets using its configured
  25-frame GOP.

Those groups could not complete against each other. Compressed packets remained
in `MergedGopOutput::pending_gops_` instead of being released continuously to
the merged and rolling writers. There was no enforced limit on the pending GOP
map or its compressed bytes.

The recording contract also requested `record_for_seconds = 93600`, which is
26 hours. This does not match the operator's intended 24 hours.

## Evidence

Recording root:

```text
/home/jeremy/orange_data/exp/unsorted/2026_07_31_18_05_12
```

The routing CSVs prove the live descriptor shape. For example, Cam2010094 uses
`gop_index = 0` for `frame_index_within_gop = 0..29`, then starts GOP 1 at
recording frame 31. The persisted contract and supervisor plan both declare
`gop = 25`.

The authority split present in the incident build was visible in the
implementation as follows:

- `src/session/recording_session.cpp` constructs the live recording pipeline
  from the mutable GUI `encoder_config.gop_length`;
- `src/external_recorder_contract_utils.cpp` independently materializes the
  recorder GOP from `camera.recording.encode.gop_length`;
- `tools/external_recorder_ipc_probe.cpp::MergedGopOutput::note_submitted()`
  indexes submitted frames by `FrameDescriptor::gop_index`; and
- `MergedGopOutput::submit_packets()` recomputed a GOP from the packet timestamp
  and the recorder's local `gop_length_`.

`refresh_complete_locked()` requires the submitted and emitted counts in one
map entry to agree. With 30 submitted frames and 25 emitted packets assigned to
the nominal same index, the ordered release frontier could not advance.

Kernel journal evidence:

- at 18:55:19, all four `external_record` processes already had approximately
  56-58 GB anonymous RSS;
- at 18:55:25, the kernel killed Cam2010095's recorder at approximately
  58.4 GB anonymous RSS; and
- at 19:45:36, the kernel killed Cam2010093's recorder at approximately
  116.1 GB anonymous RSS.

The resident growth closely tracks the compressed bytes produced at 150 Mbps.
This is direct evidence of recording payload retention rather than a small
metadata leak.

## Failure Propagation

The recorders failed in pairs:

1. Cam2010095 was OOM-killed after about 49.5 minutes.
2. Cam2010096 then lost its client connection, forced its pending output through
   finalization, and exited with `ack_write_failed` after about 49.4 minutes.
3. Cam2010093 was OOM-killed after about 99.4 minutes.
4. Cam2010094 then lost its client connection and forced finalization. It later
   aborted with `terminate called without an active exception` after attempting
   to create an empty clip writer with no dimensions.

Orange acquisition and analytics continued after the video recorder failures.
The session therefore lacks a coordinated all-camera recording failure
boundary.

## Artifact Inventory

| Camera | Approximate intake span | Best current video evidence | Current interpretation |
| --- | ---: | --- | --- |
| 2010093 | 99.4 min | 111.8 GB shard without `moov` | Not playable; potentially recoverable |
| 2010094 | 99.4 min | Complete 178,837-frame shard, 99m 21s | Playable; prefer the shard over the shorter merged file |
| 2010095 | 49.5 min | 55.7 GB shard without `moov` | Not playable; potentially recoverable |
| 2010096 | 49.4 min | Complete 88,901-frame shard/base, 49m 23s | Playable |

There is no directly playable synchronized four-camera span because the
Cam2010093 and Cam2010095 containers lack their terminal MP4 metadata. Preserve
their shard MP4s, encode CSVs, routing CSVs, and finalization sidecars. Any
recovery attempt must operate on copies.

The rolling products are not authoritative:

- Cam2010094 clip 2 occupies about 33.8 GB but is not readable by `ffprobe`,
  even though its finalization sidecar says `complete`.
- Cam2010094 attempted to create a zero-byte clip 3 before aborting.
- Cam2010096 clip 0 contains 54,000 encoded packets but its metadata covers
  recording frame IDs 1 through 64,800.
- The 64,800-frame metadata span is exactly 2,160 descriptor GOPs times 30,
  while the 54,000-packet video span is 2,160 recorder GOPs times 25.

This is also a semantic alignment failure: a playable clip cannot safely be
aligned to acquisition frames using the affected metadata.

## Bitrate, Frame Rate, And Storage

The storage budget is based on bitrate and elapsed time, not directly on frame
rate:

```text
bytes = bitrate_bits_per_second * duration_seconds / 8
```

At a fixed 150 Mbps per camera:

| Duration and copies | Four-camera storage |
| --- | ---: |
| 24 hours, one copy | 6.48 TB decimal / about 5.89 TiB |
| 24 hours, three copies | 19.44 TB decimal / about 17.68 TiB |
| 26 hours, one copy | 7.02 TB decimal / about 6.39 TiB |
| 26 hours, three copies | 21.06 TB decimal / about 19.15 TiB |

Frame rate changes the bits available per encoded frame, assuming the encoder
continues to deliver 150 Mbps:

| Frame rate | Approximate bits per frame |
| ---: | ---: |
| 30 fps | 5.0 Mbit |
| 100 fps | 1.5 Mbit |
| 700 fps | 0.214 Mbit |

Frame rate changes storage indirectly only when it changes the achieved
bitrate, such as under constant-quality encoding, unusually easy content,
encoder throughput limits, skipped frames, or dropped frames.

The July 31 files confirm the fixed-rate calculation empirically. A nominal
30-minute clip is about 33.76 GB; 150 Mbps for 1,800 seconds predicts 33.75 GB.

## Unintended Output Multiplicity

The affected single-shard rolling path can write the same encoded packet into
three physical video representations:

1. the encoder shard MP4;
2. the merged session MP4; and
3. the active rolling clip MP4.

This does not perform three encodes, but it can require approximately three
times the storage and write bandwidth. Even if shard MP4s are deleted after a
successful merge, preflight must budget peak simultaneous disk usage, not only
the desired final retained size.

The run's recorder status reported approximately 5.7-5.9 TB free and
`storage_min_free_bytes = 0`. Thus storage preflight had no meaningful hard
threshold. A healthy 24-hour run would not have fit one complete four-camera
copy at the measured bitrate with the reported remaining capacity, much less
the current three-output peak.

## Design Decisions

### 1. One resolved recording authority

At recording preparation, Orange must freeze one immutable per-stream runtime
recording configuration. The live ingress, recorder contract, supervisor plan,
encoder, recording snapshot, and H5/session metadata must all reference that
same resolved object or its fingerprint.

The recorder protocol handshake must include at least:

- frame rate;
- GOP length;
- codec;
- rate-control mode;
- target and maximum bitrate;
- output dimensions and pixel contract; and
- session and stream identity.

The recorder must reject the session before accepting payload frames if the
handshake differs from its launch plan.

### 2. Do not infer packet identity twice

The encoded packet completion path should retain the source submission identity
through encode completion. Returned packets should carry the canonical GOP and
recording-frame association established at submission. The merger must not
derive a second GOP identity from timestamps and a separately configured
length.

As defense in depth, the recorder must validate descriptor rollover:

- `frame_index_within_gop` must be below the declared GOP length;
- the next GOP must begin at the declared boundary; and
- monotonic recording-frame and GOP relationships must hold.

A 25/30 mismatch should fail at the first inconsistent descriptor rather than
after tens of gigabytes have accumulated.

### 3. Bound every queue and reorder surface

The external merged output needs enforced limits for:

- pending GOP count;
- pending compressed bytes;
- maximum age of the release frontier;
- merged-writer queued packets and bytes; and
- rolling-writer queued packets and bytes.

The limits must appear in status telemetry. Crossing a hard limit must stop the
recording coherently; silently retaining more payload is not allowed.

The existing camera `split_gop.max_inflight_gops` and
`split_gop.max_buffered_bytes` do not currently bound
`MergedGopOutput::pending_gops_` in the external recorder.

### 4. Make rolling clips the primary long-run representation

For long-running recording, write each encoded packet once into the active
rolling clip. Treat `recording_session.json` and `recording_clip_index.json` as
the logical full recording.

Recommended policy:

- rolling clips are the authoritative retained video artifacts;
- do not simultaneously write a full merged session MP4;
- do not write a duplicate single-shard MP4 in production rolling mode;
- preserve diagnostic shards only under an explicit diagnostic/recovery flag;
  and
- if a monolithic MP4 is wanted later, create it as an optional offline remux
  from finalized clips.

This makes the normal retained-copy multiplier one and limits crash exposure to
the active clip rather than the entire 24-hour container.

### 5. Duration-aware storage preflight

For capped VBR/CBR streams, preflight should use a conservative rate, normally
`max_bitrate_bps`, and calculate:

```text
required_peak_bytes =
    sum(stream_max_bitrate_bps * duration_seconds / 8)
    * peak_simultaneous_copy_multiplier
    * safety_headroom
    + reserved_free_space
```

The plan and UI should display:

- requested duration in hours and seconds;
- estimated bytes per camera and aggregate;
- retained and peak copy multipliers;
- current available bytes;
- required safety headroom; and
- pass, warning, or fail status.

Constant-quality modes require either an explicit maximum bitrate, a validated
empirical upper bound, or a clearly labeled non-guaranteed estimate. The UI
must not present a fixed-quality estimate as a hard storage guarantee.

### 6. Coordinated failure semantics

When any required full-frame recorder dies, reports a queue-bound violation,
or disconnects unexpectedly:

- latch the complete recording session failed;
- stop accepting new recording frames for all required cameras;
- drain/finalize the surviving recorders within a bounded timeout;
- leave camera streaming optional and separate from recording state;
- show the operator a persistent recording-failed state; and
- record the common failure boundary and per-camera last accepted frame.

Continuing analytics is acceptable only if the UI explicitly says video
recording has failed. It must not look like the recording is still healthy.

### 7. Finalization means validated media

`complete` must require more than a successful trailer-writing call. At minimum
the post-finalization validator must prove:

- nonzero dimensions and a readable video stream;
- expected codec and frame rate;
- sidecar packet count equals container packet/frame count;
- first and last packets can be decoded; and
- clip metadata frame coverage agrees with packet associations.

The Cam2010094 clip-2 result proves the present finalization status is not a
sufficient media-validity claim.

## Implementation Checklist

### Recovery first

- [ ] Mark the incident tree read-only for ordinary cleanup workflows.
- [ ] Record checksums and sizes for Cam2010093/95 shard MP4s and their CSVs.
- [ ] Determine whether the MP4 sample tables can be reconstructed from the
      surviving packet-size/timestamp evidence.
- [ ] Attempt reconstruction only on copies and validate decoded samples across
      the recovered time range.
- [ ] Publish an explicit recovered/not-recoverable inventory per camera.

### Recurrence prevention

- [x] Persist the exact resolved live pipeline GOP in the recording snapshot.
- [x] Generate the external contract from the same resolved runtime config.
- [x] Add recorder handshake GOP/config fingerprint validation for the
      explicitly scoped frame-rate/GOP identity.
- [x] Carry submission GOP/frame identity through encoder completion.
- [x] Reject invalid descriptor GOP rollover immediately.
- [x] Bound pending GOP count, pending bytes, and frontier age.
- [x] Supply nonzero queue limits to every `FFmpegWriter` in this path.
- [x] Export current/peak pending bytes and RSS-adjacent health telemetry.

### Storage and artifact topology

- [ ] Add duration/rate/camera-count/copy-multiplier storage estimation.
- [ ] Enforce a nonzero reserved-free-space policy.
- [x] Make rolling clips the only production video write in rolling mode.
- [x] Make shard preservation explicitly diagnostic.
- [ ] Make monolithic reconstruction an optional offline remux.
- [ ] Correct the 24-hour profile to `86400` seconds.

### Lifecycle and validation

- [ ] Propagate one required-recorder failure to a coordinated recording stop.
- [ ] Persist the common failure boundary and each stream's last accepted frame.
- [ ] Prevent creation of an empty terminal clip.
- [ ] Require media validation before claiming a clip is complete.
- [ ] Reject metadata/video frame-range disagreement.

### Tests before another overnight run

- [ ] A 25/30 mismatch fixture fails at the first bad boundary with bounded RSS.
- [ ] An accelerated many-clip test demonstrates flat pending bytes and bounded
      process RSS.
- [ ] A four-camera storage-preflight test covers 24- and 26-hour plans and
      retained versus peak copy counts.
- [ ] Killing one recorder produces a coordinated, bounded four-camera stop.
- [ ] Every finalized clip passes open, first/middle/last decode, count, and
      metadata-alignment checks.
- [ ] A real 30-minute four-camera run has one finalized clip per camera, no
      duplicate production MP4 writes, no drops, and stable recorder RSS.
- [ ] A longer soak crosses several rollovers with stable memory before a
      24-hour acquisition is authorized.

## Correctness Slice Implemented 2026-08-04

The first fail-closed slice is now implemented:

- [x] GUI pipeline construction freezes one `ResolvedRecordingConfig` per
      selected camera and later external-contract materialization consumes
      that exact value instead of rebuilding codec/profile/GOP fields from
      mutable camera defaults.
- [x] The IPC HELLO in both directions carries `frame_rate`,
      `resolved_gop_length`, and an independently computed
      `frame_rate_and_gop_v1` fingerprint. Either peer rejects a mismatch
      before a `FRAME` descriptor is accepted.
- [x] The recorder requires a validated `CLIENT_HELLO` before frame intake.
- [x] Every frame descriptor is checked against the recorder's declared GOP.
      The preserved 25/30 regression reaches its first invalid relationship at
      frame 26 and is rejected there.
- [x] `recording_snapshot.json` now records each selected camera's frozen
      runtime frame rate, requested and resolved GOP, encoder profile, and the
      same scoped frame-rate/GOP fingerprint placed in the recorder contract.
- [x] The merged output registers the canonical
      `{recording_frame_id, gop_index, frame_index_within_gop}` at submission.
      NVENC's returned timestamp is now only an opaque lookup key for that
      immutable identity. The merger no longer divides a returned timestamp by
      its own GOP length. Missing, duplicate, or unknown completion identities,
      and incomplete final GOPs, fail finalization.
- [x] `MergedGopOutput` now has hard defaults of 8 pending GOPs and 256 MiB of
      compressed payload. Both limits are present in the materialized contract,
      supervisor command, status JSON, and summary JSON; crossing either limit
      throws a recording failure instead of retaining payload indefinitely.
- [x] The release frontier now also has a 2000 ms hard age limit. Every external
      shard, merged-session, and rolling-clip `FFmpegWriter` has an explicit
      512-packet/128-MiB queue ceiling, and a queue overflow or writer-thread
      failure aborts the recorder at the push that observes it rather than only
      surfacing at finalization.
- [x] Live status records current and peak pending GOP/byte/frontier health,
      merged and rolling writer queue occupancy, and Linux `VmRSS`/`VmHWM`.
      Final summaries preserve peak buffer values and a final process-memory
      snapshot for soak-test review.
- [x] Per-frame latency samples no longer accumulate without limit. Each metric
      uses a deterministic run-wide reservoir capped at 65,536 doubles and the
      summary discloses observed/retained/capacity counts for its approximate
      p95.
- [x] Rolling output now writes each ordered encoded packet only to the active
      clip MP4. It no longer opens a monolithic session writer. With the
      default `preserve_shard_mp4s = false`, encoder shard MP4 writers are not
      opened either; diagnostic shards remain an explicit opt-in.
- [x] Summary metadata declares `authoritative_video_output.mode =
      "rolling_clips"`, reports that no session MP4 was written, and leaves the
      legacy flat `outputs.mp4` path empty. GUI/headless manifest bridges and
      strict verification resolve media from `rolling_output.clips[]`.

Focused contract and phased-start tests cover the 25/30 HELLO mismatch, the
frame-26 descriptor mismatch, exact preservation of out-of-order completion
identity, duplicate/unknown identity rejection, exact-limit and over-limit
pending-budget and frontier-age behavior, frozen-config snapshot and contract
materialization, and propagation of every hard limit through the supervisor
command.

This does not complete the long-run hardening checklist. Coordinated
four-camera stop, duration-aware storage preflight, media/range validation, and
a real multi-rollover RSS soak remain required before another overnight run.
The current fingerprint scope also does not yet bind bitrate, dimensions,
or the complete pixel/encoder contract; its name is intentionally narrow so
artifacts do not overclaim what was checked.

## Acceptance Criteria For Long Recording

Do not call the rolling path production-ready until all of the following hold:

- every artifact reports the same runtime configuration fingerprint and GOP;
- pending compressed bytes remain bounded and return near baseline after every
  GOP release;
- recorder RSS reaches a stable plateau rather than tracking bytes encoded;
- each encoded packet is written to one production video artifact;
- projected peak disk use plus headroom fits before recording starts;
- any required-recorder failure terminates the recording group coherently;
- every clip is independently playable and metadata-aligned; and
- a multi-rollover soak passes before an overnight run.
