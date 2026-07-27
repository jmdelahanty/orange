#include "encoder_qp_map.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>

namespace orange::encoding {
namespace {

bool set_error(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

bool valid_qp_delta(const int value)
{
    return value >= -51 && value <= 51;
}

double axis_distance_to_interval(const double value,
                                 const double interval_min,
                                 const double interval_max)
{
    if (value < interval_min) {
        return interval_min - value;
    }
    if (value > interval_max) {
        return value - interval_max;
    }
    return 0.0;
}

std::string map_checksum(const std::vector<int8_t>& values)
{
    uint64_t hash = 14695981039346656037ULL;
    for (const int8_t value : values) {
        hash ^= static_cast<uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << hash;
    return out.str();
}

}  // namespace

std::string normalize_qp_map_mode(std::string mode)
{
    std::transform(mode.begin(), mode.end(), mode.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    std::replace(mode.begin(), mode.end(), '-', '_');
    if (mode.empty() || mode == "none" || mode == "disabled") {
        return kQpMapModeOff;
    }
    return mode;
}

bool validate_qp_map_policy(const QpMapPolicy& input,
                            std::string* error_out)
{
    const std::string mode = normalize_qp_map_mode(input.mode);
    if (mode != kQpMapModeOff && mode != kQpMapModeStaticDishPrior) {
        return set_error(error_out, "QP map mode must be off or static_dish_prior");
    }
    if (mode == kQpMapModeOff) {
        return true;
    }
    if (!std::isfinite(input.circle.center_x_px) ||
        !std::isfinite(input.circle.center_y_px) ||
        !std::isfinite(input.circle.radius_px) ||
        input.circle.radius_px <= 0.0) {
        return set_error(error_out, "static_dish_prior requires a finite positive circle radius");
    }
    if (!std::isfinite(input.halo_px) || input.halo_px < 0.0) {
        return set_error(error_out, "QP map halo_px must be finite and non-negative");
    }
    if (!valid_qp_delta(input.inside_delta_qp) ||
        !valid_qp_delta(input.halo_delta_qp) ||
        !valid_qp_delta(input.outside_delta_qp)) {
        return set_error(error_out, "QP map deltas must be within [-51, 51]");
    }
    if (input.inside_delta_qp > input.halo_delta_qp ||
        input.halo_delta_qp > input.outside_delta_qp) {
        return set_error(
            error_out,
            "QP map deltas must be ordered inside <= halo <= outside");
    }
    return true;
}

bool build_circle_qp_delta_map(const QpMapPolicy& input,
                               const uint32_t frame_width,
                               const uint32_t frame_height,
                               const uint32_t block_size,
                               QpDeltaMap* map_out,
                               std::string* error_out)
{
    if (!map_out) {
        return set_error(error_out, "QP map output is required");
    }
    *map_out = {};

    QpMapPolicy policy = input;
    policy.mode = normalize_qp_map_mode(policy.mode);
    if (!validate_qp_map_policy(policy, error_out)) {
        return false;
    }
    if (policy.mode == kQpMapModeOff) {
        return true;
    }
    if (frame_width == 0 || frame_height == 0 || block_size == 0) {
        return set_error(error_out, "QP map frame and block dimensions must be positive");
    }
    if (frame_width > std::numeric_limits<uint32_t>::max() - (block_size - 1) ||
        frame_height > std::numeric_limits<uint32_t>::max() - (block_size - 1)) {
        return set_error(error_out, "QP map grid dimensions overflow");
    }

    QpDeltaMap result;
    result.mode = policy.mode;
    result.frame_width = frame_width;
    result.frame_height = frame_height;
    result.block_size = block_size;
    result.grid_width = (frame_width + block_size - 1U) / block_size;
    result.grid_height = (frame_height + block_size - 1U) / block_size;
    result.circle = policy.circle;
    result.halo_px = policy.halo_px;
    result.inside_delta_qp = policy.inside_delta_qp;
    result.halo_delta_qp = policy.halo_delta_qp;
    result.outside_delta_qp = policy.outside_delta_qp;
    const std::size_t value_count =
        static_cast<std::size_t>(result.grid_width) * result.grid_height;
    result.values.resize(value_count);

    const double radius_squared = policy.circle.radius_px * policy.circle.radius_px;
    const double halo_radius = policy.circle.radius_px + policy.halo_px;
    const double halo_radius_squared = halo_radius * halo_radius;
    for (uint32_t block_y = 0; block_y < result.grid_height; ++block_y) {
        const double y0 = static_cast<double>(block_y) * block_size;
        const double y1 = std::min(
            static_cast<double>(frame_height),
            static_cast<double>(block_y + 1U) * block_size);
        for (uint32_t block_x = 0; block_x < result.grid_width; ++block_x) {
            const double x0 = static_cast<double>(block_x) * block_size;
            const double x1 = std::min(
                static_cast<double>(frame_width),
                static_cast<double>(block_x + 1U) * block_size);
            const double dx = axis_distance_to_interval(
                policy.circle.center_x_px, x0, x1);
            const double dy = axis_distance_to_interval(
                policy.circle.center_y_px, y0, y1);
            const double minimum_distance_squared = dx * dx + dy * dy;

            int delta = policy.outside_delta_qp;
            if (minimum_distance_squared <= radius_squared) {
                delta = policy.inside_delta_qp;
                ++result.inside_block_count;
            } else if (minimum_distance_squared <= halo_radius_squared) {
                delta = policy.halo_delta_qp;
                ++result.halo_block_count;
            } else {
                ++result.outside_block_count;
            }
            result.values[
                static_cast<std::size_t>(block_y) * result.grid_width + block_x] =
                static_cast<int8_t>(delta);
        }
    }

    result.checksum = map_checksum(result.values);
    *map_out = std::move(result);
    return true;
}

}  // namespace orange::encoding
