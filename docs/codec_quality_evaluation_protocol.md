# Codec Quality Evaluation Protocol

Date: 2026-04-08
Scope: practical protocol for comparing Orange recording codec settings and
future map-guided encoding strategies against the right reference layer.

See also:

- `docs/nvenc_benchmark_runsheet.md`
- `docs/nvenc_throughput_todo.md`
- `docs/encoding_importance_map_todo.md`
- `docs/pre_encoder_reference_capture_plan.md`
- `docs/pre_encoder_reference_capture_todo.md`
- `docs/nvenc_direct_input_plan.md`
- `docs/recording_metadata.md`

## Goal

Decide whether a recording change actually improves task-relevant quality
without confusing codec damage with unrelated differences in acquisition,
preprocessing, or scene content.

This protocol is meant to answer questions like:

- does `hevc` preserve fish detail better than `h264` at the same practical
  throughput point?
- does `AQ` / `TemporalAQ` help or hurt for mostly static dish imagery?
- does a future static dish-prior or fish-aware map improve subject detail
  relative to current defaults?
- what quality gain, if any, is worth the bitrate or throughput cost?

## Core Rules

- Use the same source clip across strategies whenever possible.
- Keep reference capture short and explicit. Do not treat long uncompressed
  recording as the default workflow.
- Compare candidate outputs against the matching reference layer, not against an
  unrelated representation.
- Evaluate both image quality and runtime behavior. A prettier clip that causes
  queue growth or drops is not a clean win.
- Treat whole-frame perceptual judgment as secondary when the biologically
  important ROI is small relative to the frame.

## Reference Layers

Use the reference layer that matches the question being asked.

### 1. Sensor-Native Reference

Examples:

- raw Bayer
- Mono8
- unprocessed camera frames

Use this when the question is:

- did the camera or acquisition path lose information?
- did debayer, resize, or color conversion damage detail before encode?

Do not use this as the default comparator for codec tuning, because it mixes
codec effects with preprocessing effects.

### 2. Pre-Encoder Reference

Definition:

- the prepared recording-path frame immediately before compression

Use this when the question is:

- what did the codec remove?
- which codec strategy preserves the prepared fish detail better?
- did a map-guided scheme help relative to current encode settings?

This is the preferred reference for codec-strategy work because it isolates
codec damage from:

- debayer choices,
- resize choices,
- color-space conversion choices,
- and any future importance-map or delta-QP decisions.

### 3. Decoded Candidate Outputs

Definition:

- frames decoded from each candidate encoded video

Use these for:

- visual review,
- ROI crop comparisons,
- downstream segmentation / keypoint / labeling evaluation,
- file-size and bitrate tradeoff review.

### 4. Lossless Encoded Proxy

Examples:

- short `lossless` NVENC clips

Use this only as a practical fallback when a pre-encoder reference capture mode
does not yet exist.

It is useful, but it is still a codec decision, not perfect ground truth.

## Recommended Benchmark Ladder

The intended comparison ladder is:

1. capture a short reference clip,
2. encode the same clip with candidate strategies,
3. decode candidate outputs,
4. compare ROI detail, temporal behavior, file size, and runtime stability,
5. run downstream tasks on the decoded outputs,
6. only then decide whether a strategy is worth using at scale.

Preferred reference:

- short pre-encoder reference clip

Fallback reference if needed:

- short lossless clip with tightly controlled scene conditions

## Clip Selection

Keep the evaluation set small, repeatable, and representative.

Recommended first clip set:

- `empty_or_near_empty_static`
- `fish_mostly_still`
- `slow_swim`
- `rapid_motion_or_turn`
- `problem_case_reflection_or_low_contrast`

Guidelines:

- `5-10 s` per clip is usually enough for first-pass comparison
- keep camera settings fixed
- keep framing, lighting, and exposure fixed
- keep output resolution fixed inside one comparison block
- keep one recording session per run unless the test explicitly studies
  contention

## Protocol A: Preferred Offline Codec Bake-Off

Use this once a pre-encoder reference-capture mode exists.

1. Capture a short pre-encoder reference clip for each selected scene class.
2. Preserve enough metadata to reconstruct exact preprocessing state.
3. Encode that exact same clip with each candidate strategy offline.
4. Decode each candidate output to an analysis-friendly format.
5. Compare the decoded outputs against the same pre-encoder reference.
6. Run downstream task evaluation on the decoded outputs.

Why this is preferred:

- every codec sees the exact same source frames,
- differences are much easier to attribute,
- biological motion is no longer a confounder,
- quality comparisons become much more credible.

## Protocol B: Interim Live-Capture Fallback

Use this only until pre-encoder reference capture exists.

1. Record a short high-fidelity proxy reference, ideally `lossless`.
2. Record candidate settings immediately afterward under the same fixed scene
   conditions.
3. Keep the clips short enough that lighting and behavior do not drift badly.
4. Interpret differences cautiously because the fish motion will not be
   frame-identical.

This can still answer some questions well:

- throughput and stability differences,
- gross artifact differences,
- whether a setting is obviously unacceptable,
- whether a strategy is promising enough to justify a cleaner offline test.

It is weaker for fine-grained quality claims on a moving fish.

## What To Evaluate

### ROI-First Visual Review

Look at both:

- full-frame decoded output
- fish ROI crops

Prioritize:

- body edge fidelity
- fin and tail preservation
- eye and head detail
- small contrast features on or near the fish
- temporal stability inside the ROI
- visible blockiness, shimmer, ringing, or flicker near the fish

Background quality still matters, but only up to the point where it affects:

- fish visibility,
- segmentation / keypoint quality,
- or operator usefulness.

### Runtime Behavior

For each run, keep:

- output video
- `recording_snapshot.json`
- `Cam*_pipeline_perf.csv`
- `nvidia_smi_dmon.csv` when available
- per-run notes on GPU, clocks, and anything unusual

Track at minimum:

- sustained encode FPS
- drops / failures
- queue growth
- buffer-depth trends
- GPU encoder utilization
- file size / bitrate

### Downstream Task Quality

Whenever possible, compare decoded candidates using the same downstream task:

- segmentation
- keypoints
- detection
- tracking
- labeling utility

This should be a primary decision signal, not an optional afterthought.

### Objective Metrics

If objective image metrics are added later, use them carefully:

- ROI-focused metrics are more informative than whole-frame averages
- whole-frame perceptual metrics may overweight static background
- objective metrics should support, not replace, ROI review and downstream-task
  evaluation

## First Comparison Matrix For Importance-Map Work

For the first map-guided study, keep the matrix narrow:

- current default recording mode
- same codec / preset with cheaper RC behavior such as `cqp`
- short `lossless` reference proxy
- future `static_dish_prior` map output

Only after that should the matrix broaden to:

- motion-informed maps
- bbox-informed maps
- alternative codec / preset ladders

## Decision Policy

A strategy is a real improvement only if it satisfies both:

1. runtime correctness:
   - no sustained queue growth
   - no unacceptable drops or encode failures
   - stable sustained FPS for the target operating point
2. quality value:
   - better fish-detail retention, downstream-task quality, or both
   - at an acceptable bitrate / file-size cost

Do not treat "looks cleaner overall" as enough if the fish ROI is not improved.

## Current Recommended Next Step

The best first implementation for quality evaluation in this repo is:

1. add a short pre-encoder reference-capture mode,
2. keep it bounded to small clips,
3. use it to compare current defaults, `cqp`, and future static dish-prior
   encoding,
4. only then decide whether motion-aware or fish-aware maps are worth the extra
   complexity.

Implementation planning for that slice now lives in:

- `docs/pre_encoder_reference_capture_plan.md`
- `docs/pre_encoder_reference_capture_todo.md`
