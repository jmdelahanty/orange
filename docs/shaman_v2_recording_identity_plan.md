# Shaman-v2 recording identity plan

Date: 2026-08-24

## Worktree isolation

This work is based on Orange commit
`a5e789b2f61bd095eae7a3dd6d8d3fa16f4e0690` in the dedicated worktree
`/tmp/orange-frame-bound-acquisition-identity-20260824` and branch
`feature/shaman-v2-recording-identity`.

The source checkout `/home/jeremy/orange-gop-split-a16` is intentionally not
modified or cleaned. At handoff it contained 15 modified tracked files with a
650-insertion/13-deletion diff.

## Contract objective

Every Shaman-v2 slot emitted for a recorded acquisition must carry the actual
positive `recording_frame_id` and a fixed-size cryptographic token identifying
the immutable Orange recording. A closed camera-binding record must make the
configured acquisition camera ID, camera serial, and numeric Shaman camera ID
explicit. Non-recording slots must carry neither a recording frame ID nor a
recording token.

The token is derived from canonical closed-schema identity input, not a path
guess. Same-frame YOLO and pose enrichment slots inherit the entire identity
from their base acquisition state. ABI mismatch remains fail-closed.

## Implementation checklist

- [ ] Define and validate the closed recording-token payload and camera-binding
      payload, with canonical JSON and SHA-256 rules.
- [ ] Version the Shaman-v2 slot ABI and append a fixed-size token field.
- [ ] Extend the acquisition-to-IPC event so the base slot receives the true
      recording frame ID, camera frame ID, host timestamp, and recording ID.
- [ ] Ensure non-recording slots are explicitly token-free.
- [ ] Ensure YOLO and pose publications cannot substitute identity from a
      different acquisition or recording.
- [ ] Persist the recording-token authority and camera bindings in finalized
      Orange evidence without changing existing closed v1 records in place.
- [ ] Add ABI, queue round-trip, producer, inheritance, recording-transition,
      token-mismatch, and manifest validation tests.
- [ ] Update the Shaman-v2 and recording artifact contracts.
- [ ] Run the focused Orange tests and `git diff --check`.
- [ ] Commit and push the dedicated feature branch without merging or deploying.
