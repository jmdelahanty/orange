# Video Metadata Comment Contract Plan

Date: 2026-06-21

## Goal

Orange should make every durable MP4 self-describing enough for downstream
Palette/Crimson consumers to recover encoder provenance and the pre-encode
source pixel contract. The MP4 `format.tags.comment` is useful because it
travels with the media file, but it should be a compact projection of metadata
Orange already owns, not a new parallel source of truth.

This applies to:

- full-frame camera MP4s,
- rolling full-frame clips,
- crop/derived MP4s, including external crop recorder outputs.

## Design Decision

Do not introduce a separate standalone video metadata schema that duplicates
the recording contracts. Instead, extend the existing artifact surfaces:

- `recording_snapshot.json` and `recording_session.json` remain the recording
  and output discovery contracts.
- `recording_outputs[serial].full` and `recording_outputs[serial].crop` remain
  the stable index from camera/output kind to video, metadata, keyframes,
  summary, packet counts, and status.
- Per-recorder summaries such as `Cam<serial>_external_summary.json` and
  `Cam<serial>_crop_external_summary.json` remain the per-video structured
  sidecars.
- `VideoEncodeProfile` remains the owner of the normalized encode profile and
  should become the owner of the source pixel contract used to derive MP4
  metadata tags.
- MP4 `title` and `comment` tags are derived from the same structured data and
  validated against it after finalization.

The only new sidecar file we should consider is a compatibility sidecar for an
MP4 that has no existing per-video JSON summary. Existing external recorder
summary JSONs should be extended rather than shadowed by a second file.

## Current State

In-process full-frame and crop writers already route MP4 metadata through
`VideoEncodeProfile`:

- `src/video_encode_profile.h`
- `src/video_encode_profile.cpp`
- `src/FFmpegWriter.cpp`
- `src/encoder_hw_worker.cpp`
- `src/crop_and_encode_worker.cpp`

`FFmpegWriter` already writes arbitrary container metadata tags into
`oc->metadata`, and `build_video_encode_metadata_tags()` already produces
`title` and `comment` tags for in-process outputs.

External recorder paths currently write simpler MP4 tags directly in
`tools/external_recorder_ipc_probe.cpp`, such as `producer`, `camera_serial`,
`codec`, `preset`, and `tuning`. Those tags are not yet the same
`title`/`comment` contract produced by `VideoEncodeProfile`, and they do not
carry a first-class source pixel contract.

The structured fields needed for the GoodCopBadCop examples generally already
exist across:

- `raw/external_recorder_contract.json`
- `raw/external_crop_recorder_contract.json`
- `Cam<serial>_external_summary.json`
- `Cam<serial>_crop_external_summary.json`
- `recording_session.json`
- `recording_snapshot.json`

The gap is normalization, embedding, and validation.

## Metadata Ownership

### Recording Discovery

Owned by `recording_snapshot.json`, `recording_session.json`, and
`recording_outputs`.

These should answer:

- which videos exist,
- which camera/output kind each video belongs to,
- where the video, metadata CSV, keyframe sidecar, summary JSON, and status JSON
  live,
- whether the output is finalized, incomplete, or failed,
- packet/frame counts and count source,
- recording backend mode such as `in_process` or `external_ipc`.

They should not duplicate every encode/source-pixel field from every sidecar.
They may carry pointers to the per-video summary and a small status summary.

### Encode Profile

Owned by `VideoEncodeProfile`.

This should answer:

- encoder name, normally `nvenc`,
- codec,
- preset,
- tuning,
- rate-control mode,
- quality value or QP,
- GOP length,
- FPS,
- output width and height,
- output kind, such as `full` or `crop`,
- source and encode GPU IDs when available,
- bitrate target/max/VBV after overrides are applied,
- output mode and factor/requested dimensions.

The existing comment builder should be extended rather than replaced.

### Source Pixel Contract

Owned by `VideoEncodeProfile` or a small helper object attached to it.

This should answer what Orange handed to the encoder before compression:

- stable contract id, for example `orange.camera.mono8.full_frame.v1` or
  `orange.crop.mono8.v1`,
- source pixel format,
- dtype,
- numeric range,
- color space,
- channel order,
- memory layout,
- source width and height,
- stride when known,
- pixel origin,
- source origin, such as `camera_dma`, `cuda_device_buffer`, or
  `analytics_crop`,
- transform to encoder, such as `mono8_to_nv12` or `crop_then_nv12`,
- expected encoded stream pixel format/color range when known.

This should be structured in JSON sidecars and projected into the MP4 comment
with flat keys such as `source_pixel_contract`, `source_pixel_format`, and
`source_transform_to_encoder`.

### Recorder Runtime

Owned by the external recorder contract and per-video summary JSONs.

These should answer:

- routing policy,
- shard count and shard GPU IDs,
- recorder GPU and analytics/source GPU,
- encode queue depth,
- prewarm slots/bytes/peer-copy mode,
- frames received/ACKed/encoded/skipped/dropped,
- packets written and MP4 bytes,
- merged output path and per-shard diagnostic outputs,
- MP4 metadata embedding attempted/succeeded/failure reason.

For external outputs, the summary JSON should also include the derived
`mp4_tags_expected` object or equivalent structured fields that can reconstruct
the comment.

## MP4 Tag Shape

Every durable MP4 should have:

```text
format.tags.title = <camera or stream id>
format.tags.comment = <semicolon-separated encode/source-pixel summary>
```

The `comment` should be stable but compact. Example:

```text
nvenc codec=hevc; preset=p1; tuning=ll; res=4512x4512; fps=100; color=0; output_kind=full; gop=25; rc=vbr; target_bps=150000000; max_bps=150000000; vbv=150000000; source_pixel_contract=orange.camera.mono8.full_frame.v1; source_pixel_format=mono8; source_pixel_dtype=uint8; source_pixel_range=0_255; source_color_space=linear_gray; source_channel_order=gray; source_memory_layout=HxW; source_width=4512; source_height=4512; source_coordinate_origin=top_left; source_origin=camera_dma; source_transform_to_encoder=mono8_to_nv12; encoder_input_format=nv12; encoded_pix_fmt=yuv420p; encoded_color_range=pc
```

Crop videos should include:

```text
output_kind=crop
source_camera=<serial>
res=<crop_width>x<crop_height>
source_pixel_contract=<crop contract id>
```

Keep the legacy numeric `color=<0|1>` field for compatibility, but do not rely
on it as the only pixel-format contract.

Orange mono acquisition videos should also stamp the encoded stream as full
range: HEVC/H264 VUI `video_full_range_flag = 1` and FFmpeg/MP4
`color_range = pc`. `yuv420p` remains the expected decoded FFmpeg pixel format;
the full-range flag is a numeric range interpretation, not a buffer-layout
change from Orange's NV12 encoder input.

For current crop media, `orange.video_metadata` schema v2 and the MP4 comment
also declare:

- `role=runtime_derived_acquisition_input`;
- `video_pixel_coordinate_space=crop_frame_pixels`; and
- `source_geometry_coordinate_space=full_frame_pixels`.

The retained `coordinate_space=full_frame_pixels` comment field is a deprecated
alias for source/placement geometry. Historical crop schema-v1 metadata with
`role=sidecar` is a legacy contract and is not rewritten.

## Sidecar Shape

For existing summary JSONs, add a `video_metadata` block rather than creating a
new top-level file:

```json
{
  "video_metadata": {
    "schema_id": "orange.video_metadata",
    "schema_version": 1,
    "video_path": "Cam2010093_external.mp4",
    "stream_id": "2010093",
    "camera_serial": "2010093",
    "output_kind": "full",
    "encoder": {
      "name": "nvenc",
      "codec": "hevc",
      "preset": "p1",
      "tuning": "ll",
      "rate_control_mode": "vbr",
      "quality_value": 20,
      "gop": 25,
      "encode_fps": 100,
      "target_bitrate_bps": 150000000,
      "max_bitrate_bps": 150000000,
      "vbv_buffer_size": 150000000,
      "color_mode_code": 0
    },
    "source_pixel_contract": {
      "id": "orange.camera.mono8.full_frame.v1",
      "format": "mono8",
      "dtype": "uint8",
      "range": "0_255",
      "color_space": "linear_gray",
      "channel_order": "gray",
      "memory_layout": "HxW",
      "width": 4512,
      "height": 4512,
      "stride_bytes": null,
      "origin": "top_left",
      "source_origin": "camera_dma",
      "transform_to_encoder": "mono8_to_nv12",
      "encoded_stream_pix_fmt_expected": "yuv420p",
      "encoded_stream_color_range_expected": "pc"
    },
    "mp4_tags_expected": {
      "title": "Cam2010093",
      "comment": "nvenc codec=hevc; ..."
    },
    "mp4_metadata_embedding": {
      "attempted": true,
      "succeeded": true,
      "reason": ""
    }
  }
}
```

The schema id above is intentionally scoped to the embedded block, not to the
entire recorder summary file. If an output has no natural summary JSON, write a
neighboring `*_video_metadata.json` with the same block as its root.

## Validation

Post-finalization validation should run `ffprobe` over every finalized MP4
listed in `recording_outputs`.

Validation should check:

- `format.tags.title` exists for camera/stream outputs,
- `format.tags.comment` exists,
- comment parses into key/value fields,
- comment codec/preset/tuning/fps/GOP/bitrate agree with the structured
  sidecar,
- comment source pixel contract fields exist,
- stream width/height/FPS/codec/pixel format agree with the structured
  sidecar where applicable,
- encoded stream pixel format/color range are checked separately from the
  source pixel format/color space,
- the sidecar records whether embedding was attempted and whether it succeeded.

Use the existing validators as the integration point:

- `scripts/recording_output_validation.py`
- `scripts/verify_external_recorder_session.py`
- GUI crop validation paths that already inspect external crop summaries.

## Implementation Order

1. Extend `VideoEncodeProfile` with a structured source pixel contract helper.
2. Extend `build_video_encode_metadata_tags()` to include the explicit
   source-pixel keys in the MP4 comment for in-process full and crop outputs.
3. Add JSON serialization for the encode profile/source pixel contract so
   recorder summaries can embed the same information.
4. Update external full-frame recorder MP4 writers to use the same tag builder
   instead of ad hoc simple tags.
5. Update external crop recorder MP4 writers to use the same tag builder with
   `output_kind = crop`.
6. Add `video_metadata` blocks to full-frame and crop external summary JSONs,
   including `mp4_tags_expected` and `mp4_metadata_embedding`.
7. Add post-finalization ffprobe validation gates.
8. Backfill or sidecar-patch important historical recordings only after the
   forward contract is stable.

## Non-Goals

- Do not make MP4 comments the authoritative schema.
- Do not duplicate all recorder/session metadata inside
  `recording_snapshot.json`.
- Do not put calibration-derived coordinate transforms in this contract. The
  per-camera raster coordinate frame already lives under
  `camera_runtime[serial].coordinate_frame`; spatial calibration remains a
  separate artifact.
- Do not require external shard diagnostic MP4s to carry the full contract if
  they are not durable outputs. The merged/final MP4s must carry it.
