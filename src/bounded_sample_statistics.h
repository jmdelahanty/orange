#ifndef ORANGE_BOUNDED_SAMPLE_STATISTICS_H
#define ORANGE_BOUNDED_SAMPLE_STATISTICS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace orange {

// Fixed-memory statistics for recording-duration telemetry. Count, mean, min,
// and max are exact. Percentiles are exact while every sample fits, then are
// estimated from a deterministic Algorithm-R reservoir. Determinism keeps
// diagnostics and unit tests reproducible without introducing a global RNG.
class BoundedSampleStatistics {
public:
    static constexpr std::size_t kDefaultMaxRetainedSamples = 8192;

    explicit BoundedSampleStatistics(
        const std::size_t max_retained_samples = kDefaultMaxRetainedSamples)
        : max_retained_samples_(max_retained_samples)
    {
        retained_samples_.reserve(max_retained_samples_);
    }

    void Reset()
    {
        sample_count_ = 0;
        sum_ = 0.0L;
        min_ = std::numeric_limits<double>::infinity();
        max_ = -std::numeric_limits<double>::infinity();
        retained_samples_.clear();
    }

    void Add(const double value)
    {
        ++sample_count_;
        sum_ += static_cast<long double>(value);
        min_ = std::min(min_, value);
        max_ = std::max(max_, value);

        if (max_retained_samples_ == 0) {
            return;
        }
        if (retained_samples_.size() < max_retained_samples_) {
            retained_samples_.push_back(value);
            return;
        }

        const std::uint64_t replacement_index =
            mix64(sample_count_ + kReservoirSeed) % sample_count_;
        if (replacement_index < max_retained_samples_) {
            retained_samples_[static_cast<std::size_t>(replacement_index)] = value;
        }
    }

    std::uint64_t sample_count() const
    {
        return sample_count_;
    }

    std::size_t retained_sample_count() const
    {
        return retained_samples_.size();
    }

    std::size_t max_retained_samples() const
    {
        return max_retained_samples_;
    }

    bool empty() const
    {
        return sample_count_ == 0;
    }

    bool percentiles_exact() const
    {
        return sample_count_ <= retained_samples_.size();
    }

    double min() const
    {
        return empty() ? 0.0 : min_;
    }

    double max() const
    {
        return empty() ? 0.0 : max_;
    }

    double mean() const
    {
        return empty()
            ? 0.0
            : static_cast<double>(sum_ / static_cast<long double>(sample_count_));
    }

    std::vector<double> sorted_retained_samples() const
    {
        std::vector<double> sorted = retained_samples_;
        std::sort(sorted.begin(), sorted.end());
        return sorted;
    }

private:
    static constexpr std::uint64_t kReservoirSeed = 0x8f3f73b5cf1c9adeULL;

    static std::uint64_t mix64(std::uint64_t value)
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    std::size_t max_retained_samples_ = kDefaultMaxRetainedSamples;
    std::uint64_t sample_count_ = 0;
    long double sum_ = 0.0L;
    double min_ = std::numeric_limits<double>::infinity();
    double max_ = -std::numeric_limits<double>::infinity();
    std::vector<double> retained_samples_;
};

inline double percentile_from_sorted_samples(
    const std::vector<double>& sorted_samples,
    const double percentile_0_to_1)
{
    if (sorted_samples.empty()) {
        return 0.0;
    }
    if (sorted_samples.size() == 1) {
        return sorted_samples.front();
    }

    const double clamped = std::max(0.0, std::min(1.0, percentile_0_to_1));
    const double index = clamped * static_cast<double>(sorted_samples.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(index);
    const std::size_t upper = std::min(sorted_samples.size() - 1, lower + 1);
    const double fraction = index - static_cast<double>(lower);
    return sorted_samples[lower] +
        (sorted_samples[upper] - sorted_samples[lower]) * fraction;
}

}  // namespace orange

#endif  // ORANGE_BOUNDED_SAMPLE_STATISTICS_H
