#pragma once

#include <cstdint>

namespace orange::recording {

// Plain snapshot shared by native full-frame/crop writers, external encoder
// shards, merged outputs, and rolling clips. The writer owns the atomic hot-path
// counters; this type adds no threads, locks, allocations, or device operations.
struct PacketWriteStats {
    std::uint64_t submissions_accepted = 0;
    std::uint64_t submission_bytes_accepted = 0;
    std::uint64_t submissions_rejected = 0;
    std::uint64_t write_attempts = 0;
    std::uint64_t packets_written = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t write_failures = 0;
    int first_write_error_code = 0;

    // Add each disjoint shard/clip snapshot exactly once, not successive live
    // snapshots of the same writer. First error is in aggregation order, not
    // necessarily chronological across independent encoder shards.
    void accumulate(const PacketWriteStats& other) noexcept
    {
        submissions_accepted += other.submissions_accepted;
        submission_bytes_accepted += other.submission_bytes_accepted;
        submissions_rejected += other.submissions_rejected;
        write_attempts += other.write_attempts;
        packets_written += other.packets_written;
        bytes_written += other.bytes_written;
        write_failures += other.write_failures;
        if (first_write_error_code == 0) {
            first_write_error_code = other.first_write_error_code;
        }
    }
};

// Packet-stage accounting only. For terminal evidence, drain/join the writer
// before taking its snapshot. Balanced live or zero-packet counters do NOT prove
// encoding, muxer flush/trailer success, source-frame coverage, or file durability.
// In particular an IPC source-detach ACK is not a packet write.
inline bool packet_writes_balanced(const PacketWriteStats& stats) noexcept
{
    return stats.submissions_rejected == 0 &&
        stats.write_failures == 0 &&
        stats.submissions_accepted == stats.write_attempts &&
        stats.write_attempts == stats.packets_written;
}

}  // namespace orange::recording
