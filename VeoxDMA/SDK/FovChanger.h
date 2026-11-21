#pragma once
#include <cstdint>

namespace FovChanger {

// Runs one FOV hold pass. Call at worker loop rate (~20-60 Hz).
void Tick();

// Restores original captured FOV on all held vcams.
void Restore();

// Fully resets target tracking and holding state.
void Reset();

// Returns true if the changer is currently holding an overridden FOV.
bool IsHolding();

} // namespace FovChanger
