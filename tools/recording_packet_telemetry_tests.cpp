#include "recording_packet_telemetry.h"
#include "video_container_finalization.h"

#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
using orange::recording::PacketWriteStats;
using orange::recording::packet_writes_balanced;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void test_packet_stage_boundaries()
{
    static_assert(std::is_trivially_copyable_v<PacketWriteStats>);
    PacketWriteStats stats;
    require(packet_writes_balanced(stats), "empty packet accounting is balanced");
    stats.submissions_accepted = 1;
    stats.submission_bytes_accepted = 100;
    require(!packet_writes_balanced(stats), "acceptance is not a completed write");
    stats.write_attempts = 1;
    require(!packet_writes_balanced(stats), "an attempt is not a successful write");
    stats.packets_written = 1;
    stats.bytes_written = 100;
    require(packet_writes_balanced(stats), "successful drained packets balance");
    stats.submissions_rejected = 1;
    require(!packet_writes_balanced(stats), "rejection cannot be hidden by balanced writes");
    stats.submissions_rejected = 0;
    stats.write_failures = 1;
    require(!packet_writes_balanced(stats), "write failure cannot be hidden by balanced counts");
}

void test_equivalence_with_existing_rules()
{
    // Exhaust the small counter states, including impossible/inconsistent ones.
    // Preserve both the old writer balance rule and the external v2 proof's
    // one-packet-per-encoded-frame check; neither implies master-source coverage.
    for (std::uint64_t accepted = 0; accepted < 4; ++accepted)
    for (std::uint64_t attempts = 0; attempts < 4; ++attempts)
    for (std::uint64_t written = 0; written < 4; ++written)
    for (std::uint64_t rejected = 0; rejected < 2; ++rejected)
    for (std::uint64_t failures = 0; failures < 2; ++failures) {
        PacketWriteStats stats;
        stats.submissions_accepted = accepted;
        stats.write_attempts = attempts;
        stats.packets_written = written;
        stats.submissions_rejected = rejected;
        stats.write_failures = failures;
        const bool old_writer_rule = rejected == 0 && failures == 0 &&
            accepted == attempts && attempts == written;
        require(packet_writes_balanced(stats) == old_writer_rule,
                "shared rule must preserve writer acceptance semantics");
        for (std::uint64_t frames = 0; frames < 4; ++frames) {
            const bool old_proof_rule = rejected == 0 && failures == 0 &&
                accepted == frames && attempts == frames && written == frames;
            require((packet_writes_balanced(stats) && written == frames) == old_proof_rule,
                    "shared rule must preserve external v2 packet/frame parity");
        }
    }
}

void test_disjoint_aggregation()
{
    PacketWriteStats first{2, 100, 0, 2, 2, 100, 0, 0};
    PacketWriteStats second{3, 200, 1, 3, 2, 150, 1, -5};
    PacketWriteStats third{1, 30, 0, 1, 0, 0, 1, -28};
    PacketWriteStats total;
    total.accumulate(first);
    require(packet_writes_balanced(total), "a successful first clip balances");
    total.accumulate(second);
    total.accumulate(third);
    require(total.submissions_accepted == 6 &&
                total.submission_bytes_accepted == 330 &&
                total.submissions_rejected == 1 && total.write_attempts == 6 &&
                total.packets_written == 4 && total.bytes_written == 250 &&
                total.write_failures == 2 && total.first_write_error_code == -5,
            "all counters and the first aggregated error must survive shard/clip reduction");
    require(!packet_writes_balanced(total), "aggregation cannot hide a failed clip");
    require(first.write_failures == 0 && second.first_write_error_code == -5,
            "aggregation must not mutate individual video evidence");
    total = {};
    total.accumulate(first);
    require(total.first_write_error_code == 0 && total.packets_written == 2,
            "a new recording must not inherit an old error or count");
}

void test_packet_success_is_not_container_completion()
{
    using OrangeVideoContainerFinalization::Outcome;
    using OrangeVideoContainerFinalization::PacketWritesComplete;
    Outcome outcome;
    outcome.packet_writes = {1, 100, 0, 1, 1, 100, 0, 0};
    require(!PacketWritesComplete(outcome), "balanced writes still require a muxer flush");
    outcome.muxer_flush_attempted = true;
    require(!PacketWritesComplete(outcome), "attempted flush is not successful flush");
    outcome.muxer_flush_succeeded = true;
    require(PacketWritesComplete(outcome), "drained and flushed packet stage completes");
    outcome.writer_error_latched = true;
    require(!PacketWritesComplete(outcome), "writer failure must remain authoritative");
    outcome.writer_error_latched = false;
    outcome.packet_writes.submissions_rejected = 1;
    require(!PacketWritesComplete(outcome), "finalization must use the shared balance rule");
}
}  // namespace

int main()
{
    try {
        test_packet_stage_boundaries();
        test_equivalence_with_existing_rules();
        test_disjoint_aggregation();
        test_packet_success_is_not_container_completion();
        std::cout << "recording_packet_telemetry_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
