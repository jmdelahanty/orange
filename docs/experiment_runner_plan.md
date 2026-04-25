# Experiment Runner Plan

Date: 2026-04-01
Scope: automate acquisition and recording benchmark experiments against the
current shipping recording path, while preserving a path to distributed
headless capture across remote camera servers.

Status:

- single-host runner and headless-backend unification are in scope now
- distributed headless experiment orchestration is explicitly deferred until the
  single-host path is working and trusted

See also:

- `docs/nvenc_benchmark_runsheet.md`
- `docs/headless_experiment_backend.md`
- `docs/headless_cli_design.md`
- `docs/headless_codec_quality_test_handoff.md`
- `docs/process_isolated_recorder_plan.md`
- `docs/nvenc_throughput_todo.md`
- `docs/output_artifacts_contract.md`
- `src/orange.cpp`
- `src/orange_headless_client.cpp`

## Goal

Add a repeatable experiment system that can:

- execute benchmark matrices automatically,
- collect stable artifacts for every run,
- score runs against explicit pass/fail policy,
- work first on one host,
- and leave a clear extension path to later span remote headless camera servers
  connected over the switch.

The system should benchmark the current shipping recording path, not a separate
legacy encode path.

## Core Design Decision

The experiment system should be a control-plane module over the existing
recording pipeline, not a new hot-path acquisition implementation.

That means:

- do not invent a second recording backend just for benchmarking,
- do not automate by scraping console output,
- do not treat the current `GPUVideoEncoder` headless path as the benchmark
  truth long-term.

Instead:

- unify GUI and headless around the same worker-based recording backend,
- use the existing recording artifacts as the experiment source of truth,
- and add an experiment runner that configures, starts, stops, evaluates, and
  summarizes runs.

## Current Implementation Status

As of the current branch state:

- GUI recording uses `ModernRecordingPipeline`
- headless recording now also uses `ModernRecordingPipeline`
- headless launches `acquire_frames(...)` instead of the old
  `GPUVideoEncoder`-driven path
- headless pre-creates the standard recording snapshot / summary artifacts

That means backend parity is much better than before, and this plan is no
longer theoretical.

However, this is still not the final session abstraction.

The important remaining gaps are:

- there is still no real `RecordingSession` type
- GUI and headless still duplicate lifecycle code above the worker pair
- the remote control contract still uses a loose `encoder_basic_setup` string
- the legacy remote stop semantics have not been cleanly migrated onto the
  modern `acquire_frames(...)` path yet

So the correct reading is:

- shared data-plane parity is partially in place
- control-plane unification is not done yet

## Why Headless Had To Be Updated

Today the main GUI path uses the modern worker pipeline:

- `acquire_frames(...)`
- `EncoderPreprocessWorker`
- `EncoderHwWorker`

See:

- `src/orange.cpp:3973`
- `src/orange.cpp:3983`
- `src/orange.cpp:4054`

Today the headless path still uses the legacy single-class encoder path:

- `acquire_frames_headless(...)`
- `GPUVideoEncoder`

See:

- `src/acquire_frames_headless.cpp:95`
- `src/orange_headless_client.cpp:165`

That meant a benchmark runner built directly on headless would measure a
different backend than the shipping GUI recording path. For NVENC throughput and
artifact-based evaluation, that was the wrong abstraction boundary.

That requirement is now partially satisfied:

- headless now has access to the same worker-based recording backend used by the
  main app.

What remains is making the control-plane lifecycle equally shared and reliable.

## Proposed Architecture

Split the work into four layers.

### 1. Shared Recording Backend

Introduce a reusable recording-session backend that both GUI and headless can
instantiate.

Responsibilities:

- create per-camera worker graph,
- allocate per-camera `CameraResources`,
- start worker threads,
- start acquisition threads on `acquire_frames(...)`,
- manage clean stop / drain / join,
- initialize per-run snapshot / artifact roots.

This backend should own the modern worker path:

- `EncoderPreprocessWorker`
- `EncoderHwWorker`
- optional `COpenGLDisplay`
- optional `YoloWorker`
- optional `CropAndEncodeWorker`

Suggested types:

- `RecordingSessionConfig`
- `PerCameraSessionConfig`
- `RecordingSession`

Suggested responsibilities:

- `RecordingSessionConfig`
  - resolved encoder config
  - output root
  - sync mode
  - display / YOLO enable flags
  - selected cameras
- `RecordingSession`
  - `start()`
  - `request_stop()`
  - `wait_stopped()`
  - `recording_folder()`
  - `session_id()`

### 2. Headless Parity Adapter

Refactor headless so it uses the shared recording backend instead of
`GPUVideoEncoder`.

Keep the existing ENet/network control plane in place:

- host broadcasts config / start / stop commands
- remote headless clients open cameras and run acquisition

See current control surfaces:

- `src/project.cpp:1611`
- `src/project.cpp:1627`
- `src/orange_headless_client.cpp:286`

But change what happens after `STARTCAMTHREAD`:

- instead of `acquire_frames_headless(...)` + `GPUVideoEncoder`,
- construct a `RecordingSession` backed by `acquire_frames(...)` and the modern
  workers.

This should preserve the distributed topology while eliminating the backend
mismatch.

### 3. Experiment Runner

Add a new module that executes an experiment spec over the shared recording
backend.

Responsibilities:

- parse experiment spec,
- expand matrix into concrete runs,
- apply one run config at a time,
- start a recording session,
- wait through warmup,
- record for the target duration,
- stop and drain,
- collect run artifacts,
- evaluate pass/fail,
- emit experiment-level manifest and summary.

Suggested types:

- `ExperimentSpec`
- `ExperimentRun`
- `ExperimentRunner`
- `RunEvaluation`

### 4. Report / Evaluation Layer

Use existing artifacts and analysis scripts as the evaluation source of truth.

Per run:

- read `recording_snapshot.json`
- read `Cam*_pipeline_perf.csv`
- generate plots and stats using `scripts/plot_pipeline_perf.py`
- compute pass/fail using the benchmark policy

At experiment level:

- produce `runs.csv`
- produce `runs.json`
- produce a compact summary of best stable settings per GPU / codec class

## Current Scope Boundary

In the current implementation pass, build only:

- shared worker-based recording backend
- headless parity on that backend
- single-host experiment runner
- local artifact evaluation and reporting

Defer for now:

- host-coordinated distributed experiment orchestration across remote headless
  servers
- experiment-level aggregation across multiple machines
- remote run scheduling / retries / fleet management

## Proposed Config Model

Use an explicit JSON spec file.

Example shape:

```json
{
  "experiment_id": "2026_04_01_nvenc_block_a_a6000",
  "notes": "2.8 MP @ 400 FPS single-session sweep",
  "selection": {
    "camera_serials": ["02010093"],
    "gpu_ids": [0]
  },
  "fixed": {
    "duration_s": 30,
    "warmup_s": 10,
    "display": false,
    "yolo": false,
    "sync_mode": "free_run",
    "output_root": "/abs/path/to/experiments"
  },
  "matrix": {
    "codec": ["h264", "hevc"],
    "preset": ["p1", "p3", "p5", "p7"],
    "tuning": ["ull", "ll", "hq"],
    "rate_control_mode": ["vbr"]
  },
  "policy": {
    "target_fps_tolerance_pct": 1.0,
    "require_zero_acq_starve": true,
    "require_zero_pre_drops": true,
    "require_zero_enc_fail": true,
    "require_zero_camera_drops": true
  }
}
```

Selection rules for the first version:

- `selection.camera_serials` should be the primary selector for acquisition
  experiments.
- The runner should fail the run if the requested serial set is missing or
  ambiguous.
- `selection.gpu_ids` should be treated as an explicit runtime placement input,
  not a descriptive note.
- When `selection.camera_serials` is explicit, `selection.gpu_ids` should map
  onto those cameras as runtime `gpu_id` overrides before camera open.
- When camera selection is `all`, `selection.gpu_ids` can still act as an
  allowed-GPU filter.
- Each run manifest should capture both numeric `gpu_id` and resolved GPU
  hardware metadata so `GPU 0` can be interpreted later as a concrete device
  such as `NVIDIA RTX A6000`.

Temporary compatibility note:

- The local runner now accepts structured selection through
  `--experiment-spec`.
- The remote compatibility path still accepts camera selection through
  `encoder_basic_setup` using tokens like `camera=all`, `camera=02010093`, or
  `camera=02010093+02010094`.

The first version should only support the current practical dimensions:

- GPU id
- codec
- preset
- tuning
- rate control mode
- quality value
- GOP length
- output geometry
- display / YOLO enabled
- duration / warmup

## Run Lifecycle

Each run should follow the same lifecycle:

1. Resolve run config from fixed fields + one matrix point.
2. Create experiment run directory / manifest entry.
3. Start a `RecordingSession`.
4. Wait until recording folder and snapshot exist.
5. Allow warmup interval.
6. Record for configured duration.
7. Request stop and wait for clean drain/finalize.
8. Verify required artifacts exist.
9. Run evaluation and plot generation.
10. Write run result into experiment summary.

The runner should never reuse a partially failed run directory silently.

## Artifact Contract

Each run should produce:

- recording folder with standard runtime artifacts
- `recording_snapshot.json`
- `Cam*_pipeline_perf.csv`
- optional `Cam*_yolo_perf.csv` when enabled
- experiment-local plot output
- experiment-local per-run summary row

Additionally, the experiment runner should produce:

- `experiment_spec.json`
- `runs.csv`
- `runs.json`
- `summary.json`

Suggested experiment-level row fields:

- `experiment_id`
- `run_id`
- `camera_serial`
- `gpu_id`
- `gpu_name`
- `gpu_pci_bus_id`
- `codec`
- `preset`
- `tuning`
- `rate_control_mode`
- `quality_value`
- `gop_length`
- `duration_s`
- `warmup_s`
- `display`
- `yolo`
- `recording_folder`
- `status`
- `pass_fail`
- `reason`
- `enc_fps_mean`
- `enc_fps_p95`
- `acq_starve_final`
- `pre_drops_final`
- `enc_fail_final`
- `dropped_frames_camera`

## Pass / Fail Evaluation

The experiment runner should embed the measurement policy from the benchmark
run sheet.

For the first version, evaluate a run as:

- `pass`
  - sustained `enc_fps` within configured tolerance of target
  - `dropped_frames_camera == 0` / `camera_frame_id_gaps == 0`
  - `acq_starve == 0`
  - `pre_drops == 0`
  - `enc_fail == 0`
  - no steady queue growth
- `marginal`
  - no hard failures, but clear waits / low-watermark pressure / reduced
    sustained encode FPS
- `fail`
  - any nonzero `dropped_frames_camera` / `camera_frame_id_gaps` when
    `require_zero_camera_drops=true`
  - any nonzero `pre_drops` or `enc_fail`
  - sustained encode deficit
  - obvious queue runaway

Keep camera frame integrity, SDK receive errors, and encode-path failure reasons
separate:

- `dropped_frames_camera` is the legacy row name for true camera frame-ID gaps
- `get_frame_errors` is SDK receive/buffer pressure such as
  `EVT_CameraGetFrame` returning `EVT_ERROR_NOMEM`
- `acq_starve`, `pre_drops`, `enc_fail` are pipeline health
- strict experiment policy can fail on both classes, but the row reason should
  preserve whether the failure came from source health or pipeline health

## Implementation Phases

### Phase 1: Shared Backend Extraction

Goal:

- make the modern worker pipeline reusable outside the GUI start/stop block

Current status:

- partially done
- `ModernRecordingPipeline` now exists for the per-camera recording worker pair
- full session ownership is still missing

Tasks:

- extract worker/session construction from `src/orange.cpp`
- create `RecordingSessionConfig`
- create `RecordingSession`
- move worker lifetime / start / stop / drain logic behind that interface

Definition of done:

- GUI streaming/recording path still works through the new abstraction

### Phase 2: Headless Backend Parity

Goal:

- make headless distributed capture use the same worker-based recording backend

Current status:

- substantially done
- headless now records through the modern worker pair
- stop/drain coordination is restored on the modern path
- the remaining gap is structured remote control parity, not local recording
  backend parity

Tasks:

- replace `GPUVideoEncoder` usage in headless recording mode
- adapt headless acquisition to use `acquire_frames(...)` instead of the legacy
  headless acquisition loop for recording runs
- preserve existing ENet control flow and distributed startup/stop semantics
- ensure snapshot / pipeline artifact generation works in headless sessions

Additional required cleanup:

- migrate the legacy remote stop semantics onto the modern acquisition path
- remove or bypass unused fanout bookkeeping in record-only benchmark mode
- replace the loose `encoder_basic_setup` string contract with structured run
  config

Definition of done:

- one-host headless and remote-server headless produce the same recording-path
  artifacts as GUI sessions

### Phase 3: Single-Host Experiment Runner

Goal:

- run benchmark matrices automatically on one machine using the shared backend

Tasks:

- add `ExperimentSpec` parser
- add matrix expansion
- add run lifecycle orchestration
- add artifact verification
- add pass/fail evaluation
- add experiment summary writing

Current status:

- implemented in the local headless CLI
- current limitations are intentionally narrow:
  - local mode only
  - `display=false`
  - `yolo=false`
  - `sync_mode=free_run`
  - remote orchestration remains deferred

Definition of done:

- one JSON spec can execute Block A or Block B automatically on one host

### Phase 4: Distributed Experiment Coordination

Status:

- deferred for now
- do not block Phases 1-3 on this work

Goal:

- support experiments spanning remote headless servers over the switch

Tasks:

- define experiment-level remote command contract
- add run IDs / experiment IDs to remote start commands
- make remote headless clients write the same artifact set
- collect or register remote run artifact locations in a shared summary

Notes:

- keep host-driven orchestration
- do not require peers to infer experiment sequencing themselves

Definition of done:

- one experiment spec can launch coordinated runs across host + remote servers

### Phase 5: Experiment UX / Tooling

Goal:

- make it easy to launch, inspect, and compare runs

Tasks:

- keep `--experiment-spec <path>` as the main entry point
- optionally add a small GUI launcher later
- auto-run `plot_pipeline_perf.py`
- produce experiment summary table and plots

Definition of done:

- one command launches a run sheet and leaves behind a complete experiment
  folder with machine-readable results

## Recommended File Layout

Suggested new files:

- `src/recording_session.h`
- `src/recording_session.cpp`
- `src/experiment_runner.h`
- `src/experiment_runner.cpp`
- `src/experiment_spec.h`
- `src/experiment_spec.cpp`
- `scripts/run_experiment.py` or in-process CLI glue
- `docs/experiment_runner_contract.md` later if needed

The first implementation should avoid pushing experiment logic into worker
classes themselves.

## Important Risks

### 1. GUI / Headless Behavior Drift

If the shared backend is not truly shared, the experiment runner may benchmark a
different path than production.

Mitigation:

- extract, do not duplicate, the worker recording path.

### 2. Distributed Artifact Fragmentation

Remote headless runs may write valid local artifacts but fail to produce a
coherent experiment-level summary.

Mitigation:

- require explicit `experiment_id` and `run_id`
- require each node to report final artifact path back to the host

### 3. Stop / Drain Correctness

Experiment automation will stress repeated start/stop sequences more than normal
manual use.

Mitigation:

- reuse the existing recording drain semantics
- treat incomplete artifact finalization as a hard run failure

### 4. Legacy Headless Compatibility

The current headless path may still be needed temporarily for existing users.

Mitigation:

- keep the legacy headless path behind a compatibility switch during transition
- make the experiment runner explicitly target the modern backend

## Recommendation

Implementation priority should now be:

1. finish control-plane unification with `RecordingSession`,
2. fix modern headless stop/drain coordination,
3. add single-host experiment runner,
4. only after that, reconsider distributed headless orchestration.

That order matters. If the runner is built first on top of the current legacy
headless path, it will automate the wrong backend and the benchmark results will
be harder to trust.
