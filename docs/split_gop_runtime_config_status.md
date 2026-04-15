# Split-GOP Runtime Config Status

## Purpose

This note records the current state of split-GOP runtime configuration on
`exp/gop-split-a16`, why there are currently two recording-related fields on
`CameraParams`, and the intended migration to a single runtime resolution model.

This is intentionally a status-and-plan artifact, not a final architecture
document. It is meant to keep the branch-level history understandable while the
runtime/config refactor is still in progress.

## Branch Context

The active runtime branch is:

- `exp/gop-split-a16`

Recent configuration/status commits on that branch:

- `80fd97e` `experiment_specs: add validated split-gop 100fps gop25 runs`
- `2efef9d` `config: add schema 3 split-gop camera configs`
- `9b4dc89` `config: fix schema 3 project integration`

The split-GOP runtime validation and telemetry work already on this branch
includes:

- ordered GOP release and backlog control
- helper routing counters
- latency telemetry
- dual-GPU `nvidia-smi dmon` capture
- static/runtime topology capture
- validated `100 fps`, `HEVC`, `gop=25`, `hybrid_split`, `raw` transfer runs on:
  - camera `2010095` using `GPU1 + GPU2`
  - camera `2010096` using `GPU5 + GPU6`

## Current Config Model

Schema 3 camera configs now persist a top-level `recording` object. That object
contains the broader recording intent for a camera:

- `recording.encode`
- `recording.output`
- `recording.strategy`
- `recording.constraints`
- `recording.resources`

The relevant runtime structs are currently:

- `CameraRecordingConfig`
- `RecordingStrategyConfig`

On the branch today, `CameraParams` contains both:

- `recording`
- `recording_strategy`

This is a transitional state.

## What The Two Fields Mean

### `camera_params.recording`

`recording` is the schema-3 persisted config model.

It is the on-disk representation of what we want for that camera:

- encode settings
- output settings
- split-GOP strategy settings
- topology/resource preferences

This is the field that should survive long-term as the persisted camera config.

### `camera_params.recording_strategy`

`recording_strategy` is the older runtime-only scheduling struct that the
split-GOP implementation was originally built around.

It currently describes only the scheduling and buffering policy:

- `mode`
- `split_gop.placement`
- `split_gop.encoder_gpu_ids`
- `source_encoder_policy`
- `transfer_mode`
- split-GOP queue/buffer limits

This field exists today only because the runtime implementation was completed
before schema 3 was added.

## Why Both Exist Right Now

The branch history happened in this order:

1. The runtime split-GOP implementation was built around
   `CameraParams.recording_strategy`.
2. Later, schema 3 added `CameraParams.recording` as the persisted camera config
   surface.
3. Schema 3 was then integrated onto `exp/gop-split-a16` without rewriting the
   entire runtime path in the same change.

So the branch currently keeps both fields and mirrors them.

Current synchronization behavior:

- camera JSON parse fills `camera_params.recording`
- parse then copies:
  - `camera_params.recording_strategy = camera_params.recording.strategy`
- headless experiment-spec overrides still target `RecordingStrategyConfig`
- headless override application copies the override into both:
  - `camera_params.recording_strategy`
  - `camera_params.recording.strategy`
- camera config save writes `recording`
- before save, the writer copies:
  - `camera_params.recording_strategy -> camera_params.recording.strategy`

So `recording_strategy` is currently a runtime alias/bridge, not an independent
source of truth.

## What Is Actually Implemented Today

The branch does **not** have several independent recording implementations. It
has one recording pipeline with mode-dependent behavior.

### Real, validated paths

These are real and tested:

- `single_session`
- `split_gop + multi_gpu + hybrid_split`

### Modeled in config, but not fully implemented

These should be treated as transitional or incomplete:

- `split_gop + single_gpu`
  - config can express it
  - runtime does not currently instantiate multiple encode lanes on one GPU
- `split_gop + pure_offload`
  - ingress understands the policy
  - helper-target construction is still centered on `hybrid_split`

## Where Runtime Still Reads The Old Alias

The branch still uses `recording_strategy` directly in several core runtime
places:

- `EncoderHwWorker`
- `RecordingIngress`
- headless experiment-spec override application
- some runtime summaries/logging

This is why removing `recording_strategy` immediately would be risky.

## Another Important Transitional Gap

Schema 3 added more than just strategy:

- `recording.encode`
- `recording.output`
- `recording.resources`

But runtime is not yet consistently driven from those persisted fields.

Today:

- codec/preset/tuning/rate-control/quality/GOP still primarily flow in through
  pipeline constructor arguments and headless CLI/spec settings
- resource knobs such as acquisition pool size and encoder-entry pool size are
  still largely env-based or local helper based

So schema 3 currently persists the desired shape cleanly, but runtime still
pulls parts of the effective configuration from multiple sources.

## Goal State

The goal is to move to a single runtime resolution model:

- persisted source of truth: `camera_params.recording`
- runtime derived config: `ResolvedRecordingConfig`

That means:

- `CameraParams.recording` remains the camera-config field
- `CameraParams.recording_strategy` is removed after migration
- runtime code consumes a resolved, per-run config object instead of directly
  reading scattered camera/config/env/CLI fields

## Proposed `ResolvedRecordingConfig`

The intended runtime object should look conceptually like:

- `encode`
- `output`
- `strategy`
- `resources`
- optional provenance / resolution notes

That struct should be built once per camera/run from:

