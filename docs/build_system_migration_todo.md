# Build System Migration TODO

Transition goal: replace the ad-hoc `build.sh`/`quick_build/*.sh` flow with a
single mature, reproducible CMake-based workflow.

## Scope

- In-scope targets: `orange`, `orange_client`, `yolo_offline`, `evt_lens_probe`.
- In-scope build modes: release, debug, CUDA debug flags, NVTX, YOLO profiling.
- In-scope dependencies: CUDA, TensorRT, FFmpeg, OpenCV, ENet, EVT SDK, GLFW/GLEW.

## Why This Migration Is Needed

- Current build logic is duplicated across `build.sh`, `quick_build/*.sh`, and CMake.
- Paths and dependency assumptions are hardcoded in multiple places.
- Build mode behavior is fragmented (flags and output directories differ by script).
- CI/reproducible builds are difficult because local shell scripts encode environment.

## Current-State Gaps (Observed)

- `CMakeLists.txt` hardcodes debug defaults and architecture assumptions.
- `build.sh` and `quick_build` scripts use different compile/link invocations.
- Build outputs are manually symlinked into `targets/*` using custom shell logic.
- Feature flags (`NVTX`, `YOLO_PROFILE`, CUDA debug) are defined via shell flags rather
  than a canonical CMake option model.

## Migration Plan

### Phase 0: Baseline and Freeze

- [ ] Define a feature matrix for existing modes:
  - [ ] `--debug`
  - [ ] `--cuda-debug`
  - [ ] `--nvtx`
  - [ ] `--yolo-profile`
  - [ ] combinations used in practice
- [ ] Capture baseline compile/link flags and artifact locations per mode.
- [ ] Capture expected runtime behavior for each target in each mode.
- [ ] Freeze new changes to `build.sh` unless critical bugfix.

### Phase 1: Normalize CMake Target Definitions

- [ ] Remove global compile/link side effects where possible.
- [ ] Move to target-scoped settings (`target_compile_definitions`, `target_include_directories`, `target_link_libraries`).
- [ ] Define explicit CMake options:
  - [ ] `ORANGE_ENABLE_NVTX`
  - [ ] `ORANGE_ENABLE_YOLO_PROFILE`
  - [ ] `ORANGE_ENABLE_CUDA_DEBUG`
  - [ ] `ORANGE_USE_FAST_MATH`
- [ ] Ensure all four executables build from CMake as first-class targets.
- [ ] Ensure feature options are applied consistently to all relevant targets.

### Phase 2: Dependency Configuration Hardening

- [ ] Replace hardcoded `$HOME`-based paths with cache variables:
  - [ ] `ORANGE_FFMPEG_ROOT`
  - [ ] `ORANGE_TENSORRT_ROOT`
  - [ ] `ORANGE_EVT_ROOT`
- [ ] Add configure-time checks with clear failure messages for missing deps.
- [ ] Define expected include/lib layouts for each external dependency.
- [ ] Add docs for dependency path overrides via CMake cache or environment.

### Phase 3: CMake Presets as Canonical Entry Point

- [ ] Add `CMakePresets.json` with configure presets:
  - [ ] `dev-debug`
  - [ ] `release`
  - [ ] `release-nvtx`
  - [ ] `debug-cuda`
  - [ ] `release-yolo-profile`
- [ ] Add corresponding build presets.
- [ ] Set standardized output roots (for example `out/build/<preset>`).
- [ ] Add a single command table in docs for all common workflows.

### Phase 4: Compatibility Layer for Existing Workflows

- [ ] Convert `build.sh` into a thin compatibility shim:
  - [ ] parse existing flags
  - [ ] map flags to CMake presets/options
  - [ ] print deprecation warning
- [ ] Convert `quick_build/*.sh` to wrappers around `cmake --preset` and `cmake --build --preset`.
- [ ] Keep temporary compatibility symlinks for `targets/orange*` until runtime scripts are updated.

### Phase 5: Runtime Script and Docs Cleanup

- [ ] Update `run.sh` to derive binary path from preset output (or selected default preset).
- [ ] Remove outdated README instructions that reference manual source edits in `build.sh`.
- [ ] Document one standard flow:
  - [ ] configure
  - [ ] build
  - [ ] run
  - [ ] profile
- [ ] Add troubleshooting for missing dependency headers/libs in configure step.

### Phase 6: CI and Quality Gates

- [ ] Add CI checks for CMake configure and compile (at least `evt_lens_probe` plus any target possible in CI environment).
- [ ] Add a "no hardcoded local paths" check for CMake files.
- [ ] Add a "preset smoke test" script for dev machines.
- [ ] Require CMake path for PR validation once stable.

### Phase 7: Decommission Legacy Build Scripts

- [ ] Mark `build.sh` as deprecated in docs.
- [ ] Announce cutoff date for full script removal.
- [ ] Remove legacy compile/link logic from `build.sh`.
- [ ] Remove dead build scripts after two stable cycles.

## Definition of Done

- [ ] A fresh clone can be built through CMake presets without editing scripts.
- [ ] All supported targets compile through CMake with documented dependency overrides.
- [ ] Feature modes (debug/NVTX/YOLO profile/CUDA debug) are available as preset or option.
- [ ] Runtime entry points no longer depend on custom handcrafted `targets/*` symlink logic.
- [ ] `build.sh` is either a shim or removed.

## Open Decisions

- [ ] Primary GPU architecture policy (single arch vs multi-arch list).
- [ ] Whether FFmpeg/TensorRT are expected from system packages, custom installs, or both.
- [ ] Whether to support both OpenCV package discovery and fixed local install roots.
- [ ] Whether to keep `sudo` in run commands or require capability-based setup.

## Suggested Execution Order

- [ ] Complete Phase 0 and Phase 1 in one PR series.
- [ ] Land Phase 2 and Phase 3 together (dependency model + presets).
- [ ] Land Phase 4 and Phase 5 together (compat + docs + runtime scripts).
- [ ] Land Phase 6 and Phase 7 after at least one full release cycle on presets.
