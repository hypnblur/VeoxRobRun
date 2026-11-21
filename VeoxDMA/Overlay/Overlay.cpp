#include "pch.h"
#include "Overlay.h"
#include "OverlayNotify.h"
#include "ConfigInstance.h"
#include "Input/InputManager.h"
#include "ImGUI/imgui.h"
#include "ImGUI/imgui_impl_dx11.h"
#include "ImGUI/imgui_impl_win32.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <dwmapi.h>
#include <exception>
#include <thread>
#include <timeapi.h>
#include <vector>
#include <intrin.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

extern void RequestReattach();

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HANDLE g_frameLatencyWaitable = nullptr;
HANDLE g_paceTimer = nullptr;
UINT g_swapChainFlags = 0;
bool g_flipModel = false;
bool g_tearingSupported = false;
bool g_swapChainTransparent = false;
std::atomic<float> g_currentFps{ 0.f };

std::vector<RECT> g_monitors;
int g_appliedMonitor = -1;

bool g_menuOpen = true;
bool g_transparent = false;
bool g_clickThrough = true;
bool g_alwaysOnTop = true;
int g_frameCap = 240;
std::atomic<bool> g_requestExit{ false };

#ifdef _DEBUG
bool g_debugVisible = false;
#endif

std::function<std::string()> g_status;

struct ChromeState {
	bool transparent = false;
	bool clickThrough = false;
	bool alwaysOnTop = false;
	bool valid = false;
};
ChromeState g_appliedChrome{};

// ---- window/device helpers ------------------------------------------------

void CreateRenderTarget()
{
	if (!g_swapChain || !g_device) return;
	ID3D11Texture2D* backBuffer = nullptr;
	g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
	if (!backBuffer) return;
	g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
	backBuffer->Release();
}

void CleanupRenderTarget()
{
	if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

void CleanupSwapChain()
{
	if (g_context) {
		g_context->OMSetRenderTargets(0, nullptr, nullptr);
		g_context->Flush();
	}
	CleanupRenderTarget();
	if (g_frameLatencyWaitable) {
		CloseHandle(g_frameLatencyWaitable);
		g_frameLatencyWaitable = nullptr;
	}
	if (g_swapChain) {
		g_swapChain->Release();
		g_swapChain = nullptr;
	}
	g_swapChainFlags = 0;
	g_flipModel = false;
	g_tearingSupported = false;
}

bool CreateSwapChainForMode(HWND hWnd, bool transparent)
{
	if (!g_device) return false;
	CleanupSwapChain();

	IDXGIDevice1* dxgiDevice = nullptr;
	if (FAILED(g_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || !dxgiDevice)
		return false;
	// Cap the DXGI queue to 1 frame in flight (default is 3) in every mode so
	// rendered overlays never pile up behind DWM or GPU backpressure.
	dxgiDevice->SetMaximumFrameLatency(1);

	IDXGIAdapter* adapter = nullptr;
	HRESULT hr = dxgiDevice->GetAdapter(&adapter);
	dxgiDevice->Release();
	if (FAILED(hr) || !adapter)
		return false;

	IDXGIFactory2* factory2 = nullptr;
	hr = adapter->GetParent(IID_PPV_ARGS(&factory2));
	adapter->Release();
	if (FAILED(hr) || !factory2)
		return false;

	factory2->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);

	RECT rc{};
	GetClientRect(hWnd, &rc);
	const UINT width = (rc.right > rc.left) ? static_cast<UINT>(rc.right - rc.left) : 0;
	const UINT height = (rc.bottom > rc.top) ? static_cast<UINT>(rc.bottom - rc.top) : 0;

	bool created = false;
	if (!transparent) {
		// Opaque overlay (e.g. DMA fuser): use Flip-Discard + Waitable Frame
		// Latency + Allow Tearing so Windows can promote the overlay to Direct
		// / Independent Flip and scan out immediately without DWM copy lag.
		BOOL allowTearing = FALSE;
		IDXGIFactory5* factory5 = nullptr;
		if (SUCCEEDED(factory2->QueryInterface(IID_PPV_ARGS(&factory5))) && factory5) {
			if (FAILED(factory5->CheckFeatureSupport(
				DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing)))) {
				allowTearing = FALSE;
			}
			factory5->Release();
		}

		DXGI_SWAP_CHAIN_DESC1 sd1{};
		sd1.Width = width;
		sd1.Height = height;
		sd1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd1.SampleDesc.Count = 1;
		sd1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd1.BufferCount = 2;
		sd1.Scaling = DXGI_SCALING_STRETCH;
		sd1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		sd1.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
		if (allowTearing)
			sd1.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

		IDXGISwapChain1* sc1 = nullptr;
		hr = factory2->CreateSwapChainForHwnd(g_device, hWnd, &sd1, nullptr, nullptr, &sc1);
		if (FAILED(hr) && allowTearing) {
			allowTearing = FALSE;
			sd1.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
			hr = factory2->CreateSwapChainForHwnd(g_device, hWnd, &sd1, nullptr, nullptr, &sc1);
		}
		if (SUCCEEDED(hr) && sc1) {
			g_swapChain = sc1;
			g_swapChainFlags = sd1.Flags;
			g_flipModel = true;
			g_tearingSupported = (allowTearing != FALSE);
			IDXGISwapChain2* sc2 = nullptr;
			if (SUCCEEDED(sc1->QueryInterface(IID_PPV_ARGS(&sc2))) && sc2) {
				sc2->SetMaximumFrameLatency(1);
				g_frameLatencyWaitable = sc2->GetFrameLatencyWaitableObject();
				sc2->Release();
			}
			created = true;
		}
	}

	if (!created) {
		// Transparent layered mode (or fallback on older/WARP drivers): keeps
		// per-pixel DWM alpha via DwmExtendFrameIntoClientArea while still
		// honoring SetMaximumFrameLatency(1) above.
		DXGI_SWAP_CHAIN_DESC sd{};
		sd.BufferCount = 2;
		sd.BufferDesc.Width = width;
		sd.BufferDesc.Height = height;
		sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow = hWnd;
		sd.SampleDesc.Count = 1;
		sd.Windowed = TRUE;
		sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

		hr = factory2->CreateSwapChain(g_device, &sd, &g_swapChain);
		if (SUCCEEDED(hr) && g_swapChain) {
			g_swapChainFlags = sd.Flags;
			g_flipModel = false;
			g_tearingSupported = false;
			created = true;
		}
	}

	factory2->Release();
	if (!created) return false;

	g_swapChainTransparent = transparent;
	CreateRenderTarget();
	return g_rtv != nullptr;
}

bool CreateDeviceD3D(HWND hWnd)
{
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL level;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		levels, 2, D3D11_SDK_VERSION, &g_device, &level, &g_context);
	if (hr == DXGI_ERROR_UNSUPPORTED)
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
			levels, 2, D3D11_SDK_VERSION, &g_device, &level, &g_context);
	if (FAILED(hr)) return false;

	return CreateSwapChainForMode(hWnd, g_transparent);
}

void CleanupDeviceD3D()
{
	CleanupSwapChain();
	if (g_context) { g_context->Release(); g_context = nullptr; }
	if (g_device) { g_device->Release(); g_device = nullptr; }
}

BOOL CALLBACK AddMonitor(HMONITOR, HDC, LPRECT rect, LPARAM list)
{
	reinterpret_cast<std::vector<RECT>*>(list)->push_back(*rect);
	return TRUE;
}

void DetectMonitors()
{
	g_monitors.clear();
	EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&g_monitors));
	if (g_monitors.empty())
		g_monitors.push_back({ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) });
	g_appliedMonitor = -1;
}

void ApplyOverlayMonitor(HWND hWnd)
{
	int& index = ConfigInstance.Overlay.MonitorIndex;
	if (g_monitors.empty()) return;
	index = std::clamp(index, 0, (int)g_monitors.size() - 1);
	if (index == g_appliedMonitor) return;
	const RECT& r = g_monitors[index];
	SetWindowPos(hWnd, g_alwaysOnTop ? HWND_TOPMOST : HWND_TOP,
		r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
	g_appliedMonitor = index;
}

void ApplyOverlayPerPixelTransparency(HWND hwnd, bool transparent)
{
	BOOL composition = FALSE;
	if (SUCCEEDED(DwmIsCompositionEnabled(&composition)) && composition) {
		MARGINS margins{};
		if (transparent) {
			margins.cxLeftWidth = margins.cxRightWidth = margins.cyTopHeight = margins.cyBottomHeight = -1;
			// A WS_EX_LAYERED window is never drawn until it has layered
			// attributes; without this the overlay (and menu) stays invisible.
			// Opaque layer alpha keeps DWM's per-pixel alpha from the swap chain.
			SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
		}
		DwmExtendFrameIntoClientArea(hwnd, &margins);
	}
	else if (transparent) {
		SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
	}
}

void SyncOverlayWindowChrome(HWND hwnd)
{
	const bool clickThrough = g_clickThrough && !g_menuOpen;
	const ChromeState wanted{ g_transparent, clickThrough, g_alwaysOnTop, true };
	if (g_appliedChrome.valid
		&& g_appliedChrome.transparent == wanted.transparent
		&& g_appliedChrome.clickThrough == wanted.clickThrough
		&& g_appliedChrome.alwaysOnTop == wanted.alwaysOnTop)
		return;

	LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
	if (wanted.transparent) ex |= WS_EX_LAYERED; else ex &= ~WS_EX_LAYERED;
	if (wanted.clickThrough) ex |= WS_EX_TRANSPARENT; else ex &= ~WS_EX_TRANSPARENT;
	ex |= WS_EX_APPWINDOW;
	ex &= ~WS_EX_TOOLWINDOW;
	SetWindowLong(hwnd, GWL_EXSTYLE, ex);
	ApplyOverlayPerPixelTransparency(hwnd, wanted.transparent);
	SetWindowPos(hwnd, wanted.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	if (g_device && wanted.transparent != g_swapChainTransparent)
		CreateSwapChainForMode(hwnd, wanted.transparent);
	g_appliedChrome = wanted;
}

void WaitForFrameCap()
{
	using clock = std::chrono::steady_clock;
	static clock::time_point next{};
	static int lastCap = -1;

	const int cap = g_frameCap;
	const auto now = clock::now();

	if (cap <= 0)
	{
		lastCap = cap;
		next = now;
		return;
	}

	const auto interval = std::chrono::nanoseconds(1'000'000'000LL / cap);
	if (lastCap != cap || next.time_since_epoch().count() == 0 ||
		now - next > interval)
	{
		// First frame after a cap change, or after a long stall (menu drag,
		// display change): start a fresh schedule instead of firing a burst
		// of catch-up frames.
		lastCap = cap;
		next = now + interval;
		return;
	}

	next += interval;
	const auto remain = next - now;
	if (remain <= clock::duration::zero())
		return;

	// Use a high-resolution waitable timer (~0.5 ms granularity) for the bulk
	// wait so high-refresh caps (240/360/480 Hz) don't suffer Sleep(1) jitter,
	// then spin the final 150 us for exact frame alignment.
	const auto spin = std::chrono::microseconds(150);
	if (remain > spin) {
		const auto sleepNs = std::chrono::duration_cast<std::chrono::nanoseconds>(remain - spin).count();
		if (g_paceTimer && sleepNs >= 100'000LL) {
			LARGE_INTEGER due{};
			due.QuadPart = -(sleepNs / 100LL); // 100 ns units, relative
			if (SetWaitableTimer(g_paceTimer, &due, 0, nullptr, nullptr, FALSE)) {
				WaitForSingleObject(g_paceTimer, INFINITE);
			} else {
				std::this_thread::sleep_for(remain - spin);
			}
		} else {
			std::this_thread::sleep_for(remain - spin);
		}
	}
	while (clock::now() < next) {
		_mm_pause();
	}
}

bool KeyJustPressed(int vk)
{
	static bool previous[256] = {};
	if (vk < 0 || vk > 255) return false;
	const bool dmaDown = Keyboard::IsReady() && Keyboard::IsKeyDown(static_cast<uint32_t>(vk));
	const bool hostDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
	const bool down = dmaDown || hostDown;
	const bool pressed = down && !previous[vk];
	previous[vk] = down;
	return pressed;
}

static const int kMenuKeyVk[] = {
	VK_INSERT, // 0: Insert
	VK_HOME,   // 1: Home
	VK_DELETE, // 2: Delete
	VK_END,    // 3: End
	VK_F11,    // 4: F11
	VK_F12,    // 5: F12
	VK_RSHIFT  // 6: Right Shift
};

static const int kPanicKeyVk[] = {
	0,         // 0: None
	VK_END,    // 1: End
	VK_F8,     // 2: F8
	VK_F9,     // 3: F9
	VK_F10,    // 4: F10
	VK_F11,    // 5: F11
	VK_F12     // 6: F12
};

static int GetMenuKeyVk()
{
	int idx = ConfigInstance.Overlay.MenuKey;
	if (idx >= 0 && idx < (int)(sizeof(kMenuKeyVk) / sizeof(kMenuKeyVk[0])))
		return kMenuKeyVk[idx];
	return VK_INSERT;
}

static int GetPanicKeyVk()
{
	int idx = ConfigInstance.Overlay.PanicKey;
	if (idx >= 0 && idx < (int)(sizeof(kPanicKeyVk) / sizeof(kPanicKeyVk[0])))
		return kPanicKeyVk[idx];
	return 0;
}

static const int kReattachKeyVk[] = {
	0,         // 0: Off
	VK_F1,     // 1: F1
	VK_F2,     // 2: F2
	VK_F3,     // 3: F3
	VK_F4,     // 4: F4
	VK_F5,     // 5: F5
	VK_F6,     // 6: F6
	VK_F7,     // 7: F7
	VK_F8,     // 8: F8
	VK_F9,     // 9: F9
	VK_F10,    // 10: F10
	VK_F11,    // 11: F11
	VK_F12,    // 12: F12
	VK_INSERT, // 13: Insert
	VK_HOME,   // 14: Home
	VK_END,    // 15: End
	VK_DELETE, // 16: Delete
	VK_PRIOR,  // 17: Page Up
	VK_NEXT    // 18: Page Down
};

static int GetReattachKeyVk()
{
	int idx = ConfigInstance.Overlay.ReattachKey;
	if (idx >= 0 && idx < (int)(sizeof(kReattachKeyVk) / sizeof(kReattachKeyVk[0])))
		return kReattachKeyVk[idx];
	return 0;
}

void DrawFps()
{
	if (!ConfigInstance.Overlay.FPSEnabled) return;
	char buf[32];
	std::snprintf(buf, sizeof(buf), "FPS: %.0f", ImGui::GetIO().Framerate);
	ImGui::GetBackgroundDrawList()->AddText(ImVec2(10.f, 10.f), IM_COL32(0, 255, 0, 255), buf);
}

void DrawDebugOverlay()
{
#ifdef _DEBUG
	if (!g_debugVisible) return;
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	ImFont* font = ImGui::GetFont();
	const float size = 16.f;
	const float lineH = 20.f;
	const ImU32 white = IM_COL32(255, 255, 255, 255);
	const ImU32 green = IM_COL32(120, 230, 150, 255);
	const ImU32 amber = IM_COL32(255, 200, 80, 255);
	const ImU32 bg = IM_COL32(16, 16, 20, 190);
	const ImGuiIO& io = ImGui::GetIO();

	char buf[256];
	float y = 24.f;
	const float x = 16.f;
	dl->AddRectFilled(ImVec2(x - 8.f, y - 8.f), ImVec2(x + 340.f, y + lineH * 7 + 8.f), bg, 8.f);
	auto line = [&](const char* text, ImU32 col) {
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
				if (dx || dy)
					dl->AddText(font, size, ImVec2(x + dx, y + dy), IM_COL32(0, 0, 0, 220), text);
		dl->AddText(font, size, ImVec2(x, y), col, text);
		y += lineH;
	};

	line("VeoxDMA  Overlay  Debug", amber);
	char capBuf[16];
	if (g_frameCap > 0) std::snprintf(capBuf, sizeof(capBuf), "%d", g_frameCap);
	else std::snprintf(capBuf, sizeof(capBuf), "off");
	std::snprintf(buf, sizeof(buf), "FPS: %.0f   Frame cap: %s", io.Framerate, capBuf);
	line(buf, white);
	std::snprintf(buf, sizeof(buf), "Display: %.0fx%.0f   Monitor: %d/%d",
		io.DisplaySize.x, io.DisplaySize.y, ConfigInstance.Overlay.MonitorIndex,
		(int)g_monitors.size());
	line(buf, white);
	std::snprintf(buf, sizeof(buf), "Menu: %s   Transparent: %s   Click-through: %s",
		g_menuOpen ? "open" : "closed", g_transparent ? "on" : "off",
		(g_clickThrough && !g_menuOpen) ? "on" : "off");
	line(buf, white);
	std::snprintf(buf, sizeof(buf), "Always on top: %s", g_alwaysOnTop ? "on" : "off");
	line(buf, white);
	if (g_status) {
		const std::string status = g_status();
		std::snprintf(buf, sizeof(buf), "Attach: %s", status.c_str());
		line(buf, green);
	}
	line("Press Delete to hide", amber);
#else
	(void)g_status;
#endif
}

} // namespace

// ---- public API -----------------------------------------------------------

namespace Overlay {

bool MenuOpen() { return g_menuOpen; }

void SetMenuOpen(bool open)
{
	g_menuOpen = open;
	g_appliedChrome.valid = false; // click-through depends on menu state
}

void ToggleMenu() { SetMenuOpen(!g_menuOpen); }

bool PanicActive()
{
	return ConfigInstance.Overlay.PanicMode;
}

void TogglePanic()
{
	ConfigInstance.Overlay.PanicMode = !ConfigInstance.Overlay.PanicMode;
	if (ConfigInstance.Overlay.PanicMode) {
		SetMenuOpen(false);
		Notify("PANIC: Visuals & Aim Suspended", 0xFF3333FFu, 2.5f);
	} else {
		Notify("PANIC: Resumed", 0x33FF33FFu, 2.0f);
	}
}

bool Transparent() { return g_transparent; }

void SetTransparent(bool on)
{
	g_transparent = on;
	ConfigInstance.Overlay.Transparent = on;
	g_appliedChrome.valid = false;
}

bool ClickThrough() { return g_clickThrough; }

void SetClickThrough(bool on)
{
	g_clickThrough = on;
	ConfigInstance.Overlay.ClickThroughWhenClosed = on;
	g_appliedChrome.valid = false;
}

bool AlwaysOnTop() { return g_alwaysOnTop; }

void SetAlwaysOnTop(bool on)
{
	g_alwaysOnTop = on;
	ConfigInstance.Overlay.AlwaysOnTop = on;
	g_appliedChrome.valid = false;
}

int FrameCap() { return g_frameCap; }

void SetFrameCap(int fps)
{
	g_frameCap = std::clamp(fps, 0, 1000);
	ConfigInstance.Overlay.FrameCap = g_frameCap;
}

const char* PresentModeName()
{
	if (g_flipModel)
		return g_tearingSupported ? "FLIP+TEAR" : "FLIP";
	return g_swapChainTransparent ? "DISCARD(transparent)" : "DISCARD";
}

float CurrentFps()
{
	return g_currentFps.load(std::memory_order_relaxed);
}

int MonitorCount() { return (int)g_monitors.size(); }
int MonitorIndex() { return ConfigInstance.Overlay.MonitorIndex; }

void SetMonitor(int index)
{
	if (g_monitors.empty()) return;
	ConfigInstance.Overlay.MonitorIndex = std::clamp(index, 0, (int)g_monitors.size() - 1);
	g_appliedMonitor = -1;
}

bool GetMonitorInfo(int index, MonitorInfo& out)
{
	if (index < 0 || index >= (int)g_monitors.size()) return false;
	const RECT& r = g_monitors[index];
	out.width = r.right - r.left;
	out.height = r.bottom - r.top;
	out.primary = (r.left == 0 && r.top == 0);
	return true;
}

void RequestExit() { g_requestExit.store(true); }

ID3D11Device* GetDevice() { return g_device; }
ID3D11DeviceContext* GetDeviceContext() { return g_context; }

void Notify(const char* msg, unsigned int colour, float seconds)
{
	OverlayNotify::Show(msg, colour, seconds);
}

} // namespace Overlay

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK OverlayWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam)) return true;
	switch (message)
	{
	case WM_SIZE:
		if (g_device && g_swapChain && wParam != SIZE_MINIMIZED) {
			const UINT w = LOWORD(lParam);
			const UINT h = HIWORD(lParam);
			if (w > 0 && h > 0) {
				if (g_context) g_context->OMSetRenderTargets(0, nullptr, nullptr);
				CleanupRenderTarget();
				g_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, g_swapChainFlags);
				CreateRenderTarget();
			}
		}
		return 0;
	case WM_DISPLAYCHANGE:
		DetectMonitors();
		ApplyOverlayMonitor(hWnd);
		return 0;
	case WM_CLOSE:
		DestroyWindow(hWnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(hWnd, message, wParam, lParam);
}

namespace Overlay {

int Run(HINSTANCE instance, const wchar_t* title, const wchar_t* windowClass,
	const Callbacks& callbacks)
{
	g_status = callbacks.status;
	g_transparent = ConfigInstance.Overlay.Transparent;
	g_clickThrough = ConfigInstance.Overlay.ClickThroughWhenClosed;
	g_alwaysOnTop = ConfigInstance.Overlay.AlwaysOnTop;
	g_frameCap = std::clamp(ConfigInstance.Overlay.FrameCap, 0, 1000);
	g_appliedChrome.valid = false;

	ImGui_ImplWin32_EnableDpiAwareness();

	WNDCLASSEX wc{};
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = OverlayWindowProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = windowClass;
	if (!RegisterClassEx(&wc))
	{
		MessageBoxW(nullptr, L"Could not register the overlay window class.",
			L"VeoxDMA - Startup Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
		return -1;
	}

	DetectMonitors();
	HWND hWnd = CreateWindowEx(WS_EX_APPWINDOW, windowClass, title, WS_POPUP,
		0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
		NULL, NULL, instance, NULL);
	if (!hWnd)
	{
		MessageBoxW(nullptr, L"Could not create the overlay window.",
			L"VeoxDMA - Startup Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
		UnregisterClass(windowClass, instance);
		return -1;
	}

	if (!CreateDeviceD3D(hWnd))
	{
		MessageBoxW(nullptr, L"Could not create the Direct3D 11 device.",
			L"VeoxDMA - Startup Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
		CleanupDeviceD3D();
		DestroyWindow(hWnd);
		UnregisterClass(windowClass, instance);
		return -1;
	}

	ApplyOverlayMonitor(hWnd);
	SyncOverlayWindowChrome(hWnd);
	ShowWindow(hWnd, SW_SHOWDEFAULT);
	UpdateWindow(hWnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);

	// Font/asset loading happens in init(); an exception there used to take the
	// process down with no message and no chance to clean up the window.
	if (callbacks.init)
	{
		try
		{
			callbacks.init();
		}
		catch (const std::exception& e)
		{
			MessageBoxA(nullptr, e.what(), "VeoxDMA - UI Init Failed",
				MB_OK | MB_ICONERROR | MB_TOPMOST);
			ImGui::DestroyContext();
			CleanupDeviceD3D();
			DestroyWindow(hWnd);
			UnregisterClass(windowClass, instance);
			return -1;
		}
		catch (...)
		{
			MessageBoxA(nullptr, "Unknown exception while initialising the UI.",
				"VeoxDMA - UI Init Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
			ImGui::DestroyContext();
			CleanupDeviceD3D();
			DestroyWindow(hWnd);
			UnregisterClass(windowClass, instance);
			return -1;
		}
	}
	ImGui_ImplWin32_Init(hWnd);
	ImGui_ImplDX11_Init(g_device, g_context);

	const bool timerPeriodSet = timeBeginPeriod(1) == TIMERR_NOERROR;
	g_paceTimer = CreateWaitableTimerExW(
		nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

	MSG msg{};
	while (msg.message != WM_QUIT)
	{
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
				break;
		}
		if (msg.message == WM_QUIT)
			break;

		if (g_requestExit.exchange(false))
		{
			DestroyWindow(hWnd);
			continue;
		}

		const int menuVk = GetMenuKeyVk();
		if (KeyJustPressed(menuVk)) {
			if (ConfigInstance.Overlay.PanicMode) {
				TogglePanic();
				SetMenuOpen(true);
			} else {
				ToggleMenu();
			}
		}

		const int panicVk = GetPanicKeyVk();
		if (panicVk > 0 && KeyJustPressed(panicVk))
			TogglePanic();

		const int reattachVk = GetReattachKeyVk();
		if (reattachVk > 0 && KeyJustPressed(reattachVk))
			RequestReattach();

#ifdef _DEBUG
		if (KeyJustPressed(VK_DELETE))
			g_debugVisible = !g_debugVisible;
#endif

		ApplyOverlayMonitor(hWnd);
		SyncOverlayWindowChrome(hWnd);

		// Wait until the previous frame's slot is free BEFORE sampling the
		// camera and building draw lists, so projected positions reflect the
		// latest camera at the instant of submission.
		if (g_frameLatencyWaitable)
			WaitForSingleObjectEx(g_frameLatencyWaitable, 100, TRUE);

		if (callbacks.tick) callbacks.tick();

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		g_currentFps.store(ImGui::GetIO().Framerate, std::memory_order_relaxed);

		if (!ConfigInstance.Overlay.PanicMode)
		{
			DrawFps();
			if (callbacks.draw) callbacks.draw();
			OverlayNotify::Draw();
			DrawDebugOverlay();
		}
		else
		{
			OverlayNotify::Draw();
		}

		ImGui::Render();

		if (g_rtv && g_swapChain)
		{
			const D2D1::ColorF& bg = ConfigInstance.Overlay.BackgroundColour;
			const float clear[4] = { bg.r, bg.g, bg.b, g_transparent ? 0.f : 1.f };
			g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
			g_context->ClearRenderTargetView(g_rtv, clear);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			const UINT presentFlags = (g_flipModel && g_tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
			g_swapChain->Present(0, presentFlags);
		}
		WaitForFrameCap();
	}

	if (callbacks.shutdown) callbacks.shutdown();
	if (g_paceTimer) { CloseHandle(g_paceTimer); g_paceTimer = nullptr; }
	if (timerPeriodSet) timeEndPeriod(1);
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	CleanupDeviceD3D();
	if (IsWindow(hWnd)) DestroyWindow(hWnd);
	UnregisterClass(windowClass, instance);
	return 0;
}

} // namespace Overlay
