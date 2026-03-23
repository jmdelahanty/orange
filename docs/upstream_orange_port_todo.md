# Upstream Orange Port TODO

Date: 2026-03-16
Scope: pull selected capabilities from upstream `orange` into this fork without
regressing the fork's current acquisition, recording, encoding, IPC, and YOLO
pipeline.

## Goal

Strengthen the fork in the areas where upstream still appears better:

- control-plane networking and peer/runtime abstractions
- path/config handling and deployability
- build target structure and target gating
- optional distributed orchestration
- optional calibration / 3D tooling

## Non-Goals

- Do not replace the fork's current workerized recording / encoding pipeline
  with upstream's older frame saver / detector flow.
- Do not import legacy shell-build habits as the long-term build path.
- Do not merge unrelated upstream product surface area just because it exists.
- Do not collapse the fork's current schemas and control messages into one
  undocumented hybrid protocol.

## Current Assumptions

- The fork remains the source of truth for:
  - `src/yolo_worker.cpp`
  - `src/encoder_preprocess_worker.cpp`
  - `src/encoder_hw_worker.cpp`
  - `src/crop_and_encode_worker.cpp`
  - `src/frame_ipc_manager.h`
  - `src/recording_sync.h`
  - the runtime artifact contracts in `docs/`
- Upstream is mined mainly for productization and control-plane improvements.

## Port Buckets

### Copy directly

- `src/enet_types.h`
- `src/enet_runtime_threaded.h`
- `src/enet_runtime_threaded.cpp`
- `src/enet_runtime_unified.h`
- `src/enet_fb_helpers.h`
- `schema/ctrl.fbs`
- `src/ctrl_generated.h`

### Adapt heavily

- `src/utils.h`
- `src/utils.cpp`
- `src/enet_utils.h`
- `src/enet_utils.cpp`
- `src/cam_server.cpp`
- `src/host_client_imgui.h`
- `src/host_client_imgui.cpp`
- `src/realtime_tool.h`
- `src/realtime_tool.cpp`
- `src/detect3d.h`
- `src/detect3d.cpp`
- `src/aruco_nano.h`
- `CMakeLists.txt`

### Skip

- `src/FrameSaver.h`
- `src/FrameSaver.cpp`
- `src/FrameDetector.h`
- `src/FrameDetector.cpp`
- `src/threadworker.cpp`
- `src/yolo_offline.cpp`
- `install.sh`
- `uninstall.sh`
- `server_build.sh`
- `start_server.sh`

## Phase 0: Boundary Decisions

- [ ] Confirm whether distributed multi-PC orchestration is still a real
  product requirement for the fork.
- [ ] Confirm whether 3D / calibration / triangulation remains in scope.
- [ ] Decide whether upstream control protocol should be:
  - [ ] adopted directly for control-plane traffic
  - [ ] translated at the boundary into the fork's existing message model
  - [ ] mined only for design patterns, with no schema adoption
- [ ] Decide where imported upstream networking code should live:
  - [ ] keep under current `src/`
  - [ ] isolate under a new `src/net_runtime/` or similar folder
- [ ] Freeze any new ad hoc edits to the fork's existing raw ENet wrapper while
  the runtime migration is being designed.

## Phase 1: Network Runtime Import

Goal: replace the fork's lower-level ENet host wrapper with upstream's cleaner
threaded runtime primitives, while keeping current fork behavior working.

- [ ] Import upstream runtime files:
  - [ ] `src/enet_types.h`
  - [ ] `src/enet_runtime_threaded.h`
  - [ ] `src/enet_runtime_threaded.cpp`
  - [ ] `src/enet_runtime_unified.h`
  - [ ] `src/enet_fb_helpers.h`
- [ ] Keep imports isolated from the current `src/network_base.*` path at first.
- [ ] Add a minimal compile-only smoke path that proves the new runtime builds
  in this fork.
- [ ] Introduce an `AppContext`-style owner for ENet init/deinit and peer
  registry.
- [ ] Decide how peer naming works in this fork:
  - [ ] carry upstream's name registry pattern
  - [ ] keep current direct peer tracking for YOLO / Indigo consumers
  - [ ] support both temporarily during migration
- [ ] Define a migration shim from current fork network users onto the new
  runtime.

## Phase 2: Control-Plane Integration

Goal: reintroduce stronger orchestration without destabilizing the fork's
recording pipeline.

- [ ] Port `schema/ctrl.fbs` and generated headers into the fork.
- [ ] Decide whether the fork should:
  - [ ] speak upstream control messages natively
  - [ ] translate upstream control messages into existing fork actions
- [ ] Extract the useful ideas from upstream `cam_server.cpp`:
  - [ ] phase-based command handling
  - [ ] timeout-based readiness checks
  - [ ] cleanup ordering after stop
  - [ ] explicit bringup / state replies
- [ ] Re-implement orchestration against the fork's current worker pipeline
  rather than upstream's older acquire/record flow.
- [ ] Keep YOLO IPC / external consumer support intact while integrating the
  new control plane.

## Phase 3: Path and Config Handling

Goal: replace hardcoded local assumptions with explicit, user-configurable path
 handling.

- [ ] Port upstream home-directory resolution logic.
- [ ] Port support for `~/.config/orange/config.json`.
- [ ] Define config keys the fork should honor:
  - [ ] `recording_folder`
  - [ ] `codec`
  - [ ] any additional fork-specific settings needed now
- [ ] Fold this into the fork's current project/path setup rather than adding a
  second path bootstrap path.
- [ ] Define path precedence explicitly:
  - [ ] config file
  - [ ] runtime UI / CLI overrides
  - [ ] repository defaults
- [ ] Update docs to match the actual path precedence.

## Phase 4: Build System Port

Goal: bring back upstream's target structure while preserving the fork's newer
 executables and tooling.

- [ ] Replace source globs with explicit source lists where practical.
- [ ] Remove hardcoded `Debug` default from `CMakeLists.txt`.
- [ ] Add explicit target options for:
  - [ ] debug
  - [ ] CUDA debug
  - [ ] NVTX
  - [ ] YOLO profiling
- [ ] Preserve fork-specific targets:
  - [ ] `orange`
  - [ ] `orange_client`
  - [ ] `yolo_offline`
  - [ ] `evt_lens_probe`
  - [ ] `evt_aperture_characterize`
- [ ] Decide whether `cam_server` returns as:
  - [ ] a first-class target
  - [ ] an optional feature target
  - [ ] a later follow-up only
- [ ] Ensure imported runtime/control files have clear target ownership.

## Phase 5: Distributed UI / Host-Client Flow

Goal: recover the parts of upstream's distributed product experience that are
still worth supporting.

- [ ] Review upstream `host_client_imgui` phase hooks and readiness model.
- [ ] Decide whether the fork wants:
  - [ ] a UI-driven distributed workflow
  - [ ] a headless distributed workflow only
  - [ ] both
- [ ] If UI-driven flow stays in scope:
  - [ ] port the host/client phase model
  - [ ] rewrite data wiring against the fork's current camera/resource structs
  - [ ] remove reliance on outdated streaming and detection assumptions
- [ ] If headless flow stays in scope:
  - [ ] mine upstream `cam_server` and `orange_headless_client` for lifecycle
    logic only
  - [ ] avoid reviving the older recording thread architecture

## Phase 6: Optional 3D / Calibration Recovery

Goal: bring back calibration/triangulation only if it serves a current use
case.

- [ ] Confirm whether calibration parity is needed with current fork features.
- [ ] If yes, port as a single tranche:
  - [ ] `src/realtime_tool.h`
  - [ ] `src/realtime_tool.cpp`
  - [ ] `src/detect3d.h`
  - [ ] `src/detect3d.cpp`
  - [ ] `src/aruco_nano.h`
- [ ] Keep the module isolated from the main acquisition path until validated.
- [ ] Define integration points with current fork detection output:
  - [ ] consume YOLO boxes directly
  - [ ] consume a calibration-specific 2D marker path
  - [ ] support both
- [ ] Add a separate validation checklist before exposing it in the main UI.

## Phase 7: Cleanup and Convergence

- [ ] Remove dead compatibility shims once new network/runtime path is stable.
- [ ] Remove duplicate ENet abstractions after migration.
- [ ] Remove duplicate path/bootstrap logic after config migration lands.
- [ ] Document the final control-plane architecture.
- [ ] Document the final target ownership in CMake.
- [ ] Document which upstream modules were intentionally not ported.

## Open Questions

- [ ] Should the fork eventually converge back toward upstream's control schema,
  or intentionally keep a fork-specific control layer?
- [ ] Does Indigo integration still depend on upstream peer naming assumptions?
- [ ] Is `cam_server` still desired as a product artifact, or only as a design
  reference?
- [ ] Is calibration / 3D an active roadmap item or historical functionality?
- [ ] Should the new network runtime replace `src/network_base.*` completely, or
  sit beside it until one full release cycle passes?

## Suggested Execution Order

- [ ] Land Phase 0 first so the migration has real boundaries.
- [ ] Land Phase 1 and Phase 2 together only after the runtime boundary is
  agreed.
- [ ] Land Phase 3 before large host/client rework so path/config assumptions do
  not keep changing.
- [ ] Land Phase 4 in parallel with Phase 1 or immediately after it.
- [ ] Treat Phase 5 and Phase 6 as optional feature recoveries, not baseline
  blockers.

## Definition of Done

- [ ] The fork has a clearer, modernized control-plane/network runtime.
- [ ] Path/config behavior is no longer hardcoded around one local machine
  assumption.
- [ ] CMake target structure reflects actual supported binaries and feature
  flags.
- [ ] Any distributed workflow reintroduced works with the fork's current
  recording/YOLO pipeline.
- [ ] Optional calibration/3D code is either properly reintegrated or
  explicitly left out.
- [ ] The fork has one documented answer for why each major upstream-only module
  was copied, adapted, or skipped.
