#include "pch.h"
#include "OverlayNotify.h"
#include "ConfigInstance.h"
#include "Overlay.h"
#include "Esp/EspDraw.h"
#include "ImGUI/imgui.h"
#include <chrono>
#include <mutex>

namespace {

std::mutex g_notifyMutex;
std::string g_notifyText;
unsigned int g_notifyColor = 0xFFFFFFFFu;
std::chrono::steady_clock::time_point g_notifyExpiry{};

// Status window position, persisted across frames so it stays draggable.
bool g_statusPosInit = false;
ImVec2 g_statusPos{ 20.f, 20.f };

constexpr ImU32 kAccent = IM_COL32(30, 120, 255, 255);
constexpr ImU32 kText = IM_COL32(235, 238, 245, 255);
constexpr ImU32 kTextDim = IM_COL32(110, 118, 135, 255);
constexpr ImU32 kBg = IM_COL32(26, 28, 34, 225);
constexpr ImU32 kBorder = IM_COL32(50, 70, 120, 210);

struct StatusEntry {
	const char* label;
	bool enabled;
};

} // namespace

void OverlayNotify::Show(const char* msg, unsigned int col, float seconds)
{
	std::lock_guard<std::mutex> lock(g_notifyMutex);
	g_notifyText = msg ? msg : "";
	g_notifyColor = col;
	g_notifyExpiry = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds((int)(seconds * 1000.f));
}

void OverlayNotify::Draw()
{
	// ---- feature status window --------------------------------------------
	// The menu draws its own info bar in the same corner, so hide this one
	// while the menu is open.
	if (!Overlay::MenuOpen() && ConfigInstance.Overlay.StatusPanel)
	{
		const PlayerConfig& cfg = ConfigInstance.Player;
		StatusEntry entries[] = {
			{ "Player ESP", cfg.BoxEsp || cfg.Skeleton || cfg.Chams || cfg.Health || cfg.Distance },
			{ "Skeleton", cfg.Skeleton },
			{ "Health", cfg.Health },
			{ "Distance", cfg.Distance },
		};
		const int count = (int)(sizeof(entries) / sizeof(entries[0]));

		const float pad = 10.f;
		const float dotR = 4.f;
		const float lineH = ImGui::GetTextLineHeightWithSpacing();
		float maxW = 0.f;
		for (int i = 0; i < count; ++i)
		{
			const float w = ImGui::CalcTextSize(entries[i].label).x;
			if (w > maxW) maxW = w;
		}
		const float winW = pad + dotR * 2.f + 8.f + maxW + pad;
		const float winH = pad + lineH * count + pad;

		if (!g_statusPosInit)
		{
			g_statusPos = ImVec2(20.f, 20.f);
			g_statusPosInit = true;
		}

		ImGui::SetNextWindowPos(g_statusPos, ImGuiCond_Once);
		ImGui::SetNextWindowSize(ImVec2(winW, winH));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, kBg);
		ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
		ImGui::Begin("##veox_status_overlay", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);

		g_statusPos = ImGui::GetWindowPos();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 cursor = ImGui::GetCursorScreenPos();

		ImFont* statusFont = ImGui::GetFont();
		const float statusFontSize = ImGui::GetFontSize();
		for (int i = 0; i < count; ++i)
		{
			const float y = cursor.y + lineH * i + lineH * 0.5f;
			const ImU32 col = entries[i].enabled ? kAccent : kTextDim;
			dl->AddCircleFilled(ImVec2(cursor.x + dotR, y), dotR, col);
			Esp::DrawOutlinedText(dl, ImVec2(cursor.x + dotR * 2.f + 8.f, y - lineH * 0.5f),
				entries[i].enabled ? kText : kTextDim, statusFont, statusFontSize, entries[i].label);
		}

		ImGui::End();
		ImGui::PopStyleVar(3);
		ImGui::PopStyleColor(2);
	}

	// ---- timed corner toast ------------------------------------------------
	std::string text;
	unsigned int color = 0xFFFFFFFFu;
	{
		std::lock_guard<std::mutex> lock(g_notifyMutex);
		if (g_notifyText.empty()) return;
		if (std::chrono::steady_clock::now() > g_notifyExpiry)
		{
			g_notifyText.clear();
			return;
		}
		text = g_notifyText;
		color = g_notifyColor;
	}
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	ImFont* toastFont = ImGui::GetFont();
	const float toastFontSize = ImGui::GetFontSize();
	const ImVec2 sz = ImGui::GetIO().DisplaySize;
	const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
	const float x = sz.x - ts.x - 20.f;
	Esp::DrawOutlinedText(dl, ImVec2(x, 20.f), color, toastFont, toastFontSize, text.c_str());
}
