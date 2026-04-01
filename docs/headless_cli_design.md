# Headless CLI Design

Date: 2026-04-01
Scope: command-line entry points for `orange_client`, with emphasis on local
single-host experiments and compatibility with the existing remote headless
workflow.

See also:

- `docs/headless_experiment_backend.md`
- `docs/experiment_runner_plan.md`
- `docs/nvenc_benchmark_runsheet.md`
- `src/orange_headless_client.cpp`

## Why This Is Needed

The current `orange_client` binary is launched from the command line, but it is
not yet a real local experiment CLI.

Today it behaves primarily as a remote-controlled network agent:

- it starts ENet,
- waits for host control messages,
- opens cameras on `OPENCAMERA`,
- starts recording threads on `STARTTHREAD`.

That is fine for distributed capture, but it is the wrong UX for a user running
benchmarks on a machine with cameras attached directly to that same machine.

For local experiments, requiring a separate host/controller adds unnecessary
control-plane complexity and makes one-off testing slower.

## Design Goals

- support direct local headless recording on the same machine as the camera
- preserve the current remote-controlled mode for distributed setups
- support quick one-off runs from flags
- support repeatable matrix runs from `--experiment-spec`
- keep one recording backend under both modes
- avoid creating separate "local recording" and "remote recording"
  implementations

## Top-Level Modes

`orange_client` should support two explicit modes.

### `--mode local`

Use cameras physically attached to the current machine and run the recording
session directly from CLI inputs.

This should become the primary path for:

- NVENC throughput benchmarking on a local rig
- one-off codec / preset / AQ experiments
- local artifact generation without host orchestration

### `--mode remote`

Keep the existing ENet/network-controlled behavior.

This remains useful for:

- distributed camera servers over the switch
- host-managed synchronized runs
- future multi-machine experiment orchestration

## Invocation Model

The CLI should support two layers of configuration:

- direct flags for ad hoc runs
- `--experiment-spec <path>` for structured or repeated runs

The important rule is:

- direct flags are for one run
- experiment spec is for one or more planned runs
- both should compile down to the same internal `RecordingSessionConfig`

## Proposed CLI Shape

Minimal first version:

```bash
orange_client --mode local --camera 02010093 --record-folder /path/to/run
orange_client --mode local --camera all --codec hevc --preset p1 --tuning ll
orange_client --mode local --experiment-spec /path/to/spec.json
orange_client --mode remote
```

Recommended direct flags:

- `--mode local|remote`
- `--camera <serial|all>` (repeatable)
- `--config-folder <path>`
- `--record-folder <path>`
- `--codec <h264|hevc>`
- `--preset <p1..p7>`
- `--tuning <ull|ll|hq>`
- `--rate-control <vbr|cbr|cqp>`
- `--quality <int>`
- `--gop <int>`
- `--sync-mode <free_run|ptp_gate>`
- `--display <on|off>`
- `--yolo <on|off>`
- `--experiment-spec <path>`

Nice-to-have later:

- `--output-width <int>`
- `--output-height <int>`
- `--downsample-factor <int>`
- `--duration <seconds>`
- `--warmup <seconds>`
- `--dry-run`

## Selection Rules

Camera selection should be explicit.

- `--camera all` means all cameras visible to the chosen mode
- `--camera <serial>` selects one camera
- repeated `--camera` flags select a subset
- selection should be by serial, not by transient scan index

Validation rules:

- fail if a requested serial is missing
- fail if the same serial is requested twice
- print available serials on selection failure

For `--mode local`, selection applies to cameras discovered on the same host.

For `--mode remote`, selection should eventually be part of the structured
remote run contract. Until that exists, the current temporary `camera=...`
parsing inside `encoder_basic_setup` remains a compatibility layer.

## Precedence Rules

If both direct flags and `--experiment-spec` are present:

- `--mode` still matters because it determines how the run is launched
- `--experiment-spec` provides the run matrix / fixed settings
- direct per-run recording flags should either:
  - be rejected when `--experiment-spec` is present, or
  - be treated only as overrides if we explicitly document that

Recommended first implementation:

- reject mixed per-run flags with `--experiment-spec`
- allow only:
  - `--mode`
  - `--experiment-spec`
  - maybe `--config-folder`

That keeps the first CLI predictable.

## Internal Architecture

The CLI should be thin.

It should not directly own acquisition/worker lifecycle logic.

Instead:

1. parse args into `CliInvocation`
2. resolve that into either:
   - one `RecordingSessionConfig`, or
   - one `ExperimentSpec`
3. hand off to:
   - `RecordingSession` for a single local run
   - `ExperimentRunner` for spec-driven local runs
   - existing remote event loop for `--mode remote`

That means the current remote client loop stays, but local mode should bypass it
entirely and start the session directly.

## Recommended Implementation Order

### Phase 1

Add explicit local mode for a single run.

- `orange_client --mode local`
- direct flags for one recording session
- explicit `--camera`
- no experiment spec yet

This is the fastest useful path for the current benchmark work.

### Phase 2

Add local `--experiment-spec`.

- single-host matrix automation
- artifact collection and evaluation
- no network orchestration required

### Phase 3

Refit remote mode onto the same structured config model.

- keep ENet control plane
- replace loose `encoder_basic_setup` string over time
- carry camera selection and run metadata explicitly

## Non-Goals For Now

- replacing the distributed topology
- multi-host experiment scheduling
- a GUI benchmark launcher
- solving remote fleet management

Those are useful later, but they should not block a clean local headless CLI.

## Immediate Recommendation

For the current work, the next implementation should be:

1. add `--mode local`
2. add direct single-run flags including repeatable `--camera`
3. keep `--mode remote` behavior intact
4. add `--experiment-spec` only after local single-run mode works

That sequence matches the current need: benchmark a local camera rig without
requiring host/network orchestration, while preserving the remote path for later
distributed experiments.
