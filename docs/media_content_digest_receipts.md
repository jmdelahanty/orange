# Media content digest receipts (recorder side of the receipt-backed sealer)

Status 2026-10-09: implemented, default off. Approved by Jeremy (via
Citrus) with the hard rule that receipts never delay or disturb capture or
data writing. Citrus's receipt-aware sealer is on branch
`agent/citrus/receipt-backed-sealer-20261009` (release 2.1.0 pending);
Palette accepted the design. Until 2.1.0 is released the 2.0.0 sealer hashes
receipt files as ordinary files, so leave the knob off for deliveries.

## Why

The sealer (`citrus-recording-transfer` 2.0.0) reads every media byte about
three times plus the copy: about 4 to 5 h of hashing for a 24 h, 9.4 TB
session. With a producer digest per clip the sealer takes the digest from
the receipt (size must match), checks the source did not change by
size / mtime / ctime / inode, hashes every destination byte and refuses on
any mismatch. A wrong receipt can only cause a refusal, never a wrong
acceptance.

## What the recorder does

`src/media_content_digest.{h,cpp}`, wired into
`tools/external_recorder_ipc_probe.cpp` (both recorder builds):

- One `ContentDigestHasher` per recorder process (one process per camera
  and role), started at option parsing when enabled. Its thread runs
  SCHED_IDLE (nice 19 fallback) so it only takes a core nobody else wants;
  the recorders are not on the isolated cores.
- After a video is finalized (writer destroyed: trailer, playback-intent
  patch, `.finalization.json` sidecar) the writer path calls
  `request_content_digest_receipt`, a try-push into a bounded queue
  (default 64). It never blocks and never waits for a digest; a full queue
  abandons that file's receipt (counted). Three sites: each rolling clip,
  the merged full-frame video, the direct single-output video.
- The hasher reads the closed file back with plain `read()` in 4 MB chunks
  (no mmap, no fadvise) into a streaming SHA-256, at IO priority idle
  (`ionice -c 3` for that thread) and under a per-process rate cap (default
  16 MB/s, so eight recorder processes read at most about the four-camera
  recording rate). A 30 s clip was just written and is read from the page
  cache; a production-length clip (15 to 30 min, 17 to 34 GB per full-frame
  file) has partly left the cache by its finalize, so the read-back then
  hits the NVMe drives on bridge 20 at that capped rate while the next clip
  records. That case still needs its own no-impact proof (requested by
  Jeremy 2026-10-09; see "Production clip lengths" below).
- The receipt `<video>.content_digest.json` is written beside the video by
  temp + rename, never partial. Closed schema `orange.media_content_digest`
  version 1 (Citrus's exact field set): `schema_id`, `schema_version`,
  `algorithm` "sha256", `sha256`, `size_bytes`, `computed`
  "after_finalization_readback", `video_path` (relative to the recording
  folder that holds `recording_session.json`), `computed_at_utc`,
  `session_id` (= that folder's basename = `recording_session.json`
  session_id), `producer_pid`.
- At the recorder's own finalization, before the summary is written, the
  hasher gets a bounded wait (default 10 s) and the rest is abandoned; the
  sealer hashes those files itself. The summary carries
  `content_digest_receipts {enabled, requested, written, abandoned, failed,
  queue_capacity, finish_wait_ms}`.

## Knobs

| Where | Key | Default |
| --- | --- | --- |
| recorder env (read by the recorder) | `ORANGE_EXTERNAL_RECORDER_CONTENT_DIGEST_RECEIPTS` | 0 |
| headless spec | `fixed.external_recorder_content_digest_receipts` (exports the env for the supervised recorders) | false |
| app config | `recording.external_ipc.content_digest_receipts` (same export) | unset |
| recorder env | `ORANGE_EXTERNAL_RECORDER_RECORDING_ROOT` (run folder; default: parent of the summary JSON's directory) | derived |
| recorder env | `ORANGE_EXTERNAL_RECORDER_CONTENT_DIGEST_QUEUE` | 64 |
| recorder env | `ORANGE_EXTERNAL_RECORDER_CONTENT_DIGEST_FINISH_WAIT_MS` | 10000 |
| recorder env | `ORANGE_EXTERNAL_RECORDER_CONTENT_DIGEST_RATE_MBPS` (per-process read-back cap; 0 = uncapped) | 16 |

Tests: `media_content_digest_tests` (format, digest equals the streaming
file hash, bounded queue, bounded finish, no partial files).

## No-impact proof

Same spec (`fourcam_palette_fish_19220_1_rolling_crops_shadow`, 120 s,
four cameras, rolling crops, pose on), same recorder binary, receipts off
then on; compared: camera frame-id gaps, recorder drops, encode queue high
water, merged pending-GOP and MP4 writer queue peaks, detect latency, and
every receipt verified against `sha256sum` of its video. Results (2026-10-09, runs `..._receipts_off_20261009_005153`,
`..._receipts_on_20261009_005434` and, after the ordering fix,
`..._receipts_on_20261009_010011`):

| Gate | off | on | on (final ordering) |
| --- | --- | --- | --- |
| camera frame-id gaps (4 cameras) | 0 | 0 | 0 |
| recorder drops (8 recorders) | 0 | 0 | 0 |
| frames received = encoded | 12002 | 12001 | 12002 |
| encode queue high-water full / crop | 17 / 1 | 17 / 1 | 17 / 1 |
| merged pending-GOP peak | 3 | 3 | 3 |
| MP4 writer queue peak (packets) | 0 | 0 | 0 |
| receipts requested / written / abandoned / failed per recorder | 0 | 4 / 4 / 0 / 0 | 4 / 4 / 0 / 0 |
| receipt files verified against sha256sum | n/a | 32 / 32 | 32 / 32 |

Citrus's 2.1.0 draft (PR #6, fc865ef) accepted all 32 receipts of the
`_005434` run with `--validate-source-only` in 1.1 s. In a clip directory
the receipt's mtime is the last one (about 20 s after the MP4, sidecars and
CSV, at idle priority with eight hashers sharing the pool), so a receipt
means everything for that video is final.
