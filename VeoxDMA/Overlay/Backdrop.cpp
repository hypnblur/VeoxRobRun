#include "pch.h"
#include "Backdrop.h"
#include "ConfigInstance.h"
#include <vector>

namespace {

HWND g_hwnd = nullptr;
HINSTANCE g_instance = nullptr;
bool g_visible = false;
bool g_fullscreen = true;
int g_customW = 0;
int g_customH = 0;
int g_monitorIndex = 0;

struct MonitorRect { RECT rc; bool primary; };
std::vector<MonitorRect> g_monitors;

BOOL CALLBACK CollectMonitors(HMONITOR, HDC, LPRECT rect, LPARAM lp)
{
	auto* list = reinterpret_cast<std::vector<MonitorRect>*>(lp);
	MonitorRect m;
	m.rc = *rect;
	m.primary = (rect->left == 0 && rect->top == 0);
	list->push_back(m);
	return TRUE;
}

void RefreshMonitors()
{
	g_monitors.clear();
	EnumDisplayMonitors(nullptr, nullptr, CollectMonitors,
		reinterpret_cast<LPARAM>(&g_monitors));
	if (g_monitors.empty()) {
		MonitorRect m;
		m.rc = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
		m.primary = true;
		g_monitors.push_back(m);
	}
}

LRESULT CALLBACK BackdropProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_CLOSE: return 0;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);
		HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
		FillRect(hdc, &ps.rcPaint, brush);
		DeleteObject(brush);
		EndPaint(hWnd, &ps);
		return 0;
	}
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

} // namespace

namespace Backdrop {

void Create(HINSTANCE instance)
{
	g_instance = instance;
	RefreshMonitors();

	WNDCLASSEX wc{};
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = BackdropProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wc.lpszClassName = L"VeoxBackdrop";
	RegisterClassEx(&wc);

	int idx = std::clamp(g_monitorIndex, 0, (int)g_monitors.size() - 1);
	const RECT& r = g_monitors[idx].rc;
	int w = g_customW > 0 ? g_customW : (r.right - r.left);
	int h = g_customH > 0 ? g_customH : (r.bottom - r.top);

	g_hwnd = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
		L"VeoxBackdrop", L"", WS_POPUP,
		r.left, r.top, w, h,
		NULL, NULL, instance, NULL);
}

void Destroy()
{
	if (g_hwnd) {
		DestroyWindow(g_hwnd);
		g_hwnd = nullptr;
	}
	if (g_instance)
		UnregisterClass(L"VeoxBackdrop", g_instance);
	g_visible = false;
}

void Show()
{
	if (!g_hwnd) return;
	ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
	g_visible = true;
}

void Hide()
{
	if (!g_hwnd) return;
	ShowWindow(g_hwnd, SW_HIDE);
	g_visible = false;
}

bool IsVisible() { return g_visible; }

void SetResolution(int width, int height)
{
	g_customW = width;
	g_customH = height;
	if (!g_hwnd) return;

	RefreshMonitors();
	int idx = std::clamp(g_monitorIndex, 0, (int)g_monitors.size() - 1);
	const RECT& r = g_monitors[idx].rc;
	int w = width > 0 ? width : (r.right - r.left);
	int h = height > 0 ? height : (r.bottom - r.top);
	SetWindowPos(g_hwnd, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE);
}

void SetFullscreen(bool fullscreen)
{
	g_fullscreen = fullscreen;
	if (fullscreen)
		SetResolution(0, 0);
}

void SetMonitor(int index)
{
	g_monitorIndex = index;
	if (!g_hwnd) return;
	RefreshMonitors();
	int idx = std::clamp(index, 0, (int)g_monitors.size() - 1);
	const RECT& r = g_monitors[idx].rc;
	int w = g_customW > 0 ? g_customW : (r.right - r.left);
	int h = g_customH > 0 ? g_customH : (r.bottom - r.top);
	SetWindowPos(g_hwnd, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE);
}

} // namespace Backdrop
