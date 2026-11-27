#pragma once
#include <cstdint>
#include <memory>
#include "c/include/abc_online.h"

namespace Base::ABCurves {

// Zero-allocation-per-step C++ wrapper around the ABCurves fixed-point GRU Renderer.
// Takes smooth native mouse count deltas (Windows Y-down convention) and emits
// integer HID mouse reports with realistic 1 kHz cadence, bursts, and gaps.
class RendererStream {
public:
    struct Report {
        int dx = 0;
        int dy = 0;
    };

    RendererStream() = default;
    ~RendererStream() = default;
    RendererStream(RendererStream&&) noexcept = default;
    RendererStream& operator=(RendererStream&&) noexcept = default;
    RendererStream(const RendererStream&) = delete;
    RendererStream& operator=(const RendererStream&) = delete;

    // Initializes the shared embedded 44 KB model and default 256-tick profile
    // once per process. Safe to call from any thread.
    static bool EnsureInitialized();
    static bool Available();

    // Starts a fresh rendering stream cloned from the immutable 256-tick
    // template profile. Call when acquiring a new target or resuming aim.
    bool Begin(std::uint64_t seed);
    void Reset();
    bool Active() const { return active_ && state_ != nullptr; }

    // Feeds a smooth delta (in Windows Y-down native mouse counts) spanning
    // `dtSec` seconds, stepping the 1 kHz GRU renderer across whole 1 ms ticks
    // and returning the summed integer HID report clamped to [-limit, limit].
    Report Step(float smoothDx, float smoothDy, float dtSec, int limit = 127);

private:
    std::unique_ptr<abc_online_renderer_t> state_;
    bool active_ = false;
    float accumMs_ = 0.f;
    float pendingDx_ = 0.f;
    float pendingDy_ = 0.f;
};

} // namespace Base::ABCurves
