# Split-GOP Runtime Config Status

## Purpose

This note records the current state of split-GOP runtime configuration on
`exp/gop-split-a16`, what was transitional during schema-3 integration, and the
remaining migration steps toward a single runtime resolution model.

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
- `710f3f2` `recording: add resolved runtime config`
- `b341250` `recording: migrate runtime consumers to resolved config`
- `55352c1` `recording: resolve resource pool sizes from config`

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
- `ResolvedRecordingConfig`

On the branch today, `CameraParams` contains:

- `recording`

The older `recording_strategy` alias has now been removed from `CameraParams`.

## What The Current Fields Mean

### `camera_params.recording`

`recording` is the schema-3 persisted config model.

It is the on-disk representation of what we want for that camera:

- encode settings
- output settings
- split-GOP strategy settings
- topology/resource preferences

This is the field that should survive long-term as the persisted camera config.

### `ResolvedRecordingConfig.strategy`

`RecordingStrategyConfig` is still the runtime scheduling sub-struct that the
split-GOP implementation uses internally.

It currently describes only the scheduling and buffering policy:

- `mode`
- `split_gop.placement`
- `split_gop.encoder_gpu_ids`
- `source_encoder_policy`
- `transfer_mode`
- split-GOP queue/buffer limits

It now lives inside the resolved runtime object rather than as a second field on
`CameraParams`.

## What Was Transitional

The branch history happened in this order:

1. The runtime split-GOP implementation was built around
   `RecordingStrategyConfig` directly.
2. Later, schema 3 added `CameraParams.recording` as the persisted camera config
   surface.
3. Schema 3 was then integrated onto `exp/gop-split-a16` without rewriting the
   entire runtime path in the same change.
4. `ResolvedRecordingConfig` was added so GUI/headless/spec/env flows could
   produce one per-run runtime object.
5. Runtime consumers were migrated to read from that resolved object.

The branch no longer keeps both camera fields.

The old transitional behavior was:

- camera JSON parse fills `camera_params.recording`
- headless experiment-spec overrides still target `RecordingStrategyConfig`
- headless override application now writes into:
  - `camera_params.recording.strategy`
- camera config save writes `recording`
- runtime resolution builds:
  - `ResolvedRecordingConfig`

So the alias/bridge step has been removed, but the strategy sub-struct still
exists as part of the resolved runtime config.

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

## What Is Cleaned Up Now

These runtime paths now consume `ResolvedRecordingConfig` instead of the old
camera-level alias:

- `ModernRecordingPipeline`
- `EncoderHwWorker`
- `RecordingIngress`
- GUI pipeline construction
- headless pipeline construction

Headless experiment specs still express overrides as `RecordingStrategyConfig`
JSON, but those overrides now land in `camera_params.recording.strategy` and
then flow through the resolver.

GUI and headless callsites now build one explicit runtime-override object before
calling the resolver, rather than passing a long list of loose encode/output
arguments.

## Remaining Transitional Gaps

Schema 3 added more than just strategy:

- `recording.encode`
- `recording.output`
- `recording.resources`

But runtime is not yet consistently driven from those persisted fields.

Today:

- codec/preset/tuning/rate-control/quality/GOP now flow through a single
  override struct into `ResolvedRecordingConfig`
- direct-input now also flows through that same override path for GUI/headless
  runs
- `recording.resources.acquire_work_entries` now feeds `CameraResources`
  allocation by default
- `recording.resources.encoder_entry_pool_size` now feeds
  `EncoderPreprocessWorker` allocation by default
- the corresponding env vars still intentionally override config values for
  experiments:
  - `ORANGE_ACQUIRE_WORK_ENTRIES_MAX`
  - `ORANGE_ENCODER_ENTRY_POOL_SIZE`
  - `ORANGE_NVENC_DIRECT_INPUT` remains supported as a fallback for older
    workflows, but it is no longer the normal headless control path

So schema 3 currently persists the desired shape cleanly, but runtime still
pulls parts of the effective configuration from multiple sources.

## Goal State

The goal is to move to a single runtime resolution model:

- persisted source of truth: `camera_params.recording`
- runtime derived config: `ResolvedRecordingConfig`

That means:

- `CameraParams.recording` remains the camera-config field
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
