#include "pch.h"
#include "VeoxMenu.h"
#include "ConfigInstance.h"
#include "ConfigFile.h"
#include "RobRunReader.h"
#include "DeviceSession.h"
#include "InputManager.h"
#include "../Aim/AimController.h"
#include "../Overlay/Overlay.h"
#include "../Esp/Radar.h"
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
#include <vector>

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
constexpr float kW = 900.f;
constexpr float kH = 560.f;
constexpr float kTop = 66.f;
constexpr float kSide = 176.f;
constexpr float kPad = 18.f;
constexpr float kGap = 14.f;
constexpr float kCardHeader = 40.f;
constexpr float kRow = 22.f;
constexpr float kCardW = (kW - kSide - kPad * 2.f - kGap) * 0.5f;
// The ImGui window is a little larger than the visible panel so the drop
// shadow around the panel is not clipped away by the window draw list.
constexpr float kGlow = 14.f;

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
ImVec4 kPanel = ImVec4(255 / 255.f, 255 / 255.f, 255 / 255.f, 0.028f);
ImVec4 kCard = ImVec4(13 / 255.f, 12 / 255.f, 19 / 255.f, 0.94f);
ImVec4 kCardTop = ImVec4(18 / 255.f, 16 / 255.f, 26 / 255.f, 1.f);
ImVec4 kPopup = ImVec4(12 / 255.f, 11 / 255.f, 18 / 255.f, 1.f);
ImVec4 kStroke = ImVec4(1.f, 1.f, 1.f, 0.055f);
ImVec4 kTrack = ImVec4(1.f, 1.f, 1.f, 0.075f);
ImVec4 kText = ImVec4(236 / 255.f, 233 / 255.f, 247 / 255.f, 1.f);
ImVec4 kTextDim = ImVec4(140 / 255.f, 135 / 255.f, 166 / 255.f, 1.f);
ImVec4 kTextFaint = ImVec4(92 / 255.f, 88 / 255.f, 114 / 255.f, 1.f);

bool g_highContrast = false;

void ApplyPalette()
{
	if (g_highContrast)
	{
		// Solid fills and brighter strokes/text so the menu stays readable
		// over bright game scenes.
		kBgTop = ImVec4(18 / 255.f, 16 / 255.f, 27 / 255.f, 1.f);
		kBgBottom = ImVec4(8 / 255.f, 7 / 255.f, 12 / 255.f, 1.f);
		kPanel = ImVec4(1.f, 1.f, 1.f, 0.07f);
		kCard = ImVec4(25 / 255.f, 23 / 255.f, 35 / 255.f, 1.f);
		kCardTop = ImVec4(35 / 255.f, 32 / 255.f, 49 / 255.f, 1.f);
		kPopup = ImVec4(25 / 255.f, 23 / 255.f, 35 / 255.f, 1.f);
		kStroke = ImVec4(1.f, 1.f, 1.f, 0.17f);
		kTrack = ImVec4(1.f, 1.f, 1.f, 0.22f);
		kText = ImVec4(1.f, 1.f, 1.f, 1.f);
		kTextDim = ImVec4(0.82f, 0.80f, 0.90f, 1.f);
		kTextFaint = ImVec4(0.64f, 0.62f, 0.74f, 1.f);
	}
	else
	{
		kBgTop = ImVec4(11 / 255.f, 9 / 255.f, 18 / 255.f, 1.f);
		kBgBottom = ImVec4(4 / 255.f, 4 / 255.f, 7 / 255.f, 1.f);
		kPanel = ImVec4(255 / 255.f, 255 / 255.f, 255 / 255.f, 0.028f);
		kCard = ImVec4(13 / 255.f, 12 / 255.f, 19 / 255.f, 0.94f);
		kCardTop = ImVec4(18 / 255.f, 16 / 255.f, 26 / 255.f, 1.f);
		kPopup = ImVec4(12 / 255.f, 11 / 255.f, 18 / 255.f, 1.f);
		kStroke = ImVec4(1.f, 1.f, 1.f, 0.055f);
		kTrack = ImVec4(1.f, 1.f, 1.f, 0.075f);
		kText = ImVec4(236 / 255.f, 233 / 255.f, 247 / 255.f, 1.f);
		kTextDim = ImVec4(140 / 255.f, 135 / 255.f, 166 / 255.f, 1.f);
		kTextFaint = ImVec4(92 / 255.f, 88 / 255.f, 114 / 255.f, 1.f);
	}
}

// ---- state ----------------------------------------------------------------
int g_page = 0;
int g_drawnPage = 0;
float g_fade = 1.f;
float g_slide = 0.f;
float g_slideDir = 1.f;
bool g_scrollReset = false;
char g_search[96] = "";
bool g_readCatalog = false;

const char* kPageNames[] = { "Visuals", "Radar", "Aim", "Misc", "Settings" };
constexpr int kPageCount = IM_ARRAYSIZE(kPageNames);

ID3D11ShaderResourceView* g_icon[kPageCount] = {};

std::map<ImGuiID, float> g_cardHeights;
std::map<ImGuiID, float> g_cardHover;
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
	const float h = g_cardHeights.count(id) ? g_cardHeights[id] : minHeight;

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

	Text(font::inter_bold,
		ImVec2(bb.Min.x + 14.f, bb.Min.y + (kCardHeader - font::inter_bold->FontSize) * 0.5f - 1.f),
		C(kTextDim), title);

	ImGui::SetCursorScreenPos(ImVec2(bb.Min.x, bb.Min.y + kCardHeader));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 12.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 7.f));
	ImGui::BeginChild(id, ImVec2(width, h - kCardHeader), ImGuiChildFlags_None,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings);
	g_currentCard = id;
}

void EndCard()
{
	ImGuiWindow* child = ImGui::GetCurrentWindow();
	const float content = child->ContentSize.y + ImGui::GetStyle().WindowPadding.y;
	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
	g_cardHeights[g_currentCard] = kCardHeader + content + 1.f;
}

// ---- widgets --------------------------------------------------------------
bool ToggleRow(const char* label, bool* v)
{
	if (!Match(label)) return false;

	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	const ImRect bb(p, p + ImVec2(w, kRow));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	bool hov, held, pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);

	static std::map<ImGuiID, float> anim;
	float& t = anim[id];
	t = ImLerp(t, *v ? 1.f : 0.f, ImGui::GetIO().DeltaTime * 14.f);

	if (pressed) *v = !*v;

	ImDrawList* dl = window->DrawList;
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, RowTextY(p.y, kRow, font::inter_medium_widget)),
		C(Mix(kTextDim, kText, ImMax(t, hov ? 0.55f : 0.f))), label);

	const float tw = 38.f, th = 20.f;
	const ImVec2 tmin(p.x + w - tw, p.y + (kRow - th) * 0.5f);
	const ImVec2 tmax(tmin.x + tw, tmin.y + th);
	dl->AddRectFilled(tmin, tmax, C(Mix(kTrack, kAccent, t), 0.35f + 0.65f * t), th * 0.5f);
	if (t > 0.01f)
		dl->AddShadowCircle(ImVec2((tmin.x + tmax.x) * 0.5f, (tmin.y + tmax.y) * 0.5f),
			th * 0.5f, C(kAccent, 0.30f * t), 18.f, ImVec2(0, 0), 0);

	const float kx = ImLerp(tmin.x + 10.f, tmax.x - 10.f, t);
	dl->AddCircleFilled(ImVec2(kx, (tmin.y + tmax.y) * 0.5f), 7.f,
		C(Mix(kTextDim, ImVec4(1, 1, 1, 1), t)));

	return pressed;
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
	const ImRect swatch(ImVec2(track.Min.x - 10.f - sw, p.y + 3.f), ImVec2(track.Min.x - 10.f, p.y + 19.f));

	bool thov, theld, tpressed = ImGui::ButtonBehavior(track, ImHashStr("tgl", 0, id), &thov, &theld);
	bool shov, sheld, spressed = ImGui::ButtonBehavior(swatch, ImHashStr("sw", 0, id), &shov, &sheld);

	static std::map<ImGuiID, float> anim;
	float& t = anim[id];
	t = ImLerp(t, *v ? 1.f : 0.f, ImGui::GetIO().DeltaTime * 14.f);

	char popup[96];
	std::snprintf(popup, sizeof(popup), "##colour_%s", label);

	if (tpressed) *v = !*v;
	if (spressed) ImGui::OpenPopup(popup);

	ImDrawList* dl = window->DrawList;
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, RowTextY(p.y, kRow, font::inter_medium_widget)),
		C(Mix(kTextDim, kText, ImMax(t, (thov || shov) ? 0.55f : 0.f))), label);

	dl->AddRectFilled(track.Min, track.Max, C(Mix(kTrack, kAccent, t), 0.35f + 0.65f * t), th * 0.5f);
	if (t > 0.01f)
		dl->AddShadowCircle(ImVec2((track.Min.x + track.Max.x) * 0.5f, (track.Min.y + track.Max.y) * 0.5f),
			th * 0.5f, C(kAccent, 0.30f * t), 18.f, ImVec2(0, 0), 0);
	const float kx = ImLerp(track.Min.x + 10.f, track.Max.x - 10.f, t);
	dl->AddCircleFilled(ImVec2(kx, (track.Min.y + track.Max.y) * 0.5f), 7.f,
		C(Mix(kTextDim, ImVec4(1, 1, 1, 1), t)));

	dl->AddRectFilled(swatch.Min, swatch.Max,
		C(ImVec4(colour.r, colour.g, colour.b, 1.f)), 4.f);
	dl->AddRect(swatch.Min, swatch.Max, C(kStroke, shov ? 2.2f : 1.f), 4.f);

	if (ImGui::BeginPopup(popup))
	{
		float col[4] = { colour.r, colour.g, colour.b, colour.a };
		ImGui::PushItemWidth(220.f);
		if (ImGui::ColorPicker4("##picker", col,
			ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview |
			ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel))
			colour = D2D1::ColorF(col[0], col[1], col[2], col[3]);
		ImGui::PopItemWidth();
		ImGui::EndPopup();
	}

	return tpressed;
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

	if (ImGui::BeginPopup(popup))
	{
		float col[4] = { colour.r, colour.g, colour.b, colour.a };
		ImGui::PushItemWidth(220.f);
		if (ImGui::ColorPicker4("##picker", col,
			ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview |
			ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel))
			colour = D2D1::ColorF(col[0], col[1], col[2], col[3]);
		ImGui::PopItemWidth();
		ImGui::EndPopup();
	}

	return pressed;
}

bool SliderIntRow(const char* label, int* v, int lo, int hi, const char* suffix = "")
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

bool ComboRow(const char* label, int* current, const char* const* items, int count, float fieldW = 150.f)
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

	const ImRect field(ImVec2(p.x + w - fieldW, p.y + 2.f), ImVec2(p.x + w, p.y + 26.f));
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

	int sel = ImClamp(*current, 0, count - 1);
	const char* preview = items[sel] ? items[sel] : "-";
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		ImVec2(field.Min.x + 10.f, RowTextY(field.Min.y, field.GetHeight(), font::inter_medium_small)),
		C(kText), preview);

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

bool ButtonRow(const char* label, const char* text, float width = 130.f, bool accent = false)
{
	if (label && *label && !Match(label)) return false;

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
		dl->AddRectFilled(bb.Min, bb.Max, C(kAccent, 0.13f * sel[index]), 8.f);
		dl->AddRect(bb.Min, bb.Max, C(kAccent, 0.32f * sel[index]), 8.f);
	}
	else if (hover[index] > 0.01f)
	{
		dl->AddRectFilled(bb.Min, bb.Max, C(ImVec4(1, 1, 1, 1), 0.035f * hover[index]), 8.f);
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
	ImGui::BeginGroup();
	{
		if (MatchAny({ "Box ESP", "Health", "Distance", "Primary Weapon", "Secondary Weapon",
			"Weapon Name", "Looking At You", "Looking Alert", "Show Role" }))
		{
			BeginCard("PLAYER ESP", kCardW);
			ColorToggleRow("Box ESP", &ConfigInstance.Player.BoxEsp, ConfigInstance.Player.BoxColour);
			ColorToggleRow("Health", &ConfigInstance.Player.Health, ConfigInstance.Player.HealthTextColour);
			ColorToggleRow("Distance", &ConfigInstance.Player.Distance, ConfigInstance.Player.DistanceColour);
			ToggleRow("Primary Weapon", &ConfigInstance.Player.PrimaryWeapon);
			ToggleRow("Secondary Weapon", &ConfigInstance.Player.SecondaryWeapon);
			ColorToggleRow("Weapon Name", &ConfigInstance.Player.WeaponEsp, ConfigInstance.Player.WeaponColour);
			ColorToggleRow("Looking At You", &ConfigInstance.Player.LookingAtYou, ConfigInstance.Player.LookingColour);
			ToggleRow("Looking Alert", &ConfigInstance.Player.LookingAlert);
			ColorToggleRow("Show Role", &ConfigInstance.Player.ShowRole, ConfigInstance.Player.RoleColour);
			EndCard();
		}

		if (MatchAny({ "VisCheck", "WallCheck", "Pose Smoothing", "Native Camera" }))
		{
			BeginCard("SKELETON", kCardW);
			ToggleRow("Enable VisCheck", &ConfigInstance.Player.VisCheck);
			if (ConfigInstance.Player.VisCheck)
			{
				ColorRow("Active (animating)", ConfigInstance.Player.VisibleColour);
				ColorRow("Frozen (culled)", ConfigInstance.Player.HiddenColour);
			}

			ToggleRow("Enable WallCheck", &ConfigInstance.Player.WallCheck);
			{
				auto status = RobRunReader::GetBvhStatus();
				if (status.loaded)
					Note("BVH: %u nodes, %u triangles", status.nodeCount, status.triCount);
				else
					Note("BVH: city_colliders.bvh not loaded");
			}

			ToggleRow("Pose Smoothing", &ConfigInstance.Player.PoseSmoothing);
			if (ConfigInstance.Player.PoseSmoothing)
				SliderFloatRow("Blend", &ConfigInstance.Player.PoseSmoothAlpha, 0.1f, 1.f);

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

	ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		if (MatchAny({ "Name Colour", "Distance Colour", "Health Colour", "Weapon Colour",
			"Role Colour", "Looking Colour", "State Colour", "Box Colour" }))
		{
			BeginCard("TEXT & COLOURS", kCardW);
			ColorRow("Name", ConfigInstance.Player.NameColour);
			ColorRow("Distance", ConfigInstance.Player.DistanceColour);
			ColorRow("Health", ConfigInstance.Player.HealthTextColour);
			ColorRow("Weapon", ConfigInstance.Player.WeaponColour);
			ColorRow("Role", ConfigInstance.Player.RoleColour);
			ColorRow("Looking", ConfigInstance.Player.LookingColour);
			ColorRow("State", ConfigInstance.Player.StateColour);
			ColorRow("Box", ConfigInstance.Player.BoxColour);
			EndCard();
		}

		if (MatchAny({ "Max Distance", "Font Size" }))
		{
			BeginCard("RANGE", kCardW);
			SliderIntRow("Max Distance", &ConfigInstance.Player.MaxDistance, 0, 1500, "m");
			SliderIntRow("Font Size", &ConfigInstance.Player.FontSize, 1, 24, "px");
			EndCard();
		}

		if (MatchAny({ "World ESP", "Show Empty", "Show Distance", "World Distance", "World Font" }))
		{
			BeginCard("WORLD ESP", kCardW);
			ToggleRow("Enable World ESP", &ConfigInstance.World.Enabled);
			ToggleRow("Show Empty", &ConfigInstance.World.ShowEmpty);
			ToggleRow("Show Distance", &ConfigInstance.World.ShowDistance);
			SliderIntRow("Max Distance", &ConfigInstance.World.MaxDistance, 0, 2000, "m");
			SliderIntRow("Font Size", &ConfigInstance.World.FontSize, 1, 24, "px");
			EndCard();
		}
	}
	ImGui::EndGroup();
}

void DrawRadar()
{
	ImGui::BeginGroup();
	{
		BeginCard("RADAR", kCardW);
		ToggleRow("Enable Radar", &ConfigInstance.Radar.Enabled);

		const auto& maps = Radar::GetMapFiles();
		int mapIdx = Radar::GetCurrentMapIndex();
		if (!maps.empty())
		{
			std::vector<const char*> items;
			items.reserve(maps.size());
			for (const auto& m : maps) items.push_back(m.c_str());
			int sel = ImClamp(mapIdx, 0, (int)items.size() - 1);
			if (ComboRow("Map", &sel, items.data(), (int)items.size()))
				Radar::SetMap(sel);
		}
		else
		{
			Note("No maps found in the maps folder.");
		}

		ToggleRow("Show Players", &ConfigInstance.Radar.ShowPlayers);
		ToggleRow("Show Vehicles", &ConfigInstance.Radar.ShowVehicles);
		ToggleRow("Show Loot", &ConfigInstance.Radar.ShowLoot);
		ToggleRow("Rotate With Camera", &ConfigInstance.Radar.RotateWithCamera);
		EndCard();

		BeginCard("DISPLAY", kCardW);
		SliderFloatRow("Size", &ConfigInstance.Radar.Size, 150.f, 800.f, "px");
		SliderFloatRow("Zoom", &ConfigInstance.Radar.Zoom, 0.2f, 10.f, "x");
		SliderFloatRow("Opacity", &ConfigInstance.Radar.Opacity, 0.1f, 1.f);
		EndCard();
	}
	ImGui::EndGroup();

	ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("MAP BOUNDS", kCardW);
		Note("World coordinates used to project entity positions onto the map image.");
		SectionLabel("X AXIS");
		SliderFloatRow("Min X", &ConfigInstance.Radar.WorldMinX, -5000.f, 5000.f);
		SliderFloatRow("Max X", &ConfigInstance.Radar.WorldMaxX, -5000.f, 5000.f);
		SectionLabel("Z AXIS");
		SliderFloatRow("Min Z", &ConfigInstance.Radar.WorldMinZ, -5000.f, 5000.f);
		SliderFloatRow("Max Z", &ConfigInstance.Radar.WorldMaxZ, -5000.f, 5000.f);
		if (ButtonRow("", "Reset Bounds", 130.f))
		{
			ConfigInstance.Radar.WorldMinX = -600.f;
			ConfigInstance.Radar.WorldMaxX = 600.f;
			ConfigInstance.Radar.WorldMinZ = -400.f;
			ConfigInstance.Radar.WorldMaxZ = 400.f;
		}
		EndCard();

		BeginCard("NOTE", kCardW);
		Note("The radar reads entity positions from the same snapshot as the ESP. "
			"Bounds only affect where those positions land on the image.");
		EndCard();
	}
	ImGui::EndGroup();
}

void DrawAim()
{
	using namespace Base::Hardware;

	static int deviceIndex = 0;
	static char comPort[16] = "";
	static char ipAddr[64] = "";
	static char netPort[16] = "8899";
	static char macId[16] = "";
	static int baudIndex = 0;
	static bool manualPort = false;
	static bool ferrumController = false;
	static bool needsRefresh = true;

	static const char* deviceNames[] = { "Off", "Kmbox USB", "Kmbox Net", "Makcu", "Makcu Controller",
		"Ferrum", "Makcu Medius", "Kmbox NB Pro" };
	static const DeviceKind deviceKinds[] = { DeviceKind::Off, DeviceKind::KmboxUsb, DeviceKind::KmboxNet,
		DeviceKind::Makcu, DeviceKind::Controller, DeviceKind::Ferrum, DeviceKind::Medius,
		DeviceKind::KmboxNbPro };
	static const int baudRates[] = { 115200, 921600, 1500000, 2000000, 3000000, 4000000 };
	static const char* baudLabels[] = { "115200", "921600", "1500000", "2000000", "3000000", "4000000" };

	ImGui::BeginGroup();
	{
		BeginCard("AIMBOT", kCardW);
		ToggleRow("Enable Aim", &ConfigInstance.Aim.Enabled);

		static const char* aimTypes[] = { "Memory Aim", "Hardware Aim" };
		ComboRow("Aim Type", &ConfigInstance.Aim.AimType, aimTypes, IM_ARRAYSIZE(aimTypes));

		static const char* bones[] = { "Head", "Neck", "Chest", "Pelvis" };
		ComboRow("Target Bone", &ConfigInstance.Aim.TargetBone, bones, IM_ARRAYSIZE(bones));

		ComboRow("Aim Key", &ConfigInstance.Aim.AimKey, Aim::KeyNames(), Aim::KeyCount());

		SliderIntRow("FOV", &ConfigInstance.Aim.Fov, 5, 180, " deg");
		SliderIntRow("Smoothness", &ConfigInstance.Aim.Smoothness, 1, 100);
		SliderIntRow("Max Distance", &ConfigInstance.Aim.MaxDistance, 10, 1000, "m");

		ToggleRow("Visible Bones Only", &ConfigInstance.Aim.VisibleBonesOnly);
		ToggleRow("Show FOV Circle", &ConfigInstance.Aim.ShowFov);
		ToggleRow("Show Aim Debug", &ConfigInstance.Aim.ShowDebug);
		EndCard();

		BeginCard("MEMORY AIM", kCardW);
		ToggleRow("Write Camera Angles", &ConfigInstance.Aim.MemoryWrite);
		if (ConfigInstance.Aim.MemoryWrite)
		{
			ToggleRow("Cursor Fallback", &ConfigInstance.Aim.CursorFallback);
			SliderIntRow("Response", &ConfigInstance.Aim.ResponseMs, 0, 250, " ms");
			Note("0 ms derives the response from Smoothness. The game may rebuild the camera "
				"from its own input, so a write can be reverted.");
		}
		EndCard();
	}
	ImGui::EndGroup();

	ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("PREDICTION", kCardW);
		ToggleRow("Target Prediction", &ConfigInstance.Aim.Prediction);
		if (ConfigInstance.Aim.Prediction)
		{
			SliderIntRow("Lead Strength", &ConfigInstance.Aim.PredictionStrength, 0, 200, "%");
			SliderIntRow("Vertical Lead", &ConfigInstance.Aim.PredictionYScale, 0, 100, "%");
		}
		ToggleRow("Hard Lock", &ConfigInstance.Aim.HardLock);
		EndCard();

		BeginCard("WEAPON", kCardW);
		ToggleRow("No Recoil", &ConfigInstance.Aim.NoRecoil);
		ToggleRow("No Spread", &ConfigInstance.Aim.NoSpread);
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
}

void DrawMisc()
{
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

		BeginCard("READ-ONLY DMA", kCardW);
		Note("ROB RUN is an external DMA reader. It never injects into the game process.");
		Note("- VisCheck compares animator poses. It is not a wall check.");
		Note("- WallCheck needs a city_colliders.bvh next to the exe.");
		Note("- Memory aim writes the camera block over DMA; the game can rebuild it from input.");
		Note("- Hardware aim sends the same pixel delta through the selected device.");
		EndCard();
	}
	ImGui::EndGroup();

	ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("SESSION", kCardW);
		const auto snapshot = RobRunReader::GetSnapshot();
		if (snapshot)
		{
			const auto world = snapshot->world ? *snapshot->world : RobRunWorld::State{};
			Note("Entities %zu  (C:%d L:%d V:%d G:%d)", snapshot->entities.size(),
				world.containers, world.loots, world.vehicles, world.grenades);
			Note("Skeletons %u | Aim poses %u | Pose retries %u", snapshot->skeletonCount,
				snapshot->poseCount, snapshot->poseFailures);
			Note("Read failures %u", snapshot->failureCount);
		}
		else
		{
			Note("No snapshot yet. Waiting for the game process.");
		}
		Note("Overlay %.0f FPS", ImGui::GetIO().Framerate);
		EndCard();

		BeginCard("SHORTCUTS", kCardW);
		Note("INSERT  -  show or hide this menu");
		Note("DELETE  -  toggle the debug overlay (debug builds)");
		Note("Right click a toggle with a cog for extra options.");
		EndCard();
	}
	ImGui::EndGroup();
}

void DrawSettings(bool attached, const std::string& statusText)
{
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

		ToggleRow("High Contrast Menu", &ConfigInstance.Overlay.HighContrastMenu);
		Note("Solid card fills and brighter text, for bright game scenes.");

		ToggleRow("Show FPS", &ConfigInstance.Overlay.FPSEnabled);

		int cap = Overlay::FrameCap();
		if (SliderIntRow("Frame Cap", &cap, 0, 360, " fps"))
			Overlay::SetFrameCap(cap);

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
	}
	ImGui::EndGroup();

	ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("CONFIG", kCardW);
		if (ButtonRow("", "Save Config", 130.f, true))
		{
			const bool ok = ConfigFile::Save();
			Overlay::Notify(ok ? "Config saved" : "Config save failed",
				ok ? IM_COL32(74, 201, 126, 255) : IM_COL32(255, 75, 75, 255), 2.f);
		}
		if (ButtonRow("", "Load Config", 130.f))
		{
			const bool ok = ConfigFile::Load();
			Overlay::Notify(ok ? "Config loaded" : "Config load failed",
				ok ? IM_COL32(74, 201, 126, 255) : IM_COL32(255, 75, 75, 255), 2.f);
		}
		Note("C:\\VeoxDMA\\RobRun\\config.json");
		Note("Saved automatically when the overlay closes.");
		EndCard();

		BeginCard("STATUS", kCardW);
		Note("%s", attached ? "DMA connected" : "DMA disconnected");
		Note("%s", statusText.c_str());
		Note("Overlay %.0f FPS", ImGui::GetIO().Framerate);
		const auto snapshot = RobRunReader::GetSnapshot();
		if (snapshot)
			Note("%zu entities | %u skeletons | %u read failures", snapshot->entities.size(),
				snapshot->skeletonCount, snapshot->failureCount);
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

	ImGui::SetCursorScreenPos(pos + ImVec2(12.f, kTop + 16.f));
	ImGui::BeginGroup();
	{
		const ImTextureID icons[kPageCount] = {
			(ImTextureID)(intptr_t)g_icon[0],
			(ImTextureID)(intptr_t)g_icon[1],
			(ImTextureID)(intptr_t)g_icon[2],
			(ImTextureID)(intptr_t)g_icon[3],
			(ImTextureID)(intptr_t)g_icon[4],
		};
		for (int i = 0; i < kPageCount; i++)
			if (PageButton(i, kPageNames[i], icons[i], kSide - 24.f))
				g_page = i;
	}
	ImGui::EndGroup();

	// footer status
	const auto snapshot = RobRunReader::GetSnapshot();
	const float footerY = kH - 84.f;
	dl->AddLine(pos + ImVec2(14.f, footerY), pos + ImVec2(kSide - 14.f, footerY), C(kStroke), 1.f);

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
	colors[ImGuiCol_ScrollbarGrab] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.55f);
	colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.8f);
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
	g_icon[2] = LoadTextureFromMemory(ragebot, sizeof(ragebot));
	g_icon[3] = LoadTextureFromMemory(misc, sizeof(misc));
	g_icon[4] = LoadTextureFromMemory(settings, sizeof(settings));
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

	ImGui::SetNextWindowSize(ImVec2(kW + kGlow * 2.f, kH + kGlow * 2.f), ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - kW) * 0.5f - kGlow,
		(io.DisplaySize.y - kH) * 0.5f - kGlow), ImGuiCond_Always);

	ImGui::Begin("##ROBRUN", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);

	{
		const ImVec2 pos = ImGui::GetWindowPos() + ImVec2(kGlow, kGlow);
		ImDrawList* dl = ImGui::GetWindowDrawList();

		// drop shadow
		dl->AddShadowRect(pos, pos + ImVec2(kW, kH), C(kAccent, 0.22f), 34.f, ImVec2(0, 4), 0, 16.f);
		dl->AddShadowRect(pos, pos + ImVec2(kW, kH), C(ImVec4(0, 0, 0, 1), 0.55f), 18.f, ImVec2(0, 3), 0, 16.f);

		// base gradient
		dl->AddRectFilledMultiColor(pos, pos + ImVec2(kW, kH),
			C(kBgTop), C(kBgTop), C(kBgBottom), C(kBgBottom), 14.f);
		dl->AddRect(pos, pos + ImVec2(kW, kH), C(kStroke, 1.f), 14.f, 0, 1.f);

		// ambient purple glow behind the content
		dl->PushClipRect(pos + ImVec2(kSide, kTop), pos + ImVec2(kW, kH), true);
		dl->AddShadowCircle(pos + ImVec2(kSide, kTop + 120.f), 110.f, C(kAccent, 0.10f), 320.f, ImVec2(0, 0), 0);
		dl->AddShadowCircle(pos + ImVec2(kW, kTop + 120.f), 110.f, C(kAccent, 0.10f), 320.f, ImVec2(0, 0), 0);
		dl->AddShadowCircle(pos + ImVec2((kSide + kW) * 0.5f, kH), 120.f, C(kAccent, 0.08f), 320.f, ImVec2(0, 0), 0);
		dl->PopClipRect();

		// top bar divider
		dl->AddLine(pos + ImVec2(kSide, kTop), pos + ImVec2(kW, kTop), C(kStroke), 1.f);

		// sidebar panel (drawn before the top bar so the logo stays on top)
		dl->AddRectFilled(pos, pos + ImVec2(kSide, kH), C(kPanel), 12.f, ImDrawFlags_RoundCornersLeft);
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
		ImGui::BeginChild("##content", contentSize, ImGuiChildFlags_None,
			ImGuiWindowFlags_NoSavedSettings);
		if (g_scrollReset)
		{
			ImGui::SetScrollY(0.f);
			g_scrollReset = false;
		}

		switch (g_drawnPage)
		{
		case 0: DrawVisuals(); break;
		case 1: DrawRadar(); break;
		case 2: DrawAim(); break;
		case 3: DrawMisc(); break;
		case 4: DrawSettings(attached, statusText); break;
		default: break;
		}

		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
	}

	ImGui::End();
}

} // namespace VeoxMenu
