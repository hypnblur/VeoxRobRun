#pragma once
#include <Windows.h>

namespace Backdrop {

void Create(HINSTANCE instance);
void Destroy();
void Show();
void Hide();
bool IsVisible();
void SetResolution(int width, int height);
void SetFullscreen(bool fullscreen);
void SetMonitor(int index);

} // namespace Backdrop
