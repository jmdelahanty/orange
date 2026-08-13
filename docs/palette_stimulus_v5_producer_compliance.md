# Palette stimulus schema-v5 producer compliance

Status: read-only producer boundary implemented; no production v5 output enabled

Palette authority is pinned to repository `/home/jeremy/palette-stimulus-v5`,
branch `sun`, commit:

```text
7ffb8143f54cc54f811e306a1e5be4396ffc1dc3
```

Normative consumer sources at that commit are:

- `docs/citrus_stimulus_coordinate_output_contract.md`
- `docs/stimulus_coordinate_contract.md`
- `fisheye.shared.stimulus_coordinate_contract`

Local supplemental or timestamped archive notes do not override that tracked
wire contract or its executable validator.

## Authority flow

The intended future identity chain is:

```text
Orange accepted acquisition
  -> Shaman-v2 recording_frame_id (one-based, camera/session scoped)
  -> Citrus stimulus-state staging row
  -> Orange finalized acquisition-index authority
  -> source_acquisition_frame_index = recording_frame_id - 1
  -> Palette schema-v5 H5 mapping record and arrays
```

`camera_frame_id` and Citrus `triggering_camera_frame_id` remain useful external
provenance. Neither is an acquisition-frame index and neither may be substituted
when `recording_frame_id` is missing.

The target held by a chaser has a second, independent source identity. Citrus
must retain the original Shaman-v2 `recording_frame_id` that supplied that
target, even when later stimulus states reuse the held position.

## Orange implementation boundary

`src/session/acquisition_index_authority.*` is a disconnected, read-only
producer-side boundary. It does not run during acquisition and is not linked
into the Orange GUI or headless runtime.

The resolver accepts one camera only when all of the following remain true:

- `recording_session.json` is a completed Orange schema-v1 session;
- `orange.recording.frame_identity` is finalized and binds the selected camera;
- an external-IPC camera has passed returned-encoder identity verification (or
  an in-process camera carries its explicit producer-declared authority);
- `orange.recording.acquisition_index_mapping` is the exact closed finalized
  version-1 record;
- both semantic-record digests match;
- the selected metadata CSV remains inside the recording bundle and its byte
  checksum matches;
- `frame_id` and canonical `recording_frame_id` exist, match row by row, and
  form the declared dense sequence; and
- the total acquisition count fits Palette's signed-int64 domain.

The same proof is now required when Orange first seals
`orange.recording.acquisition_index_mapping`. A legacy or in-process metadata
CSV that contains only `frame_id` remains a valid recording artifact, but it
receives an unsealed mapping with
`reason_code=metadata_identity_alias_unavailable`; it is not silently promoted
to the stronger Palette acquisition-index authority. A checksum-valid CSV in
which the aliases disagree is likewise retained as unsealed evidence with
`reason_code=metadata_identity_alias_mismatch`.

The builder then converts an explicit vector of raw Shaman-v2
`recording_frame_id` values into the exact little-endian signed-int64 Palette
array and constructs the closed
`citrus.stimulus_source_acquisition_mapping` version-1 record. Its array digest
uses Palette's `numpy_dtype_shape_c_order_bytes_v1` byte profile, and its record
digest uses compact, sorted-key canonical JSON.

This boundary does not yet:

- receive Citrus stimulus rows;
- build `stimulus_state_key`;
- build held-target acquisition arrays;
- write or mutate an H5 file;
- select Shaman v2 as Citrus runtime authority; or
- publish a canonical artifact into `recording_session.json`.

## Safe deployment order

1. Add a Citrus `RuntimeTrackingState` populated from Shaman v2 while the
   legacy reader remains authoritative.
2. Compare legacy and v2 target selection, freshness, target hold, camera
   identity, homography result, and chaser output without allowing both to
   control one arena.
3. Stage current-state and held-target `recording_frame_id` values for every
   persisted chaser row.
4. After recording finalization, resolve Orange's sealed per-camera authority.
5. Create a new canonical H5 candidate off the rendering/acquisition hot path.
6. Materialize the exact v5 row identity, source mapping, target mapping,
   coordinate descriptor, surface manifest, arena frame, calibration evidence,
   and digests.
7. Run Palette's executable preflight before publishing the candidate path.
8. Validate an asymmetric coordinate sweep and a four-camera PTP experiment.
9. Promote `shaman_v2_authoritative` and schema-v5 output only after the
   comparison and consumer gates pass.

At every stage, failure leaves the raw evidence intact and produces no v5
claim. Existing schema-v4 and schema-v6 artifacts retain their actual version;
neither is relabeled as schema v5. Current recordings remain eligible for
Palette's explicit metadata-and-calibration-only import.

## Validation status

The source component has focused fixtures for:

- a valid dense authority and one-based to zero-based conversion;
- stale semantic-record and artifact checksums;
- checksum-valid disagreement between `frame_id` and `recording_frame_id`;
- an unlisted camera or open-ended mapping record;
- Palette-compatible array and record digest vectors; and
- empty, out-of-domain, malformed-digest, or unsealed mapping inputs.

The recording-session suite also passes the real manifest-writer output
directly through the strict resolver. This prevents the writer and consumer
schemas from drifting behind separately maintained mock fixtures.

The digest vectors were independently generated with NumPy and Python's
`hashlib` in the local `juicebox` environment. Full Palette preflight remains a
deferred validation gate because that environment has NumPy and HDF5 support
but does not currently contain Palette's Zarr dependency. No dependency was
installed as part of this work.
