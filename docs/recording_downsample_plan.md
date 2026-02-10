# Recording Downsample Plan (Future Work)

Goal: allow users to configure a recording downsample factor (e.g., 2x) so
recorded videos are smaller than the live stream resolution. Default should
remain full resolution.

## Proposed behavior

- Add a `record_downsample` factor (integer >= 1).
- Default `record_downsample = 1` (no downsample).
- Optionally allow per‑camera override in each camera config JSON.
- Encoder output resolution is `camera_width / record_downsample` and
  `camera_height / record_downsample` (rounded down to an even number).

## Config options

Two possible placements:

1) Global (encoder config)
   - `record_downsample` in the encoder config JSON
2) Per‑camera (camera config JSON)
   - `record_downsample` in each camera JSON

If both exist, per‑camera overrides global.

## Pipeline changes (high level)

- `EncoderPreprocessWorker`
  - Allocate a downsample buffer sized to the target resolution.
  - Resize the RGBA image before NV12 conversion (use NPP resize).
  - Convert the resized image to NV12 for encoding.
- `EncoderHwWorker`
  - Initialize NVENC with downsampled width/height.
  - Update metadata tags to report the downsampled resolution.
- `GPUVideoEncoder` (headless path)
  - Mirror the same downsample logic and encoder dimensions.

## Constraints / validation

- `record_downsample` must be integer >= 1.
- Output width/height must be even (NV12 requirement).
- If the factor yields invalid dimensions, log and fall back to `1`.

## Testing ideas

- Record with `record_downsample=2`, verify output resolution and playback.
- Confirm metadata tags match the downsampled size.
- Confirm frames are not dropped and the output starts with a valid keyframe.
