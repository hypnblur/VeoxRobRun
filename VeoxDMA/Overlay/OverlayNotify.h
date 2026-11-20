#pragma once
#include <string>

// On-screen overlay extras: a small draggable feature-status window plus timed
// corner toasts. Pure drawing + ImGui only, safe to call once per frame.
namespace OverlayNotify {

void Show(const char* msg, unsigned int col = 0xFFFFFFFFu, float seconds = 2.f);
void Draw();

} // namespace OverlayNotify
