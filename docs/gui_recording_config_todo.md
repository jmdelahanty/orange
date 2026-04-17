# GUI Recording Config TODO

Purpose: define how Orange should expose the new recording and app-storage
configuration model in the GUI without turning the main UI into a raw config
editor.

This note is intentionally about UI surface and rollout order, not the full
runtime implementation of split-GOP recording.

## Current State

The branch already has:

- schema 3 per-camera `recording` config
- app-level storage config
- resolved runtime recording config
- validated split-GOP runs on A16 `PIX` pairs
- live `/run/orange` pointer support plus canonical `~/orange_data/.orange`
  fallback

The GUI currently already exposes the core single-session recording controls in
[orange.cpp](/home/jeremy/orange-gop-split-a16/src/orange.cpp:2550), including:

- save folder
- codec
- preset
- tuning
- rate control
- quality value
- GOP length
- output resize / downsample

That existing panel is a good operator UI baseline and should stay simple.

## UI Design Goals

1. Keep the primary recording workflow easy to use.
2. Expose the validated split-GOP settings without forcing JSON edits.
3. Separate operator-facing controls from engineering/debug controls.
4. Make app-level storage settings visible in the GUI, but not per camera.
5. Surface runtime diagnostics and resolved state as read-only information where
   possible.

## What Should Be Editable In The Main Recording UI

These are the controls that are directly relevant to normal recording use:

- recording root / save folder
- codec
- rate control mode
- quality value
- GOP length
- output resize mode and dimensions/factor
- recording mode:
  - `single_session`
  - `split_gop`

If `split_gop` is selected, the main UI should also expose:

- source GPU id (readable as the camera's source GPU)
- helper encoder GPU ids
- transfer mode
- source encoder policy

These are the settings an operator is most likely to choose deliberately.

## What Should Move Into An Advanced Per-Camera Panel

These are important but should not clutter the main recording row:

- `constraints.require_peer_access`
- `constraints.preferred_topology_class`
- `resources.acquire_work_entries`
- `resources.encoder_entry_pool_size`

Recommended UI shape:

- collapsible section or modal
- one panel per selected/open camera
- explicit reset-to-default action

## What Should Be In App Settings, Not Camera Settings

These belong to a machine/session-wide settings area:

- `storage.default_recording_root`
- `storage.latest_recording.write_local_pointer`
- `storage.latest_recording.canonical_pointer_root`
- `storage.latest_recording.write_run_pointer`
- `storage.latest_recording.run_pointer_path`

These settings should not be repeated inside each camera block.

Recommended UI shape:

- dedicated "App Storage" or "Recording Paths" settings panel
- includes explanation of:
  - local pointer
  - canonical pointer
  - `/run/orange` live IPC pointer

## What Should Be Read-Only Diagnostics, Not Editable Controls

These should be visible, but as status/diagnostics:

- source GPU and helper GPU runtime info
- topology class (`PIX`, `PHB`, etc.)
- peer-access capability / enablement
- helper routing counters
- backlog/overflow state
- latency summaries
- active pointer targets
- resolved runtime recording config summary

Recommended UI shape:

- "Recording Diagnostics" panel
- available while recording or after a run
- copy-to-clipboard support for paths and JSON snippets

## What Should Stay Out Of The GUI For Now

These are still engineering-oriented knobs and should remain config/debug-only
until there is a strong reason to expose them:

- writer queue packet/byte caps
- split-GOP inflight/backlog caps
- overflow/fail-on-overflow flags
- env-compatibility overrides
- experimental fallback/debug toggles

## Proposed Rollout Order

### Phase 1: Basic Split-GOP Visibility

Add to the existing recording controls:

- current recording mode
- current source GPU
- helper GPU ids
- transfer mode
- source encoder policy

Goal:

- make split-GOP usable from the GUI without exposing every advanced field

### Phase 2: Per-Camera Advanced Recording Panel

Add a collapsible advanced panel for:

- peer-access requirement
- preferred topology class
- resource pool sizes

Goal:

- expose the validated tuning knobs without cluttering the main panel

### Phase 3: App Storage Settings Panel

Add GUI support for:

- default recording root
- canonical pointer root
- `/run` pointer enable/path

Goal:

- stop requiring manual JSON edits for storage behavior

### Phase 4: Read-Only Diagnostics

Add a diagnostics surface showing:

- topology/runtime evidence
- routing counters
- latency summaries
- pointer destinations

Goal:

- let operators and developers verify what the system actually resolved at
  runtime

## Suggested First Implementation Slice

The first GUI patch should be small:

1. Add a `recording mode` control to the main recording panel.
2. When `split_gop` is selected, show:
   - helper GPU ids
   - transfer mode
   - source encoder policy
3. Add a short read-only summary line for:
   - resolved output shape
   - source GPU
   - helper GPU set

This gives immediate value without a large UI refactor.

## Non-Goals

This TODO does not propose:

- turning the GUI into a full JSON editor
- exposing every internal queue/backlog limit immediately
- replacing experiment specs with GUI flows
- removing the current simple recording controls

## Follow-Up

Once this UI plan is accepted, the next implementation step should be a small
GUI patch that covers only Phase 1.
