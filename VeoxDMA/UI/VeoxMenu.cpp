#include "pch.h"
#include <fstream>
#include <commdlg.h>

// Native open/save dialog so a config can live anywhere, not only beside the exe.
static std::wstring PickConfigPath(bool save)
{
	wchar_t path[MAX_PATH]{};
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter = L"Loot filter config (*.json)\0*.json\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = path;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrDefExt = L"json";
	ofn.lpstrTitle = save ? L"Export loot filter config" : L"Import loot filter config";
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
	return ok ? std::wstring(path) : std::wstring();
}


#pragma comment(lib, "comdlg32.lib")
#include "VeoxMenu.h"
#include "ConfigInstance.h"
#include "ConfigFile.h"
#include "RobRunReader.h"
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "SkinChanger.h"
#include "DeviceSession.h"
#include "InputManager.h"
#include "../Aim/AimController.h"
#include "../Overlay/Overlay.h"
#include "../Esp/Radar.h"
#include "../Esp/WebRadar.h"
#include "../Esp/IconSprites.h"
#include "../Overlay/Backdrop.h"
#include "imgui_internal.h"
#include "imgui_settings.h"
#include "font.h"
#include "textures.h"
#include <d3d11.h>
#include <wincodec.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <algorithm>
#include <map>
#include <string>
#include <filesystem>
#include <vector>

extern void RequestReattach();

// ---------------------------------------------------------------------------
// ROB RUN menu.
//
// The window is fully custom drawn: the chrome, cards, toggles, sliders and
// combos are all painted with the draw list so the look does not depend on the
// stock ImGui style. Purple accent on near-black, thin borders, animated
// hover/selection states and a fade+slide page transition.
// ---------------------------------------------------------------------------
namespace VeoxMenu {
namespace {

// ---- geometry -------------------------------------------------------------
// kW/kH/kCardW are refreshed every frame from the actual window size so the
// menu can be resized; the rest of the layout hangs off them.
float kW = 900.f;
float kH = 560.f;
constexpr float kTop = 66.f;
constexpr float kSide = 176.f;
constexpr float kPad = 18.f;
constexpr float kGap = 14.f;
constexpr float kCardHeader = 40.f;
constexpr float kRow = 22.f;
float kCardW = 337.f;
bool g_singleColumn = false;
constexpr float kMinW = 680.f;
constexpr float kMinH = 420.f;
// Below this width the two-column grid gets too tight for the row labels, so
// the pages fall back to a single full-width column.
constexpr float kTwoColumnMin = 800.f;
// The panel fills the whole ImGui window, so ImGui's own resize borders and
// grip line up exactly with the visible menu edges.
constexpr float kGlow = 0.f;

// ---- palette --------------------------------------------------------------
const ImVec4 kAccent = ImVec4(124 / 255.f, 92 / 255.f, 255 / 255.f, 1.f);
const ImVec4 kAccentBright = ImVec4(166 / 255.f, 140 / 255.f, 255 / 255.f, 1.f);
const ImVec4 kGreen = ImVec4(74 / 255.f, 201 / 255.f, 126 / 255.f, 1.f);
const ImVec4 kRed = ImVec4(255 / 255.f, 75 / 255.f, 75 / 255.f, 1.f);
const ImVec4 kTransparent = ImVec4(0.f, 0.f, 0.f, 0.f);

// Mutable so the High Contrast Menu setting can swap the whole surface at
// runtime. ApplyPalette() resets them every frame.
ImVec4 kBgTop = ImVec4(11 / 255.f, 9 / 255.f, 18 / 255.f, 1.f);
ImVec4 kBgBottom = ImVec4(4 / 255.f, 4 / 255.f, 7 / 255.f, 1.f);
ImVec4 kCard = ImVec4(13 / 255.f, 12 / 255.f, 19 / 255.f, 0.94f);
ImVec4 kCardTop = ImVec4(18 / 255.f, 16 / 255.f, 26 / 255.f, 1.f);
ImVec4 kPopup = ImVec4(12 / 255.f, 11 / 255.f, 18 / 255.f, 1.f);
// The sidebar and its page buttons reuse the card surface, so the nav and the
// page cards read as one continuous sheet (most visible in High Contrast).
ImVec4 kNav = ImVec4(13 / 255.f, 12 / 255.f, 19 / 255.f, 0.94f);
ImVec4 kNavActive = ImVec4(18 / 255.f, 16 / 255.f, 26 / 255.f, 1.f);
ImVec4 kStroke = ImVec4(1.f, 1.f, 1.f, 0.055f);
ImVec4 kTrack = ImVec4(1.f, 1.f, 1.f, 0.075f);
ImVec4 kText = ImVec4(236 / 255.f, 233 / 255.f, 247 / 255.f, 1.f);
ImVec4 kTextDim = ImVec4(140 / 255.f, 135 / 255.f, 166 / 255.f, 1.f);
ImVec4 kTextFaint = ImVec4(92 / 255.f, 88 / 255.f, 114 / 255.f, 1.f);

bool g_highContrast = false;

// ---- selectable fonts ----------------------------------------------------
struct FontEntry { const char* name; ImFont* ptr; };
constexpr int kFontCount = 7;
FontEntry g_fonts[kFontCount] = {
	{ "Maven Pro",  nullptr },
	{ "Atkinson",   nullptr },
	{ "Arial",      nullptr },
	{ "Segoe UI",   nullptr },
	{ "Consolas",   nullptr },
	{ "Tahoma",     nullptr },
	{ "Verdana",    nullptr },
};
int g_fontIndex = 0;

static void ApplySelectedFont()
{
	if (g_fonts[g_fontIndex].ptr)
		ImGui::GetIO().FontDefault = g_fonts[g_fontIndex].ptr;
	ConfigInstance.Overlay.FontName = g_fonts[g_fontIndex].name;
}

void ApplyPalette()
{
	if (g_highContrast)
	{
		kBgTop = ImVec4(55 / 255.f, 50 / 255.f, 72 / 255.f, 1.f);
		kBgBottom = kBgTop;
		kCard = ImVec4(62 / 255.f, 56 / 255.f, 80 / 255.f, 1.f);
		kCardTop = ImVec4(72 / 255.f, 65 / 255.f, 92 / 255.f, 1.f);
		kPopup = kCard;
		kStroke = ImVec4(1.f, 1.f, 1.f, 0.25f);
		kTrack = ImVec4(1.f, 1.f, 1.f, 0.30f);
		kText = ImVec4(1.f, 1.f, 1.f, 1.f);
		kTextDim = ImVec4(0.86f, 0.84f, 0.92f, 1.f);
		kTextFaint = ImVec4(0.69f, 0.67f, 0.76f, 1.f);
	}
	else
	{
		kBgTop = ImVec4(11 / 255.f, 9 / 255.f, 18 / 255.f, 1.f);
		kBgBottom = ImVec4(4 / 255.f, 4 / 255.f, 7 / 255.f, 1.f);
		kCard = ImVec4(13 / 255.f, 12 / 255.f, 19 / 255.f, 0.94f);
		kCardTop = ImVec4(18 / 255.f, 16 / 255.f, 26 / 255.f, 1.f);
		kPopup = ImVec4(12 / 255.f, 11 / 255.f, 18 / 255.f, 1.f);
		kStroke = ImVec4(1.f, 1.f, 1.f, 0.055f);
		kTrack = ImVec4(1.f, 1.f, 1.f, 0.075f);
		kText = ImVec4(236 / 255.f, 233 / 255.f, 247 / 255.f, 1.f);
		kTextDim = ImVec4(140 / 255.f, 135 / 255.f, 166 / 255.f, 1.f);
		kTextFaint = ImVec4(92 / 255.f, 88 / 255.f, 114 / 255.f, 1.f);
	}

	// Sidebar always shares the card surface; the active page button takes the
	// card header tone so it looks like the top of the page it opens.
	kNav = kCard;
	kNavActive = kCardTop;
}

// ---- state ----------------------------------------------------------------
int g_page = 0;
int g_drawnPage = 0;
float g_fade = 1.f;
float g_slide = 0.f;
float g_slideDir = 1.f;
bool g_scrollReset = false;
bool g_dragging = false;
ImVec2 g_dragDelta = ImVec2(0.f, 0.f);
ImVec2 g_lastPos = ImVec2(-1.f, -1.f);
// The saved placement is applied once, on the first frame the menu is drawn.
bool g_placementApplied = false;
char g_search[96] = "";
bool g_readCatalog = false;

const char* kPageNames[] = { "Visuals", "Radar", "Loot Filter", "Aim", "Server", "Misc", "Skins", "Settings" };
constexpr int kPageCount = IM_ARRAYSIZE(kPageNames);

ID3D11ShaderResourceView* g_icon[kPageCount] = {};

std::map<ImGuiID, float> g_cardHeights;
std::map<ImGuiID, float> g_cardHover;
std::map<ImGuiID, float> g_forcedHeights;
ImGuiID g_currentCard = 0;

// ---- helpers --------------------------------------------------------------
inline ImU32 C(const ImVec4& c, float a = 1.f)
{
	ImVec4 x = c;
	x.w *= a;
	return ImGui::GetColorU32(x);
}

inline ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t)
{
	t = ImClamp(t, 0.f, 1.f);
	return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
		a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

inline float TextW(ImFont* font, const char* s)
{
	if (!font || !s) return 0.f;
	return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, s).x;
}

inline void Text(ImFont* font, const ImVec2& p, ImU32 col, const char* s)
{
	if (!font || !s || !*s) return;
	ImGui::GetWindowDrawList()->AddText(font, font->FontSize, p, col, s);
}

inline void TextRight(ImFont* font, float right, float y, ImU32 col, const char* s)
{
	Text(font, ImVec2(right - TextW(font, s), y), col, s);
}

inline float RowTextY(float rowTop, float rowHeight, ImFont* font)
{
	return rowTop + (rowHeight - font->FontSize) * 0.5f;
}

// Small purple pill marking rows that write into the game process over DMA.
// Draws right-aligned ending at rightX, vertically centred in the row.
inline void MemoryWriteBadge(float rightX, float rowTop, float rowHeight, bool active)
{
	const char* text = "MEM WRITE";
	ImFont* f = font::inter_medium_small;
	const float tw = TextW(f, text);
	constexpr float bh = 16.f;
	const ImVec2 mx(rightX, rowTop + (rowHeight - bh) * 0.5f);
	const ImRect bb(ImVec2(mx.x - tw - 12.f, mx.y), ImVec2(mx.x, mx.y + bh));

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(bb.Min, bb.Max, C(kAccent, active ? 0.20f : 0.10f), 4.f);
	dl->AddRect(bb.Min, bb.Max, C(kAccent, active ? 0.55f : 0.28f), 4.f, 0, 1.f);
	dl->AddText(f, f->FontSize,
		ImVec2(bb.Min.x + 6.f, bb.Min.y + (bh - f->FontSize) * 0.5f - 1.f),
		C(kAccentBright, active ? 1.f : 0.65f), text);
}

inline bool Match(const char* label)
{
	if (!g_search[0] || !label) return true;
	std::string hay(label), needle(g_search);
	auto lower = [](unsigned char c) { return (char)std::tolower(c); };
	std::transform(hay.begin(), hay.end(), hay.begin(), lower);
	std::transform(needle.begin(), needle.end(), needle.begin(), lower);
	return hay.find(needle) != std::string::npos;
}

bool MatchAny(std::initializer_list<const char*> labels)
{
	if (!g_search[0]) return true;
	for (const char* l : labels)
		if (Match(l)) return true;
	return false;
}

void Note(const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	ImGui::PushFont(font::inter_medium_small);
	ImGui::PushStyleColor(ImGuiCol_Text, C(kTextDim));
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
	ImGui::TextWrapped("%s", buf);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
	ImGui::PopFont();
}

void SectionLabel(const char* text)
{
	if (!text || !*text) return;
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	const float y = p.y + 6.f;

	window->DrawList->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		ImVec2(p.x, y), C(kTextFaint), text);
	const float lineX = p.x + TextW(font::inter_medium_small, text) + 10.f;
	if (lineX < p.x + w - 4.f)
		window->DrawList->AddLine(ImVec2(lineX, y + 6.f), ImVec2(p.x + w, y + 6.f), C(kStroke), 1.f);

	ImGui::Dummy(ImVec2(0.f, 20.f));
}

// ---- card -----------------------------------------------------------------
void BeginCard(const char* title, float width, float minHeight = 62.f)
{
	ImGuiWindow* parent = ImGui::GetCurrentWindow();
	const ImGuiID id = parent->GetID(title);
	const float natural = g_cardHeights.count(id) ? g_cardHeights[id] : minHeight;
	const float forced = g_forcedHeights.count(id) ? g_forcedHeights[id] : 0.f;
	const float h = ImMax(natural, forced);

	const ImVec2 p = parent->DC.CursorPos;
	const ImRect bb(p, p + ImVec2(width, h));

	float& hover = g_cardHover[id];
	hover = ImLerp(hover, ImGui::IsMouseHoveringRect(bb.Min, bb.Max) ? 1.f : 0.f,
		ImGui::GetIO().DeltaTime * 10.f);

	ImDrawList* dl = parent->DrawList;
	dl->AddRectFilled(bb.Min, bb.Max, C(kCard), 10.f);
	dl->AddRect(bb.Min, bb.Max, C(Mix(kStroke, kAccent, hover * 0.5f), 1.f), 10.f, 0, 1.f);

	dl->AddRectFilled(bb.Min, ImVec2(bb.Max.x, bb.Min.y + kCardHeader), C(kCardTop), 10.f,
		ImDrawFlags_RoundCornersTop);
	dl->AddLine(ImVec2(bb.Min.x, bb.Min.y + kCardHeader),
		ImVec2(bb.Max.x, bb.Min.y + kCardHeader), C(kStroke), 1.f);
	dl->AddRectFilled(ImVec2(bb.Min.x + 12.f, bb.Min.y + 1.f),
		ImVec2(bb.Min.x + 44.f, bb.Min.y + 2.5f), C(kAccent, 0.85f), 1.f);

	const char* titleEnd = ImGui::FindRenderedTextEnd(title);
	std::string displayTitle(title, titleEnd);
	Text(font::inter_bold,
		ImVec2(bb.Min.x + 14.f, bb.Min.y + (kCardHeader - font::inter_bold->FontSize) * 0.5f - 1.f),
		C(kTextDim), displayTitle.c_str());

	ImGui::SetCursorScreenPos(ImVec2(bb.Min.x, bb.Min.y + kCardHeader));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 12.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 7.f));
	ImGui::BeginChild(id, ImVec2(width, h - kCardHeader), ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings);
	g_currentCard = id;
}

void EndCard()
{
	ImGuiWindow* child = ImGui::GetCurrentWindow();
	// ContentSize excludes the padding on both sides, so add it back twice.
	const float content = child->ContentSize.y + ImGui::GetStyle().WindowPadding.y * 2.f;
	// Restore the page spacing before EndChild so the vertical gap between
	// cards matches the horizontal gap between columns.
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
	ImGui::EndChild();
	g_cardHeights[g_currentCard] = kCardHeader + content + 1.f;
}

// Last frame's natural height of a card, used to line cards up before they are
// drawn. Unknown cards return 0 so the caller can fall back to the partner.
float CardNaturalHeight(const char* title)
{
	if (!title || !*title) return 0.f;
	ImGuiWindow* parent = ImGui::GetCurrentWindow();
	auto it = g_cardHeights.find(parent->GetID(title));
	return it == g_cardHeights.end() ? 0.f : it->second;
}

// Force a card to at least this tall. The card still grows if its content
// needs more room, so nothing is ever clipped.
void ForceCardHeight(const char* title, float height)
{
	if (!title || !*title) return;
	ImGuiWindow* parent = ImGui::GetCurrentWindow();
	g_forcedHeights[parent->GetID(title)] = height;
}

// ---- widgets --------------------------------------------------------------
bool ToggleRow(const char* label, bool* v, bool memoryWrite = false)
{
	if (!Match(label)) return false;

	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	const ImRect bb(p, p + ImVec2(w, kRow));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	bool hov = false, held = false;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);

	if (pressed) *v = !*v;

	static std::map<ImGuiID, float> anim;
	float& t = anim[id];
	t = ImLerp(t, *v ? 1.f : 0.f, ImGui::GetIO().DeltaTime * 14.f);

	ImDrawList* dl = window->DrawList;
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, RowTextY(p.y, kRow, font::inter_medium_widget)),
		C(Mix(kTextDim, kText, ImMax(t, hov ? 0.55f : 0.f))), label);

	const float tw = 38.f, th = 20.f;
	const ImVec2 tmin(p.x + w - tw, p.y + (kRow - th) * 0.5f);
	const ImVec2 tmax(tmin.x + tw, tmin.y + th);
	if (memoryWrite)
		MemoryWriteBadge(tmin.x - 8.f, p.y, kRow, *v);
	dl->AddRectFilled(tmin, tmax, C(Mix(kTrack, kAccent, t), 0.35f + 0.65f * t), th * 0.5f);
	if (t > 0.01f)
	{
		// Keep the glow inside the switch instead of letting it halo the row.
		dl->PushClipRect(tmin, tmax, true);
		dl->AddShadowCircle(ImVec2((tmin.x + tmax.x) * 0.5f, (tmin.y + tmax.y) * 0.5f),
			th * 0.5f, C(kAccent, 0.30f * t), 18.f, ImVec2(0, 0), 0);
		dl->PopClipRect();
	}

	const float kx = ImLerp(tmin.x + 10.f, tmax.x - 10.f, t);
	dl->AddCircleFilled(ImVec2(kx, (tmin.y + tmax.y) * 0.5f), 7.f,
		C(Mix(kTextDim, ImVec4(1, 1, 1, 1), t)));

	return pressed;
}

// Quick-pick colours shown above the picker. The picker below still allows
// any colour; these just save a trip around the wheel for the usual choices.
const ImVec4 kColourPresets[] = {
	ImVec4(1.00f, 1.00f, 1.00f, 1.f), ImVec4(1.00f, 0.18f, 0.18f, 1.f),
	ImVec4(0.20f, 0.95f, 0.35f, 1.f), ImVec4(0.20f, 0.80f, 1.00f, 1.f),
	ImVec4(1.00f, 0.82f, 0.15f, 1.f), ImVec4(0.70f, 0.35f, 1.00f, 1.f),
	ImVec4(1.00f, 0.50f, 0.15f, 1.f), ImVec4(1.00f, 0.30f, 0.70f, 1.f),
	ImVec4(0.35f, 1.00f, 0.85f, 1.f), ImVec4(0.45f, 0.55f, 1.00f, 1.f),
	ImVec4(0.60f, 0.60f, 0.65f, 1.f), ImVec4(0.08f, 0.07f, 0.11f, 1.f),
};

// Colour panel opened by a swatch: preset row on top, full picker below.
// The panel is anchored beside the row instead of spawning at the mouse, so
// it stays on screen when the menu sits against a monitor edge.
void DrawColourPopup(const char* popupId, const char* title,
	D2D1::ColorF& colour, const ImRect& anchor)
{
	const ImGuiIO& io = ImGui::GetIO();
	constexpr float panelW = 236.f;
	constexpr float panelH = 340.f; // estimate, only used to clamp on screen

	ImVec2 pos(anchor.Max.x + 10.f, anchor.Min.y - 8.f);
	if (pos.x + panelW + 8.f > io.DisplaySize.x)
		pos.x = anchor.Min.x - panelW - 10.f;
	pos.x = ImClamp(pos.x, 8.f, ImMax(io.DisplaySize.x - panelW - 8.f, 8.f));
	pos.y = ImClamp(pos.y, 8.f, ImMax(io.DisplaySize.y - panelH - 8.f, 8.f));

	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(panelW, 0.f), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f, 12.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f, 8.f));
	if (ImGui::BeginPopup(popupId))
	{
		ImGui::PushFont(font::inter_bold);
		ImGui::PushStyleColor(ImGuiCol_Text, kText);
		ImGui::TextUnformatted(title);
		ImGui::PopStyleColor();
		ImGui::PopFont();

		const ImVec2 linePos = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(0.f, 1.f));
		ImGui::GetWindowDrawList()->AddRectFilled(linePos,
			ImVec2(linePos.x + ImGui::GetContentRegionAvail().x, linePos.y + 1.f),
			C(kAccent, 0.40f));

		ImDrawList* dl = ImGui::GetWindowDrawList();
		constexpr int kCols = 6;
		constexpr float kGap = 6.f;
		const float avail = ImGui::GetContentRegionAvail().x;
		const float sw = (avail - kGap * (kCols - 1)) / kCols;
		constexpr float sh = 16.f;
		const int count = (int)(sizeof(kColourPresets) / sizeof(kColourPresets[0]));

		for (int i = 0; i < count; ++i)
		{
			if (i % kCols) ImGui::SameLine(0.f, kGap);
			ImGui::PushID(i);
			const ImVec2 q = ImGui::GetCursorScreenPos();
			const bool hit = ImGui::InvisibleButton("##preset", ImVec2(sw, sh));
			const bool hov = ImGui::IsItemHovered();
			const ImVec4& pc = kColourPresets[i];
			const bool selected =
				std::fabs(colour.r - pc.x) < 0.02f &&
				std::fabs(colour.g - pc.y) < 0.02f &&
				std::fabs(colour.b - pc.z) < 0.02f;

			dl->AddRectFilled(q, q + ImVec2(sw, sh),
				ImGui::GetColorU32(ImVec4(pc.x, pc.y, pc.z, 1.f)), 4.f);
			dl->AddRect(q, q + ImVec2(sw, sh),
				selected ? C(kAccentBright) : C(kStroke, hov ? 1.f : 0.7f),
				4.f, 0, selected ? 2.f : 1.f);

			if (hit)
				colour = D2D1::ColorF(pc.x, pc.y, pc.z, colour.a);
			ImGui::PopID();
		}

		float col[4] = { colour.r, colour.g, colour.b, colour.a };
		ImGui::PushItemWidth(avail);
		if (ImGui::ColorPicker4("##picker", col,
			ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview |
			ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueBar))
			colour = D2D1::ColorF(col[0], col[1], col[2], col[3]);
		ImGui::PopItemWidth();
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar(2);
}

bool ColorToggleRow(const char* label, bool* v, D2D1::ColorF& colour)
{
	if (!Match(label)) return false;

	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	const ImRect bb(p, p + ImVec2(w, kRow));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	const float tw = 38.f, th = 20.f, sw = 28.f;
	const ImVec2 tmin(p.x + w - tw, p.y + (kRow - th) * 0.5f);
	const ImRect track(tmin, ImVec2(tmin.x + tw, tmin.y + th));
	const ImRect swatch(ImVec2(track.Min.x - 10.f - sw, p.y + (kRow - 16.f) * 0.5f),
		ImVec2(track.Min.x - 10.f, p.y + (kRow + 16.f) * 0.5f));

	bool hov = false, held = false;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);

	char popup[96];
	std::snprintf(popup, sizeof(popup), "##colour_%s", label);

	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const bool mouseInSwatch = swatch.Contains(mouse);

	if (pressed)
	{
		if (mouseInSwatch)
		{
			ImGui::OpenPopup(popup);
		}
		else
		{
			*v = !*v;
		}
	}

	static std::map<ImGuiID, float> anim;
	float& t = anim[id];
	t = ImLerp(t, *v ? 1.f : 0.f, ImGui::GetIO().DeltaTime * 14.f);

	ImDrawList* dl = window->DrawList;
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, RowTextY(p.y, kRow, font::inter_medium_widget)),
		C(Mix(kTextDim, kText, ImMax(t, hov ? 0.55f : 0.f))), label);

	dl->AddRectFilled(track.Min, track.Max, C(Mix(kTrack, kAccent, t), 0.35f + 0.65f * t), th * 0.5f);
	if (t > 0.01f)
	{
		// Clip the glow to the switch so it does not bleed past the track.
		dl->PushClipRect(track.Min, track.Max, true);
		dl->AddShadowCircle(ImVec2((track.Min.x + track.Max.x) * 0.5f, (track.Min.y + track.Max.y) * 0.5f),
			th * 0.5f, C(kAccent, 0.30f * t), 18.f, ImVec2(0, 0), 0);
		dl->PopClipRect();
	}
	const float kx = ImLerp(track.Min.x + 10.f, track.Max.x - 10.f, t);
	dl->AddCircleFilled(ImVec2(kx, (track.Min.y + track.Max.y) * 0.5f), 7.f,
		C(Mix(kTextDim, ImVec4(1, 1, 1, 1), t)));

	const bool shov = hov && mouseInSwatch;
	dl->AddRectFilled(swatch.Min, swatch.Max,
		C(ImVec4(colour.r, colour.g, colour.b, 1.f)), 4.f);
	dl->AddRect(swatch.Min, swatch.Max, C(kStroke, shov ? 2.2f : 1.f), 4.f);

	DrawColourPopup(popup, label, colour, swatch);

	return pressed && !mouseInSwatch;
}

bool ColorRow(const char* label, D2D1::ColorF& colour)
{
	if (!Match(label)) return false;

	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	const ImRect bb(p, p + ImVec2(w, kRow));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	const ImRect swatch(ImVec2(p.x + w - 36.f, p.y + 3.f), ImVec2(p.x + w, p.y + 19.f));
	bool hov, held, pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);

	char popup[96];
	std::snprintf(popup, sizeof(popup), "##colour_%s", label);
	if (pressed) ImGui::OpenPopup(popup);

	ImDrawList* dl = window->DrawList;
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, RowTextY(p.y, kRow, font::inter_medium_widget)),
		C(Mix(kTextDim, kText, hov ? 0.55f : 0.f)), label);
	dl->AddRectFilled(swatch.Min, swatch.Max,
		C(ImVec4(colour.r, colour.g, colour.b, 1.f)), 4.f);
	dl->AddRect(swatch.Min, swatch.Max, C(kStroke, hov ? 2.2f : 1.f), 4.f);

	DrawColourPopup(popup, label, colour, swatch);

	return pressed;
}

bool SliderIntRow(const char* label, int* v, int lo, int hi, const char* suffix = "",
	const char* zeroText = nullptr)
{
	if (!Match(label)) return false;

	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	constexpr float rowH = 34.f;
	const ImRect bb(p, p + ImVec2(w, rowH));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	const ImVec2 tmin(p.x, p.y + 26.f);
	const ImVec2 tmax(p.x + w, p.y + 30.f);
	const ImRect hit(ImVec2(tmin.x, tmin.y - 8.f), ImVec2(tmax.x, tmax.y + 8.f));

	bool hov, held;
	ImGui::ButtonBehavior(hit, id, &hov, &held);

	bool changed = false;
	float t = (float)(*v - lo) / (float)ImMax(hi - lo, 1);
	if (held)
	{
		const float mx = ImGui::GetIO().MousePos.x;
		t = ImClamp((mx - tmin.x) / ImMax(w, 1.f), 0.f, 1.f);
		const int nv = lo + (int)std::lround(t * (float)(hi - lo));
		if (nv != *v) { *v = nv; changed = true; }
		t = (float)(*v - lo) / (float)ImMax(hi - lo, 1);
	}

	ImDrawList* dl = window->DrawList;
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, p.y + 1.f), C(Mix(kTextDim, kText, hov || held ? 0.5f : 0.f)), label);

	char buf[64];
	if (zeroText && *v == lo)
		std::snprintf(buf, sizeof(buf), "%s", zeroText);
	else
		std::snprintf(buf, sizeof(buf), "%d%s", *v, suffix);
	TextRight(font::inter_medium_small, p.x + w, p.y + 3.f, C(kText), buf);

	dl->AddRectFilled(tmin, tmax, C(kTrack), 2.f);
	dl->AddRectFilled(tmin, ImVec2(tmin.x + w * t, tmax.y), C(kAccent), 2.f);
	dl->AddCircleFilled(ImVec2(tmin.x + w * t, (tmin.y + tmax.y) * 0.5f),
		hov || held ? 6.f : 5.f, C(kAccentBright));

	return changed;
}

bool SliderFloatRow(const char* label, float* v, float lo, float hi, const char* suffix = "")
{
	if (!Match(label)) return false;

	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	constexpr float rowH = 34.f;
	const ImRect bb(p, p + ImVec2(w, rowH));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	const ImVec2 tmin(p.x, p.y + 26.f);
	const ImVec2 tmax(p.x + w, p.y + 30.f);
	const ImRect hit(ImVec2(tmin.x, tmin.y - 8.f), ImVec2(tmax.x, tmax.y + 8.f));

	bool hov, held;
	ImGui::ButtonBehavior(hit, id, &hov, &held);

	bool changed = false;
	float t = (*v - lo) / ImMax(hi - lo, 0.0001f);
	if (held)
	{
		const float mx = ImGui::GetIO().MousePos.x;
		t = ImClamp((mx - tmin.x) / ImMax(w, 1.f), 0.f, 1.f);
		const float nv = lo + t * (hi - lo);
		if (nv != *v) { *v = nv; changed = true; }
		t = (*v - lo) / ImMax(hi - lo, 0.0001f);
	}

	ImDrawList* dl = window->DrawList;
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, p.y + 1.f), C(Mix(kTextDim, kText, hov || held ? 0.5f : 0.f)), label);

	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.2f%s", *v, suffix);
	TextRight(font::inter_medium_small, p.x + w, p.y + 3.f, C(kText), buf);

	dl->AddRectFilled(tmin, tmax, C(kTrack), 2.f);
	dl->AddRectFilled(tmin, ImVec2(tmin.x + w * t, tmax.y), C(kAccent), 2.f);
	dl->AddCircleFilled(ImVec2(tmin.x + w * t, (tmin.y + tmax.y) * 0.5f),
		hov || held ? 6.f : 5.f, C(kAccentBright));

	return changed;
}

bool ComboRow(const char* label, int* current, const char* const* items, int count,
	float fieldW = 150.f, bool memoryWrite = false)
{
	if (!Match(label) || count <= 0) return false;

	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	constexpr float rowH = 30.f;
	const ImRect bb(p, p + ImVec2(w, rowH));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	// Never let the field run into the label when the card is narrow.
	const float labelW = TextW(font::inter_medium_widget, label);
	const float fw = ImClamp(fieldW, 80.f, ImMax(80.f, w - labelW - 14.f));
	const ImRect field(ImVec2(p.x + w - fw, p.y + 2.f), ImVec2(p.x + w, p.y + 26.f));
	bool hov, held, pressed = ImGui::ButtonBehavior(field, id, &hov, &held);

	char popup[96];
	std::snprintf(popup, sizeof(popup), "##combo_%s", label);
	if (pressed) ImGui::OpenPopup(popup);

	ImDrawList* dl = window->DrawList;
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, RowTextY(p.y, rowH, font::inter_medium_widget)),
		C(Mix(kTextDim, kText, hov ? 0.5f : 0.f)), label);

	dl->AddRectFilled(field.Min, field.Max, C(ImVec4(1, 1, 1, 0.045f)), 6.f);
	dl->AddRect(field.Min, field.Max, C(Mix(kStroke, kAccent, hov ? 0.4f : 0.f)), 6.f, 0, 1.f);

	if (memoryWrite)
		MemoryWriteBadge(field.Min.x - 8.f, p.y, rowH, true);

	int sel = ImClamp(*current, 0, count - 1);
	const char* preview = items[sel] ? items[sel] : "-";
	dl->PushClipRect(field.Min, ImVec2(field.Max.x - 22.f, field.Max.y), true);
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		ImVec2(field.Min.x + 10.f, RowTextY(field.Min.y, field.GetHeight(), font::inter_medium_small)),
		C(kText), preview);
	dl->PopClipRect();

	// chevron
	const ImVec2 cv(field.Max.x - 14.f, field.Min.y + 13.f);
	dl->AddLine(ImVec2(cv.x - 4.f, cv.y - 2.f), ImVec2(cv.x, cv.y + 2.f), C(kTextDim), 1.5f);
	dl->AddLine(ImVec2(cv.x, cv.y + 2.f), ImVec2(cv.x + 4.f, cv.y - 2.f), C(kTextDim), 1.5f);

	bool changed = false;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f, 6.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 2.f));
	ImGui::PushStyleColor(ImGuiCol_PopupBg, C(kPopup));
	ImGui::PushStyleColor(ImGuiCol_Text, C(kText));
	ImGui::PushStyleColor(ImGuiCol_Header, C(kAccent, 0.22f));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, C(kAccent, 0.30f));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, C(kAccent, 0.38f));
	if (ImGui::BeginPopup(popup))
	{
		ImGui::PushItemWidth(ImMax(fieldW, 150.f));
		for (int i = 0; i < count; i++)
		{
			if (ImGui::Selectable(items[i], i == sel))
			{
				*current = i;
				changed = true;
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::PopItemWidth();
		ImGui::EndPopup();
	}
	ImGui::PopStyleColor(5);
	ImGui::PopStyleVar(2);

	return changed;
}

// Compact button for a horizontal strip of actions.
bool InlineButton(const char* text, float width, bool accent = false)
{
	if (accent) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.53f, 0.95f, 1.0f));
	}
	const bool pressed = ImGui::Button(text, ImVec2(width, 26.f));
	if (accent) ImGui::PopStyleColor(2);
	return pressed;
}

bool ButtonRow(const char* label, const char* text, float width = 130.f, bool accent = false)
{	if (label && *label && !Match(label)) return false;

	ImGuiWindow* window = ImGui::GetCurrentWindow();
	// An empty label hashes to the window ID, which trips ImGui's
	// "cannot have an empty ID at the root of a window" assert. Fall back to
	// the button text so every row still gets a stable unique ID.
	const char* idLabel = (label && *label) ? label : text;
	const ImGuiID id = window->GetID((idLabel && *idLabel) ? idLabel : "##btn");
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	constexpr float rowH = 30.f;
	const ImRect bb(p, p + ImVec2(w, rowH));
	const ImRect button(ImVec2(p.x + w - width, p.y + 2.f), ImVec2(p.x + w, p.y + 28.f));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	bool hov, held, pressed = ImGui::ButtonBehavior(button, id, &hov, &held);

	ImDrawList* dl = window->DrawList;
	if (label && *label)
		dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
			ImVec2(p.x, RowTextY(p.y, rowH, font::inter_medium_widget)),
			C(kTextDim), label);

	ImU32 fill = accent
		? C(kAccent, hov ? 0.85f : 0.65f)
		: C(ImVec4(1, 1, 1, hov ? 0.09f : 0.055f));
	dl->AddRectFilled(button.Min, button.Max, fill, 6.f);
	if (!accent)
		dl->AddRect(button.Min, button.Max, C(kStroke), 6.f);

	const float tw = TextW(font::inter_medium_small, text);
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		ImVec2((button.Min.x + button.Max.x - tw) * 0.5f,
			RowTextY(button.Min.y, button.GetHeight(), font::inter_medium_small)),
		C(accent ? ImVec4(1, 1, 1, 1) : kText), text);

	return pressed;
}

bool PageButton(int index, const char* label, ImTextureID icon, float width)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const ImRect bb(p, p + ImVec2(width, 40.f));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	bool hov, held, pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
	if (held) pressed = true;

	static float sel[kPageCount] = {};
	static float hover[kPageCount] = {};
	const float dt = ImGui::GetIO().DeltaTime;
	sel[index] = ImLerp(sel[index], g_page == index ? 1.f : 0.f, dt * 12.f);
	hover[index] = ImLerp(hover[index], hov ? 1.f : 0.f, dt * 12.f);

	ImDrawList* dl = window->DrawList;
	if (sel[index] > 0.01f)
	{
		// Fill with the card surface instead of an accent wash, so the selected
		// page fuses with the cards it opens. The accent border and left bar
		// still mark it as active.
		dl->AddRectFilled(bb.Min, bb.Max, C(Mix(kNav, kNavActive, sel[index])), 8.f);
		dl->AddRect(bb.Min, bb.Max, C(kAccent, 0.32f * sel[index]), 8.f);
	}
	else if (hover[index] > 0.01f)
	{
		dl->AddRectFilled(bb.Min, bb.Max, C(kNavActive, 0.55f * hover[index]), 8.f);
	}

	const float cy = (bb.Min.y + bb.Max.y) * 0.5f;
	if (sel[index] > 0.01f)
	{
		const float ih = 18.f * sel[index];
		dl->AddRectFilled(ImVec2(bb.Min.x, cy - ih * 0.5f),
			ImVec2(bb.Min.x + 3.f, cy + ih * 0.5f), C(kAccent, sel[index]), 2.f);
	}

	ImU32 iconCol = C(Mix(kTextFaint, kAccentBright, ImMax(sel[index], hover[index] * 0.5f)),
		0.7f + 0.3f * ImMax(sel[index], hover[index]));
	if (icon)
		dl->AddImage(icon, ImVec2(bb.Min.x + 14.f, cy - 8.f),
			ImVec2(bb.Min.x + 30.f, cy + 8.f), ImVec2(0, 0), ImVec2(1, 1), iconCol);
	else
		dl->AddCircle(ImVec2(bb.Min.x + 22.f, cy), 6.f, iconCol, 16, 1.4f);

	ImU32 textCol = C(Mix(kTextDim, kText, ImMax(sel[index], hover[index] * 0.55f)));
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(bb.Min.x + 40.f, cy - font::inter_medium_widget->FontSize * 0.5f), textCol, label);

	return pressed;
}

// ---- texture loading ------------------------------------------------------
ID3D11ShaderResourceView* LoadTextureFromMemory(const unsigned char* data, size_t size)
{
	ID3D11Device* device = Overlay::GetDevice();
	if (!device || !data || size == 0) return nullptr;

	IWICImagingFactory* factory = nullptr;
	IWICStream* stream = nullptr;
	IWICBitmapDecoder* decoder = nullptr;
	IWICBitmapFrameDecode* frame = nullptr;
	IWICFormatConverter* converter = nullptr;
	ID3D11Texture2D* texture = nullptr;
	ID3D11ShaderResourceView* view = nullptr;

	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&factory));
	if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
	if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), (DWORD)size);
	if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(stream, nullptr,
		WICDecodeMetadataCacheOnLoad, &decoder);
	if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
	if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
	if (SUCCEEDED(hr)) hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
		WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeMedianCut);

	UINT width = 0, height = 0;
	std::vector<BYTE> pixels;
	if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);
	if (SUCCEEDED(hr) && width && height)
	{
		pixels.resize((size_t)width * height * 4);
		hr = converter->CopyPixels(nullptr, width * 4, (UINT)pixels.size(), pixels.data());
	}

	if (SUCCEEDED(hr))
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA sub{};
		sub.pSysMem = pixels.data();
		sub.SysMemPitch = width * 4;

		hr = device->CreateTexture2D(&desc, &sub, &texture);
	}

	if (SUCCEEDED(hr))
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		hr = device->CreateShaderResourceView(texture, &srv, &view);
	}

	if (texture) texture->Release();
	if (converter) converter->Release();
	if (frame) frame->Release();
	if (decoder) decoder->Release();
	if (stream) stream->Release();
	if (factory) factory->Release();

	return SUCCEEDED(hr) ? view : nullptr;
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------
void DrawVisuals()
{
	// Four rows of two cards. Each row is forced to its taller card, so the
	// columns line up and end flush without leaving one huge empty card.
	if (!g_singleColumn)
	{
		const float r1 = ImMax(CardNaturalHeight("PLAYER ESP"), CardNaturalHeight("PLAYER WEAPONS"));
		const float r2 = ImMax(CardNaturalHeight("PLAYER FLAGS"), CardNaturalHeight("RANGE"));
		const float r3 = ImMax(CardNaturalHeight("TEXT & COLOURS"), CardNaturalHeight("COLOURS"));
		const float r4 = ImMax(CardNaturalHeight("SKELETON & CHAMS"), CardNaturalHeight("WORLD ESP"));
		ForceCardHeight("PLAYER ESP", r1);
		ForceCardHeight("PLAYER WEAPONS", r1);
		ForceCardHeight("PLAYER FLAGS", r2);
		ForceCardHeight("RANGE", r2);
		ForceCardHeight("TEXT & COLOURS", r3);
		ForceCardHeight("COLOURS", r3);
		ForceCardHeight("SKELETON & CHAMS", r4);
		ForceCardHeight("WORLD ESP", r4);
	}

	ImGui::BeginGroup();
	{
		if (MatchAny({ "Box ESP", "Health", "Distance", "Snaplines", "Off-Screen Indicators", "Head Dot", "Hide Teammates", "Hidden Player ESP" }))
		{
			BeginCard("PLAYER ESP", kCardW);
			ToggleRow("Hide Teammates", &ConfigInstance.Player.HideTeammates);
			ToggleRow("Hidden Player ESP", &ConfigInstance.Player.HiddenPlayerEsp);
			ColorToggleRow("Box ESP", &ConfigInstance.Player.BoxEsp, ConfigInstance.Player.BoxColour);
			ColorToggleRow("Health", &ConfigInstance.Player.Health, ConfigInstance.Player.HealthTextColour);
			ColorToggleRow("Distance", &ConfigInstance.Player.Distance, ConfigInstance.Player.DistanceColour);
			ColorToggleRow("Snaplines", &ConfigInstance.Player.Snaplines, ConfigInstance.Player.SnaplineColour);
			if (ConfigInstance.Player.Snaplines) {
				static const char* snapOrigins[] = { "Bottom", "Crosshair", "Top" };
				ComboRow("Origin", &ConfigInstance.Player.SnaplineOrigin, snapOrigins, IM_ARRAYSIZE(snapOrigins));
			}
			ColorToggleRow("Off-Screen Indicators", &ConfigInstance.Player.OffscreenArrows, ConfigInstance.Player.OffscreenColour);
			ColorToggleRow("Head Hitbox Dot", &ConfigInstance.Player.HeadDot, ConfigInstance.Player.HeadDotColour);
			if (ConfigInstance.Player.HeadDot) {
				SliderFloatRow("Dot Radius", &ConfigInstance.Player.HeadDotRadius, 1.5f, 8.0f, "px");
			}
			EndCard();
		}

		if (MatchAny({ "Looking At You", "Looking Alert", "Show Role", "Color-code by role",
			"Money & Value HUD", "Battle Mode", "Battle Mode Key" }))
		{
			BeginCard("PLAYER FLAGS", kCardW);
			ColorToggleRow("Looking At You", &ConfigInstance.Player.LookingAtYou, ConfigInstance.Player.LookingColour);
			ToggleRow("Looking Alert", &ConfigInstance.Player.LookingAlert);
			ColorToggleRow("Show Role", &ConfigInstance.Player.ShowRole, ConfigInstance.Player.RoleColour);
			if (ConfigInstance.Player.ShowRole)
				ToggleRow("Color-code by role", &ConfigInstance.Player.ExpRoleColors);
			ToggleRow("Money & Value HUD", &ConfigInstance.Player.ExpMoneyHud);
			ToggleRow("Battle Mode", &ConfigInstance.Overlay.BattleMode);
			ComboRow("Battle Mode Key", &ConfigInstance.Overlay.BattleModeKey, Aim::KeyNames(), Aim::KeyCount());
			if (ConfigInstance.Overlay.BattleModeKey == Aim::KeyCount() - 1)
				Note("Choose a specific key to toggle battle mode.");
			Note("When Battle Mode is on, only player ESP is shown.");
			EndCard();
		}

		if (MatchAny({ "Name", "Distance", "Health", "Weapon", "Role", "Criminal", "Police", "Civilian" }))
		{
			BeginCard("TEXT & COLOURS", kCardW);
			ColorRow("Name", ConfigInstance.Player.NameColour);
			ToggleRow("Color names by role", &ConfigInstance.Player.NameByRole);
			if (ConfigInstance.Player.NameByRole)
			{
				ColorRow("Criminal", ConfigInstance.Player.CriminalColour);
				ColorRow("Police", ConfigInstance.Player.PoliceColour);
				ColorRow("Civilian", ConfigInstance.Player.CivilianColour);
				ColorRow("Hubber", ConfigInstance.Player.HubberColour);
			}
			ColorRow("Distance", ConfigInstance.Player.DistanceColour);
			ColorRow("Health", ConfigInstance.Player.HealthTextColour);
			ColorRow("Weapon", ConfigInstance.Player.WeaponColour);
			EndCard();
		}

		if (MatchAny({ "Skeleton", "Draw Skeleton", "Chams", "Character Chams", "Limb Thickness",
			"VisCheck", "WallCheck", "BVH", "Interpolate", "Lead", "Native Camera",
				"Vis Ray", "Vis Max", "Hidden Alpha", "Car Cabin", "Vis Ring", "Status Ring" }))
		{
			BeginCard("SKELETON & CHAMS", kCardW);
			ToggleRow("Draw Skeleton", &ConfigInstance.Player.Skeleton);
			SliderFloatRow("Bone Thickness", &ConfigInstance.Player.SkeletonThickness, 0.5f, 6.0f, "px");
			ToggleRow("Head Circle", &ConfigInstance.Player.HeadCircle);
			ColorToggleRow("Character Chams", &ConfigInstance.Player.Chams, ConfigInstance.Player.ChamsColour);
			if (ConfigInstance.Player.Chams)
			{
				SliderFloatRow("Fill Opacity", &ConfigInstance.Player.ChamsAlpha, 0.1f, 1.0f);
				SliderFloatRow("Limb Thickness", &ConfigInstance.Player.ChamsScale, 0.5f, 2.0f, "x");
				ColorToggleRow("Chams Outline", &ConfigInstance.Player.ChamsOutline, ConfigInstance.Player.ChamsOutlineColour);
				ToggleRow("Chams VisCheck", &ConfigInstance.Player.ChamsVisCheck);
				if (ConfigInstance.Player.ChamsVisCheck)
				{
					ColorRow("Chams Visible", ConfigInstance.Player.ChamsVisibleColour);
					ColorRow("Chams Occluded", ConfigInstance.Player.ChamsHiddenColour);
				}
			}

			SectionLabel("SKELETON OCCLUSION");
			ToggleRow("Enable VisCheck", &ConfigInstance.Player.VisCheck);
			if (ConfigInstance.Player.VisCheck)
			{
				ColorRow("Visible to camera", ConfigInstance.Player.VisibleColour);
				ColorRow("Occluded", ConfigInstance.Player.HiddenColour);
				Note("Each bone is ray-cast from the camera against the city "
					"colliders, so only geometry blocks it.");
			}

			ToggleRow("Enable WallCheck", &ConfigInstance.Player.WallCheck);
			{
				auto status = RobRunReader::GetBvhStatus();
				if (status.loaded)
				{
					Note("BVH: %u nodes, %u triangles", status.nodeCount, status.triCount);
					if (!status.path.empty())
						Note("Path: %s", status.path.c_str());
				}
				else
				{
					Note("BVH: city_colliders.bvh not loaded - VisCheck falls back "
						"to animator state");
				}
				if (ButtonRow("", "Reload BVH", 110.f))
				{
					RobRunReader::ReloadBvh();
				}
				ToggleRow("Show BVH", &ConfigInstance.Player.ShowBvh);
				if (ConfigInstance.Player.ShowBvh)
					SliderIntRow("Draw Dist", &ConfigInstance.Player.BvhDrawDist, 10, 500, "m");
			}

			SliderIntRow("Vis Ray Count", &ConfigInstance.Player.VisRayCount, 2, 12);
			SliderFloatRow("Vis Max Dist", &ConfigInstance.Player.VisMaxDistance, 0.f, 500.f, "m");
			SliderFloatRow("Hidden Alpha", &ConfigInstance.Player.HiddenAlpha, 0.f, 1.f);
			ToggleRow("Car Cabin Solid", &ConfigInstance.Player.CarCabinSolid);
			ToggleRow("Vis Ring", &ConfigInstance.Player.VisRing);
			if (ConfigInstance.Player.VisRing)
				ColorRow("Vis Ring Colour", ConfigInstance.Player.VisRingColour);
			ToggleRow("Status Ring", &ConfigInstance.Player.StatusRing);
			if (ConfigInstance.Player.StatusRing)
				ColorRow("Status Ring Colour", ConfigInstance.Player.StatusRingColour);

			ToggleRow("Pose Smoothing", &ConfigInstance.Player.PoseSmoothing);
			if (ConfigInstance.Player.PoseSmoothing)
			{
				SliderFloatRow("Blend", &ConfigInstance.Player.PoseSmoothAlpha, 0.1f, 1.f);
				Note("The reader blends each new pose sample into the previous one "
					"before publishing. 1 keeps the pose raw; lower values damp "
					"jitter from partial or stale DMA reads.");
			}

			ToggleRow("Native Camera", &ConfigInstance.Player.NativeEspCamera);
			{
				const auto cam = RobRunReader::GetCameraDebug();
				if (!cam.nativeEnabled)
					Note("Managed camera (native off)");
				else if (!cam.nativeValid)
					Note("Managed camera (native not found)");
				else
					Note("Native camera  d=%.2fm  a=%.1f deg", cam.positionError,
						180.f / 3.14159265f * std::acos(std::clamp(cam.forwardDot, -1.f, 1.f)));
			}
			EndCard();
		}
	}
	ImGui::EndGroup();

	if (!g_singleColumn)
		ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		if (MatchAny({ "Primary Weapon", "Secondary Weapon", "Weapon Name" }))
		{
			BeginCard("PLAYER WEAPONS", kCardW);
			ToggleRow("Primary Weapon", &ConfigInstance.Player.PrimaryWeapon);
			ToggleRow("Secondary Weapon", &ConfigInstance.Player.SecondaryWeapon);
			ColorToggleRow("Weapon Name", &ConfigInstance.Player.WeaponEsp, ConfigInstance.Player.WeaponColour);
			ToggleRow("Weapon Sprite", &ConfigInstance.Player.WeaponSprite);
			EndCard();
		}

		if (MatchAny({ "Firing", "Reloading", "Armor" }))
		{
			BeginCard("STATE INDICATORS", kCardW);
			ColorToggleRow("Firing", &ConfigInstance.Player.ShowFiring, ConfigInstance.Player.FiringColour);
			ColorToggleRow("Reloading", &ConfigInstance.Player.ShowReloading, ConfigInstance.Player.ReloadColour);
			ColorToggleRow("Armor", &ConfigInstance.Player.ShowArmor, ConfigInstance.Player.ArmorColour);
			ToggleRow("Armor Value", &ConfigInstance.Player.ShowArmorValue);
			ColorToggleRow("Stamina", &ConfigInstance.Player.ShowStamina, ConfigInstance.Player.StaminaColour);
			ToggleRow("Spectator Alert", &ConfigInstance.Player.SpectatorAlert);
			EndCard();
		}

		if (MatchAny({ "Max Distance", "Font Size" }))
		{
			BeginCard("RANGE", kCardW);
			SliderIntRow("Max Distance", &ConfigInstance.Player.MaxDistance, 0, 1500, "m");
			SliderIntRow("Font Size", &ConfigInstance.Player.FontSize, 1, 24, "px");
			EndCard();
		}

		if (MatchAny({ "Role", "Looking", "State", "Box" }))
		{
			BeginCard("COLOURS", kCardW);
			ColorRow("Role", ConfigInstance.Player.RoleColour);
			ColorRow("Looking", ConfigInstance.Player.LookingColour);
			ColorRow("State", ConfigInstance.Player.StateColour);
			ColorRow("Box", ConfigInstance.Player.BoxColour);
			EndCard();
		}

		if (MatchAny({ "Enable World ESP", "Show Empty", "Show Distance", "Max Distance", "Font Size" }))
		{
			BeginCard("WORLD ESP", kCardW);
			ToggleRow("Enable World ESP", &ConfigInstance.World.Enabled);
			ToggleRow("Show Empty", &ConfigInstance.World.ShowEmpty);
			ToggleRow("Show Distance", &ConfigInstance.World.ShowDistance);
			ToggleRow("Security Alarms", &ConfigInstance.World.SecurityAlarms);
			ToggleRow("First Aid Cabinets", &ConfigInstance.World.FirstAidCabinets);
			ToggleRow("Crypto Workbenches", &ConfigInstance.World.CryptoWorkbenches);
			ToggleRow("Vehicle Trunks", &ConfigInstance.World.VehicleTrunks);
			ToggleRow("Drug Labs", &ConfigInstance.World.DrugWorkbenches);
			ToggleRow("Weed", &ConfigInstance.World.Weed);
			ToggleRow("Defense Blocks", &ConfigInstance.World.DefenseBlocks);

			SectionLabel("CARGO CATEGORIES");
			ToggleRow("All Cargo", &ConfigInstance.World.Cargos);
			ToggleRow("  Bushes", &ConfigInstance.World.CargoBushes);
			ToggleRow("  Crystals", &ConfigInstance.World.CargoCrystals);
			ToggleRow("  Drugs", &ConfigInstance.World.CargoDrugs);
			ToggleRow("  Cash Cargo", &ConfigInstance.World.CargoCash);
			ToggleRow("  Exchange", &ConfigInstance.World.CargoExchange);
			ToggleRow("  Boxes", &ConfigInstance.World.CargoBoxes);
			ToggleRow("  Goods", &ConfigInstance.World.CargoGoods);

			ToggleRow("House Info", &ConfigInstance.World.HouseEsp);
			SliderIntRow("House Range", &ConfigInstance.World.HouseMaxDistance, 50, 1000, "m");
			SliderIntRow("Max Distance", &ConfigInstance.World.MaxDistance, 0, 2000, "m");
			SliderIntRow("Min Cash / Value", &ConfigInstance.World.MinValueFilter, 0, 50000, " $");
			SliderIntRow("Font Size", &ConfigInstance.World.FontSize, 1, 24, "px");
			ToggleRow("Instant Loot", &ConfigInstance.World.InstantLoot, true);
			ToggleRow("Instant Safe & Door Unlock", &ConfigInstance.World.InstantUnlock, true);
			EndCard();
		}
	}
	ImGui::EndGroup();
}

void DrawRadar()
{
	// Radar settings and their display controls share one card, and the bounds
	// note lives with the bounds, so the page is two balanced cards.
	if (!g_singleColumn)
	{
		const float r1 = ImMax(CardNaturalHeight("RADAR"), CardNaturalHeight("MAP BOUNDS"));
		ForceCardHeight("RADAR", r1);
		ForceCardHeight("MAP BOUNDS", r1);
	}

	ImGui::BeginGroup();
	{
		BeginCard("RADAR", kCardW);
		ToggleRow("Enable Radar", &ConfigInstance.Radar.Enabled);
		ToggleRow("Show Players", &ConfigInstance.Radar.ShowPlayers);
		ToggleRow("Show Vehicles", &ConfigInstance.Radar.ShowVehicles);
		ToggleRow("Show Loot", &ConfigInstance.Radar.ShowLoot);
		ToggleRow("Rotate With Camera", &ConfigInstance.Radar.RotateWithCamera);
		ToggleRow("Full Map Window", &ConfigInstance.Radar.PopoutWindow);

		SectionLabel("DISPLAY");
		SliderFloatRow("Size", &ConfigInstance.Radar.Size, 100.f, 1000.f, "px");
		SliderFloatRow("Zoom", &ConfigInstance.Radar.Zoom, 0.1f, 10.f, "x");
		SliderFloatRow("Opacity", &ConfigInstance.Radar.Opacity, 0.0f, 1.f);

		SectionLabel("WEB RADAR (LOCALHOST)");
		bool webRunning = WebRadar::IsRunning();
		if (ToggleRow("Web Radar Server", &ConfigInstance.Radar.WebRadarEnabled))
		{
			if (ConfigInstance.Radar.WebRadarEnabled)
				WebRadar::Start(ConfigInstance.Radar.WebRadarPort);
			else
				WebRadar::Stop();
		}

		if (ConfigInstance.Radar.WebRadarEnabled)
		{
			if (SliderIntRow("Web Port", &ConfigInstance.Radar.WebRadarPort, 1024, 65535))
			{
				WebRadar::Start(ConfigInstance.Radar.WebRadarPort);
			}

			if (ButtonRow("", "Pop Out Web Radar", 170.f, true))
			{
				WebRadar::OpenInBrowser();
			}

			Note("URL: %s", WebRadar::GetUrl().c_str());
			Note("Open on any browser, second monitor, or mobile phone on Wi-Fi.");
		}
		EndCard();

		ImGui::Dummy(ImVec2(0, 10));
		BeginCard("IN-GAME MAP OVERLAY", kCardW);
		ToggleRow("Top-Left Minimap", &ConfigInstance.Radar.InGameMinimap);
		ToggleRow("Fullscreen Map (M)", &ConfigInstance.Radar.InGameFullMap);
		ToggleRow("Hide 3D ESP on Full Map", &ConfigInstance.Radar.Hide3DEspOnFullMap);
		SectionLabel("MARKERS");
		ToggleRow("Player Arrows", &ConfigInstance.Radar.InGameMapPlayers);
		ToggleRow("Vehicle Icons", &ConfigInstance.Radar.InGameMapVehicles);
		ToggleRow("Player Names (Full Map)", &ConfigInstance.Radar.InGameMapPlayerNames);
		ToggleRow("Dropped Bags", &ConfigInstance.Radar.InGameMapDroppedBags);
		ToggleRow("Valuables & Cargo", &ConfigInstance.Radar.InGameMapValuables);
		SliderFloatRow("Icon Scale", &ConfigInstance.Radar.InGameMapScale, 0.5f, 2.0f, "x");
		if (const auto snap = RobRunReader::GetSnapshot()) {
			if (snap->inGameMap.valid) {
				const auto& v = snap->inGameMap.fullscreenOpen ? snap->inGameMap.fullscreen : snap->inGameMap.minimap;
				char buf[96];
				std::snprintf(buf, sizeof(buf), "%s | camSize=%.0fm | rect=%s",
					snap->inGameMap.fullscreenOpen ? "Full Map (M)" : "Minimap",
					v.cameraSize, v.rectValid ? "OK" : "pending");
				Note(buf);
			} else {
				Note("Waiting for in-game map state...");
			}
		}
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn)
		ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("MAP BOUNDS", kCardW);
		Note("2D BVH collision maps calibrated to in-game world coordinates.");

		SectionLabel("MAP SELECTION");
		ToggleRow("Auto Map Select", &ConfigInstance.Radar.AutoMapSelect);
		Note("Automatically selects the 2D map for the current location.");

		const auto& mapFiles = Radar::GetMapFiles();
		if (!mapFiles.empty())
		{
			std::vector<const char*> ptrs(mapFiles.size());
			for (size_t i = 0; i < mapFiles.size(); ++i)
				ptrs[i] = mapFiles[i].c_str();

			int curMap = Radar::GetCurrentMapIndex();
			if (ComboRow("Active Map", &curMap, ptrs.data(), (int)ptrs.size()))
			{
				Radar::SetMap(curMap);
			}
		}

		SectionLabel("X AXIS");
		SliderFloatRow("Min X", &ConfigInstance.Radar.WorldMinX, -5000.f, 5000.f);
		SliderFloatRow("Max X", &ConfigInstance.Radar.WorldMaxX, -5000.f, 5000.f);
		SectionLabel("Z AXIS");
		SliderFloatRow("Min Z", &ConfigInstance.Radar.WorldMinZ, -5000.f, 5000.f);
		SliderFloatRow("Max Z", &ConfigInstance.Radar.WorldMaxZ, -5000.f, 5000.f);
		if (ButtonRow("", "Calibrate to Map", 150.f))
		{
			Radar::CalibrateBoundsForMap(Radar::GetCurrentMapName());
		}
		Note("The radar scales entity positions over the 2D BVH map using these bounds.");
		EndCard();
	}
	ImGui::EndGroup();
}

// ---- loot filter item row ---------------------------------------------------
// Multi-select for the loot filter list: click selects one row, ctrl-click
// toggles a row, shift-click extends from the anchor. The quick actions act on
// the selection when there is one, otherwise on the current category.
static std::set<std::string> s_lfSelected;
static std::string s_lfAnchor;
static std::string s_lfRangeTo;
static bool        s_lfRangeAdd = false;

void LootItemRow(const LootFilter::ItemDef& def, ItemSettings& item)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	ImGui::PushID(def.id);
	// Two registries can list the same object under different categories, so the
	// id alone is not a unique widget ID. The category joins it.
	ImGui::PushID(static_cast<int>(def.category));

	const ImGuiID rowId = window->GetID("##lr");
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	constexpr float rowH = 26.f;
	const ImRect bb(p, p + ImVec2(w, rowH));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, rowId)) {
		ImGui::PopID();
		ImGui::PopID();
		return;
	}

	bool hov, held;
	bool pressed = ImGui::ButtonBehavior(bb, rowId, &hov, &held);
	const ImVec2 mouse = ImGui::GetIO().MousePos;

	ImDrawList* dl = window->DrawList;

	if (hov)
		dl->AddRectFilled(bb.Min, bb.Max, IM_COL32(255, 255, 255, 8), 3.f);
	if (s_lfSelected.count(def.id))
		dl->AddRectFilled(bb.Min, bb.Max, IM_COL32(70, 140, 255, 46), 3.f);

	constexpr float toggleW = 36.f, toggleH = 18.f;
	constexpr float distW = 48.f, styleW = 62.f, ctrlGap = 6.f;

	const ImRect toggleRect(
		ImVec2(p.x + w - toggleW, p.y + (rowH - toggleH) * 0.5f),
		ImVec2(p.x + w, p.y + (rowH + toggleH) * 0.5f));
	const ImRect distRect(
		ImVec2(toggleRect.Min.x - ctrlGap - distW, p.y + 3.f),
		ImVec2(toggleRect.Min.x - ctrlGap, p.y + rowH - 3.f));
	const ImRect styleRect(
		ImVec2(distRect.Min.x - ctrlGap - styleW, p.y + 3.f),
		ImVec2(distRect.Min.x - ctrlGap, p.y + rowH - 3.f));

	// Edits on a selected row apply to the whole selection.
	const bool multiSel = s_lfSelected.size() > 1 && s_lfSelected.count(def.id) > 0;
	auto applyToSelection = [&](auto&& fn) {
		if (!multiSel) return;
		for (const auto& id : s_lfSelected)
			if (id != def.id) fn(ConfigInstance.LootFilter.GetItemMut(id.c_str()));
	};

	bool openFlags = false;
	if (pressed) {
		if (toggleRect.Contains(mouse)) {
			item.visible = !item.visible;
			if (multiSel) applyToSelection([&](ItemSettings& s) { s.visible = item.visible; });
		}
		else if (styleRect.Contains(mouse))
			openFlags = true;
		else {
			// Row body: selection. Ctrl toggles, Shift extends, plain replaces.
			const ImGuiIO& io = ImGui::GetIO();
			if (io.KeyShift && !s_lfAnchor.empty()) {
				s_lfRangeTo = def.id;
				s_lfRangeAdd = io.KeyCtrl;
			} else if (io.KeyCtrl) {
				if (s_lfSelected.count(def.id)) s_lfSelected.erase(def.id);
				else s_lfSelected.insert(def.id);
				s_lfAnchor = def.id;
			} else {
				s_lfSelected.clear();
				s_lfSelected.insert(def.id);
				s_lfAnchor = def.id;
			}
		}
	}

	if (hov && distRect.Contains(mouse)) {
		float wheel = ImGui::GetIO().MouseWheel;
		if (wheel != 0.f) {
			const int delta = (int)(wheel * 50.f);
			auto adjustRange = [&](ItemSettings& s) {
				s.maxDistance = ImClamp(s.maxDistance + delta, 0, 3000);
				s.useGlobalDistance = s.maxDistance == 0;
			};
			adjustRange(item);
			applyToSelection(adjustRange);
		}
	}

	static std::map<ImGuiID, float> s_lrAnim;
	float& ta = s_lrAnim[rowId];
	ta = ImLerp(ta, item.visible ? 1.f : 0.f, ImGui::GetIO().DeltaTime * 14.f);

	dl->AddRectFilled(toggleRect.Min, toggleRect.Max,
		C(Mix(kTrack, kAccent, ta), 0.35f + 0.65f * ta), toggleH * 0.5f);
	if (ta > 0.01f) {
		dl->PushClipRect(toggleRect.Min, toggleRect.Max, true);
		dl->AddShadowCircle(
			ImVec2((toggleRect.Min.x + toggleRect.Max.x) * 0.5f,
				(toggleRect.Min.y + toggleRect.Max.y) * 0.5f),
			toggleH * 0.5f, C(kAccent, 0.30f * ta), 18.f, ImVec2(0, 0), 0);
		dl->PopClipRect();
	}
	float kx = ImLerp(toggleRect.Min.x + 9.f, toggleRect.Max.x - 9.f, ta);
	dl->AddCircleFilled(ImVec2(kx, (toggleRect.Min.y + toggleRect.Max.y) * 0.5f),
		6.f, C(Mix(kTextDim, ImVec4(1, 1, 1, 1), ta)));

	bool distHov = hov && distRect.Contains(mouse);
	dl->AddRectFilled(distRect.Min, distRect.Max,
		C(ImVec4(1, 1, 1, distHov ? 0.07f : 0.04f)), 4.f);
	char distBuf[16];
	if (item.useGlobalDistance || item.maxDistance <= 0)
		strcpy_s(distBuf, "Inherited");
	else
		std::snprintf(distBuf, sizeof(distBuf), "%dm", item.maxDistance);
	float dtw = TextW(font::inter_medium_small, distBuf);
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		ImVec2(distRect.Min.x + (distW - dtw) * 0.5f,
			RowTextY(p.y, rowH, font::inter_medium_small)),
		C(distHov ? kText : kTextDim), distBuf);

	// Render-mode chip: a compact read-out of the flag set that opens the
	// checkbox popup. I=icon N=name D=dot M=distance in metres $=price.
	bool styleHov = hov && styleRect.Contains(mouse);
	dl->AddRectFilled(styleRect.Min, styleRect.Max,
		C(ImVec4(1, 1, 1, styleHov ? 0.09f : 0.05f)), 4.f);
	char flagBuf[32] = "";
	if (item.Has(LootFilter::RenderIcon))     strcat_s(flagBuf, "I ");
	if (item.Has(LootFilter::RenderName))     strcat_s(flagBuf, "N ");
	if (item.Has(LootFilter::RenderDot))      strcat_s(flagBuf, "D ");
	if (item.Has(LootFilter::RenderDistance)) strcat_s(flagBuf, "M ");
	if (item.Has(LootFilter::RenderPrice))    strcat_s(flagBuf, "$");
	if (!flagBuf[0])                          strcpy_s(flagBuf, "-");
	float stw = TextW(font::inter_medium_small, flagBuf);
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		ImVec2(styleRect.Min.x + (styleW - stw) * 0.5f,
			RowTextY(p.y, rowH, font::inter_medium_small)),
		C(styleHov ? kText : kTextDim), flagBuf);

	if (openFlags)
		ImGui::OpenPopup("##lf_flags");
	if (ImGui::BeginPopup("##lf_flags")) {
		bool changed = false;
		ImGui::TextUnformatted(def.displayName);
		ImGui::Separator();
		for (int i = 0; i < (int)std::size(LootFilter::RenderFlagBits); ++i) {
			const std::uint8_t bit = LootFilter::RenderFlagBits[i];
			// No price mined for this item: offering the toggle would only
			// produce an empty line, so it is not shown at all.
			if (bit == LootFilter::RenderPrice && def.price == 0)
				continue;
			bool on = item.Has(bit);
			if (ImGui::Checkbox(LootFilter::RenderFlagNames[i], &on)) {
				item.Set(bit, on);
				changed = true;
			}
		}
		ImGui::Separator();
		{
			char nameBuf[96]{};
			std::strncpy(nameBuf, item.customName.c_str(), sizeof(nameBuf) - 1);
			ImGui::SetNextItemWidth(220.f);
			if (ImGui::InputTextWithHint("##custom_name", "custom name (blank = catalog)",
					nameBuf, sizeof(nameBuf)))
				item.customName = nameBuf;
		}
		ImGui::Checkbox("Use global distance", &item.useGlobalDistance);
		ImGui::BeginDisabled(item.useGlobalDistance);
		ImGui::SetNextItemWidth(120.f);
		ImGui::SliderInt("Range", &item.maxDistance, 0, 3000, "%d m");
		ImGui::EndDisabled();
		ImGui::Separator();
		if (ImGui::Button("All")) {
			for (std::uint8_t bit : LootFilter::RenderFlagBits) item.Set(bit, true);
			changed = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("None")) { item.flags = 0; changed = true; }
		ImGui::SameLine();
		if (ImGui::Button("Default")) {
			item.flags = def.iconSprite[0]
				? (std::uint8_t)(LootFilter::RenderIcon | LootFilter::RenderName | LootFilter::RenderPrice)
				: (std::uint8_t)(LootFilter::RenderDot | LootFilter::RenderName | LootFilter::RenderPrice);
			changed = true;
		}
		if (changed && multiSel) {
			const std::uint8_t applied = item.flags;
			applyToSelection([&](ItemSettings& s) { s.flags = applied; });
		}
		ImGui::EndPopup();
	}

	uint32_t argb = LootFilter::RarityColors[static_cast<int>(def.rarity)];
	ImU32 rarityCol = (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFFu) | ((argb & 0xFFu) << 16);
	dl->AddCircleFilled(ImVec2(p.x + 6.f, p.y + rowH * 0.5f), 4.f, rarityCol);

	float nameStartX = p.x + 18.f;
	if (def.iconSprite[0] && ImGui::IsRectVisible(bb.Min, bb.Max)) {
		// Decode lazily and only for rows that are actually on screen: the
		// catalog is 500+ rows and every miss is a PNG decode.
		auto icon = IconSprites::Get(def.iconSprite);
		if (icon.srv) {
			constexpr float iconSz = 20.f;
			float iconX = p.x + 16.f;
			float iconY = p.y + (rowH - iconSz) * 0.5f;
			dl->AddImage(reinterpret_cast<ImTextureID>(icon.srv),
				ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
			nameStartX = iconX + iconSz + 4.f;
		}
	}

	dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(styleRect.Min.x - 8.f, p.y + rowH), true);
	const ImU32 nameColour = C(Mix(kTextDim, kText, item.visible ? 0.8f : 0.f));
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(nameStartX, RowTextY(p.y, rowH, font::inter_medium_widget)),
		nameColour, def.displayName);

	// Which category the row belongs to, so "All Items" is readable at a glance.
	{
		const float nameW = TextW(font::inter_medium_widget, def.displayName);
		const char* catName = LootFilter::CategoryNames[(int)def.category];
		dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
			ImVec2(nameStartX + nameW + 8.f, RowTextY(p.y, rowH, font::inter_medium_small)),
			C(ImVec4(kTextDim.x, kTextDim.y, kTextDim.z, 0.75f)), catName);
	}
	dl->PopClipRect();

	ImGui::PopID();
	ImGui::PopID();
}

static bool CaseContains(const char* haystack, const char* needle)
{
	if (!needle[0]) return true;
	for (const char* h = haystack; *h; h++) {
		const char* hp = h;
		const char* np = needle;
		while (*hp && *np) {
			char a = *hp; if (a >= 'A' && a <= 'Z') a += 32;
			char b = *np; if (b >= 'A' && b <= 'Z') b += 32;
			if (a != b) break;
			hp++; np++;
		}
		if (!*np) return true;
	}
	return false;
}

// Func<int>: the delegate closes over the value, so the number sits in the target.
static bool ReadFuncIntValue(std::uintptr_t object, std::uintptr_t funcOffset, int& out)
{
	if (object <= 0x10000) return false;
	const std::uintptr_t fn = RobRunMemory::ReadPtr(object + funcOffset);
	if (fn <= 0x10000) return false;
	const std::uintptr_t closure = RobRunMemory::ReadPtr(fn + 0x20);   // delegate target, not method_ptr
	if (closure <= 0x10000) return false;
	for (const std::uintptr_t off : { (std::uintptr_t)0x10, (std::uintptr_t)0x18 }) {
		const int value = RobRunMemory::ReadInt32(closure + off);
		if (value >= 10 && value < 100000000) { out = value; return true; }
	}
	return false;
}

// SellPriceModifierFunc (0x80) -> InventoryItemSellPriceModifier -> Modifier (float).
// This is the factor between the buy price and what the game actually pays.

// Which resale column applies to an item. The registry holds one float per
// family; the families are few and named, so the mapping is explicit rather
// than a guess: everything else stays on Common.
static std::string CaseInsensitiveLower(const std::string& value);

static float ResaleModifierFor(const std::string& id, const std::unordered_map<std::string, float>& modifiers,
	std::string& usedName)
{
	auto pick = [&](const char* name) -> float {
		const auto hit = modifiers.find(name);
		if (hit != modifiers.end()) { usedName = name; return hit->second; }
		return 0.f;
	};
	const std::string lower = CaseInsensitiveLower(id);
	if (lower.find("badge") != std::string::npos) { const float v = pick("PoliceBadge"); if (v > 0.f) return v; }
	if (lower.find("radio") != std::string::npos) { const float v = pick("CopRadio"); if (v > 0.f) return v; }
	if (lower.find("drill") != std::string::npos) { const float v = pick("Drills"); if (v > 0.f) return v; }
	if (lower.find("key") != std::string::npos) { const float v = pick("Keys"); if (v > 0.f) return v; }
	if (lower.find("gas") != std::string::npos && lower.find("grenade") != std::string::npos) {
		const float v = pick("GasGrenades"); if (v > 0.f) return v;
	}
	if (lower.find("cash") != std::string::npos || lower.find("money") != std::string::npos) {
		const float v = pick("Cash"); if (v > 0.f) return v;
	}
	const float common = pick("Common");
	return common;
}

static std::string CaseInsensitiveLower(const std::string& value)
{
	std::string out;
	out.reserve(value.size());
	for (unsigned char c : value) out.push_back((char)std::tolower(c));
	return out;
}

static bool ReadSellModifier(std::uintptr_t desc, float& out)
{
	if (desc <= 0x10000) return false;
	const std::uintptr_t fn = RobRunMemory::ReadPtr(desc + 0x80);
	if (fn <= 0x10000) return false;
	const std::uintptr_t closure = RobRunMemory::ReadPtr(fn + 0x20);
	if (closure <= 0x10000) return false;
	for (const std::uintptr_t off : { (std::uintptr_t)0x10, (std::uintptr_t)0x18 }) {
		const std::uintptr_t modifier = RobRunMemory::ReadPtr(closure + off);
		if (modifier <= 0x10000) continue;
		const int bits = RobRunMemory::ReadInt32(modifier + 0x20);
		float value = 0.f;
		std::memcpy(&value, &bits, sizeof(value));
		if (std::isfinite(value) && value > 0.f && value <= 20.f) { out = value; return true; }
	}
	return false;
}

static bool CaseInsensitiveEquals(const std::string& a, const std::string& b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
	return true;
}

void DrawLootFilter()
{
	static std::string s_dumpNote;
	static std::string s_configNote;
	auto& lf = ConfigInstance.LootFilter;

	if (lf.Items.empty())
		lf.InitDefaults();

	static char s_search[128] = "";
	static bool s_searchInit = false;
	if (!s_searchInit) {
		std::strncpy(s_search, lf.SearchQuery.c_str(), sizeof(s_search) - 1);
		s_searchInit = true;
	}

	const float fullW = ImGui::GetContentRegionAvail().x;
	BeginCard("LOOT FILTER", fullW);

	ToggleRow("Enable Loot Filter", &lf.Enabled);

	// ---- sizing ------------------------------------------------------------
	// Settings in two columns: a table, not nested child windows.
	if (ImGui::BeginTable("##lf_columns", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
	ImGui::TableNextColumn();
	SectionLabel("PER-CATEGORY SIZING");
	static const std::array<const char*, (std::size_t)LootFilter::Category::COUNT> kSizeCats = [] {
		std::array<const char*, (std::size_t)LootFilter::Category::COUNT> a{};
		for (std::size_t i = 0; i < a.size(); ++i) a[i] = LootFilter::CategoryNames[i];
		return a;
	}();
	ComboRow("Category##size", &lf.SizingCategory, kSizeCats.data(),
		(int)kSizeCats.size(), 190.f);
	{
		const int sel = std::clamp(lf.SizingCategory, 0, (int)kSizeCats.size() - 1);
		auto& sizing = lf.Sizing[(std::size_t)sel];
		SliderIntRow("Icon Size", &sizing.IconSize, 0, 256, " px  (0 = global)");
		SliderIntRow("Font Size", &sizing.FontSize, 0, 40, " px  (0 = global)");
		SliderFloatRow("Dot Size", &sizing.DotSize, 0.f, 12.f, " px  (0 = global)");
		SliderIntRow("Max Distance", &sizing.MaxDistance, 0, 3000, " m  (0 = per item)");
	}

	SectionLabel("DISPLAY & MAP");
	ToggleRow("Show on Map", &lf.ShowOnMap);
	ToggleRow("Show on Minimap", &lf.ShowOnMinimap);
	ToggleRow("Hide Empty Containers", &lf.HideEmpty);
	ToggleRow("Hide Looted Items", &lf.FilterTakenLoot);
	ToggleRow("Read Vehicle/Table Inventories", &lf.ReadVehicleInventories);
	SliderFloatRow("Base Dot Size", &lf.BaseDotSize, 1.5f, 8.f, " px");
	SliderIntRow("ESP Font Size", &lf.FontSize, 8, 32, " px");
	SliderIntRow("ESP Icon Size", &lf.IconSize, 16, 256, " px");
	SliderIntRow("Global Max Distance", &lf.GlobalMaxDistance, 0, 3000, " m");

	ImGui::TableNextColumn();
	SectionLabel("VALUE TIERS");
	ToggleRow("Filter By Min Price", &lf.FilterByMinPrice);
	SliderIntRow("Min Price", &lf.MinPrice, 0, 50000, " $");
	SliderIntRow("High Value >=", &lf.HighValueThreshold, 1000, 100000, " $");
	ColorRow("High Value Color", lf.HighValueColour);
	ToggleRow("High Value Pulse", &lf.HighValuePulse);
	SliderIntRow("Mid Value >=", &lf.MidValueThreshold, 500, 50000, " $");
	ColorRow("Mid Value Color", lf.MidValueColour);
	ColorRow("Standard Color", lf.StandardColour);
	{
		const float gap = 6.f;
		const float bw = (ImGui::GetContentRegionAvail().x - gap) * 0.5f - 2.f;
		if (InlineButton("Export config", bw)) {
			const std::wstring path = PickConfigPath(true);
			if (path.empty()) {
				s_configNote = "Export cancelled";
			} else {
				std::ofstream out(path.c_str(), std::ios::trunc);
				if (out) {
					out << lf.ExportJson();
					s_configNote = "Exported config";
				} else {
					s_configNote = "Could not write that file";
				}
			}
		}
		ImGui::SameLine(0.f, gap);
		if (InlineButton("Reset filter", bw, true)) {
			lf.ResetToDefaults();
			s_configNote = "Loot filter reset to defaults";
		}
		ImGui::SameLine(0.f, gap);
		if (InlineButton("Import config", bw)) {
			const std::wstring path = PickConfigPath(false);
			if (path.empty()) {
				s_configNote = "Import cancelled";
			} else {
				std::ifstream in(path.c_str());
				if (in) {
					std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
					s_configNote = lf.ImportJson(text)
						? "Imported config"
						: "That file contained no known keys";
				} else {
					s_configNote = "Could not read that file";
				}
			}
		}
		ImGui::SameLine(0.f, gap);
		if (InlineButton("Paste filter code", bw)) {
			const char* clip = ImGui::GetClipboardText();
			if (clip && *clip) lf.ImportCode(clip);
		}
	}

	SectionLabel("LIVE STATS");
	{
		const auto snapshot = RobRunReader::GetSnapshot();
		if (snapshot && snapshot->world && snapshot->world->valid) {
			int totalCash = 0, highestCash = 0, cashCount = 0;
			for (const auto& ent : snapshot->world->entities) {
				if (ent.hasCashAmount && ent.cashAmount > 0) {
					totalCash += ent.cashAmount;
					cashCount++;
					if (ent.cashAmount > highestCash) highestCash = ent.cashAmount;
				}
			}
			Note("Entities: %zu | Cash sources: %d ($%s)",
				snapshot->world->entities.size(), cashCount, FormatCash(totalCash).c_str());
			Note("Top value: $%s", FormatCash(highestCash).c_str());
		} else {
			Note("No world snapshot. Live stats appear in-raid.");
		}
	ImGui::EndTable();
	}
	}

	// ---- filter + list -----------------------------------------------------
	SectionLabel("ITEMS");
	static const std::array<const char*, (std::size_t)LootFilter::Category::COUNT + 1> kCatLabels = [] {
		std::array<const char*, (std::size_t)LootFilter::Category::COUNT + 1> a{};
		a[0] = "All Items";
		for (std::size_t i = 1; i < a.size(); ++i)
			a[i] = LootFilter::CategoryNames[i - 1];
		return a;
	}();
	ComboRow("Category", &lf.SelectedCategory, kCatLabels.data(), (int)kCatLabels.size(), 190.f);

	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	if (ImGui::InputTextWithHint("##lf_search", "Search items...", s_search, sizeof(s_search)))
		lf.SearchQuery = s_search;

	auto inCategory = [&](const LootFilter::ItemDef& def) {
		return lf.SelectedCategory <= 0 ||
			static_cast<int>(def.category) == lf.SelectedCategory - 1;
	};
	{
		const bool hasSelection = !s_lfSelected.empty();
		constexpr int kSlots = 5;
		const float gap = 5.f;
		const float bw = (ImGui::GetContentRegionAvail().x - gap * (kSlots - 1)) / kSlots - 2.f;
		auto slot = [&](int index) { if (index > 0) ImGui::SameLine(0.f, gap); };

		slot(0);
		if (InlineButton("Show Category", bw, true)) {
			for (const auto& def : LootFilter::kItemCatalog)
				if (inCategory(def)) lf.GetItemMut(def.id).visible = true;
		}
		slot(1);
		if (InlineButton("Hide Category", bw)) {
			for (const auto& def : LootFilter::kItemCatalog)
				if (inCategory(def)) lf.GetItemMut(def.id).visible = false;
		}
		ImGui::BeginDisabled(!hasSelection);
		slot(2);
		if (InlineButton("Show Selected", bw)) {
			for (const auto& id : s_lfSelected) lf.GetItemMut(id.c_str()).visible = true;
		}
		slot(3);
		if (InlineButton("Hide Selected", bw)) {
			for (const auto& id : s_lfSelected) lf.GetItemMut(id.c_str()).visible = false;
		}
		slot(4);
		if (InlineButton("Clear Selection", bw)) {
			s_lfSelected.clear();
			s_lfAnchor.clear();
		}
		ImGui::EndDisabled();
	}

	static const char* kSortLabels[] = {
		"Name  >  Rarity", "Rarity  >  Name", "Price  >  High to Low"
	};
	ComboRow("Sort", &lf.SortMode, kSortLabels, (int)std::size(kSortLabels), 190.f);

	static const char* kPriceLabels[] = { "Sell price", "Buy price", "Unlock price" };
	ComboRow("Price shown", &lf.PriceMode, kPriceLabels, (int)std::size(kPriceLabels), 190.f);

	static const char* kAuthorityLabels[] = {
		"1 - lowest", "2", "3", "4", "5 - highest"
	};
	int authorityIndex = std::clamp(lf.AuthorityLevel - 1, 0, 4);
	if (ComboRow("Authority level", &authorityIndex, kAuthorityLabels, 5, 190.f))
		lf.AuthorityLevel = authorityIndex + 1;

	std::vector<const LootFilter::ItemDef*> shown;
	shown.reserve(256);
	for (const auto& def : LootFilter::kItemCatalog) {
		if (lf.SelectedCategory > 0 &&
			static_cast<int>(def.category) != lf.SelectedCategory - 1)
			continue;
		if (s_search[0] && !CaseContains(def.displayName, s_search))
			continue;
		shown.push_back(&def);
	}

	std::sort(shown.begin(), shown.end(), [&](const LootFilter::ItemDef* a, const LootFilter::ItemDef* b) {
		switch (std::clamp(lf.SortMode, 0, 2)) {
		case 1:
			if (a->rarity != b->rarity) return a->rarity < b->rarity;
			return std::string_view(a->displayName) < std::string_view(b->displayName);
		case 2:
			if (a->price != b->price) return a->price > b->price;
			return std::string_view(a->displayName) < std::string_view(b->displayName);
		default:
			{
				const int cmp = std::string_view(a->displayName).compare(std::string_view(b->displayName));
				if (cmp != 0) return cmp < 0;
				return a->rarity < b->rarity;
			}
		}
	});

	if (!ImGui::GetIO().WantTextInput &&
		ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
		for (const auto* def : shown) s_lfSelected.insert(def->id);
		s_lfAnchor = shown.empty() ? std::string() : shown.front()->id;
	}

	{
		int enabled = 0;
		for (const auto* def : shown)
			if (lf.IsItemVisible(def->id)) enabled++;
		Note("%d shown | %d enabled | %d selected", (int)shown.size(), enabled,
			(int)s_lfSelected.size());
	}

	for (const auto* def : shown)
		LootItemRow(*def, lf.GetItemMut(def->id));

	if (shown.empty())
		Note("No items match the current filter.");

	// Bottom of ITEMS: dump the price picture to a file to hand back.
	{
		static std::string s_configNote;
	static bool s_flagsMigrated = false;
		const float gap = 8.f;
		const float bw = (ImGui::GetContentRegionAvail().x - gap) * 0.5f - 2.f;
		if (InlineButton("Dump item prices", bw, true)) {
			wchar_t exePath[MAX_PATH]{};
			GetModuleFileNameW(nullptr, exePath, MAX_PATH);
			std::wstring dir(exePath);
			const std::size_t slash = dir.find_last_of(L"\\/");
			if (slash != std::wstring::npos) dir.resize(slash + 1);
			const std::wstring path = dir + L"item-prices-dump.tsv";

			std::ofstream out(path.c_str(), std::ios::trunc);
			if (out) {
				out << "# VeoxDMA price dump - hand this file back so the table can be completed.\n";
				out << "# id\tname\tcategory\trarity\tcatalog_price\tbuy_price\tsell_base\n";

				// The registry holds one static PER ITEM, built at launch, so this
				// lists every item the build knows - not just what is on the map.
				int registryRows = 0;
				std::unordered_map<std::string, std::pair<int, int>> prices;
				std::unordered_map<std::string, float> modifiers;
				{
					const std::uintptr_t mClass = Il2Cpp::FindClassByName("InventoryItemSellPriceModifiers");
					if (mClass > 0x10000) {
						const std::uintptr_t mStatics = Il2Cpp::StaticFields(mClass);
						if (mStatics > 0x10000) {
							for (const auto& mf : Il2Cpp::Fields(mClass, false)) {
								if (mf.offset <= 0) continue;
								const std::uintptr_t entry = RobRunMemory::ReadPtr(mStatics + (std::uintptr_t)mf.offset);
								if (entry <= 0x10000) continue;
								const int bits = RobRunMemory::ReadInt32(entry + 0x20);
								float value = 0.f;
								std::memcpy(&value, &bits, sizeof(value));
								if (!std::isfinite(value) || value <= 0.f || value > 20.f) continue;
								std::string name = mf.name;
								const std::size_t close = name.find('>');
								if (!name.empty() && name[0] == '<') name = name.substr(1, close == std::string::npos ? std::string::npos : close - 1);
								modifiers[name] = value;
							}
						}
					}
				}
				const std::uintptr_t klass = Il2Cpp::FindClassByName("InventoryItemDescriptions");
				if (klass > 0x10000) {
					const std::uintptr_t statics = Il2Cpp::StaticFields(klass);
					if (statics > 0x10000) {
						for (const auto& field : Il2Cpp::Fields(klass, false)) {
							if (field.offset <= 0) continue;
							const std::uintptr_t desc = RobRunMemory::ReadPtr(
								statics + (std::uintptr_t)field.offset);
							if (desc <= 0x10000) continue;

							std::wstring wideId;
							if (!RobRunMemory::ReadString(RobRunMemory::ReadPtr(desc + 0x10), wideId))
								continue;
							const std::string id = RobRunMemory::WideToUtf8(wideId);
							if (id.empty() || id.size() > 64) continue;

							int basePrice = 0, sellPrice = 0;
							ReadFuncIntValue(desc, 0x78, basePrice);
							ReadFuncIntValue(desc, 0x88, sellPrice);
							if (basePrice <= 0 && sellPrice <= 0) continue;
							prices[id] = { basePrice, sellPrice };
							registryRows++;
						}
					}
				}

				// Live entity values as a cross-check for anything the registry missed.
				std::unordered_map<std::string, int> liveSell;
				if (const auto snapshot = RobRunReader::GetSnapshot()) {
					if (snapshot->world && snapshot->world->valid) {
						for (const auto& ent : snapshot->world->entities) {
							if (ent.hasValue && ent.value > 0 && !ent.label.empty())
								liveSell[ent.label] = ent.value;
						}
					}
				}

				for (const auto& def : LootFilter::kItemCatalog) {
					int basePrice = 0, sellPrice = 0;
					auto hit = prices.find(def.id);
					if (hit == prices.end()) {
						for (const auto& kv : prices) {
							if (CaseInsensitiveEquals(kv.first, def.id)) { hit = prices.find(kv.first); break; }
						}
					}
					if (hit != prices.end()) { basePrice = hit->second.first; sellPrice = hit->second.second; }
					if (sellPrice <= 0) {
						for (const auto& kv : liveSell) {
							if (CaseInsensitiveEquals(kv.first, def.id)) { sellPrice = kv.second; break; }
						}
					}
					out << def.id << '\t' << def.displayName << '\t'
						<< LootFilter::CategoryNames[(int)def.category] << '\t'
						<< LootFilter::RarityNames[(int)def.rarity] << '\t'
						<< def.price << '\t' << basePrice << '\t' << sellPrice << '\n';
				}
				// Sell modifiers live in their own registry: Common, Keys, Drills,
				// GasGrenades, Cash and friends, each with the float that turns a
				// buy price into what the game pays.
				out << "\n# sell modifier\tvalue\n";
				const std::uintptr_t modifierClass =
					Il2Cpp::FindClassByName("InventoryItemSellPriceModifiers");
				int modifierRows = 0;
				if (modifierClass > 0x10000) {
					const std::uintptr_t modifierStatics = Il2Cpp::StaticFields(modifierClass);
					if (modifierStatics > 0x10000) {
						for (const auto& field : Il2Cpp::Fields(modifierClass, false)) {
							if (field.offset <= 0) continue;
							const std::uintptr_t entry = RobRunMemory::ReadPtr(
								modifierStatics + (std::uintptr_t)field.offset);
							if (entry <= 0x10000) continue;
							const int bits = RobRunMemory::ReadInt32(entry + 0x20);
							float value = 0.f;
							std::memcpy(&value, &bits, sizeof(value));
							if (!std::isfinite(value) || value <= 0.f || value > 20.f) continue;
							out << field.name << '\t' << value << '\n';
							modifierRows++;
						}
					}
				}
				out.flush();
				// Authority ladder: the sell price is base * (1 + modifier), so the
				// modifiers are what turn one base number into a price per level.
				out << "\n# authority level\tpoints_required\tcargo_modifier\tloot_modifier\n";
				const std::uintptr_t ladderClass =
					Il2Cpp::FindClassByName("RobberAuthorityLevelDescriptions");
				int ladderRows = 0;
				if (ladderClass > 0x10000) {
					const std::uintptr_t ladderStatics = Il2Cpp::StaticFields(ladderClass);
					if (ladderStatics > 0x10000) {
						for (const auto& field : Il2Cpp::Fields(ladderClass, false)) {
							if (field.offset <= 0) continue;
							const std::uintptr_t level = RobRunMemory::ReadPtr(
								ladderStatics + (std::uintptr_t)field.offset);
							if (level <= 0x10000) continue;
							const int points = RobRunMemory::ReadInt32(level + 0x28);
							const int cargoBits = RobRunMemory::ReadInt32(level + 0x2C);
							const int lootBits = RobRunMemory::ReadInt32(level + 0x30);
							float cargoMod = 0.f, lootMod = 0.f;
							std::memcpy(&cargoMod, &cargoBits, sizeof(cargoMod));
							std::memcpy(&lootMod, &lootBits, sizeof(lootMod));
							if (!std::isfinite(cargoMod) || !std::isfinite(lootMod)) continue;
							if (std::fabs(cargoMod) > 100.f || std::fabs(lootMod) > 100.f) continue;
							out << field.name << '\t' << points << '\t'
								<< cargoMod << '\t' << lootMod << '\n';
							ladderRows++;
						}
					}
				}
				out.flush();
				s_configNote = "Wrote " + std::to_string(LootFilter::kItemCount) + " catalog rows, "
					+ std::to_string(registryRows) + " from the item registry, " + std::to_string(ladderRows) + " authority levels, " + std::to_string(modifierRows) + " sell modifiers -> item-prices-dump.tsv";
			} else {
				s_configNote = "Could not write item-prices-dump.tsv";
			}
		}
		ImGui::SameLine(0.f, gap);
		if (InlineButton("Copy filter code", bw)) ImGui::SetClipboardText(lf.ExportCode().c_str());
		if (!s_dumpNote.empty())
			Note("%s", s_dumpNote.c_str());
	}

	if (!s_lfRangeTo.empty()) {
		int a = -1, b = -1;
		for (int i = 0; i < (int)shown.size(); ++i) {
			if (shown[i]->id == s_lfAnchor) a = i;
			if (shown[i]->id == s_lfRangeTo) b = i;
		}
		if (a >= 0 && b >= 0) {
			if (!s_lfRangeAdd) s_lfSelected.clear();
			const int lo = (std::min)(a, b), hi = (std::max)(a, b);
			for (int i = lo; i <= hi; ++i)
				s_lfSelected.insert(shown[i]->id);
		}
		s_lfRangeTo.clear();
	}

	EndCard();
}

void DrawAim()
{
	using namespace Base::Hardware;

	static bool aimVarsInit = false;
	static int deviceIndex = 0;
	static char comPort[16] = "";
	static char ipAddr[64] = "";
	static char netPort[16] = "8899";
	static char macId[16] = "";
	static int baudIndex = 0;
	static bool manualPort = false;
	static bool ferrumController = false;
	static bool needsRefresh = true;

	if (!aimVarsInit) {
		deviceIndex = ConfigInstance.Aim.DeviceIndex;
		std::strncpy(comPort, ConfigInstance.Aim.ComPort.c_str(), sizeof(comPort) - 1);
		std::strncpy(ipAddr, ConfigInstance.Aim.IpAddr.c_str(), sizeof(ipAddr) - 1);
		std::strncpy(netPort, ConfigInstance.Aim.NetPort.c_str(), sizeof(netPort) - 1);
		std::strncpy(macId, ConfigInstance.Aim.MacId.c_str(), sizeof(macId) - 1);
		baudIndex = ConfigInstance.Aim.BaudIndex;
		manualPort = ConfigInstance.Aim.ManualPort;
		ferrumController = ConfigInstance.Aim.FerrumController;
		aimVarsInit = true;
	}

	static const char* deviceNames[] = { "Off", "Kmbox USB", "Kmbox Net", "Makcu", "Makcu Controller",
		"Ferrum", "Makcu Medius", "Kmbox NB Pro" };
	static const DeviceKind deviceKinds[] = { DeviceKind::Off, DeviceKind::KmboxUsb, DeviceKind::KmboxNet,
		DeviceKind::Makcu, DeviceKind::Controller, DeviceKind::Ferrum, DeviceKind::Medius,
		DeviceKind::KmboxNbPro };
	static const int baudRates[] = { 115200, 921600, 1500000, 2000000, 3000000, 4000000 };
	static const char* baudLabels[] = { "115200", "921600", "1500000", "2000000", "3000000", "4000000" };

	// Four rows of two cards, paired by natural height so no card is left with
	// a large empty area under its controls.
	if (!g_singleColumn)
	{
		const float r1 = ImMax(CardNaturalHeight("AIMBOT"), CardNaturalHeight("MEMORY AIM"));
		const float r2 = ImMax(CardNaturalHeight("AIM TUNING"), CardNaturalHeight("AIM VISUALS"));
		const float r3 = ImMax(CardNaturalHeight("WEAPON"), CardNaturalHeight("DEVICE"));
		const float r4 = ImMax(CardNaturalHeight("PREDICTION"), CardNaturalHeight("LIVE STATE"));
		ForceCardHeight("AIMBOT", r1);
		ForceCardHeight("MEMORY AIM", r1);
		ForceCardHeight("AIM TUNING", r2);
		ForceCardHeight("AIM VISUALS", r2);
		ForceCardHeight("WEAPON", r3);
		ForceCardHeight("DEVICE", r3);
		ForceCardHeight("PREDICTION", r4);
		ForceCardHeight("LIVE STATE", r4);
	}

	ImGui::BeginGroup();
	{
		BeginCard("AIMBOT", kCardW);
		ToggleRow("Enable Aim", &ConfigInstance.Aim.Enabled);

		static const char* aimTypes[] = { "Memory Aim", "Hardware Aim" };
		ComboRow("Aim Type", &ConfigInstance.Aim.AimType, aimTypes, IM_ARRAYSIZE(aimTypes),
			150.f, ConfigInstance.Aim.AimType == 0);

		static const char* bones[] = { "Head", "Neck", "Chest", "Pelvis" };
		ComboRow("Target Bone", &ConfigInstance.Aim.TargetBone, bones, IM_ARRAYSIZE(bones));

		static const char* priorities[] = { "Crosshair FOV", "3D Distance", "Lowest Health" };
		ComboRow("Target Priority", &ConfigInstance.Aim.TargetPriority, priorities, IM_ARRAYSIZE(priorities));

		ToggleRow("Dynamic Bone Fallback", &ConfigInstance.Aim.DynamicBoneFallback);

		ComboRow("Aim Key", &ConfigInstance.Aim.AimKey, Aim::KeyNames(), Aim::KeyCount());
		EndCard();

		BeginCard("AIM TUNING", kCardW);
		SliderIntRow("FOV", &ConfigInstance.Aim.Fov, 5, 180, " deg");
		SliderIntRow("Smoothness", &ConfigInstance.Aim.Smoothness, 1, 100);
		if (ConfigInstance.Aim.AimType == 1)
			SliderFloatRow("Hardware Strength", &ConfigInstance.Aim.HardwareGain, 0.25f, 5.f, "x");
		SliderIntRow("Max Distance", &ConfigInstance.Aim.MaxDistance, 10, 1000, "m");
		EndCard();

		BeginCard("WEAPON", kCardW);
		ToggleRow("No Recoil", &ConfigInstance.Aim.NoRecoil, true);
		ToggleRow("No Spread", &ConfigInstance.Aim.NoSpread, true);
		ToggleRow("Dynamic Recoil Curve", &ConfigInstance.Aim.DynamicRecoilCompensation, true);
		if (ConfigInstance.Aim.DynamicRecoilCompensation)
			SliderFloatRow("Curve Strength", &ConfigInstance.Aim.RecoilStrength, 0.1f, 2.0f);
		Note("No Recoil and No Spread zero weapon vectors over DMA. Dynamic Recoil applies pattern compensation.");
		EndCard();

		BeginCard("PREDICTION", kCardW);
		ToggleRow("Target Prediction", &ConfigInstance.Aim.Prediction);
		if (ConfigInstance.Aim.Prediction)
		{
			SliderIntRow("Lead Strength", &ConfigInstance.Aim.PredictionStrength, 0, 200, "%");
			SliderIntRow("Vertical Lead", &ConfigInstance.Aim.PredictionYScale, 0, 100, "%");
		}
		ToggleRow("Hard Lock", &ConfigInstance.Aim.HardLock);
		SliderIntRow("Switch Delay", &ConfigInstance.Aim.TargetSwitchDelay, 0, 1000, "ms");
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn)
		ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("MEMORY AIM", kCardW);
		ToggleRow("Write Camera Angles", &ConfigInstance.Aim.MemoryWrite, true);
		if (ConfigInstance.Aim.MemoryWrite)
		{
			ToggleRow("Cursor Fallback", &ConfigInstance.Aim.CursorFallback);
			SliderIntRow("Response", &ConfigInstance.Aim.ResponseMs, 0, 250, " ms");
			Note("0 ms derives the response from Smoothness. The game may rebuild the camera "
				"from its own input, so a write can be reverted.");
		}
		EndCard();

		BeginCard("AIM VISUALS", kCardW);
		ToggleRow("Visible Bones Only", &ConfigInstance.Aim.VisibleBonesOnly);
		ToggleRow("Skip Police Officer targets", &ConfigInstance.Aim.ExpIgnorePolice);
		ToggleRow("Skip Criminal targets", &ConfigInstance.Aim.ExpIgnoreCriminal);
		ToggleRow("Skip Civilian targets", &ConfigInstance.Aim.ExpIgnoreCivilian);
		ToggleRow("Skip Knocked Players", &ConfigInstance.Aim.SkipKnocked);
		ToggleRow("Ignore Knock (Local)", &ConfigInstance.Aim.IgnoreKnock);
		ToggleRow("Hide Default Crosshair", &ConfigInstance.Aim.HideCrosshair);
		ToggleRow("Ignore Teammates", &ConfigInstance.Aim.ExpIgnoreTeammates);
		if (ConfigInstance.Aim.ExpIgnoreTeammates)
			Note("Uses party membership, not role: same-role players outside your party stay targetable.");
		{
			const int tc = ConfigInstance.Aim.TrustedCount();
			if (tc > 0) {
				char buf[64];
				std::snprintf(buf, sizeof(buf), "Trusted Players: %d", tc);
				Note("%s  (also skipped by aimbot; manage in Server tab)", buf);
			} else {
				Note("No trusted players. Add them in the Server tab.");
			}
		}
		ToggleRow("Show FOV Circle", &ConfigInstance.Aim.ShowFov);
		ToggleRow("Show Aim Debug", &ConfigInstance.Aim.ShowDebug);
		EndCard();

		BeginCard("DEVICE", kCardW);
		ComboRow("Device", &deviceIndex, deviceNames, IM_ARRAYSIZE(deviceNames));

		const auto kind = deviceKinds[deviceIndex];
		const bool isNet = IsNetworkDevice(kind);
		const bool isSerial = kind != DeviceKind::Off && !isNet;

		if (isNet)
		{
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			ImGui::InputTextWithHint("##ip", "IP address", ipAddr, sizeof(ipAddr));
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			ImGui::InputTextWithHint("##netport", "Network port", netPort, sizeof(netPort));
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			ImGui::InputTextWithHint("##mac", "Device ID", macId, sizeof(macId));
		}

		if (isSerial)
		{
			if (needsRefresh) { Session().RefreshPorts(); needsRefresh = false; }
			const auto& ports = Session().GetPorts();

			ToggleRow("Manual COM Port", &manualPort);
			if (manualPort || kind == DeviceKind::Ferrum)
			{
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				ImGui::InputTextWithHint("##com", "COM3", comPort, sizeof(comPort));
			}
			else if (!ports.empty())
			{
				std::vector<const char*> portItems;
				portItems.reserve(ports.size());
				for (const auto& p : ports) portItems.push_back(p.port.c_str());

				int sel = -1;
				for (int i = 0; i < (int)ports.size(); i++)
					if (ports[i].port == comPort) { sel = i; break; }
				if (ComboRow("COM Port", &sel, portItems.data(), (int)portItems.size()))
				{
					std::strncpy(comPort, ports[sel].port.c_str(), sizeof(comPort) - 1);
					comPort[sizeof(comPort) - 1] = 0;
				}
				if (ButtonRow("", "Refresh Ports", 130.f))
					Session().RefreshPorts();
			}
			else
			{
				Note("No COM ports found.");
				if (ButtonRow("", "Refresh Ports", 130.f))
					Session().RefreshPorts();
			}

			if (IsMakcu(kind))
				ComboRow("Baud", &baudIndex, baudLabels, IM_ARRAYSIZE(baudLabels));
			if (kind == DeviceKind::Ferrum)
				ToggleRow("Ferrum Controller", &ferrumController);
		}

		const auto status = Session().GetStatus();
		const bool connected = status.state == LinkState::Ready;
		const bool connecting = status.state == LinkState::Connecting || status.state == LinkState::Recovering;

		if (kind != DeviceKind::Off)
		{
			if (!connected && !connecting)
			{
				if (ButtonRow("", "Connect", 130.f, true))
				{
					DeviceConfig cfg;
					cfg.kind = kind;
					cfg.port = comPort;
					cfg.ip = ipAddr;
					cfg.networkPort = netPort;
					cfg.mac = macId;
					cfg.baud = baudRates[baudIndex];
					cfg.manualPort = manualPort || kind == DeviceKind::Ferrum;
					cfg.ferrumController = ferrumController;
					Session().Connect(cfg);
				}
			}
			else
			{
				if (ButtonRow("", "Disconnect", 130.f))
					Session().Disconnect();
				if (connected && ButtonRow("", "Test Movement", 130.f))
					Session().TestMovement();
			}
		}

		Note("Status: %s", StateLabel(status.state));
		if (!status.endpoint.empty()) Note("Endpoint: %s", status.endpoint.c_str());
		if (status.protocolVerified) Note("Protocol verified.");
		if (!status.error.empty()) Note("%s", status.error.c_str());
		if (status.retryInMs > 0) Note("Retrying in %u ms", status.retryInMs);
		EndCard();

		BeginCard("LIVE STATE", kCardW);
		{
			const Aim::DebugInfo info = Aim::GetDebug();
			Note("%s", info.reason);
			Note("Key %s (%s) | %s", info.held ? "held" : "up", info.keySource,
				info.deviceReady ? "device ready" : "device idle");
			Note("Candidates %d / %d | target %d", info.candidates, info.entities, info.target);
			if (info.writeMode)
				Note("Write %s (%s) | yaw %.1f pitch %.1f | %.0fm",
					info.writeOk ? "ok" : "fail", info.writeState, info.tgtYawDeg,
					info.tgtPitchDeg, info.distance);
		}
		EndCard();
	}
	ImGui::EndGroup();

	ConfigInstance.Aim.DeviceIndex = deviceIndex;
	ConfigInstance.Aim.ComPort = comPort;
	ConfigInstance.Aim.IpAddr = ipAddr;
	ConfigInstance.Aim.NetPort = netPort;
	ConfigInstance.Aim.MacId = macId;
	ConfigInstance.Aim.BaudIndex = baudIndex;
	ConfigInstance.Aim.ManualPort = manualPort;
	ConfigInstance.Aim.FerrumController = ferrumController;
}

static std::string FormatMoney(std::int32_t amount)
{
	if (amount <= 0) return "$0";
	std::string s = std::to_string(amount);
	int n = static_cast<int>(s.length()) - 3;
	while (n > 0) {
		s.insert(n, ",");
		n -= 3;
	}
	return "$" + s;
}

static std::string FormatDistance(float meters)
{
	if (meters < 0.01f) return "0 m (You)";
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.1f m", meters);
	return buf;
}

void DrawServer()
{
	const auto snapshot = RobRunReader::GetSnapshot();
	if (!snapshot)
	{
		BeginCard("SERVER", kCardW);
		Note("No reader snapshot available.");
		EndCard();
		return;
	}

	if (!g_singleColumn)
	{
		const float r1 = ImMax(CardNaturalHeight("SERVER STATS"), CardNaturalHeight("ROSTER CONTROLS"));
		ForceCardHeight("SERVER STATS", r1);
		ForceCardHeight("ROSTER CONTROLS", r1);
	}

	static char serverSearch[64] = "";
	static int currentSort = 0;
	static bool filterAliveOnly = false;
	static bool filterExcludeParty = false;
	static bool filterExcludeSelf = false;

	int totalCount = 0;
	int aliveCount = 0;
	int deadCount = 0;
	int partyCount = 0;

	Vector3 effectiveLocalPos = snapshot->localPosition;
	if (effectiveLocalPos.IsZero() && snapshot->localEntityId >= 0)
	{
		for (const auto& ent : snapshot->entities)
		{
			if (ent.entityId == snapshot->localEntityId && !ent.position.IsZero())
			{
				effectiveLocalPos = ent.position;
				break;
			}
		}
	}

	struct PlayerItem {
		const RobRunReader::EntityData* ent = nullptr;
		float distance = 0.f;
		std::string name;
		std::string clan;
		std::string weapon;
		bool isLocal = false;
	};

	std::vector<PlayerItem> items;
	items.reserve(snapshot->entities.size());

	for (const auto& ent : snapshot->entities)
	{
		totalCount++;
		if (ent.isDead) deadCount++;
		else aliveCount++;

		if (ent.isPartyMember) partyCount++;

		PlayerItem item;
		item.ent = &ent;
		item.isLocal = (ent.entityId == snapshot->localEntityId);

		if (item.isLocal)
		{
			item.distance = 0.f;
		}
		else if (!effectiveLocalPos.IsZero() && !ent.position.IsZero())
		{
			const float dx = ent.position.x - effectiveLocalPos.x;
			const float dy = ent.position.y - effectiveLocalPos.y;
			const float dz = ent.position.z - effectiveLocalPos.z;
			item.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		}
		else
		{
			item.distance = 9999.f;
		}

		item.name = RobRunMemory::WideToUtf8(ent.nickname);
		if (item.name.empty())
		{
			item.name = item.isLocal ? "Local Player" : ("Player #" + std::to_string(ent.entityId));
		}
		item.clan = RobRunMemory::WideToUtf8(ent.clanTag);
		item.weapon = RobRunMemory::WideToUtf8(ent.weaponName);
		if (item.weapon.empty()) item.weapon = "None";

		if (filterAliveOnly && ent.isDead) continue;
		if (filterExcludeParty && ent.isPartyMember && !item.isLocal) continue;
		if (filterExcludeSelf && item.isLocal) continue;

		if (serverSearch[0])
		{
			std::string needle(serverSearch);
			std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return (char)std::tolower(c); });

			std::string hay = item.name + " " + item.clan + " " + ent.roleName + " " + item.weapon;
			std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return (char)std::tolower(c); });

			if (hay.find(needle) == std::string::npos) continue;
		}

		items.push_back(std::move(item));
	}

	const char* sortModes[] = {
		"Distance (Nearest)",
		"Health (Lowest)",
		"Name (A-Z)"
	};

	switch (currentSort)
	{
	case 0:
		std::sort(items.begin(), items.end(), [](const PlayerItem& a, const PlayerItem& b) {
			if (a.isLocal != b.isLocal) return a.isLocal;
			return a.distance < b.distance;
		});
		break;
	case 1:
		std::sort(items.begin(), items.end(), [](const PlayerItem& a, const PlayerItem& b) {
			return a.ent->health < b.ent->health;
		});
		break;
	case 2:
		std::sort(items.begin(), items.end(), [](const PlayerItem& a, const PlayerItem& b) {
			return a.name < b.name;
		});
		break;
	default: break;
	}

	ImGui::BeginGroup();
	{
		BeginCard("SERVER STATS", kCardW);
		Note("Total Players: %d  (%d alive, %d dead)", totalCount, aliveCount, deadCount);
		if (partyCount > 0)
			Note("Party Members: %d", partyCount);
		if (snapshot->spectatorAlert || snapshot->observerCount > 0)
			Note("Spectators: %d active", snapshot->observerCount);
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn)
		ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("ROSTER CONTROLS", kCardW);
		SectionLabel("FILTER & SORT");
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		ImGui::InputTextWithHint("##srv_search", "Filter name, clan, weapon...", serverSearch, sizeof(serverSearch));
		ComboRow("Sort Order", &currentSort, sortModes, IM_ARRAYSIZE(sortModes));
		ToggleRow("Alive Players Only", &filterAliveOnly);
		ToggleRow("Hide Party Members", &filterExcludeParty);
		ToggleRow("Hide Self", &filterExcludeSelf);
		EndCard();
	}
	ImGui::EndGroup();

	const float fullW = g_singleColumn ? kCardW : (kCardW * 2.f + kGap);
	const float minRosterH = items.empty() ? 80.f : (kCardHeader + 24.f + (float)items.size() * 55.f);
	char rosterTitle[64];
	std::snprintf(rosterTitle, sizeof(rosterTitle), "PLAYERS ON MAP (%d / %d)###srv_roster", (int)items.size(), totalCount);
	BeginCard(rosterTitle, fullW, minRosterH);

	if (items.empty())
	{
		if (totalCount == 0)
			Note("No player entities detected on the map. Connect to a match or attach DMA reader.");
		else
			Note("No players match the current search filter.");
	}
	else
	{
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		ImDrawList* dl = window->DrawList;

		for (size_t idx = 0; idx < items.size(); ++idx)
		{
			const auto& item = items[idx];
			const auto& ent = *item.ent;

			const float rowH = 48.f;
			const ImVec2 p = window->DC.CursorPos;
			const float w = ImGui::GetContentRegionAvail().x;
			const ImRect bb(p, p + ImVec2(w, rowH));

			ImGui::ItemSize(bb);
			const ImGuiID rowId = window->GetID((int)idx + 2000);
			ImGui::ItemAdd(bb, rowId);

			const bool hov = ImGui::IsMouseHoveringRect(bb.Min, bb.Max);

			if (item.isLocal)
			{
				dl->AddRectFilled(bb.Min, bb.Max, C(kAccent, 0.15f), 6.f);
				dl->AddRect(bb.Min, bb.Max, C(kAccent, 0.50f), 6.f, 0, 1.2f);
			}
			else if (ent.isAimingAtLocal)
			{
				dl->AddRectFilled(bb.Min, bb.Max, C(kRed, 0.14f), 6.f);
				dl->AddRect(bb.Min, bb.Max, C(kRed, 0.45f), 6.f, 0, 1.2f);
			}
			else if (hov)
			{
				dl->AddRectFilled(bb.Min, bb.Max, C(kNavActive, 0.85f), 6.f);
				dl->AddRect(bb.Min, bb.Max, C(kStroke, 1.5f), 6.f);
			}
			else
			{
				dl->AddRectFilled(bb.Min, bb.Max, C(kCard, 0.75f), 6.f);
				dl->AddRect(bb.Min, bb.Max, C(kStroke, 0.8f), 6.f);
			}

			const float leftX = bb.Min.x + 10.f;
			const float rightX = bb.Max.x - 10.f;
			const float line1Y = bb.Min.y + 7.f;
			const float line2Y = bb.Min.y + 27.f;

			float curX = leftX;

			// Role badge
			std::string roleStr = ent.roleName;
			if (roleStr.empty()) roleStr = "UNKNOWN";
			std::string roleLower = roleStr;
			std::transform(roleLower.begin(), roleLower.end(), roleLower.begin(), [](unsigned char c) { return (char)std::tolower(c); });

			ImVec4 roleColor = kTextDim;
			if (roleLower.find("crim") != std::string::npos) roleColor = ImVec4(245 / 255.f, 75 / 255.f, 75 / 255.f, 1.f);
			else if (roleLower.find("pol") != std::string::npos || roleLower.find("cop") != std::string::npos) roleColor = ImVec4(70 / 255.f, 150 / 255.f, 255 / 255.f, 1.f);
			else if (roleLower.find("civ") != std::string::npos) roleColor = kGreen;

			const float roleW = TextW(font::inter_medium_small, roleStr.c_str()) + 8.f;
			const ImRect roleRect(ImVec2(curX, line1Y), ImVec2(curX + roleW, line1Y + 16.f));
			dl->AddRectFilled(roleRect.Min, roleRect.Max, C(roleColor, 0.18f), 3.f);
			dl->AddRect(roleRect.Min, roleRect.Max, C(roleColor, 0.45f), 3.f);
			dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
				ImVec2(roleRect.Min.x + 4.f, roleRect.Min.y + 1.f), C(roleColor), roleStr.c_str());
			curX += roleW + 8.f;

			// Player Name
			dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
				ImVec2(curX, line1Y - 1.f), C(kText), item.name.c_str());
			curX += TextW(font::inter_medium_widget, item.name.c_str()) + 4.f;

			// Clan Tag
			if (!item.clan.empty())
			{
				std::string clanTagStr = "<" + item.clan + ">";
				dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
					ImVec2(curX, line1Y + 1.f), C(kAccentBright), clanTagStr.c_str());
				curX += TextW(font::inter_medium_small, clanTagStr.c_str()) + 6.f;
			}

			// Badges: YOU / PARTY / DEAD / AIMING
			if (item.isLocal)
			{
				const char* tag = "YOU";
				const float tw = TextW(font::inter_medium_small, tag) + 6.f;
				dl->AddRectFilled(ImVec2(curX, line1Y), ImVec2(curX + tw, line1Y + 16.f), C(kAccent, 0.3f), 3.f);
				dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize, ImVec2(curX + 3.f, line1Y + 1.f), C(kAccentBright), tag);
				curX += tw + 4.f;
			}
			else if (ent.isPartyMember)
			{
				const char* tag = "PARTY";
				const float tw = TextW(font::inter_medium_small, tag) + 6.f;
				dl->AddRectFilled(ImVec2(curX, line1Y), ImVec2(curX + tw, line1Y + 16.f), C(kGreen, 0.25f), 3.f);
				dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize, ImVec2(curX + 3.f, line1Y + 1.f), C(kGreen), tag);
				curX += tw + 4.f;
			}

			if (ent.isDead)
			{
				const char* tag = "DEAD";
				const float tw = TextW(font::inter_medium_small, tag) + 6.f;
				dl->AddRectFilled(ImVec2(curX, line1Y), ImVec2(curX + tw, line1Y + 16.f), C(kRed, 0.3f), 3.f);
				dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize, ImVec2(curX + 3.f, line1Y + 1.f), C(kRed), tag);
				curX += tw + 4.f;
			}
			else if (ent.isKnockedDown || ent.isKnockedOut)
			{
				const char* tag = "KNOCKED";
				const float tw = TextW(font::inter_medium_small, tag) + 6.f;
				dl->AddRectFilled(ImVec2(curX, line1Y), ImVec2(curX + tw, line1Y + 16.f), IM_COL32(255, 170, 0, 80), 3.f);
				dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize, ImVec2(curX + 3.f, line1Y + 1.f), IM_COL32(255, 200, 50, 255), tag);
				curX += tw + 4.f;
			}

			if (ent.isAimingAtLocal)
			{
				const char* tag = "AIMING AT YOU";
				const float tw = TextW(font::inter_medium_small, tag) + 6.f;
				dl->AddRectFilled(ImVec2(curX, line1Y), ImVec2(curX + tw, line1Y + 16.f), C(kRed, 0.35f), 3.f);
				dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize, ImVec2(curX + 3.f, line1Y + 1.f), C(kRed), tag);
				curX += tw + 4.f;
			}

			const bool isTrusted = ConfigInstance.Aim.IsTrusted(ent.publicId);
			if (isTrusted)
			{
				const char* tag = "TRUSTED";
				const ImVec4 tealCol(0.f, 0.8f, 0.65f, 1.f);
				const float tw = TextW(font::inter_medium_small, tag) + 6.f;
				dl->AddRectFilled(ImVec2(curX, line1Y), ImVec2(curX + tw, line1Y + 16.f), C(tealCol, 0.22f), 3.f);
				dl->AddRect(ImVec2(curX, line1Y), ImVec2(curX + tw, line1Y + 16.f), C(tealCol, 0.5f), 3.f);
				dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize, ImVec2(curX + 3.f, line1Y + 1.f), C(tealCol), tag);
				curX += tw + 4.f;
			}

			// Line 1 Right: Trust button + Distance
			std::string distStr = item.isLocal ? "0 m" : FormatDistance(item.distance);
			const float distW = TextW(font::inter_medium_widget, distStr.c_str());
			ImU32 distCol = C(kTextDim);
			if (!item.isLocal)
			{
				if (item.distance < 50.f) distCol = C(kRed);
				else if (item.distance < 120.f) distCol = IM_COL32(255, 185, 60, 255);
			}
			dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
				ImVec2(rightX - distW, line1Y - 1.f), distCol, distStr.c_str());

			// Trust / Untrust button (to the left of distance)
			if (!item.isLocal && ent.publicId)
			{
				const char* btnLabel = isTrusted ? "Untrust" : "Trust";
				const float btnW = TextW(font::inter_medium_small, btnLabel) + 10.f;
				const float btnH = 16.f;
				const float btnX = rightX - distW - btnW - 8.f;
				const ImRect btnRect(ImVec2(btnX, line1Y), ImVec2(btnX + btnW, line1Y + btnH));
				const bool btnHov = ImGui::IsMouseHoveringRect(btnRect.Min, btnRect.Max);

				const ImVec4 tealCol(0.f, 0.8f, 0.65f, 1.f);
				if (isTrusted) {
					dl->AddRectFilled(btnRect.Min, btnRect.Max, C(tealCol, btnHov ? 0.35f : 0.18f), 3.f);
					dl->AddRect(btnRect.Min, btnRect.Max, C(tealCol, 0.5f), 3.f);
					dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
						ImVec2(btnRect.Min.x + 5.f, btnRect.Min.y + 1.f), C(tealCol), btnLabel);
				} else {
					dl->AddRectFilled(btnRect.Min, btnRect.Max, C(kTextDim, btnHov ? 0.30f : 0.12f), 3.f);
					dl->AddRect(btnRect.Min, btnRect.Max, C(kTextDim, 0.4f), 3.f);
					dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
						ImVec2(btnRect.Min.x + 5.f, btnRect.Min.y + 1.f), C(kTextDim), btnLabel);
				}

				if (btnHov && ImGui::IsMouseClicked(0))
				{
					ConfigInstance.Aim.SetTrusted(ent.publicId, !isTrusted);
				}
			}

			// Line 2 Left: Health, Armor, Weapon
			float l2X = leftX;
			char hpBuf[64];
			if (ent.isDead)
			{
				std::snprintf(hpBuf, sizeof(hpBuf), "DEAD");
			}
			else if (ent.healthKnown && ent.health >= 0.f)
			{
				if (ent.hasArmor && ent.armor > 0.f)
				{
					std::snprintf(hpBuf, sizeof(hpBuf), "HP %.0f/%.0f  |  AP %.0f", ent.health, ent.maxHealth > 0.f ? ent.maxHealth : 100.f, ent.armor);
				}
				else
				{
					std::snprintf(hpBuf, sizeof(hpBuf), "HP %.0f/%.0f", ent.health, ent.maxHealth > 0.f ? ent.maxHealth : 100.f);
				}
			}
			else
			{
				std::snprintf(hpBuf, sizeof(hpBuf), "HP ?");
			}

			ImU32 hpCol = ent.isDead ? C(kRed) : (ent.health < 40.f && ent.healthKnown ? C(kRed) : C(kTextDim));
			dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
				ImVec2(l2X, line2Y), hpCol, hpBuf);
			l2X += TextW(font::inter_medium_small, hpBuf) + 14.f;

			std::string wepStr = "Weapon: " + item.weapon;
			dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
				ImVec2(l2X, line2Y), C(kTextFaint), wepStr.c_str());
			l2X += TextW(font::inter_medium_small, wepStr.c_str()) + 12.f;

			if (ent.inVehicle)
			{
				const char* vStr = "[Vehicle]";
				dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
					ImVec2(l2X, line2Y), C(kAccentBright), vStr);
				l2X += TextW(font::inter_medium_small, vStr) + 8.f;
			}
		}
	}
	EndCard();
}

void DrawMisc()
{
	// Two rows of two cards. Session stats sit under the catalog, while the
	// read-only notes and shortcuts share the other column.
	if (!g_singleColumn)
	{
		const float r1 = ImMax(CardNaturalHeight("ITEM CATALOG"), CardNaturalHeight("READ-ONLY DMA"));
		const float r2 = ImMax(CardNaturalHeight("SESSION"), CardNaturalHeight("SHORTCUTS"));
		ForceCardHeight("ITEM CATALOG", r1);
		ForceCardHeight("READ-ONLY DMA", r1);
		ForceCardHeight("SESSION", r2);
		ForceCardHeight("SHORTCUTS", r2);
	}

	ImGui::BeginGroup();
	{
		BeginCard("ITEM CATALOG", kCardW);
		if (ToggleRow("Read Item Catalog", &g_readCatalog))
			RobRunReader::SetReadCatalogEnabled(g_readCatalog);
		Note("One-off bounded class scan. It can pause briefly while it walks the class list.");

		const auto snapshot = RobRunReader::GetSnapshot();
		if (snapshot && snapshot->inventory && snapshot->inventory->valid)
		{
			const auto& inventory = *snapshot->inventory;
			SectionLabel("INVENTORY");
			Note("Grid %d columns x %d lines", inventory.columns, inventory.activeLines);
			Note("Base %d + backpack %d / max %d", inventory.baseLines,
				inventory.backpackLines, inventory.maxLines);
			Note("Slots %zu", inventory.slots.size());
			if (!inventory.catalog.empty())
				Note("Catalog entries %zu", inventory.catalog.size());
		}
		else
		{
			Note("No inventory snapshot yet. Enable the scan while in a raid.");
		}
		EndCard();

		BeginCard("SESSION", kCardW);
		{
			const auto snapshot = RobRunReader::GetSnapshot();
			if (snapshot)
			{
				const auto world = snapshot->world ? *snapshot->world : RobRunWorld::State{};
				Note("Entities %zu  (C:%d L:%d V:%d G:%d)", snapshot->entities.size(),
					world.containers, world.loots, world.vehicles, world.grenades);
				Note("Skeletons %u | Aim poses %u | Pose retries %u", snapshot->skeletonCount,
					snapshot->poseCount, snapshot->poseFailures);
				Note("Bones %u inv %u (nZ %u nB %u pI %u) chain %u",
					snapshot->boneTotal, snapshot->boneInvalid,
					snapshot->boneNormZero, snapshot->boneNormBad,
					snapshot->bonePosInf, snapshot->boneChainDrop);
				Note("Scatter ok %u fail %u | Fresh ok %u",
					snapshot->poseScatterOk, snapshot->poseScatterFail,
					snapshot->poseFreshOk);
				Note("Read failures %u", snapshot->failureCount);
			}
			else
			{
				Note("No snapshot yet. Waiting for the game process.");
			}
			Note("Overlay %.0f FPS", ImGui::GetIO().Framerate);
		}
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn)
		ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("READ-ONLY DMA", kCardW);
		Note("ROB RUN is an external DMA reader. It never injects into the game process.");
		Note("- VisCheck ray-casts each bone from the camera, so only geometry "
			"blocks it. Without city_colliders.bvh it falls back to animator state.");
		Note("- WallCheck treats a character whose bones are all occluded as hidden.");
		Note("- Rows marked MEM WRITE change game memory over DMA, not the host.");
		Note("- Hardware aim sends the same pixel delta through the selected device.");
		EndCard();

		BeginCard("SHORTCUTS", kCardW);
		Note("INSERT  -  show or hide this menu");
		Note("DELETE  -  toggle the debug overlay (debug builds)");
		Note("Right click a toggle with a cog for extra options.");
		EndCard();
	}
	ImGui::EndGroup();

	// --- Memory probe card (full width) ---
	{
		static std::vector<RobRunWorld::ProbeResult> probeResults;
		static bool probed = false;

		BeginCard("MEMORY PROBE", kCardW * 2.f + kGap);

		const auto snapshot = RobRunReader::GetSnapshot();
		const std::uintptr_t worldPtr = (snapshot && snapshot->world)
			? snapshot->world->worldPtr : 0;

		if (worldPtr) {
			if (ButtonRow("", "Probe Money Models", 180.f, true))
			{
				probeResults = RobRunWorld::ProbeMoneyModels(worldPtr);
				probed = true;
			}
			ImGui::SameLine();
			Note("world=0x%llX", (unsigned long long)worldPtr);
		} else {
			Note("No world pointer. Attach to the game first.");
		}

		if (probed && !probeResults.empty())
		{
			for (const auto& r : probeResults)
			{
				SectionLabel(r.dictName.c_str());
				Note("class: %s  ptr: 0x%llX", r.className.c_str(), (unsigned long long)r.ptr);
				for (const auto& f : r.fields)
				{
					char hex[32];
					snprintf(hex, sizeof(hex), "%llX", (unsigned long long)(std::uint64_t)f.rawValue);
					Note("  +0x%X  t=%d  %s  = 0x%s (%lld)",
						f.offset, f.typeCode, f.name.c_str(),
						hex, (long long)f.rawValue);
				}
			}
		}
		else if (probed)
		{
			Note("No money model entries found in the current scene.");
		}

		EndCard();
	}
}

void DrawSkins()
{
	const SkinChanger::State state = SkinChanger::GetState();
	auto& skin = ConfigInstance.Skin;

	BeginCard("SKIN CHANGER", kCardW * 2.f + kGap);
	ToggleRow("Skin Changer", &skin.Enabled, true);
	if (state.inMatch)
		Note("Character: %s", state.female ? "Female" : "Male");
	else
		Note("Status: %s", state.status.empty() ? "-" : state.status.c_str());
	Note("Client-side only. Your real item is drawn with the chosen item's assets, so "
		"other players wearing the same real item change too (on your screen only). "
		"Choices are saved per male / female character and applied in every match. "
		"Everything is restored when turned off or when VeoxDMA closes.");
	EndCard();

	BeginCard("COSMETICS", kCardW * 2.f + kGap);
	if (!state.inMatch)
	{
		Note("Join a match to pick cosmetics.");
	}
	else
	{
		static std::vector<std::string> labels[SkinChanger::kSlotCount];
		static std::vector<const char*> items[SkinChanger::kSlotCount];
		for (int slot = 0; slot < SkinChanger::kSlotCount; ++slot)
		{
			const auto& s = state.slots[slot];
			SectionLabel(SkinChanger::SlotName(slot));
			if (!s.supported)
			{
				Note("%s", s.status.c_str());
				continue;
			}
			const std::string wanted = SkinChanger::Wanted(slot, state.female);
			labels[slot].clear();
			items[slot].clear();
			labels[slot].push_back("Real item (no change)");
			int selected = 0;
			for (std::size_t i = 0; i < s.options.size(); ++i)
			{
				const std::string& name = i < s.names.size() ? s.names[i] : s.options[i];
				const bool duplicate = std::count(s.names.begin(), s.names.end(), name) > 1;
				labels[slot].push_back(duplicate ? name + " (" + s.options[i] + ")" : name);
				if (!wanted.empty() && _stricmp(s.options[i].c_str(), wanted.c_str()) == 0)
					selected = static_cast<int>(i) + 1;
			}
			for (const auto& l : labels[slot]) items[slot].push_back(l.c_str());
			if (ComboRow(SkinChanger::SlotName(slot), &selected, items[slot].data(),
					static_cast<int>(items[slot].size()), 280.f, true))
				SkinChanger::SetWanted(slot, state.female,
					selected > 0 ? s.options[selected - 1] : std::string());
			Note("Real: %s", s.realName.c_str());
			Note("Status: %s", s.status.empty() ? "-" : s.status.c_str());
		}
	}
	EndCard();

	BeginCard("WEAPON SKINS", kCardW * 2.f + kGap);
	if (!state.inMatch)
	{
		Note("Join a match to pick weapon skins.");
	}
	else if (!state.weaponsSupported)
	{
		Note("Weapon skin data not found (offsets changed?).");
	}
	else
	{
		if (!state.heldWeapon.empty())
			Note("Holding: %s  -  real skin: %s", state.heldWeapon.c_str(), state.heldSkin.c_str());
		Note("A chosen skin replaces every skin of that weapon, also on other players "
			"(on your screen only). Switch weapons once if it does not change right away.");

		static std::vector<std::vector<std::string>> labels;
		static std::vector<std::vector<const char*>> items;
		labels.resize(state.weapons.size());
		items.resize(state.weapons.size());
		for (std::size_t w = 0; w < state.weapons.size(); ++w)
		{
			const auto& weapon = state.weapons[w];
			const std::string wanted = SkinChanger::WantedWeaponSkin(weapon.key);
			labels[w].clear();
			items[w].clear();
			labels[w].push_back("Real skins (no change)");
			int selected = 0;
			for (std::size_t i = 0; i < weapon.skins.size(); ++i)
			{
				const std::string& name = i < weapon.names.size() ? weapon.names[i] : weapon.skins[i];
				const bool duplicate = std::count(weapon.names.begin(), weapon.names.end(), name) > 1;
				labels[w].push_back(duplicate ? name + " (" + weapon.skins[i] + ")" : name);
				if (!wanted.empty() && _stricmp(weapon.skins[i].c_str(), wanted.c_str()) == 0)
					selected = static_cast<int>(i) + 1;
			}
			for (const auto& l : labels[w]) items[w].push_back(l.c_str());
			if (ComboRow(weapon.name.c_str(), &selected, items[w].data(),
					static_cast<int>(items[w].size()), 280.f, true))
				SkinChanger::SetWantedWeaponSkin(weapon.key,
					selected > 0 ? weapon.skins[selected - 1] : std::string());
			if (!wanted.empty())
				Note("%s", weapon.status.c_str());
		}
	}
	EndCard();
}

void DrawSettings(bool attached, const std::string& statusText)
{
	// Three rows of two cards: overlay/display settings on the left, backdrop,
	// config and about on the right.
	if (!g_singleColumn)
	{
		const float r1 = ImMax(CardNaturalHeight("OVERLAY"), CardNaturalHeight("BACKDROP"));
		const float r2 = ImMax(CardNaturalHeight("DISPLAY"), CardNaturalHeight("CONFIG"));
		const float r3 = ImMax(CardNaturalHeight("STATUS"), CardNaturalHeight("ABOUT"));
		ForceCardHeight("OVERLAY", r1);
		ForceCardHeight("BACKDROP", r1);
		ForceCardHeight("DISPLAY", r2);
		ForceCardHeight("CONFIG", r2);
		ForceCardHeight("STATUS", r3);
		ForceCardHeight("ABOUT", r3);
	}

	ImGui::BeginGroup();
	{
		BeginCard("OVERLAY", kCardW);
		bool transparent = Overlay::Transparent();
		if (ToggleRow("Transparent", &transparent))
			Overlay::SetTransparent(transparent);

		bool clickThrough = Overlay::ClickThrough();
		if (ToggleRow("Click-Through When Closed", &clickThrough))
			Overlay::SetClickThrough(clickThrough);

		bool onTop = Overlay::AlwaysOnTop();
		if (ToggleRow("Always On Top", &onTop))
			Overlay::SetAlwaysOnTop(onTop);

		static const char* menuKeys[] = { "Insert", "Home", "Delete", "End", "F11", "F12", "Right Shift" };
		ComboRow("Menu Key", &ConfigInstance.Overlay.MenuKey, menuKeys, IM_ARRAYSIZE(menuKeys));

		ToggleRow("Panic Mode", &ConfigInstance.Overlay.PanicMode);
		static const char* panicKeys[] = { "Off", "End", "F8", "F9", "F10", "F11", "F12" };
		ComboRow("Panic Key", &ConfigInstance.Overlay.PanicKey, panicKeys, IM_ARRAYSIZE(panicKeys));
		if (ConfigInstance.Overlay.PanicKey > 0)
			Note("Pressing panic key instantly blanks the overlay and stops targeting.");

		static const char* reattachKeys[] = {
			"Off", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
			"Insert", "Home", "End", "Delete", "Page Up", "Page Down"
		};
		ComboRow("Re-attach Key", &ConfigInstance.Overlay.ReattachKey, reattachKeys, IM_ARRAYSIZE(reattachKeys));
		if (ConfigInstance.Overlay.ReattachKey > 0)
			Note("Pressing the hotkey re-attaches to the game process.");

		ToggleRow("High Contrast Menu", &ConfigInstance.Overlay.HighContrastMenu);
		Note("Solid card fills and brighter text, for bright game scenes.");

		ToggleRow("Camera Zoom", &ConfigInstance.Overlay.CustomCameraDistance, true);
		if (ConfigInstance.Overlay.CustomCameraDistance)
		{
			SliderFloatRow("Distance", &ConfigInstance.Overlay.CameraDistance, 1.0f, 15.0f, " m");
			Note("Pulls the third-person camera back for wide tactical overview.");
		}

		ToggleRow("FOV Changer", &ConfigInstance.Overlay.FovChanger, true);
		if (ConfigInstance.Overlay.FovChanger)
		{
			SliderFloatRow("Camera FOV", &ConfigInstance.Overlay.CameraFov, 30.0f, 140.0f, " deg");
			Note("Overrides Cinemachine virtual camera lens FOV.");
		}
		EndCard();

		BeginCard("DISPLAY", kCardW);
		ToggleRow("Show FPS", &ConfigInstance.Overlay.FPSEnabled);
		ToggleRow("Status Panel", &ConfigInstance.Overlay.StatusPanel);

		{
			static const char* fontNames[kFontCount];
			for (int i = 0; i < kFontCount; i++) fontNames[i] = g_fonts[i].name;
			if (ComboRow("Font", &g_fontIndex, fontNames, kFontCount))
				ApplySelectedFont();
		}

		int cap = Overlay::FrameCap();
		if (SliderIntRow("Frame Cap", &cap, 0, 480, " fps", "Uncapped"))
			Overlay::SetFrameCap(cap);
		Note("Caps the overlay render loop. Uncapped redraws as fast as the GPU "
			"allows, which costs CPU that the DMA reader needs.");

		const int monCount = Overlay::MonitorCount();
		if (monCount > 0)
		{
			static char labels[8][64];
			static const char* ptrs[8];
			const int count = ImMin(monCount, 8);
			int current = Overlay::MonitorIndex();
			for (int i = 0; i < count; i++)
			{
				Overlay::MonitorInfo info{};
				if (Overlay::GetMonitorInfo(i, info))
					std::snprintf(labels[i], sizeof(labels[i]), "%d  %dx%d%s", i + 1,
						info.width, info.height, info.primary ? " (primary)" : "");
				else
					std::snprintf(labels[i], sizeof(labels[i]), "Monitor %d", i + 1);
				ptrs[i] = labels[i];
			}
			if (ComboRow("Display Monitor", &current, ptrs, count))
				Overlay::SetMonitor(current);
		}
		EndCard();

		BeginCard("STATUS", kCardW);
		Note("%s", attached ? "DMA connected" : "DMA disconnected");
		Note("%s", statusText.c_str());
		Note("Overlay %.0f FPS", ImGui::GetIO().Framerate);
		{
			const auto snapshot = RobRunReader::GetSnapshot();
			if (snapshot)
				Note("%zu entities | %u skeletons | %u read failures", snapshot->entities.size(),
					snapshot->skeletonCount, snapshot->failureCount);
		}
		if (ButtonRow("", "Re-attach", 130.f, true))
			RequestReattach();
		ComboRow("Re-attach Key", &ConfigInstance.Overlay.ReattachKey, reattachKeys, IM_ARRAYSIZE(reattachKeys));
		if (ConfigInstance.Overlay.ReattachKey > 0)
			Note("Pressing the hotkey re-attaches to the game process.");
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn)
		ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("BACKDROP", kCardW);
		bool vis = Backdrop::IsVisible();
		if (ToggleRow("Show Backdrop", &vis))
		{
			if (vis) Backdrop::Show();
			else Backdrop::Hide();
		}

		bool fs = Backdrop::IsFullscreen();
		if (ToggleRow("Fullscreen", &fs))
			Backdrop::SetFullscreen(fs);

		static int bdW = 0, bdH = 0;
		static bool customRes = false;
		ToggleRow("Custom Resolution", &customRes);
		if (customRes)
		{
			ImGui::SetNextItemWidth((ImGui::GetContentRegionAvail().x - 8.f) * 0.5f);
			ImGui::InputInt("##bdw", &bdW, 0);
			ImGui::SameLine(0.f, 8.f);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			ImGui::InputInt("##bdh", &bdH, 0);
			if (ButtonRow("", "Apply Resolution", 140.f, true))
				Backdrop::SetResolution(bdW, bdH);
		}
		else if (ButtonRow("", "Reset To Native", 140.f))
		{
			Backdrop::SetFullscreen(true);
		}
		EndCard();

		BeginCard("CONFIG", kCardW);
		static std::vector<std::string> configs = ConfigFile::ListConfigs();
		static int selectedConfigIdx = 0;
		static char newConfigName[64] = "";
		static char renameConfigName[64] = "";
		static bool configListInitialized = false;

		if (!configListInitialized) {
			configs = ConfigFile::ListConfigs();
			std::string active = ConfigFile::GetActiveConfigName();
			selectedConfigIdx = 0;
			for (size_t i = 0; i < configs.size(); ++i) {
				if (configs[i] == active) {
					selectedConfigIdx = (int)i;
					break;
				}
			}
			if (selectedConfigIdx < (int)configs.size()) {
				std::strncpy(renameConfigName, configs[selectedConfigIdx].c_str(), sizeof(renameConfigName) - 1);
			}
			configListInitialized = true;
		}

		if (!configs.empty())
		{
			std::vector<const char*> configItems;
			configItems.reserve(configs.size());
			for (const auto& c : configs) configItems.push_back(c.c_str());

			int sel = ImClamp(selectedConfigIdx, 0, (int)configs.size() - 1);
			if (ComboRow("Select Config", &sel, configItems.data(), (int)configItems.size()))
			{
				selectedConfigIdx = sel;
				std::strncpy(renameConfigName, configs[sel].c_str(), sizeof(renameConfigName) - 1);
				renameConfigName[sizeof(renameConfigName) - 1] = 0;
			}
		}

		if (ButtonRow("", "Save Active Config", 140.f, true))
		{
			const std::string name = (selectedConfigIdx >= 0 && selectedConfigIdx < (int)configs.size())
				? configs[selectedConfigIdx] : ConfigFile::GetActiveConfigName();
			const bool ok = ConfigFile::Save(name);
			configs = ConfigFile::ListConfigs();
			Overlay::Notify(ok ? "Config saved" : "Config save failed",
				ok ? IM_COL32(74, 201, 126, 255) : IM_COL32(255, 75, 75, 255), 2.f);
		}

		if (ButtonRow("", "Load Active Config", 140.f))
		{
			const std::string name = (selectedConfigIdx >= 0 && selectedConfigIdx < (int)configs.size())
				? configs[selectedConfigIdx] : ConfigFile::GetActiveConfigName();
			const bool ok = ConfigFile::Load(name);
			configs = ConfigFile::ListConfigs();
			Overlay::Notify(ok ? "Config loaded" : "Config load failed",
				ok ? IM_COL32(74, 201, 126, 255) : IM_COL32(255, 75, 75, 255), 2.f);
		}

		SectionLabel("RENAME CONFIG");
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		ImGui::InputTextWithHint("##rename_cfg", "New name...", renameConfigName, sizeof(renameConfigName));
		if (ButtonRow("", "Rename Config", 110.f))
		{
			if (renameConfigName[0] && selectedConfigIdx >= 0 && selectedConfigIdx < (int)configs.size())
			{
				const std::string oldName = configs[selectedConfigIdx];
				const std::string newName = renameConfigName;
				if (ConfigFile::RenameConfig(oldName, newName))
				{
					configs = ConfigFile::ListConfigs();
					for (size_t i = 0; i < configs.size(); ++i) {
						if (configs[i] == newName) {
							selectedConfigIdx = (int)i;
							break;
						}
					}
					Overlay::Notify("Config renamed", IM_COL32(74, 201, 126, 255), 2.f);
				}
				else
				{
					Overlay::Notify("Rename failed", IM_COL32(255, 75, 75, 255), 2.f);
				}
			}
		}

		SectionLabel("NEW CONFIG");
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		ImGui::InputTextWithHint("##new_cfg", "Config name...", newConfigName, sizeof(newConfigName));
		if (ButtonRow("", "Create Config", 110.f))
		{
			if (newConfigName[0])
			{
				if (ConfigFile::CreateConfig(newConfigName))
				{
					configs = ConfigFile::ListConfigs();
					for (size_t i = 0; i < configs.size(); ++i) {
						if (configs[i] == newConfigName) {
							selectedConfigIdx = (int)i;
							break;
						}
					}
					std::strncpy(renameConfigName, newConfigName, sizeof(renameConfigName) - 1);
					newConfigName[0] = 0;
					Overlay::Notify("Config created", IM_COL32(74, 201, 126, 255), 2.f);
				}
				else
				{
					Overlay::Notify("Create failed", IM_COL32(255, 75, 75, 255), 2.f);
				}
			}
		}

		Note("Directory: %s", ConfigFile::GetConfigDirectory().c_str());
		Note("Active: %s.json", ConfigFile::GetActiveConfigName().c_str());
		Note("Saved automatically on menu close.");
		EndCard();

		BeginCard("ABOUT", kCardW);
		Note("ROB RUN external overlay");
		Note("Menu key: INSERT");
		Note("Purple on black, built for a two-PC DMA setup.");
		EndCard();
	}
	ImGui::EndGroup();
}

// ---- chrome ---------------------------------------------------------------
void DrawTopBar(const ImVec2& pos)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();

	// logo mark
	const ImRect mark(pos + ImVec2(20.f, 18.f), pos + ImVec2(50.f, 48.f));
	dl->AddRectFilled(mark.Min, mark.Max, C(kAccent, 0.16f), 9.f);
	dl->AddRect(mark.Min, mark.Max, C(kAccent, 0.45f), 9.f);
	{
		const char* letter = "R";
		const float tw = TextW(font::inter_medium_big, letter);
		dl->AddText(font::inter_medium_big, font::inter_medium_big->FontSize,
			ImVec2((mark.Min.x + mark.Max.x - tw) * 0.5f,
				(mark.Min.y + mark.Max.y - font::inter_medium_big->FontSize) * 0.5f - 1.f),
			C(kAccentBright), letter);
	}

	dl->AddText(font::inter_medium_big, font::inter_medium_big->FontSize,
		pos + ImVec2(60.f, 20.f), C(kText), "ROB RUN");
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		pos + ImVec2(61.f, 45.f), C(kTextFaint), "EXTERNAL DMA OVERLAY");

	// search field
	const float fieldW = 320.f;
	const ImVec2 fieldMin(pos.x + kSide + ((kW - kSide) - fieldW) * 0.5f, pos.y + 17.f);
	const ImVec2 fieldMax(fieldMin.x + fieldW, fieldMin.y + 32.f);

	const bool focused = ImGui::GetActiveID() == ImGui::GetID("##search");
	dl->AddRectFilled(fieldMin, fieldMax, C(ImVec4(1, 1, 1, 0.045f)), 8.f);
	dl->AddRect(fieldMin, fieldMax, C(Mix(kStroke, kAccent, focused ? 0.55f : 0.f)), 8.f, 0, 1.f);

	// magnifier
	const ImVec2 mag(fieldMin.x + 16.f, fieldMin.y + 15.f);
	dl->AddCircle(mag, 5.f, C(kTextFaint), 16, 1.4f);
	dl->AddLine(ImVec2(mag.x + 4.f, mag.y + 4.f), ImVec2(mag.x + 8.f, mag.y + 8.f), C(kTextFaint), 1.4f);

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(30.f, 8.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Text, C(kText));
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, C(kTextFaint));
	ImGui::SetCursorScreenPos(fieldMin);
	ImGui::SetNextItemWidth(fieldW);
	ImGui::InputTextWithHint("##search", "Search settings", g_search, sizeof(g_search));
	ImGui::PopStyleColor(5);
	ImGui::PopStyleVar(3);

	// clear button
	if (g_search[0])
	{
		const ImRect clear(fieldMax.x - 28.f, fieldMin.y + 8.f, fieldMax.x - 8.f, fieldMax.y - 8.f);
		const bool hovered = ImGui::IsMouseHoveringRect(clear.Min, clear.Max);
		const ImVec2 c((clear.Min.x + clear.Max.x) * 0.5f, (clear.Min.y + clear.Max.y) * 0.5f);
		dl->AddLine(ImVec2(c.x - 3.5f, c.y - 3.5f), ImVec2(c.x + 3.5f, c.y + 3.5f),
			C(kTextDim, hovered ? 1.f : 0.7f), 1.4f);
		dl->AddLine(ImVec2(c.x + 3.5f, c.y - 3.5f), ImVec2(c.x - 3.5f, c.y + 3.5f),
			C(kTextDim, hovered ? 1.f : 0.7f), 1.4f);
		ImGui::SetCursorScreenPos(clear.Min);
		if (ImGui::InvisibleButton("##clear", clear.GetSize()))
			g_search[0] = 0;
	}

	// re-attach button
	if (kW >= 720.f)
	{
		const ImRect reattach(pos + ImVec2(kW - 146.f, 19.f), pos + ImVec2(kW - 54.f, 45.f));
		const bool reattachHover = ImGui::IsMouseHoveringRect(reattach.Min, reattach.Max);
		dl->AddRectFilled(reattach.Min, reattach.Max,
			C(kAccent, reattachHover ? 0.30f : 0.14f), 7.f);
		dl->AddRect(reattach.Min, reattach.Max,
			C(kAccent, reattachHover ? 0.65f : 0.35f), 7.f, 0, 1.f);
		const char* label = "Re-attach";
		const float tw = TextW(font::inter_medium_small, label);
		const float th = font::inter_medium_small->FontSize;
		dl->AddText(font::inter_medium_small, th,
			ImVec2((reattach.Min.x + reattach.Max.x - tw) * 0.5f,
				(reattach.Min.y + reattach.Max.y - th) * 0.5f - 1.f),
			C(reattachHover ? kAccentBright : kText), label);
		ImGui::SetCursorScreenPos(reattach.Min);
		if (ImGui::InvisibleButton("##top_reattach", reattach.GetSize()))
			RequestReattach();
	}

	// close button
	const ImRect close(pos + ImVec2(kW - 46.f, 19.f), pos + ImVec2(kW - 20.f, 45.f));
	const bool closeHover = ImGui::IsMouseHoveringRect(close.Min, close.Max);
	dl->AddRectFilled(close.Min, close.Max,
		C(ImVec4(1, 1, 1, 1), closeHover ? 0.08f : 0.04f), 7.f);
	{
		const ImVec2 c((close.Min.x + close.Max.x) * 0.5f, (close.Min.y + close.Max.y) * 0.5f);
		dl->AddLine(ImVec2(c.x - 4.f, c.y - 4.f), ImVec2(c.x + 4.f, c.y + 4.f),
			C(kTextDim, closeHover ? 1.f : 0.8f), 1.5f);
		dl->AddLine(ImVec2(c.x + 4.f, c.y - 4.f), ImVec2(c.x - 4.f, c.y + 4.f),
			C(kTextDim, closeHover ? 1.f : 0.8f), 1.5f);
	}
	ImGui::SetCursorScreenPos(close.Min);
	if (ImGui::InvisibleButton("##close", close.GetSize()))
		Overlay::SetMenuOpen(false);
}

void DrawSidebar(const ImVec2& pos, bool attached)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();

	// The nav list lives in a child that stops above the footer, so a short
	// window can never draw the footer status over the page buttons.
	constexpr float footerH = 118.f;
	const float navTop = kTop + 16.f;
	const float navH = ImMax(kH - navTop - footerH, 44.f);

	ImGui::SetCursorScreenPos(pos + ImVec2(12.f, navTop));
	ImGui::BeginChild("##nav", ImVec2(kSide - 24.f, navH), ImGuiChildFlags_None,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings);
	{
		ImTextureID icons[kPageCount];
		for (int i = 0; i < kPageCount; i++)
			icons[i] = (ImTextureID)(intptr_t)g_icon[i];
		for (int i = 0; i < kPageCount; i++)
			if (PageButton(i, kPageNames[i], icons[i], kSide - 24.f))
				g_page = i;
	}
	ImGui::EndChild();

	// footer status
	const auto snapshot = RobRunReader::GetSnapshot();
	const float footerY = pos.y + kH - footerH;
	dl->AddLine(ImVec2(pos.x + 14.f, footerY), ImVec2(pos.x + kSide - 14.f, footerY), C(kStroke), 1.f);

	const ImVec2 dot(pos.x + 20.f, footerY + 18.f);
	dl->AddCircleFilled(dot, 4.f, C(attached ? kGreen : kRed));
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		ImVec2(pos.x + 32.f, footerY + 11.f), C(attached ? kTextDim : kRed),
		attached ? "DMA CONNECTED" : "DMA OFFLINE");

	char line[64];
	std::snprintf(line, sizeof(line), "%.0f FPS", ImGui::GetIO().Framerate);
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		ImVec2(pos.x + 20.f, footerY + 34.f), C(kTextFaint), line);

	if (snapshot)
		std::snprintf(line, sizeof(line), "%zu ENT", snapshot->entities.size());
	else
		std::snprintf(line, sizeof(line), "0 ENT");
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		ImVec2(pos.x + 20.f, footerY + 50.f), C(kTextFaint), line);

	SYSTEMTIME st{};
	GetLocalTime(&st);
	std::snprintf(line, sizeof(line), "%02d:%02d", st.wHour, st.wMinute);
	TextRight(font::inter_medium_small, pos.x + kSide - 20.f, footerY + 50.f, C(kTextFaint), line);

	const ImRect btn(pos + ImVec2(14.f, footerY + 70.f), pos + ImVec2(kSide - 14.f, footerY + 98.f));
	const bool btnHover = ImGui::IsMouseHoveringRect(btn.Min, btn.Max);
	dl->AddRectFilled(btn.Min, btn.Max, C(kAccent, btnHover ? 0.28f : 0.14f), 6.f);
	dl->AddRect(btn.Min, btn.Max, C(kAccent, btnHover ? 0.60f : 0.30f), 6.f, 0, 1.f);
	const char* btnLabel = "Re-attach Game";
	const float tw = TextW(font::inter_medium_small, btnLabel);
	const float th = font::inter_medium_small->FontSize;
	dl->AddText(font::inter_medium_small, th,
		ImVec2((btn.Min.x + btn.Max.x - tw) * 0.5f,
			(btn.Min.y + btn.Max.y - th) * 0.5f - 1.f),
		C(btnHover ? kAccentBright : kText), btnLabel);
	ImGui::SetCursorScreenPos(btn.Min);
	if (ImGui::InvisibleButton("##sidebar_reattach", btn.GetSize()))
		RequestReattach();
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
void ApplyStyleColors()
{
	ImGuiStyle& style = ImGui::GetStyle();
	ImVec4* colors = style.Colors;
	colors[ImGuiCol_Text] = kText;
	colors[ImGuiCol_TextDisabled] = kTextFaint;
	colors[ImGuiCol_WindowBg] = kPopup;
	colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
	colors[ImGuiCol_PopupBg] = kPopup;
	colors[ImGuiCol_Border] = ImVec4(1, 1, 1, g_highContrast ? 0.16f : 0.08f);
	colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
	colors[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, g_highContrast ? 0.10f : 0.05f);
	colors[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, g_highContrast ? 0.14f : 0.08f);
	colors[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, g_highContrast ? 0.18f : 0.10f);
	colors[ImGuiCol_TitleBg] = kPopup;
	colors[ImGuiCol_TitleBgActive] = kPopup;
	colors[ImGuiCol_TitleBgCollapsed] = kPopup;
	colors[ImGuiCol_MenuBarBg] = kPopup;
	colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
	colors[ImGuiCol_ScrollbarGrab] = ImVec4(1.f, 1.f, 1.f, g_highContrast ? 0.28f : 0.16f);
	colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.75f);
	colors[ImGuiCol_ScrollbarGrabActive] = kAccent;
	colors[ImGuiCol_CheckMark] = kAccentBright;
	colors[ImGuiCol_SliderGrab] = kAccent;
	colors[ImGuiCol_SliderGrabActive] = kAccentBright;
	colors[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.06f);
	colors[ImGuiCol_ButtonHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
	colors[ImGuiCol_ButtonActive] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.5f);
	colors[ImGuiCol_Header] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.22f);
	colors[ImGuiCol_HeaderHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.32f);
	colors[ImGuiCol_HeaderActive] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.42f);
	colors[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.07f);
	colors[ImGuiCol_SeparatorHovered] = kAccent;
	colors[ImGuiCol_SeparatorActive] = kAccentBright;
	colors[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
	colors[ImGuiCol_ResizeGripHovered] = kAccent;
	colors[ImGuiCol_ResizeGripActive] = kAccentBright;
	colors[ImGuiCol_Tab] = ImVec4(1, 1, 1, 0.05f);
	colors[ImGuiCol_TabHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.3f);
	colors[ImGuiCol_TabSelected] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
	colors[ImGuiCol_TabDimmed] = ImVec4(1, 1, 1, 0.03f);
	colors[ImGuiCol_TabDimmedSelected] = ImVec4(1, 1, 1, 0.06f);
	colors[ImGuiCol_NavCursor] = kAccent;
	colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
}

void Init()
{
	ImGuiIO& io = ImGui::GetIO();

	static ImWchar glyphRanges[] = { 0x0020, 0x00FF, 0x2000, 0x206F, 0xE000, 0xF8FF, 0 };

	ImFontConfig cfg;
	cfg.FontDataOwnedByAtlas = false;

	font::inter_medium_widget = io.Fonts->AddFontFromMemoryTTF(inter_medium, sizeof(inter_medium), 15.f, &cfg, glyphRanges);
	font::inter_medium_small = io.Fonts->AddFontFromMemoryTTF(inter_medium, sizeof(inter_medium), 13.f, &cfg, glyphRanges);
	font::inter_medium_big = io.Fonts->AddFontFromMemoryTTF(inter_bold, sizeof(inter_bold), 22.f, &cfg, glyphRanges);
	font::inter_bold = io.Fonts->AddFontFromMemoryTTF(inter_bold, sizeof(inter_bold), 16.f, &cfg, glyphRanges);
	font::icomoon = io.Fonts->AddFontFromMemoryTTF(icomoon, sizeof(icomoon), 17.f, &cfg, glyphRanges);
	font::icomoon_default = io.Fonts->AddFontFromMemoryTTF(icomoon, sizeof(icomoon), 20.f, &cfg, glyphRanges);

	if (font::inter_medium_widget)
		io.FontDefault = font::inter_medium_widget;

	// Load selectable fonts from files
	{
		wchar_t exePath[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		auto exeDir = std::filesystem::path(exePath).parent_path();
		struct { int idx; std::string path; } fontFiles[] = {
			{ 0, (exeDir / "fonts" / "MavenPro-Regular.ttf").string() },
			{ 1, (exeDir / "fonts" / "AtkinsonHyperlegible-Regular.ttf").string() },
			{ 2, "C:\\Windows\\Fonts\\arial.ttf" },
			{ 3, "C:\\Windows\\Fonts\\segoeui.ttf" },
			{ 4, "C:\\Windows\\Fonts\\consola.ttf" },
			{ 5, "C:\\Windows\\Fonts\\tahoma.ttf" },
			{ 6, "C:\\Windows\\Fonts\\verdana.ttf" },
		};
		for (auto& f : fontFiles)
		{
			if (std::filesystem::exists(f.path))
				g_fonts[f.idx].ptr = io.Fonts->AddFontFromFileTTF(f.path.c_str(), 15.f, nullptr, glyphRanges);
		}

		g_fontIndex = 0;
		for (int i = 0; i < kFontCount; i++)
		{
			if (ConfigInstance.Overlay.FontName == g_fonts[i].name && g_fonts[i].ptr)
			{
				g_fontIndex = i;
				break;
			}
		}
		ApplySelectedFont();
	}

	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 12.f;
	style.ChildRounding = 10.f;
	style.FrameRounding = 8.f;
	style.PopupRounding = 10.f;
	style.GrabRounding = 6.f;
	style.ScrollbarRounding = 6.f;
	style.TabRounding = 6.f;
	style.WindowBorderSize = 0.f;
	style.ChildBorderSize = 0.f;
	style.FrameBorderSize = 0.f;
	style.WindowPadding = ImVec2(0.f, 0.f);
	style.FramePadding = ImVec2(10.f, 6.f);
	style.ItemSpacing = ImVec2(0.f, 7.f);
	style.ItemInnerSpacing = ImVec2(6.f, 4.f);
	style.ScrollbarSize = 8.f;
	style.AntiAliasedLines = true;
	style.AntiAliasedFill = true;

	g_highContrast = ConfigInstance.Overlay.HighContrastMenu;
	ApplyPalette();
	ApplyStyleColors();

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	g_icon[0] = LoadTextureFromMemory(visuals, sizeof(visuals));
	g_icon[1] = LoadTextureFromMemory(world, sizeof(world));
	g_icon[2] = LoadTextureFromMemory(world, sizeof(world));
	g_icon[3] = LoadTextureFromMemory(ragebot, sizeof(ragebot));
	g_icon[4] = LoadTextureFromMemory(world, sizeof(world));
	g_icon[5] = LoadTextureFromMemory(misc, sizeof(misc));
	g_icon[6] = nullptr;
	g_icon[7] = LoadTextureFromMemory(settings, sizeof(settings));
}

void Draw(bool attached, const std::string& statusText)
{
	ImGuiIO& io = ImGui::GetIO();

	if (g_highContrast != ConfigInstance.Overlay.HighContrastMenu)
	{
		g_highContrast = ConfigInstance.Overlay.HighContrastMenu;
		ApplyPalette();
		ApplyStyleColors();
	}

	ImGui::SetNextWindowSizeConstraints(
		ImVec2(kMinW + kGlow * 2.f, kMinH + kGlow * 2.f),
		ImVec2(io.DisplaySize.x, io.DisplaySize.y));
	if (g_dragDelta.x != 0.f || g_dragDelta.y != 0.f)
	{
		// Apply last frame's drag, clamped so the top bar stays reachable.
		ImVec2 target = (g_lastPos.x >= 0.f)
			? g_lastPos + g_dragDelta
			: ImVec2((io.DisplaySize.x - kW) * 0.5f, (io.DisplaySize.y - kH) * 0.5f);
		target.x = ImClamp(target.x, -kW * 0.5f, ImMax(io.DisplaySize.x - kW * 0.5f, 0.f));
		target.y = ImClamp(target.y, 0.f, ImMax(io.DisplaySize.y - kTop, 0.f));
		ImGui::SetNextWindowPos(target, ImGuiCond_Always);
		g_dragDelta = ImVec2(0.f, 0.f);
	}
	else if (!g_placementApplied)
	{
		// First frame this run: reopen where the user left the menu. The
		// placement is clamped so a smaller monitor can never hide the bar.
		g_placementApplied = true;
		const WindowPlacement& saved = ConfigInstance.Overlay.MenuWindow;
		if (saved.Saved)
		{
			const float w = ImClamp(saved.Width, kMinW, io.DisplaySize.x);
			const float h = ImClamp(saved.Height, kMinH, io.DisplaySize.y);
			const ImVec2 target(
				ImClamp(saved.X, -w * 0.5f, ImMax(io.DisplaySize.x - w * 0.5f, 0.f)),
				ImClamp(saved.Y, 0.f, ImMax(io.DisplaySize.y - kTop, 0.f)));
			ImGui::SetNextWindowPos(target, ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
		}
		else
		{
			ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - kW) * 0.5f - kGlow,
				(io.DisplaySize.y - kH) * 0.5f - kGlow), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize(ImVec2(kW + kGlow * 2.f, kH + kGlow * 2.f),
				ImGuiCond_FirstUseEver);
		}
	}

	ImGui::Begin("##ROBRUN", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);

	{
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		const ImVec2 winSize = window->Size;
		kW = ImMax(winSize.x - kGlow * 2.f, kMinW);
		kH = ImMax(winSize.y - kGlow * 2.f, kMinH);
		g_singleColumn = kW < kTwoColumnMin;

		const ImVec2 pos = ImGui::GetWindowPos() + ImVec2(kGlow, kGlow);

		// Keep the saved placement in sync while the menu is open, so closing
		// the menu (which saves the config) records the final position.
		{
			WindowPlacement& place = ConfigInstance.Overlay.MenuWindow;
			const ImVec2 wp = ImGui::GetWindowPos();
			const ImVec2 ws = ImGui::GetWindowSize();
			place.Saved = true;
			place.X = wp.x;
			place.Y = wp.y;
			place.Width = ws.x;
			place.Height = ws.y;
		}

		ImDrawList* dl = ImGui::GetWindowDrawList();

		// ---- drag by the top bar ------------------------------------------
		// The window has no title bar, so dragging is handled here. The search
		// field and close button are excluded so they keep their clicks. The
		// accumulated delta is applied at the start of the next frame, which
		// keeps ImGui's clip/hit rects consistent.
		{
			const float fieldW = 320.f;
			const float fieldX = pos.x + kSide + ((kW - kSide) - fieldW) * 0.5f;
			const ImRect dragZone(pos, pos + ImVec2(kW, kTop));
			const ImRect searchRect(ImVec2(fieldX - 8.f, pos.y + 10.f),
				ImVec2(fieldX + fieldW + 8.f, pos.y + 56.f));
			const ImRect closeRect(pos + ImVec2(kW - 52.f, 13.f),
				pos + ImVec2(kW - 14.f, 51.f));
			const ImVec2 mouse = io.MousePos;
			const bool inZone = dragZone.Contains(mouse) &&
				!searchRect.Contains(mouse) && !closeRect.Contains(mouse);

			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
				g_dragging = false;
			else if (inZone && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
				!ImGui::IsAnyItemActive())
				g_dragging = true;

			if (g_dragging)
				g_dragDelta += io.MouseDelta;
		}

		// base surface: gradient normally, one flat fill in high contrast
		if (g_highContrast)
			dl->AddRectFilled(pos, pos + ImVec2(kW, kH), C(kBgTop), 14.f);
		else
			dl->AddRectFilledMultiColor(pos, pos + ImVec2(kW, kH),
				C(kBgTop), C(kBgTop), C(kBgBottom), C(kBgBottom), 14.f);
		dl->AddRect(pos, pos + ImVec2(kW, kH), C(kStroke, 1.f), 14.f, 0, 1.f);

		// ambient purple glow behind the content
		if (!g_highContrast)
		{
			dl->PushClipRect(pos + ImVec2(kSide, kTop), pos + ImVec2(kW, kH), true);
			dl->AddShadowCircle(pos + ImVec2(kSide, kTop + 120.f), 110.f, C(kAccent, 0.10f), 320.f, ImVec2(0, 0), 0);
			dl->AddShadowCircle(pos + ImVec2(kW, kTop + 120.f), 110.f, C(kAccent, 0.10f), 320.f, ImVec2(0, 0), 0);
			dl->AddShadowCircle(pos + ImVec2((kSide + kW) * 0.5f, kH), 120.f, C(kAccent, 0.08f), 320.f, ImVec2(0, 0), 0);
			dl->PopClipRect();
		}

		// top bar divider
		dl->AddLine(pos + ImVec2(kSide, kTop), pos + ImVec2(kW, kTop), C(kStroke), 1.f);

		// sidebar panel (drawn before the top bar so the logo stays on top)
		dl->AddRectFilled(pos, pos + ImVec2(kSide, kH), C(kNav), 12.f, ImDrawFlags_RoundCornersLeft);
		dl->AddLine(pos + ImVec2(kSide, 0), pos + ImVec2(kSide, kH), C(kStroke), 1.f);

		DrawTopBar(pos);
		DrawSidebar(pos, attached);

		// ---- page transition -------------------------------------------
		if (g_page != g_drawnPage)
		{
			g_fade -= io.DeltaTime / 0.09f;
			if (g_fade <= 0.f)
			{
				g_fade = 0.f;
				g_slideDir = g_page > g_drawnPage ? 1.f : -1.f;
				g_drawnPage = g_page;
				g_slide = 18.f * g_slideDir;
				g_scrollReset = true;
			}
		}
		else
		{
			g_fade = ImMin(g_fade + io.DeltaTime / 0.14f, 1.f);
		}
		g_slide = ImLerp(g_slide, 0.f, 1.f - std::exp(-io.DeltaTime * 14.f));

		const float ease = 1.f - std::pow(1.f - ImClamp(g_fade, 0.f, 1.f), 3.f);

		const ImVec2 contentMin(pos.x + kSide + g_slide, pos.y + kTop);
		const ImVec2 contentSize(kW - kSide, kH - kTop);

		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ease);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPad, kPad));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, kGap));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
		ImGui::SetCursorScreenPos(contentMin);
		ImGui::BeginChild("##content", contentSize, ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoSavedSettings);
		// Width comes from the live content region so the cards stay centred
		// whether or not the page needs a scrollbar.
		const float availW = ImGui::GetContentRegionAvail().x;
		kCardW = g_singleColumn ? availW : (availW - kGap) * 0.5f;
		if (g_scrollReset)
		{
			ImGui::SetScrollY(0.f);
			g_scrollReset = false;
		}

		g_forcedHeights.clear();

		switch (g_drawnPage)
		{
		case 0: DrawVisuals(); break;
		case 1: DrawRadar(); break;
		case 2: DrawLootFilter(); break;
		case 3: DrawAim(); break;
		case 4: DrawServer(); break;
		case 5: DrawMisc(); break;
		case 6: DrawSkins(); break;
		case 7: DrawSettings(attached, statusText); break;
		default: break;
		}

		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);

		// resize hint: two short diagonals in the bottom-right corner
		{
			const ImVec2 c(pos.x + kW - 6.f, pos.y + kH - 6.f);
			const ImU32 col = C(kTextFaint, 0.85f);
			dl->AddLine(ImVec2(c.x - 10.f, c.y), ImVec2(c.x, c.y - 10.f), col, 1.4f);
			dl->AddLine(ImVec2(c.x - 4.f, c.y), ImVec2(c.x, c.y - 4.f), col, 1.4f);
		}

		g_lastPos = window->Pos;
	}

	ImGui::End();
}

} // namespace VeoxMenu
