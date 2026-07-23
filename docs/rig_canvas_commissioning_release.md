# Rig-Owned Canvas Commissioning Release

## Purpose

A completed dry calibration is not merely an Orange session. It becomes a
stable, rig-owned Citrus authority for one canvas. The release freezes the
rig/canvas configuration and binds every camera/arena to the already accepted
projected-surface homography and physical-scale artifacts that were reviewed
during commissioning.

This layer does not fit another transform. It aggregates and validates the
existing accepted products.

## Three lifecycle layers

1. `rig_geometry_revision` identifies the bolted physical rig geometry. A
   physical camera, lens, projector, mount, or shelf-geometry change requires
   incrementing this value and recommissioning affected canvases.
2. A per-canvas commissioning release owns stable arena regions, homographies,
   projected-surface scales, QC, and source evidence.
3. Daily dish registration may adjust dish/experimental-area translation. It
   must not edit the commissioned Citrus canvas JSON or replace commissioning
   homography/scale artifacts.

Commissioning and daily registration have different optionality rules. A rig
without an active commissioning pointer retains the documented migration
fallback and is not blocked merely because commissioning was skipped. For a
canvas with an accepted commissioning release, that release is the stable base
authority and a stale/corrupt active pointer is fail-closed. Daily dish/rim
registration remains opt-in: users may record in explicit `base_only` mode
without performing it, and its absence is not an experiment-start error.

Camera-visible rectangular arena sizes may differ. That does not change the
physical experimental-area diameter: real-world experimental geometry is
converted using each camera/arena's accepted projected-surface scale.

## Storage and authority

For rig `omnifin0` and canvas `shadow`, Citrus writes:

```text
targets/rigs/omnifin0/shadow/calibration_artifacts/
  commissioning/
    commissioning_<transaction-id>/
      rig_config_snapshot.json
      canvas_config_snapshot.json
      commissioning.json
  commissioning_active.json
```

`commissioning.json` is an immutable
`citrus.calibration.rig_canvas_commissioning_release` version 1 artifact. The
small `commissioning_active.json` pointer is the only mutable part of this
layer and is replaced atomically after every gate passes.

The release records:

- rig ID, canvas name, and `rig_geometry_revision`;
- source and snapshot paths/checksums for rig and canvas JSON;
- every loaded arena/camera identity;
- active homography and scale pointer checksums;
- immutable candidate and acceptance-receipt checksums;
- the scale-to-active-homography binding;
- Orange source sessions plus `session.json` and `session_index.json` hashes;
- a frozen requirements result and the explicit operator acceptance.

## Finalization gates

Citrus rejects finalization unless:

- the request is explicitly armed;
- no experiment or calibration transaction is active;
- the current canvas checksum matches the request;
- `rig_geometry_revision` is a positive explicit value;
- all loaded arenas belong to one rig/canvas;
- every active camera has a compatible accepted homography;
- every active camera has a compatible accepted projected-surface scale;
- every scale is bound to the currently active homography;
- candidate files and committed acceptance receipts are intact;
- all source Orange sessions have readable session metadata and indices;
- the homographies form one accepted candidate set and the scales form one
  accepted candidate set.

Orange exposes these gates in **Rig-Owned Canvas Commissioning Authority**.
Refreshing is read-only. Finalization requires a separate checkbox and button.

## Runtime behavior

Citrus continues to load the individual homography and scale pointers; the
commissioning release is their parent authority. At runtime it verifies the
release manifest hash, frozen/current configuration hashes, geometry revision,
member pointers, candidates, acceptance receipts, snapshots, and source-session
indices.

For migration, no active commissioning pointer is a warning and does not by
itself block experiment start. Once a commissioning pointer exists, a stale or
invalid release is fail-closed and blocks start. The existing individual
homography and scale compatibility gates remain in force.

This fail-closed rule does not make adjunct calibration workflows mandatory.
Daily rim fitting and daily translation affect readiness only when the operator
explicitly selects a daily-registration mode/artifact. Otherwise runtime uses
the compatible commissioned base and records `base_only` plus
`daily_registration_status = "not_performed"` (or
`"available_not_selected"` when an artifact exists but was not chosen).

The implementation stores daily products beside, but never inside, the stable
release:

```text
calibration_artifacts/
  daily_registration/<transaction-id>/
    candidate.json
    registration.json        # only after explicit acceptance
    rejection.json           # when rejected
  daily_registration_runtime_selection.json
```

`registration.json` binds the exact commissioning release ID/checksum,
per-arena/camera evidence checksums, the active projected-surface homography,
translation-only results, and `valid_until_utc`. Acceptance does not select the
artifact. The atomic runtime-selection pointer independently chooses either
`base_only` or `selected_daily_registration`. The latter is rejected at start
if its checksum, validity interval, target identity, or commissioning binding
is no longer valid. Returning to `base_only` is always an explicit operator
action and does not delete the accepted artifact.

Loading an old release for review is not activation. A future rollback UI must
revalidate the full release and use an explicit activation arm before changing
`commissioning_active.json`.

## Session matrix

The Orange session capture matrix remains a live evidence/progress view. It is
not itself the authority. Its dry scale cell now accepts the
`projected_surface_scale_calibration` physical-target capture (5 mm pitch with
25 mm and 77 mm independent validation), while retaining display compatibility
with older ruler-only sessions. The persisted commissioning release is the
stable record of which accepted products and sessions became authoritative.
