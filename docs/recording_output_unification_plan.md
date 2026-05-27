# Recording Output Unification Plan

Date: 2026-05-26

Scope: define how Orange should reduce duplication between full-frame recording
and crop-video recording without merging their hot paths prematurely.

## Bottom Line

Keep full-frame and crop recording separate at the workload/scheduling level,
but unify the API boundaries around:

- output identity and artifact metadata
- encode profile/config construction
- encoded packet sinks
- start/drain/finalize lifecycle
- counters, health, and validation telemetry

Do not route crop video through the external full-frame recorder yet. Crop
recording is a sidecar workload fed by YOLO/crop production and shares crop
payloads with pose and preview. It needs different backpressure and failure
semantics from full-frame recording.

## Current State

Full-frame `real` recording:

```text
RecordingIngress
  -> EncoderPreprocessWorker
  -> EncoderHwWorker
  -> SharedRecordingOutput
  -> Cam<serial>.mp4
```

Full-frame `external_ipc` recording:

```text
RecordingIngress
  -> CUDA IPC descriptor
  -> external_recorder_ipc_probe / supervised recorder process
  -> external_recorder/Cam<serial>_external.mp4
```

Crop recording:

```text
YOLO
  -> CropProducerWorker / CropProducer
  -> CropFrame
  -> CropAndEncodeWorker
  -> NvEncoderCuda
  -> FFmpegWriter
  -> Cam<serial>_crop.mp4
```

Shared low-level pieces already exist:

- `NvEncoderCuda`
- `FFmpegWriter`
- recording folders and sidecar artifacts
- frame id / timestamp metadata

Important differences:

- Full-frame recording is continuous and ingest-authoritative.
- Crop recording is optional, detection/sidecar-driven, and may emit blank
  frames for no-detection frames.
- Full-frame recording may use split-GOP routing and process isolation.
- Crop recording currently stays in-process and should remain allowed to lag,
  skip, or drop without blocking full-frame recording or pose.
- Crop frames are owned by `CropProducer` and may have multiple consumers
  through `CropFrame` leases.

## Design Principle

Unify contracts, not ownership.

The desired architecture is:

```text
                 shared output/config/lifecycle APIs
                         /                    \
full-frame workload path                         crop sidecar workload path
RecordingIngress -> full recorder                CropProducer -> crop encoder
```

This lets Orange standardize artifacts and finalization while preserving the
latency and failure isolation that each workload needs.

## Non-Goals

- Do not put crop-video frames into the external full-frame IPC recorder in the
  first slice.
- Do not make crop encode part of `ModernRecordingPipeline`.
- Do not make full-frame recording depend on YOLO/crop state.
- Do not require crop-video encode to be lossless forever; preserve the current
  behavior first, then make it configurable.
- Do not change Palette ingest rules as part of this cleanup.

## Proposed Shared APIs

### Snapshot Schema Versioning

This work should introduce an explicit `recording_snapshot.json` schema bump.

Current docs describe `recording_snapshot.json` as schema version `1`, but the
writer should be audited before implementation because older emitted snapshots
may not consistently carry a top-level `schema_version`. The unification work
should normalize that first.

Recommended target:

```text
recording_snapshot.schema_version = 2
```

Use schema version `2` when Orange starts emitting first-class multi-output
metadata, especially:

- `outputs` / `recording_outputs` descriptors
- `encoders[serial].outputs.full`
- `encoders[serial].outputs.crop`
- crop output metadata that is intended to be interpreted alongside full-frame
  output metadata

Compatibility rule:

- Consumers reading schema `1` should keep using legacy locations:
  - `encoders[serial]` as the full-frame encoder
  - `crop_outputs[serial]` for crop artifacts
  - filename conventions for optional crop sidecars
- Consumers reading schema `2` should prefer the unified output descriptors and
  treat legacy fields as compatibility aliases when both are present.
- Producers may emit both schema-2 unified fields and legacy fields during the
  migration.

Do not bump the schema for small optional telemetry additions alone. Bump it
when the output model itself becomes multi-output and consumers are expected to
interpret `full` and `crop` through shared fields.

### `RecordingOutputDescriptor`

Purpose: one durable description for any video-like output.

Candidate fields:

```text
camera_serial
output_kind = full | crop | pose_debug | diagnostic
backend = in_process | external_ipc
role = ingest_authoritative | sidecar
video_path
metadata_path
keyframe_path
perf_path
summary_path
width
height
fps
codec
pixel_source_format
encoded_format
frame_count
packet_count
packet_count_source
status = pending | open | finalized | failed
```

Expected use:

- `recording_snapshot.json` lists every output consistently.
- `recording_session.json` can expose full and crop outputs through the same
  camera-artifact vocabulary.
- Validators can reason over outputs by `output_kind` and `role` instead of
  filename conventions alone.

### `VideoEncodeProfile`

Purpose: build NVENC settings from a named profile instead of hard-coding full
and crop settings in different workers.

Candidate fields:

```text
name
output_kind
codec
preset
tuning
rate_control_mode
quality_value
gop_length
fps
input_format
output_width
output_height
lossless
aq
temporal_aq
lookahead
direct_input
```

Initial profile mapping:

- `full_hevc_low_latency`: existing production full-frame profile.
- `crop_hevc_lossless_gop1`: current crop behavior:
  - HEVC
  - NV12 input
  - lossless tuning
  - GOP length `1`
  - QP `{0,0,0}`

The first implementation should only express existing behavior. Changing crop
quality or codec should be a later explicit experiment.

### `EncodedVideoSink`

Purpose: hide MP4 packet writing and finalization details behind a common
encoded-packet sink.

Candidate interface:

```cpp
struct EncodedPacketBatch {
    uint64_t recording_frame_id;
    uint64_t gop_index;
    bool completes_gop;
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint64_t> output_timestamps;
};

class EncodedVideoSink {
public:
    virtual bool open(const RecordingOutputDescriptor& descriptor) = 0;
    virtual bool submit(const EncodedPacketBatch& batch) = 0;
    virtual bool drain_and_close() = 0;
    virtual RecordingOutputDescriptor descriptor() const = 0;
};
```

Initial implementations:

- simple MP4 sink wrapping `FFmpegWriter` for crop video
- adapter around `SharedRecordingOutput` for in-process full-frame recording
- metadata-only descriptor adapter for external IPC outputs already written by
  the recorder

Do not require all implementations to support GOP ordering. Only full-frame
split-GOP needs GOP coordination.

### `RecordingOutputLifecycle`

Purpose: make start/drain/finalize semantics consistent without forcing the
same worker topology.

Candidate states:

```text
not_started
opening
open
draining
finalized
failed
```

Shared responsibilities:

- create parent folders
- open writer/sink
- record descriptor state
- count frames and packets
- close metadata files
- flush NVENC / writers
- publish final status
- surface finalization errors to session status

This should let crop recording stop using its own ad hoc lifecycle once a safe
adapter exists.

### `RecordingOutputCounters`

Purpose: standardize health signals across full and crop outputs.

Candidate fields:

```text
frames_offered
frames_accepted
frames_encoded
frames_skipped
frames_dropped
drop_reason_counts
packets_written
bytes_written
writer_queue_peak_packets
writer_queue_peak_bytes
writer_queue_overflow_events
finalization_errors
```

Crop-specific counters can remain crop-specific, but common counters should use
the same names.

## Current Artifact Inventory

Full-frame in-process single-clip recording currently emits one
ingest-authoritative output per camera:

- `Cam<serial>.mp4`
- `Cam<serial>_meta.csv`
- `Cam<serial>_keyframe.json`
- `Cam<serial>_pipeline_perf.csv`
- `Cam<serial>_acquisition_cadence_probe.csv`
- shared run files: `recording_session.json`, `recording_snapshot.json`,
  `ptp_sync_summary.json`, and optional model/pose/event telemetry

Full-frame rolling recording keeps the same per-camera video/metadata/keyframe
shape inside clip folders and adds run-level index files:

- `recording_clip_index.json`
- `recording_clip_index.csv`
- per-clip `clip_manifest.json`
- clip-local `Cam<serial>.mp4`, `Cam<serial>_meta.csv`, and
  `Cam<serial>_keyframe.json`

External IPC full-frame recording keeps the analytics run separate from the
recorder artifact root. The recorder owns the full-frame video output and its
diagnostic shard artifacts:

- merged compatibility MP4: `Cam<serial>_external.mp4`
- merged metadata/keyframe sidecars, for example
  `Cam<serial>_external_meta.csv` and `Cam<serial>_external_keyframe.json`
- recorder summary/finalization files, including
  `Cam<serial>_external_summary.json` and
  `Cam<serial>_external_video_sanity.json`
- GOP routing and detach/encode diagnostics, including
  `Cam<serial>_external_gop_routing.csv`, `external_detach.csv`, and
  per-shard encode CSVs
- diagnostic shard MP4s such as
  `Cam<serial>_external_shard0_gpu<id>.mp4`

Crop recording currently emits optional sidecar outputs per camera:

- `Cam<serial>_crop.mp4`
- `Cam<serial>_crop_meta.csv`
- `Cam<serial>_crop_keyframe.json`
- `Cam<serial>_crop_perf.csv`
- `Cam<serial>_crop_sidecar_perf.csv` when sidecar timing is enabled
- legacy snapshot metadata under `recording_snapshot.crop_outputs[serial]`

Experimental crop external IPC output keeps the same Orange-side crop metadata
and perf files, but moves crop video encode/write to an external recorder
process. That mode is selected with
`ORANGE_CROP_RECORDING_SINK_MODE=external_ipc` and is diagnostic until live
validation is complete. The first GUI/session supervision slice now launches
crop-suffixed recorder processes, keeps their sockets/artifacts separate from
full-frame external recorders, and indexes external crop MP4s under
`recording_outputs[serial].crop`.

Schema-2 artifacts keep these existing filenames and add
`recording_outputs[serial].full` and, when crop writing is active,
`recording_outputs[serial].crop`. The descriptor is an index over existing
outputs, not a request to move files or mux crop and full-frame streams.

## Recommended Implementation Checklist

### Phase 0: Contract Audit

- [x] List every currently emitted full-frame output artifact:
      root MP4, metadata CSV, keyframe sidecar, pipeline perf, session/index
      references, and external-recorder summaries.
- [x] List every currently emitted crop output artifact:
      crop MP4, crop metadata CSV, crop keyframe sidecar, crop perf, crop
      sidecar perf, and snapshot `crop_outputs`.
- [x] Decide canonical `output_kind` names.
- [x] Decide which outputs are `ingest_authoritative` versus `sidecar`.
- [x] Update docs with the current source-of-truth rule:
      full-frame output remains ingest-authoritative; crop output is sidecar.

### Phase 1: Shared Descriptor Only

- [x] Audit current emitted `recording_snapshot.json` files and writer code for
      top-level `schema_version` consistency.
- [x] Decide and document the top-level migration:
      `recording_snapshot.schema_version = 2` for unified output descriptors.
- [x] Add a C++ `RecordingOutputDescriptor` struct in a small shared module.
- [x] Add JSON serialization helpers for that descriptor.
- [x] Convert existing full-frame snapshot/session emitters to build descriptor
      JSON without changing file layout.
- [x] Convert existing crop snapshot/session emitters to build descriptor JSON
      without changing file layout.
- [x] Add crop MP4 container metadata tags matching the full-frame convention.
- [x] Keep all current file names unchanged.
- [x] Add unit tests for descriptor serialization.

Acceptance:

- [x] New snapshots explicitly carry `schema_version = 2`.
- [x] Legacy snapshot readers can still consume schema-1 artifacts.
- [x] Headless schema-v2 smoke validated:
      `experiment_specs/2010096_headless_recording_outputs_schema_v2_smoke_a16_gpu5.json`
      wrote matching `recording_session.json` and `recording_snapshot.json`
      `recording_outputs[2010096].full` descriptors.
- [ ] Existing GUI in-process artifacts are byte-layout compatible except for
      added metadata fields.
- [ ] Existing GUI external IPC artifacts still validate.
- [x] Existing crop-enabled artifact validator still passes:
      `/home/jeremy/orange_data/exp/unsorted/2026_04_22_22_53_43`.

### Phase 2: Encode Profile Extraction

- [x] Add `VideoEncodeProfile` and helpers that produce NVENC init params.
- [x] Express current full-frame profile through the helper without changing
      behavior.
- [x] Express current crop HEVC lossless GOP-1 profile through the helper
      without changing behavior.
- [x] Log the resolved profile for full and crop outputs.
- [x] Add tests for profile normalization and invalid values.

Acceptance:

- [x] Full-frame `real` recording produces the same codec/fps/GOP behavior:
      `/tmp/orange_video_encode_profile_smoke/2010096_headless_recording_outputs_schema_v2_profile_smoke_a16_gpu5`
      validated with `codec=hevc`, `fps=100`, and `gop_length=25`.
- [x] Crop video remains HEVC, crop-sized, GOP-1/lossless as before:
      covered by `video_encode_profile_tests`.
- [x] No new runtime config surface is required for the first slice.

### Phase 3: Simple Crop MP4 Sink

- [ ] Add `EncodedVideoSink` interface.
- [ ] Implement `SimpleMp4EncodedVideoSink` around `FFmpegWriter`.
- [ ] Move crop packet push and writer open/close code behind that sink.
- [ ] Keep crop metadata/perf writes in `CropAndEncodeWorker` for this phase.
- [ ] Preserve crop keyframe sidecar behavior.

Acceptance:

- [ ] Crop MP4, crop keyframe JSON, crop metadata CSV, and crop perf CSV remain
      compatible with current validators.
- [ ] Crop output closes cleanly when recording stops while streaming remains on.
- [ ] Crop queue/drop counters remain visible.

### Phase 3b: Experimental External Crop Video Sink

- [x] Add an opt-in crop sink selector:
      `ORANGE_CROP_RECORDING_SINK_MODE=external_ipc`.
- [x] Keep the existing in-process crop writer as the default.
- [x] Send crop-owned Mono8 CUDA buffers over the existing external recorder
      IPC protocol.
- [x] Keep Orange-side crop metadata and perf CSVs in the recording folder.
- [x] Supervise external crop recorder processes from the GUI/session layer.
- [x] Merge external crop recorder summaries into
      `recording_outputs[serial].crop`.
- [x] Teach validators to follow external crop descriptor paths for MP4 and
      keyframe validation while preserving Orange-written metadata/perf row
      alignment checks.
- [x] Keep external crop failures scoped to
      `recording_outputs[serial].crop` and
      `recording_backend.crop_recording`, without downgrading full-frame
      ingest outputs.

Acceptance:

- [ ] Four-camera GUI run with external full-frame IPC and external crop IPC
      keeps camera acquisition, YOLO, full-frame recording, crop rows, and GUI
      FPS healthy.
- [x] External crop MP4s validate with frame counts matching crop metadata.
      Latest checked artifact:
      `/home/jeremy/orange_data/exp/unsorted/2026_05_27_16_55_05`, with
      `1465` crop rows/frames on all four cameras and `0` external crop drops.
- [x] Missing or failed external crop recorder marks only the crop sidecar
      output failed, not the full-frame ingest output.
- [x] Re-run four-camera GUI external crop after the path-indexing fallback and
      256-deep crop encode queue patch. The first run at
      `/home/jeremy/orange_data/exp/unsorted/2026_05_27_16_34_46` launched
      external crop recorders but failed validation because disabled
      `merged_output` paths masked the external MP4/keyframe paths and the
      32-deep crop encode queues dropped frames. The latest checked rerun at
      `/home/jeremy/orange_data/exp/unsorted/2026_05_27_16_55_05` validates
      cleanly for recording artifacts when the separate GUI-FPS threshold is
      omitted.

### Phase 4: Lifecycle Adapter

- [ ] Add shared `RecordingOutputLifecycle` state/counter helper.
- [ ] Use it from crop output open/drain/finalize.
- [ ] Use it from full-frame in-process output where it fits cleanly.
- [ ] Do not alter external recorder process lifecycle in this phase; only
      mirror external output finalization into the same descriptor/counter
      shape after recorder summaries are read.

Acceptance:

- [ ] Stop/drain/finalize transitions are visible for full and crop outputs.
- [ ] Failed crop writer open or flush marks crop output failed without
      mislabeling the full-frame recording.
- [ ] Full-frame recorder failure still marks the recording unhealthy.

### Phase 5: Common Validation Surface

- [x] Teach validators to consume output descriptors.
- [x] Keep filename fallback for old artifacts.
- [x] Validate `output_kind = full` with strict ingest rules.
- [x] Validate `output_kind = crop` with sidecar rules:
      dimensions match crop size, crop metadata rows match crop frames, no
      unexpected crop drops in strict GUI validation.
- [x] Add summary output grouping by camera and output kind.

Acceptance:

- [x] Palette-facing full-frame validation remains strict.
- [x] Crop validation is explicit but does not become an ingest gate unless a
      caller asks for it.

## Future Optional Work

### Crop Externalization

Crop externalization is now an experimental opt-in path, not the default. It
uses crop-owned buffers, not original full camera frames, so full-frame source
lifetime remains governed by `CropProducer` rather than by crop video output.
The remaining open question is performance value: live runs must show whether
moving crop NVENC/MP4 writing out of process improves GUI frame pacing enough to
justify the extra process and IPC lifecycle.

Current experimental shape:

```text
CropProducer
  -> crop-owned CUDA IPC descriptor
  -> external crop sidecar recorder
  -> Cam<serial>_crop.mp4
```

This should be a separate sidecar recorder contract, not an extension of the
full-frame GOP recorder.

### Crop Quality Profiles

Once behavior is represented by `VideoEncodeProfile`, evaluate:

- lossless HEVC GOP-1 current profile
- lower-bitrate archival crop profile
- all-I versus short-GOP crop video
- still-image or frame-sequence crop sidecar for calibration/debug workflows

## Risks

- Over-unifying too early could make crop encode backpressure full-frame
  recording or pose.
- Moving metadata too early could break existing validators.
- A single generic sink could hide full-frame GOP ordering requirements.
- A single generic encode profile could obscure crop's current lossless/GOP-1
  expectations.

The implementation order above avoids these risks by starting with descriptors
and metadata, then moving code behind shared APIs only after behavior is
documented and covered by validation.
