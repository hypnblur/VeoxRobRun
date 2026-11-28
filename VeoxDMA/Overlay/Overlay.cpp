#include "pch.h"
#include "Overlay.h"
#include "OverlayNotify.h"
#include "ConfigInstance.h"
#include "Misc/Diag.h"
#include "Input/InputManager.h"
#include "../Aim/AimController.h"
#include "ImGUI/imgui.h"
#include "ImGUI/imgui_impl_dx11.h"
#include "ImGUI/imgui_impl_win32.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <exception>
#include <thread>
#include <timeapi.h>
#include <utility>
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
HANDLE g_paceTimer = nullptr;
bool g_resizePending = false;
bool g_compositionDirty = true;
bool g_dwmAlpha = false;
std::atomic<float> g_currentFps{ 0.f };

std::vector<RECT> g_monitors;
int g_appliedMonitor = -1;

std::atomic<bool> g_menuOpen{ true };
bool g_clickThrough = true;
bool g_alwaysOnTop = true;
constexpr int kTransparencyHotkeyId = 1;
int g_registeredTransparencyVk = 0;
int g_requestedTransparencyVk = 0;
bool g_transparencyPressed = false;
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
	if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer)
		return;
	if (FAILED(g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv)))
		g_rtv = nullptr;
	backBuffer->Release();
}

void CleanupRenderTarget()
{
	if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

void CleanupSwapChain()
{
	CleanupRenderTarget();
	if (g_swapChain) {
		g_swapChain->Release();
		g_swapChain = nullptr;
	}
	if (g_context) {
		g_context->ClearState();
		g_context->Flush();
	}
}

bool CreateOverlaySwapChain(HWND hWnd)
{
	if (!g_device) return false;

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

	IDXGIFactory* factory = nullptr;
	hr = adapter->GetParent(IID_PPV_ARGS(&factory));
	adapter->Release();
	if (FAILED(hr) || !factory)
		return false;

	factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);

	RECT rc{};
	GetClientRect(hWnd, &rc);
	const UINT width = (rc.right > rc.left) ? static_cast<UINT>(rc.right - rc.left) : 0;
	const UINT height = (rc.bottom > rc.top) ? static_cast<UINT>(rc.bottom - rc.top) : 0;

	// Both modes use DWM composition. Transparency changes the clear color
	// and alpha without replacing or rebinding the window's swap chain.
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

	hr = factory->CreateSwapChain(g_device, &sd, &g_swapChain);
	factory->Release();
	if (FAILED(hr) || !g_swapChain) {
		VeoxDiag("Overlay swap chain creation failed (0x%08lX)", static_cast<unsigned long>(hr));
		return false;
	}

	CreateRenderTarget();
	if (!g_rtv) {
		VeoxDiag("Overlay swap chain had no render target");
		CleanupSwapChain();
		return false;
	}
	return true;
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

	return CreateOverlaySwapChain(hWnd);
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

void ApplyOverlayWindowChrome(HWND hwnd, const ChromeState& wanted)
{
	LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
	if (wanted.clickThrough) ex |= WS_EX_TRANSPARENT; else ex &= ~WS_EX_TRANSPARENT;
	ex |= WS_EX_APPWINDOW;
	ex &= ~WS_EX_TOOLWINDOW;
	SetWindowLong(hwnd, GWL_EXSTYLE, ex);
	SetWindowPos(hwnd, wanted.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void SyncOverlayWindowChrome(HWND hwnd)
{
	const bool clickThrough = g_clickThrough && !g_menuOpen.load(std::memory_order_relaxed);
	const ChromeState wanted{ ConfigInstance.Overlay.Transparent, clickThrough, g_alwaysOnTop, true };
	const bool modeChanged = wanted.transparent != g_appliedChrome.transparent;
	const bool refreshComposition = std::exchange(g_compositionDirty, false);
	if (refreshComposition) {
		const MARGINS margins{ -1, -1, -1, -1 };
		g_dwmAlpha = SUCCEEDED(DwmExtendFrameIntoClientArea(hwnd, &margins));
	}
	if (refreshComposition || (!g_dwmAlpha && modeChanged)) {
		// Keep the layered window and the DWM frame in both modes. The color
		// key is only needed on systems where extended DWM alpha is unavailable.
		SetLayeredWindowAttributes(hwnd, 0, 255, !g_dwmAlpha && wanted.transparent ? LWA_COLORKEY : LWA_ALPHA);
	}
	if (g_swapChain && modeChanged)
		VeoxDiag("Overlay transparency %s (%s)", wanted.transparent ? "on" : "off", Overlay::PresentModeName());

	if (!g_appliedChrome.valid ||
		g_appliedChrome.clickThrough != wanted.clickThrough ||
		g_appliedChrome.alwaysOnTop != wanted.alwaysOnTop)
		ApplyOverlayWindowChrome(hwnd, wanted);
	g_appliedChrome = wanted;
}

void ApplyPendingResize(HWND hwnd)
{
	if (!g_resizePending)
		return;
	g_resizePending = false;
	if (!g_swapChain || IsIconic(hwnd))
		return;

	RECT rect{};
	if (!GetClientRect(hwnd, &rect) || rect.right <= 0 || rect.bottom <= 0)
		return;
	const auto width = static_cast<UINT>(rect.right);
	const auto height = static_cast<UINT>(rect.bottom);
	DXGI_SWAP_CHAIN_DESC desc{};
	if (g_rtv && SUCCEEDED(g_swapChain->GetDesc(&desc)) &&
		desc.BufferDesc.Width == width && desc.BufferDesc.Height == height)
		return;

	g_context->OMSetRenderTargets(0, nullptr, nullptr);
	CleanupRenderTarget();
	const HRESULT result = g_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
	if (FAILED(result))
		VeoxDiag("Overlay resize failed (0x%08lX)", static_cast<unsigned long>(result));
	CreateRenderTarget();
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

struct KeyEdgeState {
	int lastVk = 0;
	bool wasDown = false;
};

// The overlay window and menu run on the same thread.
bool g_hotkeyCapture = false;
int g_capturedHotkey = Hotkey::Off;
int g_waitingForHotkeyRelease = Hotkey::Off;

int CaptureVirtualKey(WPARAM key, LPARAM data)
{
	const bool extended = (data & (1 << 24)) != 0;
	if (key == VK_CONTROL) return extended ? VK_RCONTROL : VK_LCONTROL;
	if (key == VK_MENU) return extended ? VK_RMENU : VK_LMENU;
	if (key == VK_SHIFT) {
		const auto scan = static_cast<UINT>((data >> 16) & 0xff);
		return MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? VK_RSHIFT : VK_LSHIFT;
	}
	return static_cast<int>(key);
}

bool KeyJustPressed(int vk, KeyEdgeState& state, bool pollHost = true)
{
	if (vk <= 0 || vk > 255) {
		state.lastVk = 0;
		state.wasDown = false;
		return false;
	}
	if (state.lastVk != vk) {
		state.lastVk = vk;
		state.wasDown = false;
	}
	const bool dmaDown = Keyboard::IsReady() && Keyboard::IsKeyDown(static_cast<uint32_t>(vk));
	const bool hostDown = pollHost && (GetAsyncKeyState(vk) & 0x8000) != 0;
	const bool down = dmaDown || hostDown;
	const bool pressed = down && !state.wasDown;
	state.wasDown = down;
	return pressed;
}

void SyncTransparencyHotkey(HWND hwnd, int vk)
{
	if (g_requestedTransparencyVk == vk)
		return;
	g_requestedTransparencyVk = vk;
	g_transparencyPressed = false;
	if (g_registeredTransparencyVk)
		UnregisterHotKey(hwnd, kTransparencyHotkeyId);
	g_registeredTransparencyVk = 0;
	if (vk && RegisterHotKey(hwnd, kTransparencyHotkeyId, MOD_NOREPEAT, vk))
		g_registeredTransparencyVk = vk;
	else if (vk)
		VeoxDiag("Overlay transparency hotkey registration unavailable. Polling key %d.", vk);
}

void DrawFps()
{
	if (!ConfigInstance.Overlay.FPSEnabled) return;
	char buf[32];
	std::snprintf(buf, sizeof(buf), "FPS: %.0f", ImGui::GetIO().Framerate);
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	ImFont* font = ImGui::GetFont();
	const float size = ImGui::GetFontSize();
	const ImU32 outline = IM_COL32(26, 28, 32, 245);
	for (int dy = -1; dy <= 1; ++dy)
		for (int dx = -1; dx <= 1; ++dx)
			if (dx || dy)
				dl->AddText(font, size, ImVec2(10.f + dx, 10.f + dy), outline, buf);
	dl->AddText(font, size, ImVec2(10.f, 10.f), IM_COL32(0, 255, 0, 255), buf);
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
		g_menuOpen ? "open" : "closed", g_appliedChrome.transparent ? "on" : "off",
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

bool MenuOpen() { return g_menuOpen.load(std::memory_order_acquire); }

void BeginHotkeyCapture()
{
	g_capturedHotkey = Hotkey::Off;
	g_hotkeyCapture = true;
}

bool HotkeyCaptureActive() { return g_hotkeyCapture; }
int TakeCapturedHotkey() { return std::exchange(g_capturedHotkey, Hotkey::Off); }

void EndHotkeyCapture(int binding)
{
	g_hotkeyCapture = false;
	g_waitingForHotkeyRelease = binding != Hotkey::Off ? binding : g_capturedHotkey;
	g_capturedHotkey = Hotkey::Off;
}

void SetMenuOpen(bool open)
{
	if (!open && g_hotkeyCapture) EndHotkeyCapture();
	g_menuOpen.store(open, std::memory_order_release);
	g_appliedChrome.valid = false; // click-through depends on menu state
}

void ToggleMenu() { SetMenuOpen(!g_menuOpen.load(std::memory_order_relaxed)); }

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

bool Transparent() { return ConfigInstance.Overlay.Transparent; }

void SetTransparent(bool on)
{
	ConfigInstance.Overlay.Transparent = on;
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
	return Transparent() ? "DISCARD(transparent)" : "DISCARD";
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
	const bool keyboardMessage = message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
		message == WM_KEYUP || message == WM_SYSKEYUP;
	if (g_hotkeyCapture) {
		if (message == WM_KILLFOCUS) {
			Overlay::EndHotkeyCapture();
		} else if (keyboardMessage) {
			const bool fresh = ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
				(lParam & (1LL << 30)) == 0) || (message == WM_KEYUP && wParam == VK_SNAPSHOT);
			const int key = CaptureVirtualKey(wParam, lParam);
			if (fresh && !g_capturedHotkey && Hotkey::IsKeyboard(key)) g_capturedHotkey = key;
			return 0;
		} else if (message == WM_CHAR || message == WM_SYSCHAR) {
			return 0;
		} else if (message == WM_HOTKEY && wParam == kTransparencyHotkeyId) {
			if (!g_capturedHotkey) g_capturedHotkey = HIWORD(lParam);
			return 0;
		}
	}
	if (keyboardMessage && g_waitingForHotkeyRelease != Hotkey::Off &&
		CaptureVirtualKey(wParam, lParam) == g_waitingForHotkeyRelease)
		return 0;
	if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam)) return true;
	switch (message)
	{
	case WM_HOTKEY:
		if (wParam == kTransparencyHotkeyId && g_registeredTransparencyVk &&
			HIWORD(lParam) == g_registeredTransparencyVk) {
			g_transparencyPressed = !g_transparencyPressed;
			return 0;
		}
		break;
	case WM_SYSKEYDOWN:
	case WM_SYSKEYUP:
		if (wParam == VK_F10 && g_requestedTransparencyVk == VK_F10)
			return 0; // F10 is an overlay command here. Skip the system menu.
		break;
	case WM_DWMCOMPOSITIONCHANGED:
		g_compositionDirty = true;
		return 0;
	case WM_SIZE:
		g_resizePending = wParam != SIZE_MINIMIZED;
		return 0;
	case WM_DISPLAYCHANGE:
		DetectMonitors();
		return 0;
	case WM_CLOSE:
		DestroyWindow(hWnd);
		return 0;
	case WM_DESTROY:
		SyncTransparencyHotkey(hWnd, 0);
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
	g_clickThrough = ConfigInstance.Overlay.ClickThroughWhenClosed;
	g_alwaysOnTop = ConfigInstance.Overlay.AlwaysOnTop;
	g_frameCap = std::clamp(ConfigInstance.Overlay.FrameCap, 0, 1000);
	g_appliedChrome.valid = false;
	g_compositionDirty = true;

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
	HWND hWnd = CreateWindowEx(WS_EX_APPWINDOW | WS_EX_LAYERED, windowClass, title, WS_POPUP,
		0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
		NULL, NULL, instance, NULL);
	if (!hWnd)
	{
		MessageBoxW(nullptr, L"Could not create the overlay window.",
			L"VeoxDMA - Startup Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
		UnregisterClass(windowClass, instance);
		return -1;
	}

	ApplyOverlayMonitor(hWnd);
	SyncOverlayWindowChrome(hWnd);
	if (!CreateDeviceD3D(hWnd))
	{
		MessageBoxW(nullptr, L"Could not create the Direct3D 11 device.",
			L"VeoxDMA - Startup Failed", MB_OK | MB_ICONERROR | MB_TOPMOST);
		CleanupDeviceD3D();
		DestroyWindow(hWnd);
		UnregisterClass(windowClass, instance);
		return -1;
	}

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

	KeyEdgeState menuEdge{};
	KeyEdgeState transparencyEdge{};
	KeyEdgeState exitEdge{};
	KeyEdgeState panicEdge{};
	KeyEdgeState reattachEdge{};
#ifdef _DEBUG
	KeyEdgeState debugEdge{};
#endif

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

		const auto& config = ConfigInstance.Overlay;
		// Keep actions paused through the release frame and still sample
		// their edges so an already-held key cannot fire when capture ends.
		const bool suppressHotkeys = g_hotkeyCapture || g_waitingForHotkeyRelease != Hotkey::Off ||
			(MenuOpen() && GetForegroundWindow() == hWnd && ImGui::GetIO().WantTextInput);
		if (g_waitingForHotkeyRelease != Hotkey::Off && !Aim::IsKeyHeld(g_waitingForHotkeyRelease))
			g_waitingForHotkeyRelease = Hotkey::Off;
		const int exitVk = config.ExitBinding;
		if (g_requestExit.exchange(false) || (KeyJustPressed(exitVk, exitEdge) && !suppressHotkeys))
		{
			DestroyWindow(hWnd);
			continue;
		}

		const int configuredTransparencyVk = config.TransparencyBinding;
		const int transparencyVk = configuredTransparencyVk != exitVk ? configuredTransparencyVk : 0;
		SyncTransparencyHotkey(hWnd, transparencyVk == VK_F10 && !suppressHotkeys ? VK_F10 : Hotkey::Off);
		const bool polledTransparency = KeyJustPressed(transparencyVk, transparencyEdge, !g_registeredTransparencyVk);
		if ((std::exchange(g_transparencyPressed, false) || polledTransparency) && !suppressHotkeys)
			SetTransparent(!Transparent());

		// On a shared binding, handle only the highest-priority action.
		const int menuVk = config.MenuBinding;
		if (KeyJustPressed(menuVk, menuEdge) && !suppressHotkeys && menuVk != transparencyVk) {
			if (ConfigInstance.Overlay.PanicMode) {
				TogglePanic();
				SetMenuOpen(true);
			} else {
				ToggleMenu();
			}
		}

		const int panicVk = config.PanicBinding;
		if (KeyJustPressed(panicVk, panicEdge) && !suppressHotkeys && panicVk != transparencyVk && panicVk != menuVk)
			TogglePanic();

		const int reattachVk = config.ReattachBinding;
		if (KeyJustPressed(reattachVk, reattachEdge) && !suppressHotkeys &&
			reattachVk != transparencyVk && reattachVk != menuVk && reattachVk != panicVk)
			RequestReattach();

#ifdef _DEBUG
		if (KeyJustPressed(VK_DELETE, debugEdge) && !suppressHotkeys)
			g_debugVisible = !g_debugVisible;
#endif

		ApplyOverlayMonitor(hWnd);
		SyncOverlayWindowChrome(hWnd);
		ApplyPendingResize(hWnd);

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

		if (g_rtv && g_context && g_swapChain)
		{
			const D2D1::ColorF& bg = ConfigInstance.Overlay.BackgroundColour;
			const bool transparent = g_appliedChrome.transparent;
			const float clear[4] = {
				transparent ? 0.f : bg.r,
				transparent ? 0.f : bg.g,
				transparent ? 0.f : bg.b,
				transparent ? 0.f : 1.f
			};
			g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
			g_context->ClearRenderTargetView(g_rtv, clear);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			g_swapChain->Present(0, 0);
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
