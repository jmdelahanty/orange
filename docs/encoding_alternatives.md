# Encoding Alternatives (Quality vs Seekability)

## Current Behavior (HW NVENC path)
- Rate control: VBR with AQ/Temporal AQ
- Lookahead: enabled for non-low-latency tuning; disabled for low-latency
- GOP/IDR: currently 1-second GOP (gopLength = fps), IDR period = GOP
- B-frames: disabled (frameIntervalP = 1)

This is a balanced default for low latency and predictable seeking without
introducing frame reordering.

## Alternative 1: Constant QP (CQP) Mode
**Idea:** Use CONSTQP (e.g., QP ~18–22) instead of bitrate targeting.

Pros:
- Bits go to complex regions; less waste on static background.
- Often smaller files at similar perceptual quality when most of the scene is static.

Cons:
- Bitrate is unbounded; complex scenes can spike size/IO.
- Requires confidence in storage bandwidth and disk size.

Suggested use:
- High-quality archival, or when the scene is mostly static.
- Consider a “Quality (CQP)” mode in UI rather than default.

## Alternative 2: Shorter GOP (Seekability)
**Idea:** Reduce GOP from 60 → 30 (0.5s at 60fps).

Pros:
- Faster, more reliable seeking (smaller decode-forward window).
- Better for interactive scrubbing and quick random access.

Cons:
- Slightly larger bitrate/files due to more frequent IDRs.

Suggested use:
- Review/analysis workflows that require frequent seeks.

## Alternative 3: Enable B-frames
**Idea:** Allow B-frames for higher compression efficiency.

Pros:
- Better compression at the same quality; may offset shorter GOP cost.

Cons:
- Adds reordering latency and complexity.
- Requires correct PTS/DTS handling and may complicate metadata alignment.

Suggested use:
- Offline/archival mode where latency is less important.

## Recommendation (Short-term)
- Keep default VBR + AQ with 1-second GOP for stability and latency.
- Add CQP and GOP presets as **user-selectable options** for A/B testing.
- Avoid B-frames for now unless we add reordering-aware timestamping.

## Suggested UI Presets
- **Balanced (default):** VBR + AQ, GOP = 60
- **Fast Seek:** VBR + AQ, GOP = 30
- **Quality (CQP):** CONSTQP (e.g., QP 20), GOP = 60
- **Archival (future):** CQP + B-frames (requires reordering support)
