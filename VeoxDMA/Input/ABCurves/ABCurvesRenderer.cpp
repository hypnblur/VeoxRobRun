#include "pch.h"
#include "ABCurvesRenderer.h"
#include "ABCurvesRendererData.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace Base::ABCurves {

namespace {

struct SharedRendererTemplate {
    std::once_flag once;
    abc_online_model_t model{};
    abc_online_renderer_t profile{};
    bool ready = false;
};

SharedRendererTemplate& SharedTemplate()
{
    static SharedRendererTemplate s;
    return s;
}

void InitSharedTemplate()
{
    auto& s = SharedTemplate();
    if (abc_online_model_init(&s.model, kRendererBlob, kRendererBlobSize) != ABC_FIXED_OK)
        return;
    if (abc_online_reset(&s.profile, &s.model) != ABC_FIXED_OK)
        return;
    // profile_hardware in human_start.npz uses hardware Y-down; ABCurves
    // canonical coordinates are Y-up, so invert Y when observing the profile.
    for (std::size_t i = 0; i < kDefaultProfileTicks; ++i) {
        const std::int16_t dx = kDefaultProfile[i][0];
        const std::int16_t dy = static_cast<std::int16_t>(-kDefaultProfile[i][1]);
        if (abc_online_observe_raw(&s.profile, dx, dy) != ABC_FIXED_OK)
            return;
    }
    s.ready = true;
}

std::int32_t ToQ16(float value)
{
    if (!std::isfinite(value))
        return 0;
    const double scaled = std::clamp(static_cast<double>(value) * 65536.0, -2147483000.0, 2147483000.0);
    return static_cast<std::int32_t>(std::llround(scaled));
}

} // namespace

bool RendererStream::EnsureInitialized()
{
    auto& s = SharedTemplate();
    std::call_once(s.once, InitSharedTemplate);
    return s.ready;
}

bool RendererStream::Available()
{
    return EnsureInitialized();
}

void RendererStream::Reset()
{
    active_ = false;
    accumMs_ = 0.f;
    pendingDx_ = 0.f;
    pendingDy_ = 0.f;
}

bool RendererStream::Begin(std::uint64_t seed)
{
    Reset();
    if (!EnsureInitialized())
        return false;
    if (!state_)
        state_ = std::make_unique<abc_online_renderer_t>();
    *state_ = SharedTemplate().profile;
    if (abc_online_begin(state_.get(), seed ? seed : 2026ull) != ABC_FIXED_OK)
        return false;
    active_ = true;
    return true;
}

RendererStream::Report RendererStream::Step(float smoothDx, float smoothDy, float dtSec, int limit)
{
    Report out{};
    if (!std::isfinite(smoothDx) || !std::isfinite(smoothDy) || !std::isfinite(dtSec) || dtSec <= 0.f)
        return out;
    if (!Active() && !Begin(2026ull))
        return out;

    const float dtMs = std::clamp(dtSec * 1000.f, 0.05f, 50.f);
    accumMs_ += dtMs;
    pendingDx_ += smoothDx;
    pendingDy_ += smoothDy;

    const int ticks = static_cast<int>(std::floor(accumMs_));
    if (ticks <= 0)
        return out;

    const int runTicks = (std::min)(ticks, 16);
    const float perTickDx = pendingDx_ / static_cast<float>(runTicks);
    const float perTickDy = pendingDy_ / static_cast<float>(runTicks);
    accumMs_ -= static_cast<float>(ticks);
    pendingDx_ = 0.f;
    pendingDy_ = 0.f;

    // Canonical ABCurves coordinates are Y-up; negate Y on entry and exit.
    const std::int32_t qx = ToQ16(perTickDx);
    const std::int32_t qy = ToQ16(-perTickDy);

    int sumX = 0;
    int sumY = 0;
    for (int i = 0; i < runTicks; ++i) {
        abc_fixed_report_t rep{};
        if (abc_online_step(state_.get(), qx, qy, &rep) != ABC_FIXED_OK) {
            Reset();
            break;
        }
        sumX += static_cast<int>(rep.dx);
        sumY -= static_cast<int>(rep.dy);
    }

    out.dx = std::clamp(sumX, -limit, limit);
    out.dy = std::clamp(sumY, -limit, limit);
    return out;
}

} // namespace Base::ABCurves
