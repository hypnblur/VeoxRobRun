#include "pch.h"
#include "Overlay.h"
#include "OverlayNotify.h"
#include "ConfigInstance.h"
#include "ImGUI/imgui.h"
#include "ImGUI/imgui_impl_dx11.h"
#include "ImGUI/imgui_impl_win32.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <d3d11.h>
#include <dwmapi.h>
#include <thread>
#include <timeapi.h>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

std::vector<RECT> g_monitors;
int g_appliedMonitor = -1;

bool g_menuOpen = true;
bool g_transparent = false;
bool g_clickThrough = true;
bool g_alwaysOnTop = true;
int g_frameCap = 0;
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

bool CreateDeviceD3D(HWND hWnd)
{
	DXGI_SWAP_CHAIN_DESC sd{};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hWnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL level;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		levels, 2, D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &level, &g_context);
	if (hr == DXGI_ERROR_UNSUPPORTED)
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
			levels, 2, D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &level, &g_context);
	if (FAILED(hr)) return false;
	CreateRenderTarget();
	return true;
}

void CleanupDeviceD3D()
{
	CleanupRenderTarget();
	if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
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
	g_appliedChrome = wanted;
}

void WaitForFrameCap()
{
	using clock = std::chrono::steady_clock;
	static clock::time_point next = clock::now();
	const int cap = g_frameCap;
	if (cap <= 0) { next = clock::now(); return; }
	next += std::chrono::nanoseconds(1'000'000'000LL / cap);
	const auto now = clock::now();
	if (next > now) std::this_thread::sleep_for(next - now);
	else next = now;
}

bool KeyJustPressed(int vk)
{
	static bool previous[256] = {};
	if (vk < 0 || vk > 255) return false;
	const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
	const bool pressed = down && !previous[vk];
	previous[vk] = down;
	return pressed;
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
		if (g_device && wParam != SIZE_MINIMIZED) {
			CleanupRenderTarget();
			g_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
			CreateRenderTarget();
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

	if (callbacks.init) callbacks.init();
	ImGui_ImplWin32_Init(hWnd);
	ImGui_ImplDX11_Init(g_device, g_context);

	const bool timerPeriodSet = timeBeginPeriod(1) == TIMERR_NOERROR;

	MSG msg{};
	while (msg.message != WM_QUIT)
	{
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			continue;
		}

		if (g_requestExit.exchange(false))
		{
			DestroyWindow(hWnd);
			continue;
		}

		if (KeyJustPressed(VK_INSERT))
			ToggleMenu();
#ifdef _DEBUG
		if (KeyJustPressed(VK_DELETE))
			g_debugVisible = !g_debugVisible;
#endif

		ApplyOverlayMonitor(hWnd);
		SyncOverlayWindowChrome(hWnd);

		if (callbacks.tick) callbacks.tick();

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		DrawFps();
		if (callbacks.draw) callbacks.draw();
		OverlayNotify::Draw();
		DrawDebugOverlay();

		ImGui::Render();

		const D2D1::ColorF& bg = ConfigInstance.Overlay.BackgroundColour;
		const float clear[4] = { bg.r, bg.g, bg.b, g_transparent ? 0.f : 1.f };
		g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
		g_context->ClearRenderTargetView(g_rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_swapChain->Present(0, 0);
		WaitForFrameCap();
	}

	if (callbacks.shutdown) callbacks.shutdown();
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
