#pragma once

#include "spatial_roi_recorder_artifact_root.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace orange::spatial_roi::recording {

// One descriptor-bound decoded luma frame measured by the video probe. These
// values cannot be supplied directly to terminal evidence: they are exposed
// only as part of SpatialRoiRecorderVideoSanityResult, whose constructor is
// private to the probe.
struct SpatialRoiRecorderVideoSanitySample {
    std::uint64_t requested_frame_index = 0;
    double mean = 0.0;
    double stddev = 0.0;
    std::uint32_t min = 0;
    std::uint32_t max = 0;
    double black_fraction_lt8 = 0.0;
    double white_fraction_gt247 = 0.0;
    std::uint64_t decoded_bytes = 0;
};

// The probe deliberately accepts the authenticated contract values as input.
// In particular, max_media_bytes is not a caller-chosen convenience limit:
// the caller must pass the value obtained from the verified recorder contract.
struct SpatialRoiRecorderVideoSanityRequest {
    std::shared_ptr<SpatialRoiRecorderArtifactRoot> artifact_root;
    std::string video_relative_path;
    std::uint32_t encoded_width = 0;
    std::uint32_t encoded_height = 0;
    std::uint64_t expected_frame_count = 0;
    std::uint64_t max_media_bytes = 0;
    // Authenticated stream cadence used for rate, duration, and decoded PTS
    // validation.
    double expected_frame_rate = 0.0;
    // Cooperative wall-clock budget checked between probe loops and passed to
    // FFmpeg's interrupt callback. A decoder call itself is not guaranteed to
    // be interruptible. Callers MUST enforce an outer supervised process
    // deadline when this probe is used on untrusted or adversarial media.
    std::chrono::milliseconds timeout{0};
};

class SpatialRoiRecorderVideoSanityResult final {
public:
    SpatialRoiRecorderVideoSanityResult(
        const SpatialRoiRecorderVideoSanityResult&) = default;
    SpatialRoiRecorderVideoSanityResult& operator=(
        const SpatialRoiRecorderVideoSanityResult&) = default;
    SpatialRoiRecorderVideoSanityResult(
        SpatialRoiRecorderVideoSanityResult&&) noexcept = default;
    SpatialRoiRecorderVideoSanityResult& operator=(
        SpatialRoiRecorderVideoSanityResult&&) noexcept = default;

    const SpatialRoiRecorderArtifactIdentity& artifact_root_identity() const
        noexcept
    {
        return artifact_root_identity_;
    }
    const SpatialRoiRecorderArtifactIdentity& video_identity() const noexcept
    {
        return video_identity_;
    }
    const std::string& relative_path() const noexcept
    {
        return video_relative_path_;
    }
    std::uint64_t size_bytes() const noexcept { return size_bytes_; }

    // The digest uses the repository's canonical "sha256:<lowercase hex>"
    // spelling.
    const std::string& sha256() const noexcept { return sha256_; }

    const std::string& duration_seconds() const noexcept
    {
        return duration_seconds_;
    }
    // Canonical positive FFmpeg rationals (for example "100/1" and
    // "1/90000") retained as evidence of the validated stream cadence.
    const std::string& frame_rate() const noexcept { return frame_rate_; }
    const std::string& time_base() const noexcept { return time_base_; }
    // A successful fixed-profile probe always has decoded PTS.  The boolean
    // remains explicit in the evidence schema so missing or sparse timestamp
    // streams fail closed instead of being conflated with a valid timeline.
    bool has_decoded_pts() const noexcept { return has_decoded_pts_; }
    std::int64_t first_decoded_pts() const noexcept
    {
        return first_decoded_pts_;
    }
    std::int64_t last_decoded_pts() const noexcept { return last_decoded_pts_; }
    const std::string& container() const noexcept { return container_; }
    const std::string& codec() const noexcept { return codec_; }
    const std::string& decoder() const noexcept { return decoder_; }
    const std::string& pixel_format() const noexcept
    {
        return pixel_format_;
    }
    const std::string& color_range() const noexcept { return color_range_; }
    std::uint32_t bit_depth() const noexcept { return bit_depth_; }
    const std::string& chroma_subsampling() const noexcept
    {
        return chroma_subsampling_;
    }

    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }
    std::uint64_t frame_count() const noexcept { return frame_count_; }
    const std::vector<SpatialRoiRecorderVideoSanitySample>& samples() const
        noexcept
    {
        return samples_;
    }

private:
    friend class SpatialRoiRecorderVideoSanityProbe;
#if defined(ORANGE_SPATIAL_ROI_VIDEO_SANITY_TESTING)
    friend class SpatialRoiRecorderVideoSanityTestFactory;
#endif
    friend bool probe_spatial_roi_recorder_video_sanity(
        const SpatialRoiRecorderVideoSanityRequest& request,
        std::unique_ptr<SpatialRoiRecorderVideoSanityResult>* result_out,
        std::string* error_out);

    SpatialRoiRecorderVideoSanityResult(
        std::shared_ptr<SpatialRoiRecorderArtifactRoot> artifact_root,
        std::shared_ptr<SpatialRoiRecorderArtifactFile> video_file,
        SpatialRoiRecorderArtifactIdentity artifact_root_identity,
        SpatialRoiRecorderArtifactIdentity video_identity,
        std::string video_relative_path,
        std::uint64_t size_bytes,
        std::string sha256,
        std::string duration_seconds,
        std::string frame_rate,
        std::string time_base,
        bool has_decoded_pts,
        std::int64_t first_decoded_pts,
        std::int64_t last_decoded_pts,
        std::string container,
        std::string codec,
        std::string decoder,
        std::string pixel_format,
        std::string color_range,
        std::uint32_t bit_depth,
        std::string chroma_subsampling,
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t frame_count,
        std::vector<SpatialRoiRecorderVideoSanitySample> samples) noexcept
        : artifact_root_(std::move(artifact_root)),
          video_file_(std::move(video_file)),
          artifact_root_identity_(artifact_root_identity),
          video_identity_(video_identity),
          video_relative_path_(std::move(video_relative_path)),
          size_bytes_(size_bytes),
          sha256_(std::move(sha256)),
          duration_seconds_(std::move(duration_seconds)),
          frame_rate_(std::move(frame_rate)),
          time_base_(std::move(time_base)),
          has_decoded_pts_(has_decoded_pts),
          first_decoded_pts_(first_decoded_pts),
          last_decoded_pts_(last_decoded_pts),
          container_(std::move(container)),
          codec_(std::move(codec)),
          decoder_(std::move(decoder)),
          pixel_format_(std::move(pixel_format)),
          color_range_(std::move(color_range)),
          bit_depth_(bit_depth),
          chroma_subsampling_(std::move(chroma_subsampling)),
          width_(width),
          height_(height),
          frame_count_(frame_count),
          samples_(std::move(samples))
    {
    }

    // Keep the descriptor-relative authorities alive for as long as a result
    // can be consumed. The descriptor is intentionally not exposed as a
    // mutable API; it prevents a result from silently outliving the exact
    // retained artifact authority used to produce it.
    std::shared_ptr<SpatialRoiRecorderArtifactRoot> artifact_root_;
    std::shared_ptr<SpatialRoiRecorderArtifactFile> video_file_;
    SpatialRoiRecorderArtifactIdentity artifact_root_identity_{};
    SpatialRoiRecorderArtifactIdentity video_identity_{};
    std::string video_relative_path_;
    std::uint64_t size_bytes_ = 0;
    std::string sha256_;
    std::string duration_seconds_;
    std::string frame_rate_;
    std::string time_base_;
    bool has_decoded_pts_ = false;
    std::int64_t first_decoded_pts_ = 0;
    std::int64_t last_decoded_pts_ = 0;
    std::string container_;
    std::string codec_;
    std::string decoder_;
    std::string pixel_format_;
    std::string color_range_;
    std::uint32_t bit_depth_ = 0;
    std::string chroma_subsampling_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint64_t frame_count_ = 0;
    std::vector<SpatialRoiRecorderVideoSanitySample> samples_;
};

class SpatialRoiRecorderVideoSanityProbe final {
public:
    static bool Run(
        const SpatialRoiRecorderVideoSanityRequest& request,
        std::unique_ptr<SpatialRoiRecorderVideoSanityResult>* result_out,
        std::string* error_out = nullptr);
};

bool probe_spatial_roi_recorder_video_sanity(
    const SpatialRoiRecorderVideoSanityRequest& request,
    std::unique_ptr<SpatialRoiRecorderVideoSanityResult>* result_out,
    std::string* error_out = nullptr);

}  // namespace orange::spatial_roi::recording
