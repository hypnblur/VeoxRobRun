#pragma once
#include <array>
#include <cstdint>
#include <memory>

namespace Base::ABCurves {

inline constexpr double kCommonRadiansPerCount = 0.0003509487083280618;

// Pure C++20 native port of the ABCurves B2-23 Continuous Planner
// (motor GRU + ProDMP 16-head decoder + choice selector + hazard/C2-brake).
// Loads `abcurves_continuous.bin` once into a shared read-only weight block;
// each ContinuousStream holds its own fixed-size workspace with zero per-step
// heap allocations.
class ContinuousStream {
public:
    struct Delta {
        double dx = 0.0;
        double dy = 0.0;
    };

    ContinuousStream();
    ~ContinuousStream();
    ContinuousStream(ContinuousStream&&) noexcept;
    ContinuousStream& operator=(ContinuousStream&&) noexcept;
    ContinuousStream(const ContinuousStream&) = delete;
    ContinuousStream& operator=(const ContinuousStream&) = delete;

    // Loads `abcurves_continuous.bin` from explicitPath (if provided) or
    // searches next to the executable / tools/ directory. Thread-safe.
    static bool EnsureLoaded(const char* explicitPath = nullptr);
    static bool Available();

    // Resets the 640 ms ring buffer, policy mode, and RNG streams.
    void Reset(std::uint64_t seed = 7);

    // Feeds a target error (in common angular counts, canonical Y-up) and
    // advances the causal 1 ms stream by `dtSec`, returning the summed smooth
    // displacement in common angular counts over that interval.
    Delta StepTowardError(double errX, double errY, double dtSec);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Base::ABCurves
