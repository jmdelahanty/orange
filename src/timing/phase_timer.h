#pragma once

// Wall-clock phases of one long operation, for diagnostics (the GUI recording
// finalize, 2026-10-15: 117 s after a 90-minute recording, longer than Citrus
// waited). A thread that owns a timer installs it with ScopedPhaseTimer; code
// below it calls MarkPhase(name) when a step ends, and the step's duration is
// the time since the previous mark. Without an installed timer MarkPhase does
// nothing, so instrumented library code costs nothing on other paths.

#include "json.hpp"

#include <chrono>
#include <string>

namespace orange::timing {

class PhaseTimer {
public:
    PhaseTimer() { Reset(); }

    void Reset()
    {
        started_ = last_ = std::chrono::steady_clock::now();
        phases_ = nlohmann::json::array();
    }

    // Ends the step that ran since the previous mark.
    void Mark(const std::string& name)
    {
        const auto now = std::chrono::steady_clock::now();
        phases_.push_back({{"phase", name}, {"ms", milliseconds(now - last_)},
                           {"at_ms", milliseconds(now - started_)}});
        last_ = now;
    }

    nlohmann::json ToJson() const
    {
        return {{"total_ms", milliseconds(last_ - started_)}, {"phases", phases_}};
    }

private:
    static double milliseconds(const std::chrono::steady_clock::duration d)
    {
        return std::chrono::duration<double, std::milli>(d).count();
    }

    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point last_;
    nlohmann::json phases_;
};

inline PhaseTimer*& CurrentPhaseTimer()
{
    static thread_local PhaseTimer* timer = nullptr;
    return timer;
}

inline void MarkPhase(const char* name)
{
    if (PhaseTimer* timer = CurrentPhaseTimer()) timer->Mark(name);
}

// Installs a timer for the current thread; restores the previous one on exit.
class ScopedPhaseTimer {
public:
    explicit ScopedPhaseTimer(PhaseTimer* timer) : previous_(CurrentPhaseTimer())
    {
        CurrentPhaseTimer() = timer;
    }
    ~ScopedPhaseTimer() { CurrentPhaseTimer() = previous_; }
    ScopedPhaseTimer(const ScopedPhaseTimer&) = delete;
    ScopedPhaseTimer& operator=(const ScopedPhaseTimer&) = delete;

private:
    PhaseTimer* previous_;
};

}  // namespace orange::timing
