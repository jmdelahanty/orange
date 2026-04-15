#ifndef ORANGE_LATENCY_STATS_H
#define ORANGE_LATENCY_STATS_H

#include <algorithm>
#include <chrono>
#include <cstdint>

struct LatencyAggregateStats {
    uint64_t sample_count = 0;
    uint64_t total_ns = 0;
    uint64_t max_ns = 0;
    uint64_t last_ns = 0;

    double mean_ms() const {
        if (sample_count == 0) {
            return 0.0;
        }
        return static_cast<double>(total_ns) / static_cast<double>(sample_count) / 1.0e6;
    }

    double max_ms() const {
        return static_cast<double>(max_ns) / 1.0e6;
    }

    double last_ms() const {
        return static_cast<double>(last_ns) / 1.0e6;
    }
};

inline void observe_latency_ns(LatencyAggregateStats* stats, uint64_t duration_ns)
{
    if (!stats) {
        return;
    }
    stats->sample_count++;
    stats->total_ns += duration_ns;
    stats->max_ns = std::max(stats->max_ns, duration_ns);
    stats->last_ns = duration_ns;
}

inline uint64_t steady_clock_now_ns()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

#endif // ORANGE_LATENCY_STATS_H
