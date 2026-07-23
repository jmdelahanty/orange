# Calibration Package v2 Candidate Contract

Status: first non-active implementation slice.

This slice can make and validate immutable calibration **candidates**. It does
not select a candidate, write a registry pointer, change Citrus runtime state,
or participate in experiment arm. Existing Orange recording remains unchanged
and calibration packages remain optional.

## Products and authority

| Product | Schema ID | Writer/authority |
| --- | --- | --- |
| commissioned rig setup | `orange.calibration.commissioned_rig_setup`, version 2 | Citrus seals; the current tool performs a migration-only copy attributed to Citrus source authority |
| experiment canvas binding | `orange.calibration.experiment_canvas_binding`, version 2 | Citrus canvas configuration plus Orange candidate packager |
| daily registration set | `orange.calibration.daily_registration_set`, version 2 | Citrus accepted translation plus exact Orange rim observations/masks |
| recording calibration capsule | `orange.recording.calibration_capsule`, version 2 | Orange at a future arm-time integration point |

The corresponding schemas are in `docs/schemas/`. The registry-pointer schema
is present to freeze the future promotion boundary, but this slice contains no
pointer writer.

## Package identity

`package_id` is derived only from `package.json#/identity`:

```text
sha256(orange.canonical_json.v1(identity))
```

`orange.canonical_json.v1` is UTF-8 JSON with object keys sorted by Unicode
code point, no insignificant whitespace, standard JSON string escaping, and no
floating-point identity values. Signed 64-bit integers, strings, booleans,
null, arrays, and objects are permitted. Numeric calibration results stay in checksum-bound
exact-byte assets. This avoids making package identity depend on language-
specific floating-point formatting.

The package directory is named from the full identity digest. Creation uses a
sibling staging directory, flushes files and directories, and publishes with
one rename. Existing package directories are never overwritten.

## Exact-byte inventory

`inventory.json` lists every materialized member with:

- package-relative authoritative path;
- SHA-256 and byte count;
- semantic role and required/optional classification;
- target plane;
- explicit camera and arena, or explicit null global scope;
- absolute source path as non-authoritative provenance;
- source SHA-256 and byte count;
- whether a pre-existing declared checksum was verified.

The validator never needs the original path. `--verify-sources` is an optional
audit that additionally checks the original bytes are still present and
unchanged. Byte and optional source-retention audits run before an incomplete
candidate is rejected for declared required gaps. Required missing/mismatched
assets, path escapes, duplicate paths, camera/arena substitutions, altered
bytes, invalid rim-mask geometry, or a composition mismatch fail validation.

For a daily registration, the required camera-scoped products include the
accepted rim observation, its manifest and image set, spatial runtime mask,
Palette compatibility mask, raw Hough overlay, accepted inner-rim overlay,
valid centroid-gate overlay, and Citrus geometry-review observation. The
validator requires `valid_geometry.r >= outer_geometry.r` and binds the mask
back to the exact observation artifact ID. Daily identity explicitly states
that canonical experimental dimensions remain unchanged and only translation
is permitted.

## Canvas compatibility

Orange and Citrus now use the same
`citrus.calibration.canvas_projection_geometry_identity`, version 1 rule for
accepted projection products. Exact whole-file SHA-256 remains the first and
strongest match. A raw checksum mismatch is accepted only when:

1. the active commissioning pointer checksum-binds an accepted release;
2. that release checksum-binds the exact active homography pointer;
3. the release checksum-binds its accepted canvas snapshot; and
4. current and accepted snapshots have the same projection-geometry identity.

The identity excludes only calibration presentation controls, mutable
projected-surface scale caches already superseded by accepted scale artifacts,
and pixel sizes derived from positive physical experimental dimensions. Arena
placement/size, camera mapping/native raster, physical dimensions, tank/dish
semantics, canvas raster, and every field not explicitly excluded remain
fail-closed. A semantic acceptance records basis
`projection_geometry_identity_v1` and warning
`canvas_non_geometry_calibration_state_only_change`.

The shared Orange C++ implementation is
`src/gui/spatial_layout/canvas_projection_geometry_identity.h`; the standalone
package validator mirrors that same identity rule.

## Standalone commands

Create a non-active Shadow migration candidate under a disposable review root:

```bash
python3 scripts/calibration_package.py package-shadow \
  --release /path/to/accepted/commissioning.json \
  --output /tmp/orange_calibration_package_review/shadow_candidate
```

Optionally bind an exact accepted daily registration:

```bash
python3 scripts/calibration_package.py package-shadow \
  --release /path/to/accepted/commissioning.json \
  --fixture-manifest /path/to/reviewed/fixture_manifest.json \
  --build-provenance /path/to/reviewed/software_builds.json \
  --daily-registration /path/to/accepted/registration.json \
  --output /tmp/orange_calibration_package_review/shadow_with_daily
```

Validate without either live program or original source paths:

```bash
python3 scripts/calibration_package.py validate /path/to/package
```

Add `--verify-sources` only for a source-retention audit.

The tool requires an explicit output directory and has no `promote`, `select`,
or `activate` command. Package manifests record
`activation_allowed=false`, `active_pointer_written=false`, and candidate-only
lifecycle state.

## Current Shadow migration finding

The version-1 Shadow commissioning release checksum-binds homographies,
projected-surface scales, source sessions, configuration snapshots, and their
acceptance products. It does not checksum-bind one authoritative operational
fixture manifest, the holder-validation report/overlays, or the exact Orange
and Citrus build identities. The migration tool does not guess from nearby
timestamps or directories. Without explicitly reviewed `--fixture-manifest`
and `--build-provenance` inputs, it writes these as required gaps, seals an
`incomplete` candidate for inspection, and the standalone validator rejects it.

This is intentional fail-closed behavior. The fixture manifest must be bound by
an operator-reviewed follow-up, not inferred from whichever holder report is
newest. Full-resolution source captures remain opt-in through
`--include-archive-images`; accepted review overlays remain package evidence.

## Deferred integration

The following are deliberately outside this slice:

- a neutral durable registry publisher and safe ownership/chown integration;
- immutable-retry reuse after validation;
- active setup/binding/daily pointer promotion and rollback;
- Orange/Citrus arm-time composition handshake;
- recording-local capsule materialization and H5 composition fields;
- GUI registry selection/review;
- runtime hot-path reads (none are introduced here).

Those changes should proceed only after the current candidate inventory and
fixture binding are reviewed.
