# Headless Codec Quality Test Handoff

Date: 2026-04-09
Scope: handoff for the agent extending `orange_client` / experiment-spec support
so Orange can run codec-quality and future map-guided encoding studies through
the headless local experiment path.

Audience:

- the agent working on headless CLI / experiment-spec support
- the agent or engineer building the first automated codec-quality test suite

See also:

- `docs/headless_cli_design.md`
- `docs/headless_experiment_backend.md`
- `docs/experiment_runner_plan.md`
- `docs/codec_quality_evaluation_protocol.md`
- `docs/pre_encoder_reference_capture_plan.md`
- `docs/pre_encoder_reference_capture_todo.md`
- `docs/nvenc_benchmark_runsheet.md`
- `docs/nvenc_throughput_todo.md`

## What Has Been Decided

### 1. Quality Evaluation Should Prefer Pre-Encoder Reference Clips

The agreed evaluation model is:

- preferred reference: short pre-encoder reference clip
- fallback reference: short `lossless` encoded proxy
- do not treat long uncompressed capture as a normal recording mode

Reference:

- `docs/codec_quality_evaluation_protocol.md`

### 2. Pre-Encoder Reference Capture Is A Benchmark Feature, Not A Raw Mode

The agreed feature shape is:

- name / concept: `pre_encoder_reference_capture`
- opt-in only
- bounded by `max_frames` or `max_seconds`
- tap point after preprocess and before NVENC submission
- capture the prepared `NV12` representation actually seen by the encoder
- simple raw dump + sidecars in v1, not a polished container

Reference:

- `docs/pre_encoder_reference_capture_plan.md`
- `docs/pre_encoder_reference_capture_todo.md`

### 3. The Headless Path Should Remain The Shared Modern Backend

Do not invent a special benchmark-only recording backend.

The intended test path is still the modern recording pipeline already used by
headless local runs:

- `ModernRecordingPipeline`
- `EncoderPreprocessWorker`
- `EncoderHwWorker`
- `acquire_frames(...)`

Reference:

- `docs/headless_experiment_backend.md`

## Important Current State

### Committed Docs

The following planning docs are committed and should be treated as the current
design baseline:

- `docs/codec_quality_evaluation_protocol.md`
- `docs/pre_encoder_reference_capture_plan.md`
- `docs/pre_encoder_reference_capture_todo.md`
- the linked updates in:
  - `docs/encoding_importance_map_todo.md`
  - `docs/nvenc_benchmark_runsheet.md`
  - `docs/nvenc_throughput_todo.md`

### Current Headless Capabilities

The current headless local experiment path already supports:

- `orange_client --mode local`
- `--experiment-spec <path>`
- local record-only experiment matrices
- fixed `display=false`
- fixed `yolo=false`
- duration + warmup
- codec / preset / tuning / rate-control / quality matrix expansion

Relevant implementation:

- `src/orange_headless_client.cpp`

### Current Direct-Input Status

There is also an in-progress direct-NVENC-input code slice already in the
worktree.

Current understanding:

- it builds cleanly
- it is opt-in via `ORANGE_NVENC_DIRECT_INPUT=1`
- it is not yet runtime-validated and not yet committed

This matters because the future codec-quality suite should be able to compare:

- current copy path
- direct-input path

without changing the overall headless experiment framework.

## What The Headless Agent Should Build Toward

The next useful test-suite capability is not "every codec experiment forever."
It is a narrow, repeatable local matrix for:

1. current encoded output only
2. short pre-encoder reference capture
3. later static dish-prior map-guided output

The headless agent should therefore focus on:

- experiment-spec plumbing
- artifact naming and run-summary visibility
- bounded reference-capture support
- a clean test matrix that the rest of the team can reuse

## Recommended Spec Extensions

### A. Reference Capture Block

Add a structured experiment-spec block rather than loose extra flags.

Suggested shape:

```json
{
  "fixed": {
    "duration_s": 10,
    "warmup_s": 2,
    "display": false,
    "yolo": false,
    "pre_encoder_reference_capture": {
      "enabled": true,
      "max_frames": 600
    }
  }
}
```

Expected semantics:

- disabled or omitted means no reference capture
- exactly one bound should be required in v1:
  - `max_frames`
  - or `max_seconds`
- this should remain local/benchmark-first, not remote-first

### B. Direct-Input Toggle

The experiment layer should be able to distinguish:

- current copy path
- direct-input path

Suggested first implementation:

- spec-driven environment toggle for `ORANGE_NVENC_DIRECT_INPUT`

Suggested spec field:

```json
{
  "fixed": {
    "nvenc_direct_input": false
  }
}
```

This is acceptable even if the runtime implementation still uses the env var
internally.

### C. Run Summary Columns

Once the above features exist, extend the experiment summary CSV / JSON with:

- `nvenc_direct_input`
- `pre_encoder_reference_capture_enabled`
- `pre_encoder_reference_capture_max_frames`
- `pre_encoder_reference_capture_max_seconds`
- `pre_encoder_reference_capture_status`
- `pre_encoder_reference_frames_captured`
- `pre_encoder_reference_bytes_written`

These should sit alongside the existing codec/preset/tuning/quality fields.

## First Test Matrix To Support

The first useful matrix should stay intentionally small.

### Block 1: Baseline Codec Comparison

One representative scene per run family, with:

- `hevc p1 ll vbr`
- `hevc p1 ll cqp q20`
- `hevc p1 lossless`

Why:

- enough to compare default-style behavior against cheaper RC and a high-fidelity
  proxy
- avoids a huge matrix before the reference path exists

### Block 2: Reference-Capture Smoke Tests

For one representative codec point:

- reference capture disabled
- reference capture enabled with `max_frames`
- reference capture enabled with `max_seconds`

Expected assertions:

- artifacts exist when enabled
- artifacts do not exist when disabled
- capture stops at the configured bound
- encoded output still exists and completes

### Block 3: Direct-Input Cross Product

After direct-input is runtime-ready:

- copy path + no reference capture
- copy path + reference capture
- direct-input path + no reference capture
- direct-input path + reference capture

Expected goal:

- prove the headless suite can compare the same codec point across both data
  paths without inventing a second benchmark harness

## Artifact Expectations

For a normal encoded run, the existing artifacts already matter:

- encoded video
- `recording_snapshot.json`
- `Cam*_pipeline_perf.csv`
- `nvidia_smi_dmon.csv` when available

For future reference-capture runs, expect additional artifacts:

- `Cam<serial>_preenc_ref.bin`
- `Cam<serial>_preenc_ref_index.csv`
- `Cam<serial>_preenc_ref.json`

The experiment runner should preserve these in each run folder and include
their presence / absence in the run summary.

## Guardrails

### Do Not Reframe This As Raw Recording

The feature should consistently be described as:

- pre-encoder reference capture

not:

- raw video mode
- raw recording mode
- raw encoding mode

Reason:

- the captured frames are prepared `NV12` encoder input, not sensor-native raw

### Do Not Add A Second Recording Backend

Keep using the modern recording path already under headless.

The experiment/test layer should be a control-plane extension, not a new hot
path.

### Keep The First Version Local And Record-Only

The current headless experiment path already assumes:

- local mode
- display off
- yolo off

That is the right place to start for the codec-quality suite too.

### Keep The Matrix Small Until The Reference Path Works

Do not expand into a giant codec/preset sweep for reference-capture work until:

- the reference artifacts are correct
- the bounds are respected
- the summary files expose the new fields cleanly

## Suggested Implementation Order For The Headless Agent

1. Extend experiment-spec parsing with:
   - `pre_encoder_reference_capture`
   - `nvenc_direct_input`
2. Thread those settings into local run setup.
3. Extend per-run summary rows to expose the new toggles.
4. Add artifact-presence checks for the reference outputs.
5. Add a small local test suite / fixture set for the first matrix above.
6. Only then broaden the experiment matrix or add richer comparison tooling.

## Suggested Questions The Headless Agent Should Not Block On

The headless agent should make reasonable assumptions instead of stalling on:

- final GUI exposure for reference capture
- whether the long-term replay format is `.bin` forever
- future TRT-driven map guidance
- remote/distributed orchestration

The immediate job is narrower:

- make local headless experiments able to request and summarize these features

## Current Best Single Reference For The Agent

If the agent reads only one file first, it should be:

- `docs/pre_encoder_reference_capture_plan.md`

Then:

- `docs/codec_quality_evaluation_protocol.md`
- `docs/headless_cli_design.md`
- `docs/headless_experiment_backend.md`
