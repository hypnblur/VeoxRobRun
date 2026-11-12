#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <functional>
#include <string>

// External overlay host. Owns the popup window, the D3D11 device and the frame
// loop. Game-specific work (DMA reads, ESP, menu) is injected through the
// callbacks so this file stays reusable across projects.
namespace Overlay {

struct Callbacks {
	std::function<void()> init;      // after the ImGui context + device exist
	std::function<void()> tick;      // per frame, before NewFrame
	std::function<void()> draw;      // per frame, between NewFrame and Render
	std::function<void()> shutdown;  // before ImGui/device teardown
	std::function<std::string()> status; // one-line attach state for the debug view
};

// Creates the overlay window and runs the frame loop until the window closes.
int Run(HINSTANCE instance, const wchar_t* title, const wchar_t* windowClass,
	const Callbacks& callbacks);

// ---- live state -----------------------------------------------------------
bool MenuOpen();
void SetMenuOpen(bool open);
void ToggleMenu();

bool Transparent();
void SetTransparent(bool on);

bool ClickThrough();
void SetClickThrough(bool on);

bool AlwaysOnTop();
void SetAlwaysOnTop(bool on);

int FrameCap();
void SetFrameCap(int fps);

int MonitorCount();
int MonitorIndex();
void SetMonitor(int index);

struct MonitorInfo {
	int width;
	int height;
	bool primary;
};
bool GetMonitorInfo(int index, MonitorInfo& out);

void RequestExit();

ID3D11Device* GetDevice();
ID3D11DeviceContext* GetDeviceContext();

// Corner toast. Colour is IM_COL32-formatted; seconds is the lifetime.
void Notify(const char* msg, unsigned int colour = 0xFFFFFFFFu, float seconds = 2.f);

} // namespace Overlay
