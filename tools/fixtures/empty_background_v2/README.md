# Empty-background v2 fixtures (DRAFT)

Documents for the version 2 drafts, proposed to Palette on 2026-11-15:
`orange.empty_background_capture` v2 (`capture_*`) and the start-snapshot block
`orange_recording_empty_background_v2` (`snapshot_*`). Both reference
`orange.image_levels.v1` and `orange.requester.v1` by `$id`.

Everything under `valid/` must validate and everything under `invalid/` must
fail, in jsonschema and in the generated C++ parser alike. Each invalid file
breaks exactly one rule, which its name states. Digests and statistics are
illustrative, not computed from an image.

Orange keeps writing version 1 until Palette agrees to version 2.
