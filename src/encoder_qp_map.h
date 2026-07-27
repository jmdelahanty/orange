#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace orange::encoding {

inline constexpr const char* kQpMapModeOff = "off";
inline constexpr const char* kQpMapModeStaticDishPrior = "static_dish_prior";

struct QpMapCircle {
    double center_x_px = 0.0;
    double center_y_px = 0.0;
    double radius_px = 0.0;
};

struct QpMapPolicy {
    std::string mode = kQpMapModeOff;
    QpMapCircle circle;
    double halo_px = 64.0;
    int inside_delta_qp = -2;
    int halo_delta_qp = 0;
    int outside_delta_qp = 2;
    std::string source_artifact_path;
    std::string source_artifact_sha256;
    std::string source_artifact_fingerprint;

    bool enabled() const { return mode != kQpMapModeOff; }
};

struct QpDeltaMap {
    std::string mode = kQpMapModeOff;
    uint32_t frame_width = 0;
    uint32_t frame_height = 0;
    uint32_t block_size = 0;
    uint32_t grid_width = 0;
    uint32_t grid_height = 0;
    QpMapCircle circle;
    double halo_px = 0.0;
    int inside_delta_qp = 0;
    int halo_delta_qp = 0;
    int outside_delta_qp = 0;
    std::size_t inside_block_count = 0;
    std::size_t halo_block_count = 0;
    std::size_t outside_block_count = 0;
    std::string checksum;
    std::vector<int8_t> values;

    bool enabled() const { return mode != kQpMapModeOff && !values.empty(); }
    std::size_t size_bytes() const { return values.size() * sizeof(values.front()); }
};

std::string normalize_qp_map_mode(std::string mode);

bool validate_qp_map_policy(const QpMapPolicy& policy,
                            std::string* error_out = nullptr);

bool build_circle_qp_delta_map(const QpMapPolicy& policy,
                               uint32_t frame_width,
                               uint32_t frame_height,
                               uint32_t block_size,
                               QpDeltaMap* map_out,
                               std::string* error_out = nullptr);

}  // namespace orange::encoding
