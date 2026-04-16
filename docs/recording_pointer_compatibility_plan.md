# Recording Pointer Compatibility Plan

This note captures the current `latest_recording.json` behavior, the Orange and
Citrus contracts that depend on it, and a safe migration path if we want a more
canonical metadata root than `/run/orange`.

## Current Behavior

Orange currently writes two pointer files at recording start:

- local pointer:
  - `<base_folder>/.orange/latest_recording.json`
- shared/global pointer:
  - `/run/orange/latest_recording.json`

Current implementation:

- local pointer write:
  - [project.cpp](/home/jeremy/orange-gop-split-a16/src/project.cpp:1842)
- shared pointer write:
  - [project.cpp](/home/jeremy/orange-gop-split-a16/src/project.cpp:1870)

The local pointer is rooted under the same recording output tree that already
defaults to `~/orange_data/...`.

The shared pointer is rooted under `/run`, which is a system tmpfs location and
is reset on reboot.

## Why This Matters

For Orange alone, the local pointer is usually enough because it lives beside
the recording output tree.

However, Citrus currently treats the shared `/run/orange/latest_recording.json`
path as the preferred discovery mechanism for where Orange is recording.

That means this is not just an Orange-internal cleanup. It is a cross-project
contract question.

## Current Contract Surface

Orange docs:

- [docs/recording_metadata.md](/home/jeremy/orange-gop-split-a16/docs/recording_metadata.md:14)
- [docs/output_artifacts_contract.md](/home/jeremy/orange-gop-split-a16/docs/output_artifacts_contract.md:40)

Agent contracts:

- Orange consumer artifact contract:
  - [orange-consumers/output_artifacts_contract.md](/home/jeremy/agent-contracts/orange-consumers/output_artifacts_contract.md:32)
- Citrus session output contract:
  - [citrus-crimson/citrus_contract.md](/home/jeremy/agent-contracts/citrus-crimson/citrus_contract.md:30)

Important current wording:

- Orange contracts describe `/run/orange/latest_recording.json` as required but
  best-effort.
- Citrus contract describes `/run/orange/latest_recording.json` as the
  preferred place to discover `recording_folder/citrus`.

This is already a mild contract mismatch:

- Orange perspective:
  `/run/orange` is useful, but not guaranteed.
- Citrus perspective:
  `/run/orange` is the first-class discovery path.

## Recent Observation

During a successful root-run recording validation, the local pointer under the
recording output root was written successfully, but `/run/orange` failed with:

- `Error creating run metadata folder: /run/orange (Permission denied)`

That means:

- recording artifacts were valid
- `recording_snapshot.json` was valid
- local `.orange/latest_recording.json` was valid
- only the shared `/run/orange/latest_recording.json` write failed

So the current shared-pointer dependency is a real operational risk even when
recording itself succeeds.

## Proposed Direction

Do not immediately replace `/run/orange`.

Instead, move to a compatibility model with three tiers:

1. Required local pointer:
   - `<base_folder>/.orange/latest_recording.json`
2. Canonical user-data pointer:
   - default: `~/orange_data/.orange/latest_recording.json`
3. Optional shared compatibility pointer:
   - `/run/orange/latest_recording.json`

This keeps Orange aligned with the user-data tree it already uses for recording
artifacts, while preserving Citrus compatibility during migration.

## Recommended Semantics

### Local pointer

Keep as required:

- `<base_folder>/.orange/latest_recording.json`

Reason:

- always colocated with the recording artifacts
- does not depend on system runtime directories
- already works in validated runs

### Canonical user-data pointer

Add a stable user-level pointer root, defaulting to:

- `~/orange_data/.orange/latest_recording.json`

Reason:

- persistent across reboots
- matches Orange's default artifact root
- usable by tools that want a stable "latest recording" path without relying on
  `/run`

### Shared `/run` pointer

Keep temporarily as:

- compatibility path
- best-effort only

Reason:

- Citrus already prefers it
- removing it immediately would be a contract break

## Suggested Migration Phases

### Phase 0: Document reality

Update Orange docs and agent contracts so they agree on current behavior:

- `/run/orange/latest_recording.json` is compatibility-oriented and best-effort
- local pointer remains required

This should happen before changing runtime behavior.

### Phase 1: Dual-write

Orange writes all of:

- `<base_folder>/.orange/latest_recording.json`
- `~/orange_data/.orange/latest_recording.json`
- `/run/orange/latest_recording.json` best-effort

No consumer breakage.

### Phase 2: Citrus fallback expansion

Teach Citrus to resolve latest-recording discovery in this order:

1. `/run/orange/latest_recording.json`
2. `~/orange_data/.orange/latest_recording.json`
3. existing Citrus fallback:
   - `<project_root>/targets/session_logs`

At this stage Citrus is no longer hard-dependent on `/run/orange`.

### Phase 3: Contract reclassification

Once Citrus has shipped the new fallback behavior, update contracts so:

- canonical shared/user-level pointer becomes `~/orange_data/.orange/latest_recording.json`
- `/run/orange/latest_recording.json` is explicitly legacy compatibility output

### Phase 4: Optional deprecation

Only after both Orange and Citrus have lived with the new fallback model should
we decide whether `/run/orange/latest_recording.json` should:

- remain indefinitely for convenience, or
- be disabled by default, or
- become opt-in for service/system deployments

## Suggested Runtime Controls

If we implement this, the controls should live at the app/runtime layer, not in
per-camera config.

Reason:

- pointer locations are process/session metadata behavior
- they are not camera-specific recording behavior

Plausible controls:

- `ORANGE_METADATA_ROOT`
  - user-data pointer root
- `ORANGE_ENABLE_RUN_POINTER`
  - enable/disable `/run/orange` compatibility write
- `ORANGE_RUN_METADATA_ROOT`
  - advanced override for the shared pointer root

But the safest first step is likely:

- no new knobs yet
- just dual-write to a new user-data pointer in addition to current behavior

## What I Think

I do not think we should switch Citrus directly from `/run/orange` to
`~/orange_data/.orange` in one step.

I do think we should:

1. add the user-data pointer path
2. keep `/run/orange` for compatibility
3. update Citrus to understand both
4. then decide whether `/run/orange` should remain primary, secondary, or legacy

That gives us a path that is operationally cleaner without breaking the current
cross-project contract.

## Immediate Next Steps

1. Update Orange docs and agent contracts to explicitly call out the current
   Orange/Citrus contract mismatch around `/run/orange`.
2. Decide whether Orange should add the user-data pointer now or only after the
   contract update.
3. If we proceed, implement Phase 1 dual-write in Orange.
4. Then update Citrus discovery logic and contracts.
