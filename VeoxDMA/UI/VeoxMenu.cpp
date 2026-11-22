#include "pch.h"
#include <fstream>
#include <commdlg.h>

// Native open/save dialog so a loot filter config can live anywhere.
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
#include "Memory.h"
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
#include <array>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <filesystem>
#include <vector>

extern void RequestReattach();

// ---------------------------------------------------------------------------
// ROB RUN overlay menu — Top-Tab + Sub-Tab Architecture.
//
// Fully custom ImDrawList-rendered flat matte dark UI with customizable accent
// colour, pill-only toggles (zero checkboxes), single-line inline sliders,
// cross-tab global search with jump-to highlight, and zero vertical scrolling
// on normal settings sub-tabs.
// ---------------------------------------------------------------------------
namespace VeoxMenu {
namespace {

// ---- geometry -------------------------------------------------------------
float kW = 920.f;
float kH = 650.f;
constexpr float kLogoW = 64.f;
constexpr float kTop = 62.f;
constexpr float kSubBar = 46.f;
constexpr float kFooter = 36.f;
constexpr float kPadX = 18.f;
constexpr float kPadTop = 6.f;
constexpr float kPadBot = 12.f;
constexpr float kGap = 14.f;
constexpr float kCardGap = 12.f;
constexpr float kCardHeader = 36.f;
constexpr float kRow = 22.f;
float kCardW = 435.f;
float kAvailH = 450.f;
bool g_singleColumn = false;
constexpr float kMinW = 760.f;
constexpr float kMinH = 540.f;
constexpr float kTwoColumnMin = 780.f;

// ---- palette --------------------------------------------------------------
ImVec4 kAccent = ImVec4(139 / 255.f, 92 / 255.f, 246 / 255.f, 1.f);
ImVec4 kAccentBright = ImVec4(168 / 255.f, 132 / 255.f, 255 / 255.f, 1.f);
const ImVec4 kGreen = ImVec4(70 / 255.f, 194 / 255.f, 120 / 255.f, 1.f);
const ImVec4 kAmber = ImVec4(230 / 255.f, 184 / 255.f, 76 / 255.f, 1.f);
const ImVec4 kRed = ImVec4(229 / 255.f, 83 / 255.f, 83 / 255.f, 1.f);
const ImVec4 kTeal = ImVec4(46 / 255.f, 196 / 255.f, 182 / 255.f, 1.f);

ImVec4 kBg = ImVec4(16 / 255.f, 16 / 255.f, 22 / 255.f, 1.f);
ImVec4 kCard = ImVec4(20 / 255.f, 20 / 255.f, 28 / 255.f, 1.f);
ImVec4 kElevated = ImVec4(26 / 255.f, 26 / 255.f, 36 / 255.f, 1.f);
ImVec4 kControl = ImVec4(28 / 255.f, 28 / 255.f, 39 / 255.f, 1.f);
ImVec4 kPopup = ImVec4(20 / 255.f, 20 / 255.f, 28 / 255.f, 1.f);
ImVec4 kStroke = ImVec4(30 / 255.f, 30 / 255.f, 41 / 255.f, 1.f);
ImVec4 kStrokeSoft = ImVec4(25 / 255.f, 25 / 255.f, 35 / 255.f, 1.f);
ImVec4 kTrack = ImVec4(35 / 255.f, 35 / 255.f, 48 / 255.f, 1.f);
ImVec4 kText = ImVec4(228 / 255.f, 228 / 255.f, 238 / 255.f, 1.f);
ImVec4 kTextMid = ImVec4(182 / 255.f, 182 / 255.f, 200 / 255.f, 1.f);
ImVec4 kTextDim = ImVec4(104 / 255.f, 104 / 255.f, 124 / 255.f, 1.f);
ImVec4 kTextFaint = ImVec4(76 / 255.f, 76 / 255.f, 94 / 255.f, 1.f);

bool g_highContrast = false;
ImVec4 g_lastAccent = ImVec4(-1.f, -1.f, -1.f, -1.f);

// ---- selectable fonts -----------------------------------------------------
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

static void SyncFontFromConfig()
{
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

void SyncAccent()
{
	const auto& ac = ConfigInstance.Overlay.AccentColour;
	kAccent = ImVec4(
		ImClamp(ac.r, 0.08f, 1.f),
		ImClamp(ac.g, 0.08f, 1.f),
		ImClamp(ac.b, 0.08f, 1.f),
		1.f);
	kAccentBright = ImVec4(
		ImClamp(kAccent.x * 0.78f + 0.22f, 0.f, 1.f),
		ImClamp(kAccent.y * 0.78f + 0.22f, 0.f, 1.f),
		ImClamp(kAccent.z * 0.78f + 0.22f, 0.f, 1.f),
		1.f);
}

void ApplyPalette()
{
	SyncAccent();
	if (g_highContrast)
	{
		kBg = ImVec4(36 / 255.f, 34 / 255.f, 47 / 255.f, 1.f);
		kCard = ImVec4(44 / 255.f, 41 / 255.f, 57 / 255.f, 1.f);
		kElevated = ImVec4(54 / 255.f, 50 / 255.f, 70 / 255.f, 1.f);
		kControl = ImVec4(61 / 255.f, 57 / 255.f, 79 / 255.f, 1.f);
		kPopup = kCard;
		kStroke = ImVec4(74 / 255.f, 69 / 255.f, 96 / 255.f, 1.f);
		kStrokeSoft = ImVec4(65 / 255.f, 60 / 255.f, 84 / 255.f, 1.f);
		kTrack = ImVec4(76 / 255.f, 71 / 255.f, 100 / 255.f, 1.f);
		kText = ImVec4(1.f, 1.f, 1.f, 1.f);
		kTextMid = ImVec4(230 / 255.f, 228 / 255.f, 242 / 255.f, 1.f);
		kTextDim = ImVec4(180 / 255.f, 176 / 255.f, 201 / 255.f, 1.f);
		kTextFaint = ImVec4(142 / 255.f, 138 / 255.f, 166 / 255.f, 1.f);
	}
	else
	{
		kBg = ImVec4(16 / 255.f, 16 / 255.f, 22 / 255.f, 1.f);
		kCard = ImVec4(20 / 255.f, 20 / 255.f, 28 / 255.f, 1.f);
		kElevated = ImVec4(26 / 255.f, 26 / 255.f, 36 / 255.f, 1.f);
		kControl = ImVec4(28 / 255.f, 28 / 255.f, 39 / 255.f, 1.f);
		kPopup = kCard;
		kStroke = ImVec4(30 / 255.f, 30 / 255.f, 41 / 255.f, 1.f);
		kStrokeSoft = ImVec4(25 / 255.f, 25 / 255.f, 35 / 255.f, 1.f);
		kTrack = ImVec4(35 / 255.f, 35 / 255.f, 48 / 255.f, 1.f);
		kText = ImVec4(228 / 255.f, 228 / 255.f, 238 / 255.f, 1.f);
		kTextMid = ImVec4(182 / 255.f, 182 / 255.f, 200 / 255.f, 1.f);
		kTextDim = ImVec4(104 / 255.f, 104 / 255.f, 124 / 255.f, 1.f);
		kTextFaint = ImVec4(76 / 255.f, 76 / 255.f, 94 / 255.f, 1.f);
	}
}

// ---- navigation & UI state ------------------------------------------------
enum class IconKind {
	Aim, Visuals, World, Radar, Misc, Settings,
	Sliders, Shield, Bolt, Cpu, List, Users, Palette, Map, File
};

constexpr int kTabCount = 6;
constexpr const char* kTabNames[kTabCount] = { "Aim", "Visuals", "World", "Radar", "Misc", "Settings" };
constexpr IconKind kTabIcons[kTabCount] = {
	IconKind::Aim, IconKind::Visuals, IconKind::World,
	IconKind::Radar, IconKind::Misc, IconKind::Settings
};

struct SubTabDef { const char* name; IconKind icon; };
constexpr SubTabDef kAimSubs[] = {
	{ "General",          IconKind::Settings },
	{ "Targets & Recoil", IconKind::Sliders },
	{ "Triggerbot",       IconKind::Bolt },
	{ "Hardware & Pad",   IconKind::Cpu },
};
constexpr SubTabDef kVisualsSubs[] = {
	{ "Player ESP",          IconKind::Visuals },
	{ "Skeleton & VisCheck", IconKind::Shield },
	{ "States & Colours",    IconKind::Palette },
};
constexpr SubTabDef kWorldSubs[] = {
	{ "World & Loot",   IconKind::World },
	{ "Value & Sizing", IconKind::Sliders },
	{ "Catalog",        IconKind::List },
};
constexpr SubTabDef kRadarSubs[] = {
	{ "2D & Web",    IconKind::Radar },
	{ "In-Game Map", IconKind::Map },
};
constexpr SubTabDef kMiscSubs[] = {
	{ "Gameplay & Tools", IconKind::Sliders },
	{ "Skins",            IconKind::Palette },
	{ "Server",           IconKind::Users },
};
constexpr SubTabDef kSettingsSubs[] = {
	{ "Overlay & Hotkeys", IconKind::Settings },
	{ "Configs & Status",  IconKind::File },
};

const SubTabDef* GetSubs(int tab, int& count)
{
	switch (tab) {
	case 0: count = (int)std::size(kAimSubs);      return kAimSubs;
	case 1: count = (int)std::size(kVisualsSubs);  return kVisualsSubs;
	case 2: count = (int)std::size(kWorldSubs);    return kWorldSubs;
	case 3: count = (int)std::size(kRadarSubs);    return kRadarSubs;
	case 4: count = (int)std::size(kMiscSubs);     return kMiscSubs;
	case 5: count = (int)std::size(kSettingsSubs); return kSettingsSubs;
	default: count = 0; return nullptr;
	}
}

int g_tab = 0;
int g_sub[kTabCount] = { 0, 0, 0, 0, 0, 0 };
bool g_dragging = false;
ImVec2 g_dragDelta = ImVec2(0.f, 0.f);
ImVec2 g_lastPos = ImVec2(-1.f, -1.f);
bool g_placementApplied = false;

char g_search[96] = "";
bool g_searchDropdownOpen = false;
std::string g_flashLabel;
float g_flashTimer = 0.f;
bool g_dirty = false;
bool g_readCatalog = false;
bool g_aimVarsSyncNeeded = true;
bool g_compactRows = false;

static const char* kMenuKeyLabels[] = { "Insert", "Home", "Delete", "End", "F11", "F12", "Right Shift" };

std::map<ImGuiID, float> g_cardHeights;
ImGuiID g_currentCard = 0;
bool g_currentCardFixed = false;

// Global cross-tab search registry
struct SearchEntry { const char* label; int tab; int sub; const char* match = nullptr; };
constexpr SearchEntry kSearchIndex[] = {
	// Aim -> General
	{ "Enable Aim", 0, 0 }, { "Aim Type", 0, 0 }, { "Target Bone", 0, 0 },
	{ "Target Priority", 0, 0 }, { "Dynamic Bone Fallback", 0, 0 }, { "Aim Key", 0, 0 },
	{ "Write Camera Angles", 0, 0 }, { "Cursor Fallback", 0, 0 }, { "Response", 0, 0 },
	{ "Aim FOV", 0, 0 }, { "Smoothing Curve", 0, 0 }, { "Smoothness", 0, 0 },
	{ "Bezier Arc", 0, 0 }, { "Curve Speed", 0, 0 }, { "Hardware Gain", 0, 0 },
	{ "Max Distance (Aim)", 0, 0, "Max Distance" }, { "Target Prediction", 0, 0 },
	{ "Prediction Mode", 0, 0 }, { "Legacy", 0, 0, "Prediction Mode" },
	{ "Adaptive", 0, 0, "Prediction Mode" }, { "Lead Strength", 0, 0 },
	{ "Vertical Lead", 0, 0 }, { "Hard Lock", 0, 0 }, { "Switch Delay", 0, 0 },
	// Aim -> Targets & Recoil
	{ "Visible Bones Only", 0, 1 }, { "Ignore Teammates", 0, 1 }, { "Skip Knocked Players", 0, 1 },
	{ "Skip Police targets", 0, 1 }, { "Skip Criminal targets", 0, 1 }, { "Skip Civilian targets", 0, 1 },
	{ "Show FOV Circle", 0, 1 }, { "Aim Debug HUD", 0, 1 },
	{ "No Recoil", 0, 1 }, { "No Spread", 0, 1 }, { "Dynamic Recoil Curve", 0, 1 }, { "Curve Strength", 0, 1 },
	// Aim -> Triggerbot
	{ "Enable Triggerbot", 0, 2 }, { "Trigger Key", 0, 2 }, { "Trigger Bone", 0, 2 },
	{ "Min Delay", 0, 2 }, { "Max Delay", 0, 2 }, { "Burst Hold", 0, 2 },
	{ "Shot Cooldown", 0, 2 }, { "Hitbox Scale", 0, 2 }, { "Max Range", 0, 2 },
	{ "Visible Only", 0, 2 }, { "Enable Weapon Profiles", 0, 2 }, { "Weapon Class", 0, 2 },
	// Aim -> Hardware & Pad
	{ "Device", 0, 3 }, { "Manual COM Port", 0, 3 }, { "COM Port", 0, 3 }, { "Baud Rate", 0, 3 },
	{ "Ferrum Controller", 0, 3 }, { "Enable Gamepad Input", 0, 3 }, { "Host Pad Slot", 0, 3 },
	{ "Dual Gamepad Key", 0, 3 }, { "Gamepad Aim Key", 0, 3 }, { "Trigger Threshold", 0, 3 },
	{ "Stick Deadzone", 0, 3 }, { "Require Stick Move", 0, 3 }, { "Stick Output Scale", 0, 3 },
	// Visuals -> Player ESP
	{ "Hide Teammates", 1, 0 }, { "Hidden Player ESP", 1, 0 }, { "Box ESP", 1, 0 },
	{ "Health", 1, 0 }, { "Distance", 1, 0 }, { "Snaplines", 1, 0 }, { "Snapline Origin", 1, 0 },
	{ "Off-Screen Indicators", 1, 0 }, { "Head Hitbox Dot", 1, 0 }, { "Dot Radius", 1, 0 },
	{ "Weapon Name", 1, 0 }, { "Weapon Sprite", 1, 0 }, { "Max Distance (Player)", 1, 0, "Max Distance" },
	{ "Font Size (Player)", 1, 0, "Font Size" }, { "Looking At You", 1, 0 }, { "Looking Alert", 1, 0 },
	{ "Spectator Alert", 1, 0 }, { "Show Role", 1, 0 }, { "Color-code by role", 1, 0 },
	{ "Money & Value HUD", 1, 0 },
	// Visuals -> Skeleton & VisCheck
	{ "Draw Skeleton", 1, 1 }, { "Bone Thickness", 1, 1 }, { "Head Circle", 1, 1 },
	{ "Character Chams", 1, 1 }, { "Fill Opacity", 1, 1 }, { "Limb Thickness", 1, 1 },
	{ "Chams Outline", 1, 1 }, { "Chams VisCheck", 1, 1 }, { "Enable VisCheck", 1, 1 },
	{ "Enable WallCheck", 1, 1 }, { "Reload BVH", 1, 1 }, { "Show BVH", 1, 1 },
	{ "BVH Draw Dist", 1, 1 }, { "Vis Ray Count", 1, 1 }, { "Vis Max Dist", 1, 1 },
	{ "Hidden Alpha", 1, 1 }, { "Car Cabin Solid", 1, 1 }, { "Vis Ring", 1, 1 }, { "Status Ring", 1, 1 },
	// Visuals -> States & Colours
	{ "Firing", 1, 2 }, { "Reloading", 1, 2 }, { "Armor", 1, 2 }, { "Armor Value", 1, 2 },
	{ "Stamina", 1, 2 }, { "State Tags", 1, 2 }, { "Name", 1, 2 }, { "Color names by role", 1, 2 },
	{ "Criminal", 1, 2 }, { "Police", 1, 2 }, { "Civilian", 1, 2 }, { "Hubber", 1, 2 },
	{ "Pose Smoothing", 1, 2 }, { "Blend Alpha", 1, 2 }, { "Native Camera", 1, 2 },
	// World -> World & Loot
	{ "Enable World ESP", 2, 0 }, { "House Info", 2, 0 }, { "House Range", 2, 0 },
	{ "Show Empty Tag", 2, 0 }, { "Defense Blocks", 2, 0 }, { "Min Cash / Value", 2, 0 },
	{ "House / Legacy Font", 2, 0 }, { "Enable Loot Filter", 2, 0 }, { "Show on Map", 2, 0 },
	{ "Show on Minimap", 2, 0 }, { "Hide Empty Containers", 2, 0 }, { "Hide Looted Items", 2, 0 },
	{ "Read Vehicle / Table Inventories", 2, 0 }, { "Base Dot Size", 2, 0 },
	{ "ESP Font Size", 2, 0 }, { "ESP Icon Size", 2, 0 }, { "Global Max Distance", 2, 0 },
	// World -> Value & Sizing
	{ "Filter By Min Price", 2, 1 }, { "Min Price", 2, 1 }, { "High Value", 2, 1 },
	{ "High Value Color", 2, 1 }, { "High Value Pulse (Radar)", 2, 1 },
	{ "Mid Value", 2, 1 }, { "Mid Value Color", 2, 1 }, { "Per-Category Sizing", 2, 1 },
	{ "Export JSON", 2, 1 }, { "Import JSON", 2, 1 },
	// World -> Catalog
	{ "Loot Catalog", 2, 2 },
	// Radar -> 2D & Web
	{ "Enable Radar", 3, 0 }, { "Show Players", 3, 0 }, { "Show Vehicles", 3, 0 },
	{ "Show Loot", 3, 0 }, { "Rotate With Camera", 3, 0 }, { "Full Map Window", 3, 0 },
	{ "Radar Size", 3, 0 }, { "Radar Zoom", 3, 0 }, { "Radar Opacity", 3, 0 },
	{ "Web Radar Server", 3, 0 }, { "Web Port", 3, 0 }, { "Pop Out Web Radar", 3, 0 },
	{ "Auto Map Select", 3, 0 }, { "Active Map", 3, 0 }, { "Calibrate to Map", 3, 0 },
	// Radar -> In-Game Map
	{ "Top-Left Minimap", 3, 1 }, { "Fullscreen Map (M)", 3, 1 }, { "Hide 3D ESP on Full Map", 3, 1 },
	{ "Icon Scale", 3, 1 }, { "Player Arrows", 3, 1 }, { "Vehicle Icons", 3, 1 },
	{ "Player Names (Full Map)", 3, 1 }, { "Dropped Bags", 3, 1 },
	{ "Valuables & Loot", 3, 1 }, { "Cargo", 3, 1 },
	// Misc -> Gameplay & Tools
	{ "Camera Zoom", 4, 0 }, { "Camera Distance", 4, 0 }, { "FOV Changer", 4, 0 },
	{ "Camera FOV", 4, 0 }, { "Instant Loot", 4, 0 }, { "Instant Safe & Door Unlock", 4, 0 },
	{ "Ignore Knock (Local)", 4, 0 }, { "Hide Default Crosshair", 4, 0 },
	{ "Battle Mode", 4, 0 }, { "Battle Mode Key", 4, 0 }, { "Battle Mode Action", 4, 0 },
	{ "Read Item Catalog", 4, 0 }, { "Dump Item Prices", 4, 0 }, { "Probe Money Models", 4, 0 },
	// Misc -> Skins
	{ "Enable Skin Changer", 4, 1 }, { "Weapon Skins", 4, 1 }, { "Cosmetics", 4, 1 },
	// Misc -> Server
	{ "Players On Map", 4, 2 }, { "Trusted Players", 4, 2 },
	// Settings -> Overlay & Hotkeys
	{ "Transparent", 5, 0 }, { "Click-Through When Closed", 5, 0 }, { "Always On Top", 5, 0 },
	{ "High Contrast Menu", 5, 0 }, { "Accent Colour", 5, 0 }, { "Show FPS", 5, 0 },
	{ "Status Panel", 5, 0 }, { "Font", 5, 0 }, { "Frame Cap", 5, 0 },
	{ "Display Monitor", 5, 0 }, { "Menu Key", 5, 0 }, { "Panic Mode", 5, 0 },
	{ "Panic Key", 5, 0 }, { "Re-attach Key", 5, 0 }, { "Show Backdrop", 5, 0 },
	// Settings -> Configs & Status
	{ "Config Profiles", 5, 1 }, { "Save", 5, 1 }, { "Load", 5, 1 },
	{ "Connection Status", 5, 1 }, { "Re-attach Game", 5, 1 },
};

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

static bool CaseContains(const char* haystack, const char* needle)
{
	if (!needle || !needle[0]) return true;
	if (!haystack) return false;
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

static bool CaseInsensitiveEquals(const std::string& a, const std::string& b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
	return true;
}

inline void MaybeFlashRow(ImDrawList* dl, const ImRect& bb, const char* label)
{
	if (g_flashTimer <= 0.f || g_flashLabel.empty() || !label) return;
	const char* dispEnd = ImGui::FindRenderedTextEnd(label);
	std::string disp(label, dispEnd);
	if (CaseContains(disp.c_str(), g_flashLabel.c_str()) || CaseContains(g_flashLabel.c_str(), disp.c_str()))
	{
		const float pulse = ImSin(g_flashTimer * 6.5f) * 0.5f + 0.5f;
		dl->AddRectFilled(ImVec2(bb.Min.x - 4.f, bb.Min.y - 1.f),
			ImVec2(bb.Max.x + 4.f, bb.Max.y + 1.f),
			C(kAccent, 0.22f * ImMin(g_flashTimer, 1.f) * (0.4f + 0.6f * pulse)), 4.f);
	}
}

// Crisp Lucide-style vector icons drawn via ImDrawList so every tab, sub-tab
// and card header matches the mockup 1:1 and tints cleanly with kAccent.
void DrawVectorIcon(ImDrawList* dl, IconKind kind, const ImVec2& c, float r, ImU32 col)
{
	const float t = 1.45f;
	switch (kind) {
	case IconKind::Aim:
		dl->AddCircle(c, r * 0.92f, col, 20, t);
		dl->AddCircle(c, r * 0.44f, col, 16, t);
		dl->AddCircleFilled(c, 1.35f, col);
		break;
	case IconKind::Visuals: {
		// Smooth almond eye matching Lucide Eye
		dl->AddBezierCubic(ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.52f, c.y - r * 0.78f),
			ImVec2(c.x + r * 0.52f, c.y - r * 0.78f), ImVec2(c.x + r, c.y), col, t, 12);
		dl->AddBezierCubic(ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.52f, c.y + r * 0.78f),
			ImVec2(c.x + r * 0.52f, c.y + r * 0.78f), ImVec2(c.x + r, c.y), col, t, 12);
		dl->AddCircle(c, r * 0.34f, col, 14, t);
		break;
	}
	case IconKind::World:
		dl->AddCircle(c, r * 0.94f, col, 20, t);
		dl->AddLine(ImVec2(c.x - r * 0.94f, c.y), ImVec2(c.x + r * 0.94f, c.y), col, 1.2f);
		dl->AddEllipse(c, ImVec2(r * 0.44f, r * 0.94f), col, 0.f, 18, 1.2f);
		break;
	case IconKind::Radar:
		dl->AddCircle(c, r * 0.94f, col, 20, t);
		dl->AddCircle(c, r * 0.46f, col, 16, 1.2f);
		dl->AddCircleFilled(c, 1.3f, col);
		dl->AddLine(c, ImVec2(c.x + r * 0.72f, c.y - r * 0.58f), col, t);
		break;
	case IconKind::Misc: {
		// 3 stacked diamond layers (Lucide Layers)
		ImVec2 top[4] = {
			ImVec2(c.x, c.y - r * 0.92f),
			ImVec2(c.x + r * 0.95f, c.y - r * 0.36f),
			ImVec2(c.x, c.y + r * 0.16f),
			ImVec2(c.x - r * 0.95f, c.y - r * 0.36f),
		};
		dl->AddPolyline(top, 4, col, ImDrawFlags_Closed, t);
		ImVec2 mid[3] = {
			ImVec2(c.x - r * 0.92f, c.y + r * 0.08f),
			ImVec2(c.x, c.y + r * 0.55f),
			ImVec2(c.x + r * 0.92f, c.y + r * 0.08f),
		};
		dl->AddPolyline(mid, 3, col, ImDrawFlags_None, t);
		ImVec2 bot[3] = {
			ImVec2(c.x - r * 0.92f, c.y + r * 0.46f),
			ImVec2(c.x, c.y + r * 0.92f),
			ImVec2(c.x + r * 0.92f, c.y + r * 0.46f),
		};
		dl->AddPolyline(bot, 3, col, ImDrawFlags_None, t);
		break;
	}
	case IconKind::Settings: {
		// Proper 8-tooth cog/gear (Lucide Settings)
		dl->AddCircle(c, r * 0.34f, col, 14, t);
		dl->AddCircle(c, r * 0.68f, col, 18, t);
		for (int i = 0; i < 8; ++i) {
			const float a = i * (3.14159265f * 0.25f);
			const float ca = std::cos(a), sa = std::sin(a);
			dl->AddLine(
				ImVec2(c.x + ca * r * 0.64f, c.y + sa * r * 0.64f),
				ImVec2(c.x + ca * r * 0.98f, c.y + sa * r * 0.98f),
				col, 1.9f);
		}
		break;
	}
	case IconKind::Sliders: {
		// 3 vertical sliders with crossbar ticks (Lucide Sliders)
		const float offs[3] = { -r * 0.66f, 0.f, r * 0.66f };
		const float knob[3] = { r * 0.22f, -r * 0.30f, r * 0.36f };
		for (int i = 0; i < 3; ++i) {
			const float x = c.x + offs[i];
			const float ky = c.y + knob[i];
			dl->AddLine(ImVec2(x, c.y - r * 0.92f), ImVec2(x, ky - 2.2f), col, 1.3f);
			dl->AddLine(ImVec2(x, ky + 2.2f), ImVec2(x, c.y + r * 0.92f), col, 1.3f);
			dl->AddLine(ImVec2(x - 2.3f, ky), ImVec2(x + 2.3f, ky), col, 1.8f);
		}
		break;
	}
	case IconKind::Shield: {
		ImVec2 pts[6] = {
			ImVec2(c.x, c.y - r * 0.95f),
			ImVec2(c.x + r * 0.80f, c.y - r * 0.55f),
			ImVec2(c.x + r * 0.72f, c.y + r * 0.28f),
			ImVec2(c.x, c.y + r * 0.96f),
			ImVec2(c.x - r * 0.72f, c.y + r * 0.28f),
			ImVec2(c.x - r * 0.80f, c.y - r * 0.55f),
		};
		dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
		break;
	}
	case IconKind::Bolt: {
		ImVec2 pts[6] = {
			ImVec2(c.x + r * 0.14f, c.y - r * 0.96f),
			ImVec2(c.x - r * 0.72f, c.y + r * 0.10f),
			ImVec2(c.x - r * 0.02f, c.y + r * 0.10f),
			ImVec2(c.x - r * 0.16f, c.y + r * 0.96f),
			ImVec2(c.x + r * 0.72f, c.y - r * 0.10f),
			ImVec2(c.x + r * 0.02f, c.y - r * 0.10f),
		};
		dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, 1.35f);
		break;
	}
	case IconKind::Cpu: {
		const float s = r * 0.62f;
		dl->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col, 1.8f, 0, t);
		dl->AddRect(ImVec2(c.x - s * 0.45f, c.y - s * 0.45f), ImVec2(c.x + s * 0.45f, c.y + s * 0.45f), col, 1.f, 0, 1.2f);
		for (int i = -1; i <= 1; i += 2) {
			const float o = i * s * 0.45f;
			dl->AddLine(ImVec2(c.x + o, c.y - s), ImVec2(c.x + o, c.y - r * 0.96f), col, 1.2f);
			dl->AddLine(ImVec2(c.x + o, c.y + s), ImVec2(c.x + o, c.y + r * 0.96f), col, 1.2f);
			dl->AddLine(ImVec2(c.x - s, c.y + o), ImVec2(c.x - r * 0.96f, c.y + o), col, 1.2f);
			dl->AddLine(ImVec2(c.x + s, c.y + o), ImVec2(c.x + r * 0.96f, c.y + o), col, 1.2f);
		}
		break;
	}
	case IconKind::List:
		for (int i = -1; i <= 1; ++i) {
			const float y = c.y + i * r * 0.62f;
			dl->AddCircleFilled(ImVec2(c.x - r * 0.76f, y), 1.35f, col);
			dl->AddLine(ImVec2(c.x - r * 0.28f, y), ImVec2(c.x + r * 0.92f, y), col, t);
		}
		break;
	case IconKind::Users:
		// Primary person + secondary shoulder
		dl->AddCircle(ImVec2(c.x - r * 0.22f, c.y - r * 0.42f), r * 0.34f, col, 14, t);
		dl->AddBezierCubic(ImVec2(c.x - r * 0.92f, c.y + r * 0.86f),
			ImVec2(c.x - r * 0.85f, c.y + r * 0.12f),
			ImVec2(c.x + r * 0.40f, c.y + r * 0.12f),
			ImVec2(c.x + r * 0.46f, c.y + r * 0.86f), col, t, 10);
		dl->AddBezierQuadratic(ImVec2(c.x + r * 0.36f, c.y + r * 0.22f),
			ImVec2(c.x + r * 0.88f, c.y + r * 0.28f),
			ImVec2(c.x + r * 0.94f, c.y + r * 0.86f), col, 1.25f, 8);
		break;
	case IconKind::Palette:
		dl->AddCircle(c, r * 0.92f, col, 20, t);
		dl->AddCircleFilled(ImVec2(c.x - r * 0.36f, c.y - r * 0.18f), 1.3f, col);
		dl->AddCircleFilled(ImVec2(c.x + r * 0.36f, c.y - r * 0.18f), 1.3f, col);
		dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.45f), 1.2f, col);
		dl->AddCircleFilled(ImVec2(c.x + r * 0.18f, c.y + r * 0.36f), 1.4f, col);
		break;
	case IconKind::Map: {
		// Folded 3-panel map (Lucide Map)
		ImVec2 pts[6] = {
			ImVec2(c.x - r * 0.92f, c.y - r * 0.55f),
			ImVec2(c.x - r * 0.30f, c.y - r * 0.85f),
			ImVec2(c.x + r * 0.30f, c.y - r * 0.50f),
			ImVec2(c.x + r * 0.92f, c.y - r * 0.80f),
			ImVec2(c.x + r * 0.92f, c.y + r * 0.55f),
			ImVec2(c.x + r * 0.30f, c.y + r * 0.85f),
		};
		ImVec2 poly[8] = {
			pts[0], pts[1], pts[2], pts[3],
			pts[4], pts[5],
			ImVec2(c.x - r * 0.30f, c.y + r * 0.50f),
			ImVec2(c.x - r * 0.92f, c.y + r * 0.80f),
		};
		dl->AddPolyline(poly, 8, col, ImDrawFlags_Closed, t);
		dl->AddLine(poly[1], poly[6], col, 1.15f);
		dl->AddLine(poly[2], poly[5], col, 1.15f);
		break;
	}
	case IconKind::File: {
		// Document with folded corner (Lucide FileText)
		ImVec2 pts[5] = {
			ImVec2(c.x - r * 0.72f, c.y - r * 0.92f),
			ImVec2(c.x + r * 0.22f, c.y - r * 0.92f),
			ImVec2(c.x + r * 0.74f, c.y - r * 0.38f),
			ImVec2(c.x + r * 0.74f, c.y + r * 0.92f),
			ImVec2(c.x - r * 0.72f, c.y + r * 0.92f),
		};
		dl->AddPolyline(pts, 5, col, ImDrawFlags_Closed, t);
		dl->AddLine(pts[1], ImVec2(pts[1].x, pts[2].y), col, 1.2f);
		dl->AddLine(ImVec2(pts[1].x, pts[2].y), pts[2], col, 1.2f);
		break;
	}
	}
}

inline void MemoryWriteBadge(float rightX, float rowTop, float rowHeight, bool active)
{
	const char* text = "MEM WRITE";
	ImFont* f = font::inter_medium_small;
	const float fs = f->FontSize * 0.86f;
	const float tw = TextW(f, text) * 0.86f;
	constexpr float bh = 15.f;
	const ImVec2 mx(rightX, rowTop + (rowHeight - bh) * 0.5f);
	const ImRect bb(ImVec2(mx.x - tw - 10.f, mx.y), ImVec2(mx.x, mx.y + bh));

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(bb.Min, bb.Max, C(kAccent, active ? 0.18f : 0.10f), 3.f);
	dl->AddText(f, fs,
		ImVec2(bb.Min.x + 5.f, bb.Min.y + (bh - fs) * 0.5f),
		C(kAccent, active ? 1.f : 0.7f), text);
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
	const float y = p.y + 3.f;

	window->DrawList->AddText(font::inter_medium_small, font::inter_medium_small->FontSize * 0.92f,
		ImVec2(p.x, y), C(kTextFaint), text);
	const float lineX = p.x + TextW(font::inter_medium_small, text) * 0.92f + 8.f;
	if (lineX < p.x + w - 4.f)
		window->DrawList->AddLine(ImVec2(lineX, y + 6.f), ImVec2(p.x + w, y + 6.f), C(kStrokeSoft), 1.f);

	ImGui::Dummy(ImVec2(0.f, 16.f));
}

// ---- card -----------------------------------------------------------------
void BeginCard(const char* title, IconKind icon, float width, float fixedHeight = 0.f,
	bool scrollable = false, const char* meta = nullptr, bool compact = false)
{
	ImGuiWindow* parent = ImGui::GetCurrentWindow();
	const ImGuiID id = parent->GetID(title);
	const float natural = g_cardHeights.count(id) ? g_cardHeights[id] : 84.f;
	const float h = fixedHeight > 0.f ? fixedHeight : natural;

	const ImVec2 p = parent->DC.CursorPos;
	const ImRect bb(p, p + ImVec2(width, h));

	ImDrawList* dl = parent->DrawList;
	dl->AddRectFilled(bb.Min, bb.Max, C(kCard), 8.f);
	dl->AddRect(bb.Min, bb.Max, C(kStrokeSoft), 8.f, 0, 1.f);
	dl->AddLine(ImVec2(bb.Min.x, bb.Min.y + kCardHeader),
		ImVec2(bb.Max.x, bb.Min.y + kCardHeader), C(kStrokeSoft), 1.f);

	DrawVectorIcon(dl, icon, ImVec2(bb.Min.x + 20.f, bb.Min.y + kCardHeader * 0.5f), 6.f, C(kAccent));

	const char* titleEnd = ImGui::FindRenderedTextEnd(title);
	std::string displayTitle(title, titleEnd);
	Text(font::inter_medium_widget,
		ImVec2(bb.Min.x + 34.f, bb.Min.y + (kCardHeader - font::inter_medium_widget->FontSize) * 0.5f),
		C(kText), displayTitle.c_str());

	if (meta && *meta)
	{
		TextRight(font::inter_medium_small, bb.Max.x - 14.f,
			bb.Min.y + (kCardHeader - font::inter_medium_small->FontSize) * 0.5f,
			C(kTextFaint), meta);
	}

	ImGui::SetCursorScreenPos(ImVec2(bb.Min.x, bb.Min.y + kCardHeader));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.f, compact ? 9.f : 11.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, compact ? 6.f : 8.f));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings;
	if (!scrollable)
		flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	ImGui::BeginChild(id, ImVec2(width, ImMax(h - kCardHeader, 24.f)),
		ImGuiChildFlags_AlwaysUseWindowPadding, flags);
	g_currentCard = id;
	g_currentCardFixed = (fixedHeight > 0.f);
	g_compactRows = compact;
}

void EndCard()
{
	ImGuiWindow* child = ImGui::GetCurrentWindow();
	const float content = child->ContentSize.y + ImGui::GetStyle().WindowPadding.y * 2.f;
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
	ImGui::EndChild();
	if (!g_currentCardFixed)
		g_cardHeights[g_currentCard] = kCardHeader + content + 2.f;
	g_compactRows = false;
}

// ---- widgets (ZERO checkboxes anywhere) -----------------------------------
const ImVec4 kColourPresets[] = {
	ImVec4(1.00f, 1.00f, 1.00f, 1.f), ImVec4(1.00f, 0.18f, 0.18f, 1.f),
	ImVec4(0.20f, 0.95f, 0.35f, 1.f), ImVec4(0.20f, 0.80f, 1.00f, 1.f),
	ImVec4(1.00f, 0.82f, 0.15f, 1.f), ImVec4(0.55f, 0.36f, 0.96f, 1.f),
	ImVec4(1.00f, 0.50f, 0.15f, 1.f), ImVec4(1.00f, 0.30f, 0.70f, 1.f),
	ImVec4(0.35f, 1.00f, 0.85f, 1.f), ImVec4(0.45f, 0.55f, 1.00f, 1.f),
	ImVec4(0.60f, 0.60f, 0.65f, 1.f), ImVec4(0.08f, 0.07f, 0.11f, 1.f),
};

bool DrawColourPopup(const char* popupId, const char* title,
	D2D1::ColorF& colour, const ImRect& anchor)
{
	const ImGuiIO& io = ImGui::GetIO();
	constexpr float panelW = 240.f;
	constexpr float panelH = 340.f;

	ImVec2 pos(anchor.Max.x + 10.f, anchor.Min.y - 8.f);
	if (pos.x + panelW + 8.f > io.DisplaySize.x)
		pos.x = anchor.Min.x - panelW - 10.f;
	pos.x = ImClamp(pos.x, 8.f, ImMax(io.DisplaySize.x - panelW - 8.f, 8.f));
	pos.y = ImClamp(pos.y, 8.f, ImMax(io.DisplaySize.y - panelH - 8.f, 8.f));

	bool changed = false;
	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(panelW, 0.f), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f, 12.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f, 8.f));
	ImGui::PushStyleColor(ImGuiCol_PopupBg, C(kPopup));
	ImGui::PushStyleColor(ImGuiCol_Border, C(kStroke));
	if (ImGui::BeginPopup(popupId))
	{
		const char* titleEnd = ImGui::FindRenderedTextEnd(title);
		std::string dispTitle(title, titleEnd);
		ImGui::PushFont(font::inter_medium_widget);
		ImGui::PushStyleColor(ImGuiCol_Text, kText);
		ImGui::TextUnformatted(dispTitle.c_str());
		ImGui::PopStyleColor();
		ImGui::PopFont();

		const ImVec2 linePos = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(0.f, 1.f));
		ImGui::GetWindowDrawList()->AddRectFilled(linePos,
			ImVec2(linePos.x + ImGui::GetContentRegionAvail().x, linePos.y + 1.f),
			C(kStroke));

		ImDrawList* dl = ImGui::GetWindowDrawList();
		constexpr int kCols = 6;
		constexpr float kPGap = 5.f;
		const float avail = ImGui::GetContentRegionAvail().x;
		const float sw = (avail - kPGap * (kCols - 1)) / kCols;
		constexpr float sh = 15.f;
		const int count = (int)std::size(kColourPresets);

		for (int i = 0; i < count; ++i)
		{
			if (i % kCols) ImGui::SameLine(0.f, kPGap);
			ImGui::PushID(i);
			const ImVec2 q = ImGui::GetCursorScreenPos();
			const bool hit = ImGui::InvisibleButton("##preset", ImVec2(sw, sh));
			const bool hov = ImGui::IsItemHovered();
			if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			const ImVec4& pc = kColourPresets[i];
			const bool selected =
				std::fabs(colour.r - pc.x) < 0.02f &&
				std::fabs(colour.g - pc.y) < 0.02f &&
				std::fabs(colour.b - pc.z) < 0.02f;

			dl->AddRectFilled(q, q + ImVec2(sw, sh),
				ImGui::GetColorU32(ImVec4(pc.x, pc.y, pc.z, 1.f)), 3.f);
			dl->AddRect(q, q + ImVec2(sw, sh),
				selected ? C(kAccentBright) : C(kStroke, hov ? 2.f : 1.f),
				3.f, 0, selected ? 1.8f : 1.f);

			if (hit)
			{
				colour = D2D1::ColorF(pc.x, pc.y, pc.z, colour.a);
				changed = true;
			}
			ImGui::PopID();
		}

		float col[4] = { colour.r, colour.g, colour.b, colour.a };
		ImGui::PushItemWidth(avail);
		if (ImGui::ColorPicker4("##picker", col,
			ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview |
			ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_PickerHueBar))
		{
			colour = D2D1::ColorF(col[0], col[1], col[2], col[3]);
			changed = true;
		}
		ImGui::PopItemWidth();
		ImGui::EndPopup();
	}
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(2);
	if (changed) g_dirty = true;
	return changed;
}

void DrawPillSwitch(ImDrawList* dl, const ImRect& track, float t)
{
	const float th = track.GetHeight();
	dl->AddRectFilled(track.Min, track.Max, C(Mix(kTrack, kAccent, t)), th * 0.5f);
	const float r = (th - 6.f) * 0.5f;
	const float kx = ImLerp(track.Min.x + 3.f + r, track.Max.x - 3.f - r, t);
	dl->AddCircleFilled(ImVec2(kx, (track.Min.y + track.Max.y) * 0.5f), r,
		C(Mix(kTextDim, ImVec4(1, 1, 1, 1), t)));
}

bool ToggleRow(const char* label, bool* v, bool memoryWrite = false, bool tracksDirty = true)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	const ImRect bb(p, p + ImVec2(w, kRow));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	bool hov = false, held = false;
	const bool pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
	if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	if (pressed) {
		*v = !*v;
		if (tracksDirty) g_dirty = true;
	}

	static std::map<ImGuiID, float> anim;
	auto it = anim.find(id);
	if (it == anim.end()) it = anim.emplace(id, *v ? 1.f : 0.f).first;
	float& t = it->second;
	t = ImLerp(t, *v ? 1.f : 0.f, ImGui::GetIO().DeltaTime * 15.f);

	ImDrawList* dl = window->DrawList;
	MaybeFlashRow(dl, bb, label);

	const char* dispEnd = ImGui::FindRenderedTextEnd(label);
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, RowTextY(p.y, kRow, font::inter_medium_widget)),
		C(Mix(kTextMid, kText, ImMax(t, hov ? 0.6f : 0.f))), label, dispEnd);

	constexpr float tw = 32.f, th = 17.f;
	const ImRect track(ImVec2(p.x + w - tw, p.y + (kRow - th) * 0.5f),
		ImVec2(p.x + w, p.y + (kRow + th) * 0.5f));
	if (memoryWrite)
		MemoryWriteBadge(track.Min.x - 8.f, p.y, kRow, *v);
	DrawPillSwitch(dl, track, t);

	return pressed;
}

bool ColorToggleRow(const char* label, bool* v, D2D1::ColorF& colour)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	const ImRect bb(p, p + ImVec2(w, kRow));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	constexpr float tw = 32.f, th = 17.f, sw = 22.f, sh = 14.f;
	const ImRect track(ImVec2(p.x + w - tw, p.y + (kRow - th) * 0.5f),
		ImVec2(p.x + w, p.y + (kRow + th) * 0.5f));
	const ImRect swatch(ImVec2(track.Min.x - 8.f - sw, p.y + (kRow - sh) * 0.5f),
		ImVec2(track.Min.x - 8.f, p.y + (kRow + sh) * 0.5f));

	bool hov = false, held = false;
	const bool pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
	if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

	char popup[96];
	std::snprintf(popup, sizeof(popup), "##col_%u", id);

	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const bool mouseInSwatch = swatch.Contains(mouse);
	if (pressed)
	{
		if (mouseInSwatch) ImGui::OpenPopup(popup);
		else { *v = !*v; g_dirty = true; }
	}

	static std::map<ImGuiID, float> anim;
	auto it = anim.find(id);
	if (it == anim.end()) it = anim.emplace(id, *v ? 1.f : 0.f).first;
	float& t = it->second;
	t = ImLerp(t, *v ? 1.f : 0.f, ImGui::GetIO().DeltaTime * 15.f);

	ImDrawList* dl = window->DrawList;
	MaybeFlashRow(dl, bb, label);

	const char* dispEnd = ImGui::FindRenderedTextEnd(label);
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, RowTextY(p.y, kRow, font::inter_medium_widget)),
		C(Mix(kTextMid, kText, ImMax(t, hov ? 0.6f : 0.f))), label, dispEnd);

	DrawPillSwitch(dl, track, t);

	const bool shov = hov && mouseInSwatch;
	dl->AddRectFilled(swatch.Min, swatch.Max, C(ImVec4(colour.r, colour.g, colour.b, 1.f)), 3.f);
	dl->AddRect(swatch.Min, swatch.Max, shov ? C(kText) : C(ImVec4(1, 1, 1, 0.18f)), 3.f);

	DrawColourPopup(popup, label, colour, swatch);
	return pressed && !mouseInSwatch;
}

bool ColorRow(const char* label, D2D1::ColorF& colour)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	const ImRect bb(p, p + ImVec2(w, kRow));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	const ImRect swatch(ImVec2(p.x + w - 30.f, p.y + 4.f), ImVec2(p.x + w, p.y + 18.f));
	bool hov = false, held = false;
	const bool pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
	if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

	char popup[96];
	std::snprintf(popup, sizeof(popup), "##col_%u", id);
	if (pressed) ImGui::OpenPopup(popup);

	ImDrawList* dl = window->DrawList;
	MaybeFlashRow(dl, bb, label);

	const char* dispEnd = ImGui::FindRenderedTextEnd(label);
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, RowTextY(p.y, kRow, font::inter_medium_widget)),
		C(Mix(kTextMid, kText, hov ? 0.6f : 0.f)), label, dispEnd);
	dl->AddRectFilled(swatch.Min, swatch.Max, C(ImVec4(colour.r, colour.g, colour.b, 1.f)), 3.f);
	dl->AddRect(swatch.Min, swatch.Max, hov ? C(kText) : C(ImVec4(1, 1, 1, 0.18f)), 3.f);

	return DrawColourPopup(popup, label, colour, swatch);
}

// Inline click-to-edit state for slider numeric readouts
static ImGuiID g_editingSliderId = 0;
static char g_sliderEditBuf[32] = "";
static bool g_sliderEditFocus = false;

// Single-line inline slider: [Label]   [clickable readout]  [====o------]
bool SliderIntRow(const char* label, int* v, int lo, int hi, const char* suffix = "",
	const char* zeroText = nullptr)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	const ImRect bb(p, p + ImVec2(w, kRow));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	const float trackW = ImClamp(w * 0.36f, 90.f, 145.f);
	const float cy = p.y + kRow * 0.5f;
	const ImVec2 tmin(p.x + w - trackW, cy - 1.5f);
	const ImVec2 tmax(p.x + w, cy + 1.5f);
	const ImRect hit(ImVec2(tmin.x - 4.f, p.y), ImVec2(tmax.x + 4.f, p.y + kRow));

	bool hov = false, held = false;
	ImGui::ButtonBehavior(hit, id, &hov, &held);
	if (hov || held) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

	bool changed = false;
	float t = (float)(*v - lo) / (float)ImMax(hi - lo, 1);
	t = ImClamp(t, 0.f, 1.f);
	if (held)
	{
		if (g_editingSliderId == id) g_editingSliderId = 0;
		const float mx = ImGui::GetIO().MousePos.x;
		t = ImClamp((mx - tmin.x) / ImMax(trackW, 1.f), 0.f, 1.f);
		const int nv = lo + (int)std::lround(t * (float)(hi - lo));
		if (nv != *v) { *v = nv; changed = true; g_dirty = true; }
		t = ImClamp((float)(*v - lo) / (float)ImMax(hi - lo, 1), 0.f, 1.f);
	}

	ImDrawList* dl = window->DrawList;
	MaybeFlashRow(dl, bb, label);

	const char* dispEnd = ImGui::FindRenderedTextEnd(label);
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, RowTextY(p.y, kRow, font::inter_medium_widget)),
		C(Mix(kTextMid, kText, hov || held ? 0.65f : 0.f)), label, dispEnd);

	char buf[64];
	if (zeroText && *v <= lo)
		std::snprintf(buf, sizeof(buf), "%s", zeroText);
	else
		std::snprintf(buf, sizeof(buf), "%d%s", *v, suffix);

	const float textW = TextW(font::inter_medium_small, buf);
	const float pillW = ImClamp(textW + 14.f, 52.f, 78.f);
	const float padY = 3.f;
	const float pillH = font::inter_medium_small->FontSize + padY * 2.f;
	const ImRect valRect(
		ImVec2(tmin.x - 8.f - pillW, std::floor(cy - pillH * 0.5f)),
		ImVec2(tmin.x - 8.f, std::floor(cy - pillH * 0.5f) + pillH));

	if (g_editingSliderId == id)
	{
		const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(valRect.Min);
		ImGui::PushFont(font::inter_medium_small);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f, padY));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, kElevated);
		ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
		ImGui::SetNextItemWidth(pillW);
		if (g_sliderEditFocus) {
			ImGui::SetKeyboardFocusHere();
			g_sliderEditFocus = false;
		}
		char inputId[48];
		std::snprintf(inputId, sizeof(inputId), "##sedit_%u", (unsigned)id);
		const bool enter = ImGui::InputText(inputId, g_sliderEditBuf, sizeof(g_sliderEditBuf),
			ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		const bool esc = ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape);
		const bool commit = enter || ImGui::IsItemDeactivatedAfterEdit();
		const bool done = commit || esc || ImGui::IsItemDeactivated();
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(2);
		ImGui::PopFont();
		ImGui::SetCursorScreenPos(savedCursor);

		if (commit && !esc) {
			char* endp = nullptr;
			const long parsed = std::strtol(g_sliderEditBuf, &endp, 10);
			if (endp != g_sliderEditBuf) {
				const int nv = ImClamp((int)parsed, lo, hi);
				if (nv != *v) { *v = nv; changed = true; g_dirty = true; }
				t = ImClamp((float)(*v - lo) / (float)ImMax(hi - lo, 1), 0.f, 1.f);
			}
		}
		if (done) g_editingSliderId = 0;
	}
	else
	{
		const bool valHov = !held && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
			&& valRect.Contains(ImGui::GetIO().MousePos);
		if (valHov) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
			dl->AddRectFilled(valRect.Min, valRect.Max, C(kElevated), 4.f);
			dl->AddRect(valRect.Min, valRect.Max, C(kAccent, 0.55f), 4.f);
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				g_editingSliderId = id;
				std::snprintf(g_sliderEditBuf, sizeof(g_sliderEditBuf), "%d", *v);
				g_sliderEditFocus = true;
			}
		}
		TextRight(font::inter_medium_small, valRect.Max.x - 6.f,
			valRect.Min.y + padY, C(hov || held || valHov ? kText : kTextDim), buf);
	}

	dl->AddRectFilled(tmin, tmax, C(kTrack), 2.f);
	dl->AddRectFilled(tmin, ImVec2(tmin.x + trackW * t, tmax.y), C(kAccent), 2.f);
	dl->AddCircleFilled(ImVec2(tmin.x + trackW * t, cy), hov || held ? 5.5f : 4.5f, C(kAccent));

	return changed;
}

bool SliderFloatRow(const char* label, float* v, float lo, float hi, const char* suffix = "",
	const char* zeroText = nullptr)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	const ImRect bb(p, p + ImVec2(w, kRow));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	const float trackW = ImClamp(w * 0.36f, 90.f, 145.f);
	const float cy = p.y + kRow * 0.5f;
	const ImVec2 tmin(p.x + w - trackW, cy - 1.5f);
	const ImVec2 tmax(p.x + w, cy + 1.5f);
	const ImRect hit(ImVec2(tmin.x - 4.f, p.y), ImVec2(tmax.x + 4.f, p.y + kRow));

	bool hov = false, held = false;
	ImGui::ButtonBehavior(hit, id, &hov, &held);
	if (hov || held) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

	bool changed = false;
	float t = ImClamp((*v - lo) / ImMax(hi - lo, 0.0001f), 0.f, 1.f);
	if (held)
	{
		if (g_editingSliderId == id) g_editingSliderId = 0;
		const float mx = ImGui::GetIO().MousePos.x;
		t = ImClamp((mx - tmin.x) / ImMax(trackW, 1.f), 0.f, 1.f);
		const float nv = lo + t * (hi - lo);
		if (std::fabs(nv - *v) > 0.0001f) { *v = nv; changed = true; g_dirty = true; }
		t = ImClamp((*v - lo) / ImMax(hi - lo, 0.0001f), 0.f, 1.f);
	}

	ImDrawList* dl = window->DrawList;
	MaybeFlashRow(dl, bb, label);

	const char* dispEnd = ImGui::FindRenderedTextEnd(label);
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, RowTextY(p.y, kRow, font::inter_medium_widget)),
		C(Mix(kTextMid, kText, hov || held ? 0.65f : 0.f)), label, dispEnd);

	char buf[64];
	if (zeroText && *v <= lo + 0.0001f)
		std::snprintf(buf, sizeof(buf), "%s", zeroText);
	else
		std::snprintf(buf, sizeof(buf), "%.2f%s", *v, suffix);

	const float textW = TextW(font::inter_medium_small, buf);
	const float pillW = ImClamp(textW + 14.f, 52.f, 78.f);
	const float padY = 3.f;
	const float pillH = font::inter_medium_small->FontSize + padY * 2.f;
	const ImRect valRect(
		ImVec2(tmin.x - 8.f - pillW, std::floor(cy - pillH * 0.5f)),
		ImVec2(tmin.x - 8.f, std::floor(cy - pillH * 0.5f) + pillH));

	if (g_editingSliderId == id)
	{
		const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(valRect.Min);
		ImGui::PushFont(font::inter_medium_small);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f, padY));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, kElevated);
		ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
		ImGui::SetNextItemWidth(pillW);
		if (g_sliderEditFocus) {
			ImGui::SetKeyboardFocusHere();
			g_sliderEditFocus = false;
		}
		char inputId[48];
		std::snprintf(inputId, sizeof(inputId), "##sfedit_%u", (unsigned)id);
		const bool enter = ImGui::InputText(inputId, g_sliderEditBuf, sizeof(g_sliderEditBuf),
			ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		const bool esc = ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape);
		const bool commit = enter || ImGui::IsItemDeactivatedAfterEdit();
		const bool done = commit || esc || ImGui::IsItemDeactivated();
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(2);
		ImGui::PopFont();
		ImGui::SetCursorScreenPos(savedCursor);

		if (commit && !esc) {
			char* endp = nullptr;
			const float parsed = std::strtof(g_sliderEditBuf, &endp);
			if (endp != g_sliderEditBuf && std::isfinite(parsed)) {
				const float nv = ImClamp(parsed, lo, hi);
				if (std::fabs(nv - *v) > 0.0001f) { *v = nv; changed = true; g_dirty = true; }
				t = ImClamp((*v - lo) / ImMax(hi - lo, 0.0001f), 0.f, 1.f);
			}
		}
		if (done) g_editingSliderId = 0;
	}
	else
	{
		const bool valHov = !held && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
			&& valRect.Contains(ImGui::GetIO().MousePos);
		if (valHov) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
			dl->AddRectFilled(valRect.Min, valRect.Max, C(kElevated), 4.f);
			dl->AddRect(valRect.Min, valRect.Max, C(kAccent, 0.55f), 4.f);
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				g_editingSliderId = id;
				std::snprintf(g_sliderEditBuf, sizeof(g_sliderEditBuf), "%.2f", *v);
				g_sliderEditFocus = true;
			}
		}
		TextRight(font::inter_medium_small, valRect.Max.x - 6.f,
			valRect.Min.y + padY, C(hov || held || valHov ? kText : kTextDim), buf);
	}

	dl->AddRectFilled(tmin, tmax, C(kTrack), 2.f);
	dl->AddRectFilled(tmin, ImVec2(tmin.x + trackW * t, tmax.y), C(kAccent), 2.f);
	dl->AddCircleFilled(ImVec2(tmin.x + trackW * t, cy), hov || held ? 5.5f : 4.5f, C(kAccent));

	return changed;
}

bool ComboRow(const char* label, int* current, const char* const* items, int count,
	float fieldW = 152.f, bool memoryWrite = false, bool tracksDirty = true)
{
	if (count <= 0) return false;

	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	constexpr float rowH = 26.f;
	const ImRect bb(p, p + ImVec2(w, rowH));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	const char* dispEnd = ImGui::FindRenderedTextEnd(label);
	std::string dispLabel(label, dispEnd);
	const float labelW = TextW(font::inter_medium_widget, dispLabel.c_str());
	const float fw = ImClamp(fieldW, 88.f, ImMax(88.f, w - labelW - (memoryWrite ? 78.f : 14.f)));
	const ImRect field(ImVec2(p.x + w - fw, p.y + 1.f), ImVec2(p.x + w, p.y + rowH - 1.f));

	bool hov = false, held = false;
	const bool pressed = ImGui::ButtonBehavior(field, id, &hov, &held);
	if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

	char popup[96];
	std::snprintf(popup, sizeof(popup), "##cmb_%u", id);
	if (pressed) ImGui::OpenPopup(popup);

	ImDrawList* dl = window->DrawList;
	MaybeFlashRow(dl, bb, label);

	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(p.x, RowTextY(p.y, rowH, font::inter_medium_widget)),
		C(Mix(kTextMid, kText, hov ? 0.6f : 0.f)), dispLabel.c_str());

	dl->AddRectFilled(field.Min, field.Max, C(kControl), 5.f);
	dl->AddRect(field.Min, field.Max, C(Mix(kStroke, kAccent, hov ? 0.55f : 0.f)), 5.f, 0, 1.f);

	if (memoryWrite)
		MemoryWriteBadge(field.Min.x - 8.f, p.y, rowH, true);

	int sel = ImClamp(*current, 0, count - 1);
	const char* preview = items[sel] ? items[sel] : "-";
	dl->PushClipRect(field.Min, ImVec2(field.Max.x - 20.f, field.Max.y), true);
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		ImVec2(field.Min.x + 9.f, RowTextY(field.Min.y, field.GetHeight(), font::inter_medium_small)),
		C(kText), preview);
	dl->PopClipRect();

	const ImVec2 cv(field.Max.x - 12.f, (field.Min.y + field.Max.y) * 0.5f);
	dl->AddLine(ImVec2(cv.x - 3.5f, cv.y - 1.5f), ImVec2(cv.x, cv.y + 2.f), C(kTextDim), 1.4f);
	dl->AddLine(ImVec2(cv.x, cv.y + 2.f), ImVec2(cv.x + 3.5f, cv.y - 1.5f), C(kTextDim), 1.4f);

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
		ImGui::PushItemWidth(ImMax(fw, 150.f));
		for (int i = 0; i < count; i++)
		{
			ImGui::PushID(i);
			if (ImGui::Selectable(items[i] ? items[i] : "-", i == sel))
			{
				if (*current != i && tracksDirty) g_dirty = true;
				*current = i;
				changed = true;
				ImGui::CloseCurrentPopup();
			}
			ImGui::PopID();
		}
		ImGui::PopItemWidth();
		ImGui::EndPopup();
	}
	ImGui::PopStyleColor(5);
	ImGui::PopStyleVar(2);
	return changed;
}

bool InlineButton(const char* text, float width, bool accent = false, bool danger = false)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(text);
	const ImVec2 p = window->DC.CursorPos;
	const float h = 25.f;
	const ImRect bb(p, p + ImVec2(width, h));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	bool hov = false, held = false;
	const bool pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
	if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

	ImDrawList* dl = window->DrawList;
	const float alpha = ImGui::GetStyle().Alpha;
	ImU32 fill = accent
		? C(kAccent, (hov ? 0.92f : 0.80f) * alpha)
		: (danger ? C(kRed, (hov ? 0.20f : 0.10f) * alpha) : C(Mix(kControl, kElevated, hov ? 1.f : 0.f), alpha));
	ImU32 border = accent
		? C(kAccent, alpha)
		: (danger ? C(kRed, 0.7f * alpha) : C(hov ? Mix(kStroke, kAccent, 0.4f) : kStroke, alpha));

	dl->AddRectFilled(bb.Min, bb.Max, fill, 5.f);
	dl->AddRect(bb.Min, bb.Max, border, 5.f);

	const char* dispEnd = ImGui::FindRenderedTextEnd(text);
	std::string disp(text, dispEnd);
	const float tw = TextW(font::inter_medium_small, disp.c_str());
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		ImVec2((bb.Min.x + bb.Max.x - tw) * 0.5f, RowTextY(bb.Min.y, h, font::inter_medium_small)),
		C(accent ? ImVec4(1, 1, 1, alpha) : (danger ? ImVec4(kRed.x, kRed.y, kRed.z, alpha) : ImVec4(kText.x, kText.y, kText.z, alpha))),
		disp.c_str());

	return pressed;
}

bool ButtonRow(const char* label, const char* text, float width = 130.f, bool accent = false)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const char* idLabel = (label && *label) ? label : text;
	const ImGuiID id = window->GetID((idLabel && *idLabel) ? idLabel : "##btn");
	const ImVec2 p = window->DC.CursorPos;
	const float w = ImGui::GetContentRegionAvail().x;
	constexpr float rowH = 26.f;
	const ImRect bb(p, p + ImVec2(w, rowH));
	const ImRect button(ImVec2(p.x + w - width, p.y + 1.f), ImVec2(p.x + w, p.y + rowH - 1.f));

	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	bool hov = false, held = false;
	const bool pressed = ImGui::ButtonBehavior(button, id, &hov, &held);
	if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

	ImDrawList* dl = window->DrawList;
	MaybeFlashRow(dl, bb, (label && *label) ? label : text);

	if (label && *label)
		dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
			ImVec2(p.x, RowTextY(p.y, rowH, font::inter_medium_widget)),
			C(kTextMid), label);

	ImU32 fill = accent ? C(kAccent, hov ? 0.92f : 0.80f) : C(hov ? kElevated : kControl);
	dl->AddRectFilled(button.Min, button.Max, fill, 5.f);
	if (!accent)
		dl->AddRect(button.Min, button.Max, C(hov ? Mix(kStroke, kAccent, 0.4f) : kStroke), 5.f);

	const char* dispEnd = ImGui::FindRenderedTextEnd(text);
	std::string disp(text, dispEnd);
	const float tw = TextW(font::inter_medium_small, disp.c_str());
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		ImVec2((button.Min.x + button.Max.x - tw) * 0.5f,
			RowTextY(button.Min.y, button.GetHeight(), font::inter_medium_small)),
		C(accent ? ImVec4(1, 1, 1, 1) : kText), disp.c_str());

	return pressed;
}

// Pill toggle used inside popups (Loot Catalog Edit popup) — zero checkboxes.
bool PopupToggleRow(const char* label, bool* v, float width = 200.f)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiID id = window->GetID(label);
	const ImVec2 p = window->DC.CursorPos;
	const ImRect bb(p, p + ImVec2(width, kRow));
	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;

	bool hov = false, held = false;
	const bool pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
	if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	if (pressed) { *v = !*v; g_dirty = true; }

	ImDrawList* dl = window->DrawList;
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
		ImVec2(p.x, RowTextY(p.y, kRow, font::inter_medium_small)),
		C(*v || hov ? kText : kTextMid), label);

	constexpr float tw = 30.f, th = 16.f;
	const ImRect track(ImVec2(p.x + width - tw, p.y + (kRow - th) * 0.5f),
		ImVec2(p.x + width, p.y + (kRow + th) * 0.5f));
	DrawPillSwitch(dl, track, *v ? 1.f : 0.f);
	return pressed;
}

// ---- Loot Filter helpers --------------------------------------------------
static std::set<std::string> s_lfSelected;
static std::string s_lfAnchor;
static std::string s_lfRangeTo;
static bool s_lfRangeAdd = false;
static std::string s_lfStatusNote;
static std::string s_dumpStatusNote;

static bool ReadFuncIntValue(std::uintptr_t object, std::uintptr_t funcOffset, int& out)
{
	if (object <= 0x10000) return false;
	const std::uintptr_t fn = RobRunMemory::ReadPtr(object + funcOffset);
	if (fn <= 0x10000) return false;
	const std::uintptr_t closure = RobRunMemory::ReadPtr(fn + 0x20);
	if (closure <= 0x10000) return false;
	for (const std::uintptr_t off : { (std::uintptr_t)0x10, (std::uintptr_t)0x18 }) {
		const int value = RobRunMemory::ReadInt32(closure + off);
		if (value >= 10 && value < 100000000) { out = value; return true; }
	}
	return false;
}

static void RunDumpItemPrices()
{
	wchar_t exePath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	std::wstring dir(exePath);
	const std::size_t slash = dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) dir.resize(slash + 1);
	const std::wstring path = dir + L"item-prices-dump.tsv";

	std::ofstream out(path.c_str(), std::ios::trunc);
	if (!out) {
		s_dumpStatusNote = "Could not write item-prices-dump.tsv";
		Overlay::Notify("Dump failed", IM_COL32(229, 83, 83, 255), 2.f);
		return;
	}

	out << "# VeoxDMA price dump\n";
	out << "# id\tname\tcategory\trarity\tcatalog_price\tbuy_price\tsell_base\n";

	int registryRows = 0;
	std::unordered_map<std::string, std::pair<int, int>> prices;
	const std::uintptr_t klass = Il2Cpp::FindClassByName("InventoryItemDescriptions");
	if (klass > 0x10000) {
		const std::uintptr_t statics = Il2Cpp::StaticFields(klass);
		if (statics > 0x10000) {
			for (const auto& field : Il2Cpp::Fields(klass, false)) {
				if (field.offset <= 0) continue;
				const std::uintptr_t desc = RobRunMemory::ReadPtr(statics + (std::uintptr_t)field.offset);
				if (desc <= 0x10000) continue;
				std::wstring wideId;
				if (!RobRunMemory::ReadString(RobRunMemory::ReadPtr(desc + 0x10), wideId)) continue;
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

	std::unordered_map<std::string, int> liveSell;
	if (const auto snapshot = RobRunReader::GetSnapshot()) {
		if (snapshot->world && snapshot->world->valid) {
			for (const auto& ent : snapshot->world->entities)
				if (ent.hasValue && ent.value > 0 && !ent.label.empty())
					liveSell[ent.label] = ent.value;
		}
	}

	for (const auto& def : LootFilter::kItemCatalog) {
		int basePrice = 0, sellPrice = 0;
		auto hit = prices.find(def.id);
		if (hit == prices.end()) {
			for (const auto& kv : prices)
				if (CaseInsensitiveEquals(kv.first, def.id)) { hit = prices.find(kv.first); break; }
		}
		if (hit != prices.end()) { basePrice = hit->second.first; sellPrice = hit->second.second; }
		if (sellPrice <= 0) {
			for (const auto& kv : liveSell)
				if (CaseInsensitiveEquals(kv.first, def.id)) { sellPrice = kv.second; break; }
		}
		out << def.id << '\t' << def.displayName << '\t'
			<< LootFilter::CategoryNames[(int)def.category] << '\t'
			<< LootFilter::RarityNames[(int)def.rarity] << '\t'
			<< def.price << '\t' << basePrice << '\t' << sellPrice << '\n';
	}

	out << "\n# sell modifier\tvalue\n";
	int modifierRows = 0;
	const std::uintptr_t modifierClass = Il2Cpp::FindClassByName("InventoryItemSellPriceModifiers");
	if (modifierClass > 0x10000) {
		const std::uintptr_t modifierStatics = Il2Cpp::StaticFields(modifierClass);
		if (modifierStatics > 0x10000) {
			for (const auto& field : Il2Cpp::Fields(modifierClass, false)) {
				if (field.offset <= 0) continue;
				const std::uintptr_t entry = RobRunMemory::ReadPtr(modifierStatics + (std::uintptr_t)field.offset);
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

	out << "\n# authority level\tpoints_required\tcargo_modifier\tloot_modifier\n";
	int ladderRows = 0;
	const std::uintptr_t ladderClass = Il2Cpp::FindClassByName("RobberAuthorityLevelDescriptions");
	if (ladderClass > 0x10000) {
		const std::uintptr_t ladderStatics = Il2Cpp::StaticFields(ladderClass);
		if (ladderStatics > 0x10000) {
			for (const auto& field : Il2Cpp::Fields(ladderClass, false)) {
				if (field.offset <= 0) continue;
				const std::uintptr_t level = RobRunMemory::ReadPtr(ladderStatics + (std::uintptr_t)field.offset);
				if (level <= 0x10000) continue;
				const int points = RobRunMemory::ReadInt32(level + 0x28);
				const int cargoBits = RobRunMemory::ReadInt32(level + 0x2C);
				const int lootBits = RobRunMemory::ReadInt32(level + 0x30);
				float cargoMod = 0.f, lootMod = 0.f;
				std::memcpy(&cargoMod, &cargoBits, sizeof(cargoMod));
				std::memcpy(&lootMod, &lootBits, sizeof(lootBits));
				if (!std::isfinite(cargoMod) || !std::isfinite(lootMod)) continue;
				if (std::fabs(cargoMod) > 100.f || std::fabs(lootMod) > 100.f) continue;
				out << field.name << '\t' << points << '\t' << cargoMod << '\t' << lootMod << '\n';
				ladderRows++;
			}
		}
	}
	out.flush();

	s_dumpStatusNote = "Wrote " + std::to_string(LootFilter::kItemCount) + " rows (" +
		std::to_string(registryRows) + " registry, " + std::to_string(ladderRows) +
		" auth, " + std::to_string(modifierRows) + " mods) -> item-prices-dump.tsv";
	Overlay::Notify("Dumped item-prices-dump.tsv", IM_COL32(70, 194, 120, 255), 2.5f);
}

// ===========================================================================
// TAB 0: AIM
// ===========================================================================
void DrawAimGeneral()
{
	static const char* aimTypes[] = { "Memory Aim", "Hardware Aim" };
	static const char* kBones[] = { "Head", "Neck", "Chest", "Pelvis", "Closest" };
	static const char* priorities[] = { "Crosshair FOV", "3D Distance", "Lowest Health" };
	static const char* kSmoothAlgos[] = { "EMA", "Linear", "Bezier" };

	ImGui::BeginGroup();
	{
		BeginCard("Aimbot", IconKind::Aim, kCardW);
		ToggleRow("Enable Aim", &ConfigInstance.Aim.Enabled);
		ComboRow("Aim Type", &ConfigInstance.Aim.AimType, aimTypes, IM_ARRAYSIZE(aimTypes),
			150.f, ConfigInstance.Aim.AimType == 0);
		ComboRow("Target Bone", &ConfigInstance.Aim.TargetBone, kBones, IM_ARRAYSIZE(kBones));
		ComboRow("Target Priority", &ConfigInstance.Aim.TargetPriority, priorities, IM_ARRAYSIZE(priorities));
		if (ConfigInstance.Aim.TargetBone != 4)
			ToggleRow("Dynamic Bone Fallback", &ConfigInstance.Aim.DynamicBoneFallback);
		ComboRow("Aim Key", &ConfigInstance.Aim.AimKey, Aim::KeyNames(), Aim::KeyCount());
		EndCard();

		ImGui::Dummy(ImVec2(0.f, kCardGap - 7.f));
		BeginCard("Memory Aim", IconKind::Cpu, kCardW);
		ToggleRow("Write Camera Angles", &ConfigInstance.Aim.MemoryWrite, true);
		if (ConfigInstance.Aim.MemoryWrite)
		{
			ToggleRow("Cursor Fallback", &ConfigInstance.Aim.CursorFallback);
			SliderIntRow("Response", &ConfigInstance.Aim.ResponseMs, 0, 250, " ms", "Auto");
			Note("0 ms derives response from Smoothness. Fallback drives host cursor if DMA writes revert.");
		}
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Tuning & Prediction", IconKind::Sliders, kCardW, 0.f, false, nullptr, true);
		SliderIntRow("Aim FOV", &ConfigInstance.Aim.Fov, 5, 180, " deg");
		ComboRow("Smoothing Curve", &ConfigInstance.Aim.SmoothAlgorithm, kSmoothAlgos, IM_ARRAYSIZE(kSmoothAlgos));
		SliderIntRow("Smoothness", &ConfigInstance.Aim.Smoothness, 1, 100);
		if (ConfigInstance.Aim.SmoothAlgorithm == 2)
			SliderFloatRow("Bezier Arc", &ConfigInstance.Aim.BezierBend, 0.0f, 1.0f);
		if (ConfigInstance.Aim.SmoothAlgorithm == 1 || ConfigInstance.Aim.SmoothAlgorithm == 2)
			SliderFloatRow("Curve Speed", &ConfigInstance.Aim.BezierSpeed, 0.25f, 3.0f, "x");
		if (ConfigInstance.Aim.AimType == 1)
			SliderFloatRow("Hardware Gain", &ConfigInstance.Aim.HardwareGain, 0.25f, 5.0f, "x");
		SliderIntRow("Max Distance##aim", &ConfigInstance.Aim.MaxDistance, 10, 1000, " m");

		SectionLabel("Prediction");
		ToggleRow("Target Prediction", &ConfigInstance.Aim.Prediction);
		if (ConfigInstance.Aim.Prediction)
		{
			static const char* kPredModes[] = { "Legacy", "Adaptive" };
			ComboRow("Prediction Mode", &ConfigInstance.Aim.PredictionMode, kPredModes, IM_ARRAYSIZE(kPredModes));
			if (ConfigInstance.Aim.PredictionMode == 0)
			{
				SliderIntRow("Lead Strength", &ConfigInstance.Aim.PredictionStrength, 0, 200, "%");
				SliderIntRow("Vertical Lead", &ConfigInstance.Aim.PredictionYScale, 0, 100, "%");
			}
		}

		SectionLabel("Target Lock & Switching");
		ToggleRow("Hard Lock", &ConfigInstance.Aim.HardLock);
		SliderIntRow("Switch Delay", &ConfigInstance.Aim.TargetSwitchDelay, 0, 1000, " ms");
		EndCard();
	}
	ImGui::EndGroup();
}

void DrawAimTargetsRecoil()
{
	static const char* kSmoothAlgos[] = { "EMA", "Linear", "Bezier" };

	ImGui::BeginGroup();
	{
		BeginCard("Target Filters", IconKind::Shield, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Visible Bones Only", &ConfigInstance.Aim.VisibleBonesOnly);
		ToggleRow("Ignore Teammates", &ConfigInstance.Aim.ExpIgnoreTeammates);
		ToggleRow("Skip Knocked Players", &ConfigInstance.Aim.SkipKnocked);
		ToggleRow("Skip Police targets", &ConfigInstance.Aim.ExpIgnorePolice);
		ToggleRow("Skip Criminal targets", &ConfigInstance.Aim.ExpIgnoreCriminal);
		ToggleRow("Skip Civilian targets", &ConfigInstance.Aim.ExpIgnoreCivilian);
		const int tc = ConfigInstance.Aim.TrustedCount();
		Note("Trusted Players: %d (manage in Misc -> Server)", tc);
		EndCard();

		ImGui::Dummy(ImVec2(0.f, kCardGap - 7.f));
		BeginCard("Aim Visuals", IconKind::Visuals, kCardW);
		ToggleRow("Show FOV Circle", &ConfigInstance.Aim.ShowFov);
		ToggleRow("Aim Debug HUD", &ConfigInstance.Aim.ShowDebug);
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Recoil & Spread", IconKind::Sliders, kCardW);
		ToggleRow("No Recoil", &ConfigInstance.Aim.NoRecoil, true);
		ToggleRow("No Spread", &ConfigInstance.Aim.NoSpread, true);
		ToggleRow("Dynamic Recoil Curve", &ConfigInstance.Aim.DynamicRecoilCompensation, true);
		if (ConfigInstance.Aim.DynamicRecoilCompensation)
			SliderFloatRow("Curve Strength", &ConfigInstance.Aim.RecoilStrength, 0.1f, 2.0f, "x");
		Note("Zeroes weapon vectors over DMA or applies dynamic pattern compensation.");
		EndCard();

		ImGui::Dummy(ImVec2(0.f, kCardGap - 7.f));
		BeginCard("Live State", IconKind::Aim, kCardW);
		{
			const Aim::DebugInfo info = Aim::GetDebug();
			const char* curveLabel = (info.smoothAlgorithm >= 0 && info.smoothAlgorithm < 3)
				? kSmoothAlgos[info.smoothAlgorithm] : "EMA";
			const char* clsLabel = (info.activeWeaponClass >= 0 && info.activeWeaponClass < Aim::WeaponClassCount())
				? Aim::WeaponClassNames()[info.activeWeaponClass] : "Default";
			Note("%s | Curve: %s | FOV: %d deg", info.reason, curveLabel, info.activeFov);
			Note("Key %s (%s) | %s", info.held ? "held" : "up", info.keySource,
				info.deviceReady ? "device ready" : "device idle");
			Note("Weapon: %s (%s%s) | Trigger: %s",
				info.activeWeaponName.empty() ? "None" : info.activeWeaponName.c_str(),
				clsLabel, info.profileOverrideActive ? " override" : "", info.triggerState);
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

void DrawAimTriggerbot()
{
	static const char* kTriggerBones[] = { "Head", "Neck", "Chest", "Pelvis", "Any / Closest" };
	static int selectedWeaponProfile = 0;

	ImGui::BeginGroup();
	{
		BeginCard("Triggerbot", IconKind::Bolt, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Enable Triggerbot", &ConfigInstance.Aim.TriggerEnabled);
		ComboRow("Trigger Key", &ConfigInstance.Aim.TriggerKey, Aim::KeyNames(), Aim::KeyCount());
		ComboRow("Trigger Bone", &ConfigInstance.Aim.TriggerBone, kTriggerBones, IM_ARRAYSIZE(kTriggerBones));
		if (SliderIntRow("Min Delay", &ConfigInstance.Aim.TriggerMinDelayMs, 0, 500, " ms")) {
			if (ConfigInstance.Aim.TriggerMaxDelayMs < ConfigInstance.Aim.TriggerMinDelayMs)
				ConfigInstance.Aim.TriggerMaxDelayMs = ConfigInstance.Aim.TriggerMinDelayMs;
		}
		if (SliderIntRow("Max Delay", &ConfigInstance.Aim.TriggerMaxDelayMs, 0, 500, " ms")) {
			if (ConfigInstance.Aim.TriggerMinDelayMs > ConfigInstance.Aim.TriggerMaxDelayMs)
				ConfigInstance.Aim.TriggerMinDelayMs = ConfigInstance.Aim.TriggerMaxDelayMs;
		}
		SliderIntRow("Burst Hold", &ConfigInstance.Aim.TriggerBurstMs, 10, 250, " ms");
		SliderIntRow("Shot Cooldown", &ConfigInstance.Aim.TriggerCooldownMs, 0, 500, " ms");
		SliderFloatRow("Hitbox Scale", &ConfigInstance.Aim.TriggerHitboxScale, 0.25f, 2.0f, "x");
		SliderIntRow("Max Range", &ConfigInstance.Aim.TriggerMaxDistance, 5, 500, " m");
		ToggleRow("Visible Only", &ConfigInstance.Aim.TriggerVisibleOnly);
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Weapon Profiles", IconKind::Sliders, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Enable Weapon Profiles", &ConfigInstance.Aim.WeaponProfilesEnabled);
		selectedWeaponProfile = std::clamp(selectedWeaponProfile, 0, Aim::WeaponClassCount() - 1);
		ComboRow("Weapon Class", &selectedWeaponProfile, Aim::WeaponClassNames(), Aim::WeaponClassCount(), 152.f, false, false);
		{
			const Aim::DebugInfo wInfo = Aim::GetDebug();
			const char* detectedCls = (wInfo.activeWeaponClass >= 0 && wInfo.activeWeaponClass < Aim::WeaponClassCount())
				? Aim::WeaponClassNames()[wInfo.activeWeaponClass] : "None";
			Note("Equipped: %s (Class: %s)",
				wInfo.activeWeaponName.empty() ? "-" : wInfo.activeWeaponName.c_str(), detectedCls);
		}
		ImGui::PushID(selectedWeaponProfile);
		WeaponProfile& wp = ConfigInstance.Aim.WeaponProfiles[static_cast<std::size_t>(selectedWeaponProfile)];
		ToggleRow("Override Active", &wp.Enabled);
		if (wp.Enabled)
		{
			static const char* kProfileBones[] = { "Inherit", "Head", "Neck", "Chest", "Pelvis", "Closest" };
			int boneUi = std::clamp(wp.TargetBone + 1, 0, 5);
			if (ComboRow("Profile Bone", &boneUi, kProfileBones, IM_ARRAYSIZE(kProfileBones)))
				wp.TargetBone = boneUi - 1;

			static const char* kProfileCurves[] = { "Inherit", "EMA", "Linear", "Bezier" };
			int curveUi = std::clamp(wp.SmoothAlgorithmOverride + 1, 0, 3);
			if (ComboRow("Profile Curve", &curveUi, kProfileCurves, IM_ARRAYSIZE(kProfileCurves)))
				wp.SmoothAlgorithmOverride = curveUi - 1;

			SliderIntRow("FOV Override", &wp.FovOverride, 0, 180, " deg", "Inherit");
			if (wp.FovOverride > 0 && wp.FovOverride < 5) wp.FovOverride = 5;
			SliderIntRow("Smooth Override", &wp.SmoothnessOverride, 0, 100, "", "Inherit");

			SectionLabel("Profile Trigger");
			ToggleRow("Allow Trigger", &wp.TriggerEnabled);

			std::vector<const char*> trigKeyItems;
			trigKeyItems.reserve(static_cast<std::size_t>(Aim::KeyCount()) + 1);
			trigKeyItems.push_back("Inherit");
			const char* const* baseKeys = Aim::KeyNames();
			for (int i = 0; i < Aim::KeyCount(); ++i) trigKeyItems.push_back(baseKeys[i]);
			int trigKeyUi = std::clamp(wp.TriggerKeyOverride + 1, 0, Aim::KeyCount());
			if (ComboRow("Trigger Key##wp", &trigKeyUi, trigKeyItems.data(), (int)trigKeyItems.size()))
				wp.TriggerKeyOverride = trigKeyUi - 1;

			if (SliderIntRow("Min Delay##wp", &wp.TriggerMinDelayMs, -1, 500, " ms", "Inherit")) {
				if (wp.TriggerMaxDelayMs >= 0 && wp.TriggerMinDelayMs >= 0 && wp.TriggerMaxDelayMs < wp.TriggerMinDelayMs)
					wp.TriggerMaxDelayMs = wp.TriggerMinDelayMs;
			}
			if (SliderIntRow("Max Delay##wp", &wp.TriggerMaxDelayMs, -1, 500, " ms", "Inherit")) {
				if (wp.TriggerMinDelayMs >= 0 && wp.TriggerMaxDelayMs >= 0 && wp.TriggerMinDelayMs > wp.TriggerMaxDelayMs)
					wp.TriggerMinDelayMs = wp.TriggerMaxDelayMs;
			}
			SliderFloatRow("Hitbox##wp", &wp.TriggerHitboxScale, 0.f, 2.0f, "x", "Inherit");
			SliderIntRow("Range##wp", &wp.TriggerMaxDistance, 0, 500, " m", "Inherit");
			if (ButtonRow("", "Reset Profile", 120.f)) {
				wp = WeaponProfile{};
				g_dirty = true;
			}
		}
		ImGui::PopID();
		EndCard();
	}
	ImGui::EndGroup();
}

void DrawAimHardwarePad()
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

	if (g_aimVarsSyncNeeded) {
		deviceIndex = ConfigInstance.Aim.DeviceIndex;
		std::strncpy(comPort, ConfigInstance.Aim.ComPort.c_str(), sizeof(comPort) - 1);
		comPort[sizeof(comPort) - 1] = 0;
		std::strncpy(ipAddr, ConfigInstance.Aim.IpAddr.c_str(), sizeof(ipAddr) - 1);
		ipAddr[sizeof(ipAddr) - 1] = 0;
		std::strncpy(netPort, ConfigInstance.Aim.NetPort.c_str(), sizeof(netPort) - 1);
		netPort[sizeof(netPort) - 1] = 0;
		std::strncpy(macId, ConfigInstance.Aim.MacId.c_str(), sizeof(macId) - 1);
		macId[sizeof(macId) - 1] = 0;
		baudIndex = ConfigInstance.Aim.BaudIndex;
		manualPort = ConfigInstance.Aim.ManualPort;
		ferrumController = ConfigInstance.Aim.FerrumController;
		g_aimVarsSyncNeeded = false;
	}

	static const char* deviceNames[] = { "Off", "Kmbox USB", "Kmbox Net", "Makcu", "Makcu Controller",
		"Ferrum", "Makcu Medius", "Kmbox NB Pro" };
	static const DeviceKind deviceKinds[] = { DeviceKind::Off, DeviceKind::KmboxUsb, DeviceKind::KmboxNet,
		DeviceKind::Makcu, DeviceKind::Controller, DeviceKind::Ferrum, DeviceKind::Medius,
		DeviceKind::KmboxNbPro };
	static const int baudRates[] = { 115200, 921600, 1500000, 2000000, 3000000, 4000000 };
	static const char* baudLabels[] = { "115200", "921600", "1500000", "2000000", "3000000", "4000000" };
	static const char* kControllerSlots[] = { "Auto (0-3)", "Pad 1", "Pad 2", "Pad 3", "Pad 4" };

	ImGui::BeginGroup();
	{
		BeginCard("Hardware Device", IconKind::Cpu, kCardW, 0.f, false, nullptr, true);
		ComboRow("Device", &deviceIndex, deviceNames, IM_ARRAYSIZE(deviceNames));

		const auto kind = deviceKinds[ImClamp(deviceIndex, 0, (int)IM_ARRAYSIZE(deviceKinds) - 1)];
		const bool isNet = IsNetworkDevice(kind);
		const bool isSerial = kind != DeviceKind::Off && !isNet;

		if (isNet)
		{
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::InputTextWithHint("##ip", "IP address", ipAddr, sizeof(ipAddr))) g_dirty = true;
			const float half = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
			ImGui::SetNextItemWidth(half);
			if (ImGui::InputTextWithHint("##netport", "Network port", netPort, sizeof(netPort))) g_dirty = true;
			ImGui::SameLine(0.f, 8.f);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::InputTextWithHint("##mac", "Device ID", macId, sizeof(macId))) g_dirty = true;
		}

		if (isSerial)
		{
			if (needsRefresh) { Session().RefreshPorts(); needsRefresh = false; }
			const auto& ports = Session().GetPorts();

			ToggleRow("Manual COM Port", &manualPort);
			if (manualPort || kind == DeviceKind::Ferrum)
			{
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				if (ImGui::InputTextWithHint("##com", "COM3", comPort, sizeof(comPort))) g_dirty = true;
			}
			else if (!ports.empty())
			{
				std::vector<const char*> portItems;
				portItems.reserve(ports.size());
				for (const auto& p : ports) portItems.push_back(p.port.c_str());

				int sel = 0;
				for (int i = 0; i < (int)ports.size(); i++)
					if (ports[i].port == comPort) { sel = i; break; }
				if (ComboRow("COM Port", &sel, portItems.data(), (int)portItems.size()))
				{
					std::strncpy(comPort, ports[sel].port.c_str(), sizeof(comPort) - 1);
					comPort[sizeof(comPort) - 1] = 0;
				}
			}
			else
			{
				Note("No COM ports found.");
			}

			if (IsMakcu(kind))
				ComboRow("Baud Rate", &baudIndex, baudLabels, IM_ARRAYSIZE(baudLabels));
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
				if (InlineButton("Connect", 104.f, true))
				{
					DeviceConfig cfg;
					cfg.kind = kind;
					cfg.port = comPort;
					cfg.ip = ipAddr;
					cfg.networkPort = netPort;
					cfg.mac = macId;
					cfg.baud = baudRates[ImClamp(baudIndex, 0, (int)IM_ARRAYSIZE(baudRates) - 1)];
					cfg.manualPort = manualPort || kind == DeviceKind::Ferrum;
					cfg.ferrumController = ferrumController;
					Session().Connect(cfg);
				}
			}
			else
			{
				if (InlineButton("Disconnect", 104.f))
					Session().Disconnect();
				if (connected) {
					ImGui::SameLine(0.f, 8.f);
					if (InlineButton("Test Movement", 118.f))
						Session().TestMovement();
				}
			}
			if (isSerial) {
				ImGui::SameLine(0.f, 8.f);
				if (InlineButton("Refresh Ports", 104.f))
					Session().RefreshPorts();
			}
		}

		Note("Status: %s", StateLabel(status.state));
		if (!status.endpoint.empty()) Note("Endpoint: %s", status.endpoint.c_str());
		if (status.protocolVerified) Note("Protocol verified.");
		if (!status.error.empty()) Note("%s", status.error.c_str());
		if (status.retryInMs > 0) Note("Retrying in %u ms", status.retryInMs);
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Controller & Input", IconKind::Sliders, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Enable Gamepad Input", &ConfigInstance.Aim.ControllerEnabled);
		ComboRow("Host Pad Slot", &ConfigInstance.Aim.ControllerSlot, kControllerSlots, IM_ARRAYSIZE(kControllerSlots));
		ToggleRow("Dual Gamepad Key", &ConfigInstance.Aim.ControllerSecondaryEnable);
		if (ConfigInstance.Aim.ControllerSecondaryEnable)
			ComboRow("Gamepad Aim Key", &ConfigInstance.Aim.ControllerAimKey, Aim::KeyNames(), Aim::KeyCount());
		SliderFloatRow("Trigger Threshold", &ConfigInstance.Aim.ControllerTriggerThreshold, 0.05f, 0.95f);
		SliderFloatRow("Stick Deadzone", &ConfigInstance.Aim.ControllerStickDeadzone, 0.0f, 0.80f);
		ToggleRow("Require Stick Move", &ConfigInstance.Aim.ControllerRequireStick);
		SliderFloatRow("Stick Output Scale", &ConfigInstance.Aim.ControllerStickScale, 10.f, 500.f);
		{
			const Aim::DebugInfo padInfo = Aim::GetDebug();
			if (padInfo.controllerConnected)
				Note("Pad: %s | LT %.2f RT %.2f | RS (%.2f, %.2f)",
					padInfo.controllerSource, padInfo.controllerLt, padInfo.controllerRt,
					padInfo.controllerRx, padInfo.controllerRy);
			else
				Note("No gamepad detected (Makcu/Ferrum controller & host XInput).");
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

// ===========================================================================
// TAB 1: VISUALS
// ===========================================================================
void DrawVisualsEsp()
{
	static const char* snapOrigins[] = { "Bottom", "Crosshair", "Top" };

	ImGui::BeginGroup();
	{
		BeginCard("Player ESP", IconKind::Visuals, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Hide Teammates", &ConfigInstance.Player.HideTeammates);
		ToggleRow("Hidden Player ESP", &ConfigInstance.Player.HiddenPlayerEsp);
		ColorToggleRow("Box ESP", &ConfigInstance.Player.BoxEsp, ConfigInstance.Player.BoxColour);
		ColorToggleRow("Health", &ConfigInstance.Player.Health, ConfigInstance.Player.HealthTextColour);
		ColorToggleRow("Distance", &ConfigInstance.Player.Distance, ConfigInstance.Player.DistanceColour);
		ColorToggleRow("Snaplines", &ConfigInstance.Player.Snaplines, ConfigInstance.Player.SnaplineColour);
		if (ConfigInstance.Player.Snaplines)
			ComboRow("Snapline Origin", &ConfigInstance.Player.SnaplineOrigin, snapOrigins, IM_ARRAYSIZE(snapOrigins));
		ColorToggleRow("Off-Screen Indicators", &ConfigInstance.Player.OffscreenArrows, ConfigInstance.Player.OffscreenColour);
		ColorToggleRow("Head Hitbox Dot", &ConfigInstance.Player.HeadDot, ConfigInstance.Player.HeadDotColour);
		if (ConfigInstance.Player.HeadDot)
			SliderFloatRow("Dot Radius", &ConfigInstance.Player.HeadDotRadius, 1.5f, 8.0f, " px");
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Weapons & Range", IconKind::Sliders, kCardW);
		ColorToggleRow("Weapon Name", &ConfigInstance.Player.WeaponEsp, ConfigInstance.Player.WeaponColour);
		ToggleRow("Weapon Sprite", &ConfigInstance.Player.WeaponSprite);
		SliderIntRow("Max Distance##plr", &ConfigInstance.Player.MaxDistance, 0, 1500, " m");
		SliderIntRow("Font Size##plr", &ConfigInstance.Player.FontSize, 1, 24, " px");
		EndCard();

		ImGui::Dummy(ImVec2(0.f, kCardGap - 7.f));
		BeginCard("Flags & HUD", IconKind::Shield, kCardW, 0.f, false, nullptr, true);
		ColorToggleRow("Looking At You", &ConfigInstance.Player.LookingAtYou, ConfigInstance.Player.LookingColour);
		ToggleRow("Looking Alert", &ConfigInstance.Player.LookingAlert);
		ToggleRow("Spectator Alert", &ConfigInstance.Player.SpectatorAlert);
		ColorToggleRow("Show Role", &ConfigInstance.Player.ShowRole, ConfigInstance.Player.RoleColour);
		if (ConfigInstance.Player.ShowRole)
			ToggleRow("Color-code by role", &ConfigInstance.Player.ExpRoleColors);
		ToggleRow("Money & Value HUD", &ConfigInstance.Player.ExpMoneyHud);
		EndCard();
	}
	ImGui::EndGroup();
}

void DrawVisualsSkeleton()
{
	ImGui::BeginGroup();
	{
		BeginCard("Skeleton & Chams", IconKind::Visuals, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Draw Skeleton", &ConfigInstance.Player.Skeleton);
		SliderFloatRow("Bone Thickness", &ConfigInstance.Player.SkeletonThickness, 0.5f, 6.0f, " px");
		ToggleRow("Head Circle", &ConfigInstance.Player.HeadCircle);

		SectionLabel("3D Capsule Chams");
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
		else
		{
			Note("Read-only DMA overlay silhouette over character limbs.");
		}
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Visibility & BVH", IconKind::Shield, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Enable VisCheck", &ConfigInstance.Player.VisCheck);
		if (ConfigInstance.Player.VisCheck)
		{
			ColorRow("Visible to camera", ConfigInstance.Player.VisibleColour);
			ColorRow("Occluded", ConfigInstance.Player.HiddenColour);
		}
		ToggleRow("Enable WallCheck", &ConfigInstance.Player.WallCheck);
		{
			auto status = RobRunReader::GetBvhStatus();
			char bvhBuf[96];
			if (status.loaded)
				std::snprintf(bvhBuf, sizeof(bvhBuf), "BVH: %uk nodes | %uk tris",
					status.nodeCount / 1000u, status.triCount / 1000u);
			else
				std::snprintf(bvhBuf, sizeof(bvhBuf), "BVH: not loaded (animator fallback)");
			if (ButtonRow(bvhBuf, "Reload BVH", 104.f))
				RobRunReader::ReloadBvh();
		}
		ToggleRow("Show BVH", &ConfigInstance.Player.ShowBvh);
		if (ConfigInstance.Player.ShowBvh)
			SliderIntRow("BVH Draw Dist", &ConfigInstance.Player.BvhDrawDist, 10, 500, " m");
		SliderIntRow("Vis Ray Count", &ConfigInstance.Player.VisRayCount, 2, 12);
		SliderFloatRow("Vis Max Dist", &ConfigInstance.Player.VisMaxDistance, 0.f, 500.f, " m");
		SliderFloatRow("Hidden Alpha", &ConfigInstance.Player.HiddenAlpha, 0.f, 1.f);
		ToggleRow("Car Cabin Solid", &ConfigInstance.Player.CarCabinSolid);
		ColorToggleRow("Vis Ring", &ConfigInstance.Player.VisRing, ConfigInstance.Player.VisRingColour);
		ColorToggleRow("Status Ring", &ConfigInstance.Player.StatusRing, ConfigInstance.Player.StatusRingColour);
		EndCard();
	}
	ImGui::EndGroup();
}

void DrawVisualsStates()
{
	ImGui::BeginGroup();
	{
		BeginCard("State Indicators", IconKind::Shield, kCardW);
		ColorToggleRow("Firing", &ConfigInstance.Player.ShowFiring, ConfigInstance.Player.FiringColour);
		ColorToggleRow("Reloading", &ConfigInstance.Player.ShowReloading, ConfigInstance.Player.ReloadColour);
		ColorToggleRow("Armor", &ConfigInstance.Player.ShowArmor, ConfigInstance.Player.ArmorColour);
		ToggleRow("Armor Value", &ConfigInstance.Player.ShowArmorValue);
		ColorToggleRow("Stamina", &ConfigInstance.Player.ShowStamina, ConfigInstance.Player.StaminaColour);
		SectionLabel("Tag Colour");
		ColorRow("State Tags ([KD] [KO] [C] [S] [V])", ConfigInstance.Player.StateColour);
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Name & Role Colours", IconKind::Palette, kCardW, 0.f, false, nullptr, true);
		ColorRow("Name", ConfigInstance.Player.NameColour);
		ToggleRow("Color names by role", &ConfigInstance.Player.NameByRole);
		if (ConfigInstance.Player.NameByRole)
		{
			ColorRow("Criminal", ConfigInstance.Player.CriminalColour);
			ColorRow("Police", ConfigInstance.Player.PoliceColour);
			ColorRow("Civilian", ConfigInstance.Player.CivilianColour);
			ColorRow("Hubber", ConfigInstance.Player.HubberColour);
		}
		EndCard();

		ImGui::Dummy(ImVec2(0.f, kCardGap - 7.f));
		BeginCard("Rendering", IconKind::Sliders, kCardW);
		ToggleRow("Pose Smoothing", &ConfigInstance.Player.PoseSmoothing);
		if (ConfigInstance.Player.PoseSmoothing)
			SliderFloatRow("Blend Alpha", &ConfigInstance.Player.PoseSmoothAlpha, 0.1f, 1.f);
		ToggleRow("Native Camera", &ConfigInstance.Player.NativeEspCamera);
		{
			const auto cam = RobRunReader::GetCameraDebug();
			if (!cam.nativeEnabled)
				Note("Managed camera (native off)");
			else if (!cam.nativeValid)
				Note("Managed camera (native not found)");
			else
				Note("Native camera | d=%.2fm | a=%.1f deg", cam.positionError,
					180.f / 3.14159265f * std::acos(std::clamp(cam.forwardDot, -1.f, 1.f)));
		}
		EndCard();
	}
	ImGui::EndGroup();
}

// ===========================================================================
// TAB 2: WORLD
// ===========================================================================
void DrawWorldAndLoot()
{
	auto& lf = ConfigInstance.LootFilter;
	if (lf.Items.empty()) lf.InitDefaults();

	ImGui::BeginGroup();
	{
		BeginCard("World ESP", IconKind::World, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Enable World ESP", &ConfigInstance.World.Enabled);
		ToggleRow("House Info", &ConfigInstance.World.HouseEsp);
		if (ConfigInstance.World.HouseEsp)
			SliderIntRow("House Range", &ConfigInstance.World.HouseMaxDistance, 50, 1000, " m");
		ToggleRow("Show Empty Tag", &ConfigInstance.World.ShowEmpty);
		ToggleRow("Defense Blocks", &ConfigInstance.World.DefenseBlocks);
		SliderIntRow("Min Cash / Value", &ConfigInstance.World.MinValueFilter, 0, 50000, " $");
		SliderIntRow("House / Legacy Font", &ConfigInstance.World.FontSize, 1, 24, " px");

		if (lf.Enabled)
		{
			Note("Loot Filter owns item visibility & range. Turn Loot Filter off to use legacy switches.");
		}
		else
		{
			SectionLabel("Legacy Mode (Filter Off)");
			ToggleRow("Show Distance", &ConfigInstance.World.ShowDistance);
			SliderIntRow("Legacy Max Dist", &ConfigInstance.World.MaxDistance, 0, 2000, " m");
			ToggleRow("Security Alarms", &ConfigInstance.World.SecurityAlarms);
			ToggleRow("First Aid Cabinets", &ConfigInstance.World.FirstAidCabinets);
			ToggleRow("Drug Labs", &ConfigInstance.World.DrugWorkbenches);
			ToggleRow("Weed", &ConfigInstance.World.Weed);
		}
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Loot Filter Display", IconKind::Sliders, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Enable Loot Filter", &lf.Enabled);
		ToggleRow("Show on Map", &lf.ShowOnMap);
		ToggleRow("Show on Minimap", &lf.ShowOnMinimap);
		ToggleRow("Hide Empty Containers", &lf.HideEmpty);
		ToggleRow("Hide Looted Items", &lf.FilterTakenLoot);
		ToggleRow("Read Vehicle / Table Inventories", &lf.ReadVehicleInventories);
		SliderFloatRow("Base Dot Size", &lf.BaseDotSize, 1.5f, 8.f, " px");
		SliderIntRow("ESP Font Size", &lf.FontSize, 8, 32, " px");
		SliderIntRow("ESP Icon Size", &lf.IconSize, 16, 256, " px");
		SliderIntRow("Global Max Distance", &lf.GlobalMaxDistance, 0, 3000, " m");
		EndCard();
	}
	ImGui::EndGroup();
}

void DrawWorldValueSizing()
{
	auto& lf = ConfigInstance.LootFilter;
	if (lf.Items.empty()) lf.InitDefaults();

	ImGui::BeginGroup();
	{
		BeginCard("Value Tiers", IconKind::Sliders, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Filter By Min Price", &lf.FilterByMinPrice);
		SliderIntRow("Min Price", &lf.MinPrice, 0, 50000, " $");
		SliderIntRow("High Value >=", &lf.HighValueThreshold, 1000, 100000, " $");
		ColorRow("High Value Color", lf.HighValueColour);
		ToggleRow("High Value Pulse (Radar)", &lf.HighValuePulse);
		SliderIntRow("Mid Value >=", &lf.MidValueThreshold, 500, 50000, " $");
		ColorRow("Mid Value Color", lf.MidValueColour);
		EndCard();

		ImGui::Dummy(ImVec2(0.f, kCardGap - 7.f));
		BeginCard("Filter Presets", IconKind::File, kCardW, 0.f, false, nullptr, true);
		{
			const float gap = 6.f;
			const float bw = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
			if (InlineButton("Export JSON", bw))
			{
				const std::wstring path = PickConfigPath(true);
				if (path.empty()) { s_lfStatusNote = "Export cancelled"; }
				else {
					std::ofstream out(path.c_str(), std::ios::trunc);
					if (out) { out << lf.ExportJson(); s_lfStatusNote = "Exported filter JSON"; Overlay::Notify("Filter exported", IM_COL32(70, 194, 120, 255), 2.f); }
					else { s_lfStatusNote = "Could not write file"; }
				}
			}
			ImGui::SameLine(0.f, gap);
			if (InlineButton("Import JSON", bw))
			{
				const std::wstring path = PickConfigPath(false);
				if (path.empty()) { s_lfStatusNote = "Import cancelled"; }
				else {
					std::ifstream in(path.c_str());
					if (in) {
						std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
						const bool ok = lf.ImportJson(text);
						s_lfStatusNote = ok ? "Imported filter JSON" : "No known keys in file";
						if (ok) { g_dirty = true; Overlay::Notify("Filter imported", IM_COL32(70, 194, 120, 255), 2.f); }
					} else { s_lfStatusNote = "Could not read file"; }
				}
			}

			if (InlineButton("Copy Code", bw))
			{
				ImGui::SetClipboardText(lf.ExportCode().c_str());
				s_lfStatusNote = "Copied filter code to clipboard";
				Overlay::Notify("Filter code copied", IM_COL32(70, 194, 120, 255), 2.f);
			}
			ImGui::SameLine(0.f, gap);
			if (InlineButton("Paste Code", bw))
			{
				const char* clip = ImGui::GetClipboardText();
				if (clip && *clip) {
					lf.ImportCode(clip);
					g_dirty = true;
					s_lfStatusNote = "Imported filter code from clipboard";
					Overlay::Notify("Filter code imported", IM_COL32(70, 194, 120, 255), 2.f);
				} else {
					s_lfStatusNote = "Clipboard is empty";
				}
			}

			if (ButtonRow(s_lfStatusNote.empty() ? "Share or reset item rules." : s_lfStatusNote.c_str(),
				"Reset Filter", 110.f, true))
			{
				lf.ResetToDefaults();
				g_dirty = true;
				s_lfStatusNote = "Loot filter reset to defaults";
				Overlay::Notify("Loot filter reset", IM_COL32(70, 194, 120, 255), 2.f);
			}
		}
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Per-Category Sizing", IconKind::Sliders, kCardW);
		static const std::array<const char*, (std::size_t)LootFilter::Category::COUNT> kSizeCats = [] {
			std::array<const char*, (std::size_t)LootFilter::Category::COUNT> a{};
			for (std::size_t i = 0; i < a.size(); ++i) a[i] = LootFilter::CategoryNames[i];
			return a;
		}();
		ComboRow("Category##size", &lf.SizingCategory, kSizeCats.data(), (int)kSizeCats.size(), 175.f, false, false);
		{
			const int sel = std::clamp(lf.SizingCategory, 0, (int)kSizeCats.size() - 1);
			auto& sizing = lf.Sizing[(std::size_t)sel];
			SliderIntRow("Icon Size##cat", &sizing.IconSize, 0, 256, " px", "Global");
			SliderIntRow("Font Size##cat", &sizing.FontSize, 0, 40, " px", "Global");
			SliderFloatRow("Dot Size##cat", &sizing.DotSize, 0.f, 12.f, " px", "Global");
			SliderIntRow("Max Distance##cat", &sizing.MaxDistance, 0, 3000, " m", "Per item");
		}
		EndCard();

		ImGui::Dummy(ImVec2(0.f, kCardGap - 7.f));
		BeginCard("Live World Stats", IconKind::World, kCardW);
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
		}
		EndCard();
	}
	ImGui::EndGroup();
}

void LootItemRow(const LootFilter::ItemDef& def, ItemSettings& item)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	ImGui::PushID(def.id);
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

	bool hov = false, held = false;
	const bool pressed = ImGui::ButtonBehavior(bb, rowId, &hov, &held);
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	ImDrawList* dl = window->DrawList;

	if (s_lfSelected.count(def.id))
		dl->AddRectFilled(bb.Min, bb.Max, C(kAccent, 0.16f), 4.f);
	else if (hov)
		dl->AddRectFilled(bb.Min, bb.Max, C(kElevated, 0.85f), 4.f);

	// Fixed right-aligned columns: [Price 76] [Range 64] [Edit 56] [Toggle 34]
	constexpr float toggleW = 32.f, toggleH = 17.f;
	constexpr float editW = 54.f, distW = 62.f, priceW = 74.f, gap = 8.f;

	const ImRect toggleRect(
		ImVec2(p.x + w - toggleW, p.y + (rowH - toggleH) * 0.5f),
		ImVec2(p.x + w, p.y + (rowH + toggleH) * 0.5f));
	const ImRect editRect(
		ImVec2(toggleRect.Min.x - gap - editW, p.y + 2.f),
		ImVec2(toggleRect.Min.x - gap, p.y + rowH - 2.f));
	const ImRect distRect(
		ImVec2(editRect.Min.x - gap - distW, p.y + 2.f),
		ImVec2(editRect.Min.x - gap, p.y + rowH - 2.f));
	const float priceRightX = distRect.Min.x - gap;

	const bool multiSel = s_lfSelected.size() > 1 && s_lfSelected.count(def.id) > 0;
	auto applyToSelection = [&](auto&& fn) {
		if (!multiSel) return;
		for (const auto& id : s_lfSelected)
			if (id != def.id) fn(ConfigInstance.LootFilter.GetItemMut(id.c_str()));
	};

	bool openOptions = false;
	if (pressed) {
		if (toggleRect.Contains(mouse)) {
			item.visible = !item.visible;
			g_dirty = true;
			if (multiSel) applyToSelection([&](ItemSettings& s) { s.visible = item.visible; });
		}
		else if (editRect.Contains(mouse) || distRect.Contains(mouse)) {
			openOptions = true;
		}
		else {
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
				s.useGlobalDistance = (s.maxDistance == 0);
			};
			adjustRange(item);
			applyToSelection(adjustRange);
			g_dirty = true;
		}
	}

	static std::map<ImGuiID, float> s_lrAnim;
	auto it = s_lrAnim.find(rowId);
	if (it == s_lrAnim.end()) it = s_lrAnim.emplace(rowId, item.visible ? 1.f : 0.f).first;
	float& ta = it->second;
	ta = ImLerp(ta, item.visible ? 1.f : 0.f, ImGui::GetIO().DeltaTime * 15.f);
	DrawPillSwitch(dl, toggleRect, ta);

	// Edit chip
	const bool editHov = hov && editRect.Contains(mouse);
	const bool distHov = hov && distRect.Contains(mouse);
	if (hov && (toggleRect.Contains(mouse) || editHov || distHov))
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	dl->AddRectFilled(editRect.Min, editRect.Max, C(editHov ? kElevated : kControl), 4.f);
	dl->AddRect(editRect.Min, editRect.Max, C(editHov ? Mix(kStroke, kAccent, 0.5f) : kStroke), 4.f);
	{
		const char* lbl = "Edit";
		const float tw = TextW(font::inter_medium_small, lbl);
		dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
			ImVec2(editRect.Min.x + (editW - tw) * 0.5f, RowTextY(p.y, rowH, font::inter_medium_small)),
			C(editHov ? kText : kTextDim), lbl);
	}

	// Range chip (clickable + scroll-wheel)
	dl->AddRectFilled(distRect.Min, distRect.Max, C(distHov ? kElevated : kControl), 4.f);
	dl->AddRect(distRect.Min, distRect.Max, C(distHov ? Mix(kStroke, kAccent, 0.5f) : kStroke), 4.f);
	{
		char distBuf[16];
		if (item.useGlobalDistance || item.maxDistance <= 0)
			strcpy_s(distBuf, "Global");
		else
			std::snprintf(distBuf, sizeof(distBuf), "%d m", item.maxDistance);
		const float dtw = TextW(font::inter_medium_small, distBuf);
		dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
			ImVec2(distRect.Min.x + (distW - dtw) * 0.5f, RowTextY(p.y, rowH, font::inter_medium_small)),
			C(distHov ? kText : kTextDim), distBuf);
	}

	// Price column (fixed right-aligned)
	if (def.price > 0)
	{
		const std::string pStr = "$" + FormatCash((int)def.price);
		TextRight(font::inter_medium_small, priceRightX, RowTextY(p.y, rowH, font::inter_medium_small),
			C(kGreen), pStr.c_str());
	}

	// Options popup (100% pill toggles — ZERO ImGui::Checkbox)
	if (openOptions) ImGui::OpenPopup("##lf_opts");
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f, 10.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f, 6.f));
	ImGui::PushStyleColor(ImGuiCol_PopupBg, C(kPopup));
	ImGui::PushStyleColor(ImGuiCol_Border, C(kStroke));
	if (ImGui::BeginPopup("##lf_opts"))
	{
		bool changed = false;
		ImGui::PushFont(font::inter_medium_widget);
		ImGui::TextUnformatted(def.displayName);
		ImGui::PopFont();
		ImGui::Separator();

		for (int i = 0; i < (int)std::size(LootFilter::RenderFlagBits); ++i)
		{
			const std::uint8_t bit = LootFilter::RenderFlagBits[i];
			if (bit == LootFilter::RenderPrice && def.price == 0) continue;
			bool on = item.Has(bit);
			if (PopupToggleRow(LootFilter::RenderFlagNames[i], &on, 210.f))
			{
				item.Set(bit, on);
				changed = true;
			}
		}

		ImGui::Separator();
		if (PopupToggleRow("Use Global Distance", &item.useGlobalDistance, 210.f))
			g_dirty = true;
		ImGui::BeginDisabled(item.useGlobalDistance);
		ImGui::SetNextItemWidth(210.f);
		if (ImGui::SliderInt("##lf_rng", &item.maxDistance, 0, 3000, "Range: %d m"))
			g_dirty = true;
		ImGui::EndDisabled();

		{
			char nameBuf[96]{};
			std::strncpy(nameBuf, item.customName.c_str(), sizeof(nameBuf) - 1);
			ImGui::SetNextItemWidth(210.f);
			if (ImGui::InputTextWithHint("##custom_name", "Custom label (blank = default)", nameBuf, sizeof(nameBuf)))
			{
				item.customName = nameBuf;
				g_dirty = true;
			}
		}

		ImGui::Separator();
		if (InlineButton("All", 66.f)) {
			for (std::uint8_t bit : LootFilter::RenderFlagBits) item.Set(bit, true);
			changed = true;
		}
		ImGui::SameLine(0.f, 6.f);
		if (InlineButton("None", 66.f)) { item.flags = 0; changed = true; }
		ImGui::SameLine(0.f, 6.f);
		if (InlineButton("Default", 66.f)) {
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
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(2);

	// Left side: rarity dot + optional icon + name + category
	uint32_t argb = LootFilter::RarityColors[static_cast<int>(def.rarity)];
	ImU32 rarityCol = (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFFu) | ((argb & 0xFFu) << 16);
	dl->AddCircleFilled(ImVec2(p.x + 6.f, p.y + rowH * 0.5f), 3.5f, rarityCol);

	float nameStartX = p.x + 17.f;
	if (def.iconSprite[0] && ImGui::IsRectVisible(bb.Min, bb.Max)) {
		auto icon = IconSprites::Get(def.iconSprite);
		if (icon.srv) {
			constexpr float iconSz = 18.f;
			float iconX = p.x + 16.f;
			float iconY = p.y + (rowH - iconSz) * 0.5f;
			dl->AddImage(reinterpret_cast<ImTextureID>(icon.srv),
				ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
			nameStartX = iconX + iconSz + 5.f;
		}
	}

	const char* shownName = item.customName.empty() ? def.displayName : item.customName.c_str();
	dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(priceRightX - priceW - 6.f, p.y + rowH), true);
	dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
		ImVec2(nameStartX, RowTextY(p.y, rowH, font::inter_medium_widget)),
		C(item.visible ? kText : kTextDim), shownName);
	{
		const float nameW = TextW(font::inter_medium_widget, shownName);
		const char* catName = LootFilter::CategoryNames[(int)def.category];
		dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
			ImVec2(nameStartX + nameW + 8.f, RowTextY(p.y, rowH, font::inter_medium_small)),
			C(kTextFaint), catName);
	}
	dl->PopClipRect();

	ImGui::PopID();
	ImGui::PopID();
}

void DrawWorldCatalog()
{
	auto& lf = ConfigInstance.LootFilter;
	if (lf.Items.empty()) lf.InitDefaults();

	static char s_search[128] = "";
	static bool s_searchInit = false;
	if (!s_searchInit) {
		std::strncpy(s_search, lf.SearchQuery.c_str(), sizeof(s_search) - 1);
		s_searchInit = true;
	}

	const float fullW = ImGui::GetContentRegionAvail().x;
	BeginCard("Loot Catalog", IconKind::List, fullW, kAvailH, false, "Scrollable list", true);

	// Toolbar row 1: Category, Search, Sort, Price, Authority
	static const std::array<const char*, (std::size_t)LootFilter::Category::COUNT + 1> kCatLabels = [] {
		std::array<const char*, (std::size_t)LootFilter::Category::COUNT + 1> a{};
		a[0] = "All Items";
		for (std::size_t i = 1; i < a.size(); ++i) a[i] = LootFilter::CategoryNames[i - 1];
		return a;
	}();
	static const char* kSortLabels[] = { "Name > Rarity", "Rarity > Name", "Price High-Low" };
	static const char* kPriceLabels[] = { "Sell price", "Buy price", "Unlock price" };
	static const char* kAuthLabels[] = { "Auth 1", "Auth 2", "Auth 3", "Auth 4", "Auth 5" };

	const float avail = ImGui::GetContentRegionAvail().x;
	const float cGap = 8.f;
	const float wCat = 148.f, wSort = 128.f, wPrice = 104.f, wAuth = 78.f;
	const float wSearch = ImMax(110.f, avail - wCat - wSort - wPrice - wAuth - cGap * 4.f);

	ImGui::SetNextItemWidth(wCat);
	if (ImGui::Combo("##lf_cat", &lf.SelectedCategory, kCatLabels.data(), (int)kCatLabels.size())) {}
	ImGui::SameLine(0.f, cGap);
	ImGui::SetNextItemWidth(wSearch);
	if (ImGui::InputTextWithHint("##lf_search", "Search items...", s_search, sizeof(s_search)))
		lf.SearchQuery = s_search;
	ImGui::SameLine(0.f, cGap);
	ImGui::SetNextItemWidth(wSort);
	ImGui::Combo("##lf_sort", &lf.SortMode, kSortLabels, IM_ARRAYSIZE(kSortLabels));
	ImGui::SameLine(0.f, cGap);
	ImGui::SetNextItemWidth(wPrice);
	if (ImGui::Combo("##lf_price", &lf.PriceMode, kPriceLabels, IM_ARRAYSIZE(kPriceLabels))) g_dirty = true;
	ImGui::SameLine(0.f, cGap);
	ImGui::SetNextItemWidth(wAuth);
	int authIdx = std::clamp(lf.AuthorityLevel - 1, 0, 4);
	if (ImGui::Combo("##lf_auth", &authIdx, kAuthLabels, IM_ARRAYSIZE(kAuthLabels))) {
		lf.AuthorityLevel = authIdx + 1;
		g_dirty = true;
	}

	auto inCategory = [&](const LootFilter::ItemDef& def) {
		return lf.SelectedCategory <= 0 || static_cast<int>(def.category) == lf.SelectedCategory - 1;
	};

	std::vector<const LootFilter::ItemDef*> shown;
	shown.reserve(256);
	int enabledCount = 0;
	for (const auto& def : LootFilter::kItemCatalog) {
		if (!inCategory(def)) continue;
		if (s_search[0] && !CaseContains(def.displayName, s_search)) continue;
		shown.push_back(&def);
		if (lf.IsItemVisible(def.id)) ++enabledCount;
	}

	std::sort(shown.begin(), shown.end(), [&](const LootFilter::ItemDef* a, const LootFilter::ItemDef* b) {
		switch (std::clamp(lf.SortMode, 0, 2)) {
		case 1:
			if (a->rarity != b->rarity) return a->rarity < b->rarity;
			return std::string_view(a->displayName) < std::string_view(b->displayName);
		case 2:
			if (a->price != b->price) return a->price > b->price;
			return std::string_view(a->displayName) < std::string_view(b->displayName);
		default: {
			const int cmp = std::string_view(a->displayName).compare(std::string_view(b->displayName));
			if (cmp != 0) return cmp < 0;
			return a->rarity < b->rarity;
		}
		}
	});

	if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
		for (const auto* def : shown) s_lfSelected.insert(def->id);
		s_lfAnchor = shown.empty() ? std::string() : shown.front()->id;
	}

	// Bulk actions strip (neutral buttons, properly disabled when all shown/hidden)
	const bool allShown = !shown.empty() && enabledCount == (int)shown.size();
	const bool allHidden = !shown.empty() && enabledCount == 0;
	const bool hasSel = !s_lfSelected.empty();

	ImGui::BeginDisabled(allShown);
	if (InlineButton("Show Category", 108.f)) {
		for (const auto* def : shown) lf.GetItemMut(def->id).visible = true;
		g_dirty = true;
	}
	ImGui::EndDisabled();
	ImGui::SameLine(0.f, 6.f);
	ImGui::BeginDisabled(allHidden);
	if (InlineButton("Hide Category", 104.f)) {
		for (const auto* def : shown) lf.GetItemMut(def->id).visible = false;
		g_dirty = true;
	}
	ImGui::EndDisabled();
	ImGui::SameLine(0.f, 6.f);
	ImGui::BeginDisabled(!hasSel);
	if (InlineButton("Show Selected", 104.f)) {
		for (const auto& id : s_lfSelected) lf.GetItemMut(id.c_str()).visible = true;
		g_dirty = true;
	}
	ImGui::SameLine(0.f, 6.f);
	if (InlineButton("Hide Selected", 100.f)) {
		for (const auto& id : s_lfSelected) lf.GetItemMut(id.c_str()).visible = false;
		g_dirty = true;
	}
	ImGui::SameLine(0.f, 6.f);
	if (InlineButton("Clear", 56.f)) { s_lfSelected.clear(); s_lfAnchor.clear(); }
	ImGui::EndDisabled();

	{
		char statsBuf[64];
		std::snprintf(statsBuf, sizeof(statsBuf), "%d shown | %d enabled | %d sel",
			(int)shown.size(), enabledCount, (int)s_lfSelected.size());
		const ImVec2 sp = ImGui::GetCursorScreenPos();
		TextRight(font::inter_medium_small, sp.x + ImGui::GetContentRegionAvail().x,
			sp.y - 21.f, C(kTextFaint), statsBuf);
	}

	// Column headers matching LootItemRow fixed columns
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 hp = ImGui::GetCursorScreenPos();
		const float hw = ImGui::GetContentRegionAvail().x - 10.f;
		constexpr float toggleW = 32.f, editW = 54.f, distW = 62.f, gap = 8.f;
		const float editX = hp.x + hw - toggleW - gap - editW;
		const float distX = editX - gap - distW;
		const float priceR = distX - gap;

		dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize * 0.9f, ImVec2(hp.x + 16.f, hp.y), C(kTextFaint), "ITEM");
		TextRight(font::inter_medium_small, priceR, hp.y, C(kTextFaint), "PRICE");
		dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize * 0.9f, ImVec2(distX + 12.f, hp.y), C(kTextFaint), "RANGE");
		dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize * 0.9f, ImVec2(editX + 6.f, hp.y), C(kTextFaint), "DISPLAY");
		TextRight(font::inter_medium_small, hp.x + hw, hp.y, C(kTextFaint), "SHOW");
		dl->AddLine(ImVec2(hp.x, hp.y + 16.f), ImVec2(hp.x + hw + 10.f, hp.y + 16.f), C(kStrokeSoft), 1.f);
		ImGui::Dummy(ImVec2(0.f, 18.f));
	}

	// Scrollable catalog list
	const float listH = ImMax(80.f, ImGui::GetContentRegionAvail().y);
	ImGui::BeginChild("##lf_list", ImVec2(0.f, listH), ImGuiChildFlags_None, ImGuiWindowFlags_NoSavedSettings);
	if (shown.empty()) {
		Note("No items match the current filter.");
	} else {
		ImGuiListClipper clipper;
		clipper.Begin((int)shown.size(), 26.f + ImGui::GetStyle().ItemSpacing.y);
		while (clipper.Step()) {
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
				LootItemRow(*shown[i], lf.GetItemMut(shown[i]->id));
		}
	}
	ImGui::EndChild();

	if (!s_lfRangeTo.empty()) {
		int a = -1, b = -1;
		for (int i = 0; i < (int)shown.size(); ++i) {
			if (shown[i]->id == s_lfAnchor) a = i;
			if (shown[i]->id == s_lfRangeTo) b = i;
		}
		if (a >= 0 && b >= 0) {
			if (!s_lfRangeAdd) s_lfSelected.clear();
			const int lo = (std::min)(a, b), hi = (std::max)(a, b);
			for (int i = lo; i <= hi; ++i) s_lfSelected.insert(shown[i]->id);
		}
		s_lfRangeTo.clear();
	}

	EndCard();
}

// ===========================================================================
// TAB 3: RADAR
// ===========================================================================
void DrawRadar2DWeb()
{
	ImGui::BeginGroup();
	{
		BeginCard("2D Radar", IconKind::Radar, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Enable Radar", &ConfigInstance.Radar.Enabled);
		ToggleRow("Show Players", &ConfigInstance.Radar.ShowPlayers);
		ToggleRow("Show Vehicles", &ConfigInstance.Radar.ShowVehicles);
		ToggleRow("Show Loot", &ConfigInstance.Radar.ShowLoot);
		ToggleRow("Rotate With Camera", &ConfigInstance.Radar.RotateWithCamera);
		ToggleRow("Full Map Window", &ConfigInstance.Radar.PopoutWindow);

		SectionLabel("Display");
		SliderFloatRow("Radar Size", &ConfigInstance.Radar.Size, 100.f, 1000.f, " px");
		SliderFloatRow("Radar Zoom", &ConfigInstance.Radar.Zoom, 0.1f, 10.f, "x");
		SliderFloatRow("Radar Opacity", &ConfigInstance.Radar.Opacity, 0.0f, 1.f);
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Web Radar", IconKind::World, kCardW, 0.f, false, nullptr, true);
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
				WebRadar::Start(ConfigInstance.Radar.WebRadarPort);
			if (ButtonRow(WebRadar::GetUrl().c_str(), "Pop Out Web Radar", 142.f, true))
				WebRadar::OpenInBrowser();
		}
		EndCard();

		ImGui::Dummy(ImVec2(0.f, kCardGap - 7.f));
		BeginCard("Map Selection & Bounds", IconKind::Map, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Auto Map Select", &ConfigInstance.Radar.AutoMapSelect);
		const auto& mapFiles = Radar::GetMapFiles();
		if (!mapFiles.empty())
		{
			std::vector<const char*> ptrs(mapFiles.size());
			for (size_t i = 0; i < mapFiles.size(); ++i) ptrs[i] = mapFiles[i].c_str();
			int curMap = Radar::GetCurrentMapIndex();
			if (ComboRow("Active Map", &curMap, ptrs.data(), (int)ptrs.size()))
				Radar::SetMap(curMap);
		}
		SliderFloatRow("Min X", &ConfigInstance.Radar.WorldMinX, -5000.f, 5000.f);
		SliderFloatRow("Max X", &ConfigInstance.Radar.WorldMaxX, -5000.f, 5000.f);
		SliderFloatRow("Min Z", &ConfigInstance.Radar.WorldMinZ, -5000.f, 5000.f);
		SliderFloatRow("Max Z", &ConfigInstance.Radar.WorldMaxZ, -5000.f, 5000.f);
		if (ButtonRow("", "Calibrate to Map", 136.f))
			Radar::CalibrateBoundsForMap(Radar::GetCurrentMapName());
		EndCard();
	}
	ImGui::EndGroup();
}

void DrawRadarInGameMap()
{
	static const char* kMapRarities[] = { "All", "Uncommon+", "Rare+", "Epic+", "Legendary+", "Mythical" };

	ImGui::BeginGroup();
	{
		BeginCard("Overlay Modes", IconKind::Map, kCardW);
		ToggleRow("Top-Left Minimap", &ConfigInstance.Radar.InGameMinimap);
		ToggleRow("Fullscreen Map (M)", &ConfigInstance.Radar.InGameFullMap);
		ToggleRow("Hide 3D ESP on Full Map", &ConfigInstance.Radar.Hide3DEspOnFullMap);
		SliderFloatRow("Icon Scale", &ConfigInstance.Radar.InGameMapScale, 0.5f, 2.0f, "x");
		SectionLabel("Live Map State");
		if (const auto snap = RobRunReader::GetSnapshot()) {
			if (snap->inGameMap.valid) {
				const auto& v = snap->inGameMap.fullscreenOpen ? snap->inGameMap.fullscreen : snap->inGameMap.minimap;
				Note("%s | camSize=%.0fm | rect=%s",
					snap->inGameMap.fullscreenOpen ? "Full Map (M)" : "Minimap",
					v.cameraSize, v.rectValid ? "OK" : "pending");
			} else {
				Note("Waiting for in-game map state...");
			}
		} else {
			Note("Waiting for game snapshot...");
		}
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Map Markers", IconKind::Radar, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Player Arrows", &ConfigInstance.Radar.InGameMapPlayers);
		ToggleRow("Vehicle Icons", &ConfigInstance.Radar.InGameMapVehicles);
		ToggleRow("Player Names (Full Map)", &ConfigInstance.Radar.InGameMapPlayerNames);
		ToggleRow("Dropped Bags", &ConfigInstance.Radar.InGameMapDroppedBags);
		ToggleRow("Valuables & Loot", &ConfigInstance.Radar.InGameMapValuables);
		if (ConfigInstance.Radar.InGameMapValuables)
			ComboRow("Min Loot Rarity", &ConfigInstance.Radar.InGameMapMinRarity, kMapRarities, IM_ARRAYSIZE(kMapRarities));
		ToggleRow("Cargo", &ConfigInstance.Radar.InGameMapCargo);
		if (ConfigInstance.Radar.InGameMapCargo)
			ComboRow("Min Cargo Rarity", &ConfigInstance.Radar.InGameMapCargoMinRarity, kMapRarities, IM_ARRAYSIZE(kMapRarities));
		EndCard();
	}
	ImGui::EndGroup();
}

// ===========================================================================
// TAB 4: MISC
// ===========================================================================
void DrawMiscGameplay()
{
	static const char* kBattleModeActions[] = { "Hold", "Toggle" };
	static std::vector<RobRunWorld::ProbeResult> probeResults;
	static bool probed = false;

	ImGui::BeginGroup();
	{
		BeginCard("Gameplay", IconKind::Sliders, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Camera Zoom", &ConfigInstance.Overlay.CustomCameraDistance, true);
		if (ConfigInstance.Overlay.CustomCameraDistance)
			SliderFloatRow("Camera Distance", &ConfigInstance.Overlay.CameraDistance, 1.0f, 15.0f, " m");
		ToggleRow("FOV Changer", &ConfigInstance.Overlay.FovChanger, true);
		if (ConfigInstance.Overlay.FovChanger)
			SliderFloatRow("Camera FOV", &ConfigInstance.Overlay.CameraFov, 30.0f, 140.0f, " deg");
		ToggleRow("Instant Loot", &ConfigInstance.World.InstantLoot, true);
		ToggleRow("Instant Safe & Door Unlock", &ConfigInstance.World.InstantUnlock, true);
		ToggleRow("Ignore Knock (Local)", &ConfigInstance.Aim.IgnoreKnock, true);
		ToggleRow("Hide Default Crosshair", &ConfigInstance.Aim.HideCrosshair);
		EndCard();

		ImGui::Dummy(ImVec2(0.f, kCardGap - 7.f));
		BeginCard("Battle Mode", IconKind::Shield, kCardW, 0.f, false, nullptr, true);
		ToggleRow("Battle Mode", &ConfigInstance.Overlay.BattleMode);
		ComboRow("Battle Mode Key", &ConfigInstance.Overlay.BattleModeKey, Aim::KeyNames(), Aim::KeyCount());
		ComboRow("Battle Mode Action", &ConfigInstance.Overlay.BattleModeBehavior,
			kBattleModeActions, IM_ARRAYSIZE(kBattleModeActions));
		if (Aim::IsAlwaysKey(ConfigInstance.Overlay.BattleModeKey))
			Note("Choose a specific key to activate battle mode.");
		else
			Note("Suppresses World ESP, Radar & Money HUD so only Player ESP draws.");
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Diagnostics & Tools", IconKind::Cpu, kCardW, 0.f, false, nullptr, true);
		if (ToggleRow("Read Item Catalog", &g_readCatalog, false, false))
			RobRunReader::SetReadCatalogEnabled(g_readCatalog);

		const auto snapshot = RobRunReader::GetSnapshot();
		if (snapshot && snapshot->inventory && snapshot->inventory->valid) {
			const auto& inv = *snapshot->inventory;
			Note("Inventory: %d x %d | Slots %zu | Catalog %zu",
				inv.columns, inv.activeLines, inv.slots.size(), inv.catalog.size());
		} else {
			Note("No inventory snapshot yet (enable while in a raid).");
		}

		const std::uintptr_t worldPtr = (snapshot && snapshot->world) ? snapshot->world->worldPtr : 0;
		const float half = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
		if (InlineButton("Dump Item Prices", half, true))
			RunDumpItemPrices();
		ImGui::SameLine(0.f, 8.f);
		ImGui::BeginDisabled(worldPtr == 0);
		if (InlineButton("Probe Money Models", half)) {
			probeResults = RobRunWorld::ProbeMoneyModels(worldPtr);
			probed = true;
			Overlay::Notify(probeResults.empty() ? "No money models found" : "Money models probed",
				IM_COL32(70, 194, 120, 255), 2.f);
		}
		ImGui::EndDisabled();

		if (!s_dumpStatusNote.empty())
			Note("%s", s_dumpStatusNote.c_str());
		if (probed)
			Note("Probe: %zu money model(s) found (world=0x%llX)",
				probeResults.size(), (unsigned long long)worldPtr);
		EndCard();

		ImGui::Dummy(ImVec2(0.f, kCardGap - 7.f));
		BeginCard("Session Telemetry", IconKind::List, kCardW, 0.f, false, nullptr, true);
		if (snapshot) {
			const auto world = snapshot->world ? *snapshot->world : RobRunWorld::State{};
			Note("Entities %zu (C:%d L:%d V:%d G:%d)", snapshot->entities.size(),
				world.containers, world.loots, world.vehicles, world.grenades);
			Note("Skeletons %u | Aim poses %u | Retries %u",
				snapshot->skeletonCount, snapshot->poseCount, snapshot->poseFailures);
			Note("Bones %u inv %u | Scatter ok %u fail %u",
				snapshot->boneTotal, snapshot->boneInvalid,
				snapshot->poseScatterOk, snapshot->poseScatterFail);
			Note("Read failures %u | Overlay %.0f FPS", snapshot->failureCount, ImGui::GetIO().Framerate);
		} else {
			Note("No snapshot yet. Waiting for the game process.");
			Note("Overlay %.0f FPS", ImGui::GetIO().Framerate);
		}
		EndCard();
	}
	ImGui::EndGroup();
}

void DrawMiscSkins()
{
	const SkinChanger::State state = SkinChanger::GetState();
	auto& skin = ConfigInstance.Skin;

	ImGui::BeginGroup();
	{
		BeginCard("Skin Changer", IconKind::Palette, kCardW);
		ToggleRow("Enable Skin Changer", &skin.Enabled, true);
		if (state.inMatch)
			Note("Character: %s", state.female ? "Female" : "Male");
		else
			Note("Status: %s", state.status.empty() ? "-" : state.status.c_str());
		Note("Client-side only. Saved per Male/Female and restored when off or closed.");
		EndCard();

		ImGui::Dummy(ImVec2(0.f, kCardGap - 7.f));
		BeginCard("Cosmetics", IconKind::Shield, kCardW, 0.f, false, nullptr, true);
		if (!state.inMatch) {
			Note("Join a match to pick cosmetics.");
		} else {
			static std::vector<std::string> labels[SkinChanger::kSlotCount];
			static std::vector<const char*> items[SkinChanger::kSlotCount];
			for (int slot = 0; slot < SkinChanger::kSlotCount; ++slot)
			{
				const auto& s = state.slots[slot];
				if (!s.supported) {
					Note("%s: %s", SkinChanger::SlotName(slot), s.status.c_str());
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
					(int)items[slot].size(), 190.f, true))
				{
					SkinChanger::SetWanted(slot, state.female,
						selected > 0 ? s.options[selected - 1] : std::string());
				}
			}
		}
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Weapon Skins", IconKind::Sliders, kCardW, kAvailH, true, "Scrollable", true);
		if (!state.inMatch) {
			Note("Join a match to pick weapon skins.");
		} else if (!state.weaponsSupported) {
			Note("Weapon skin data not found (offsets changed?).");
		} else {
			if (!state.heldWeapon.empty())
				Note("Holding: %s | Real: %s", state.heldWeapon.c_str(), state.heldSkin.c_str());
			Note("Switch weapons once if a chosen skin does not apply immediately.");

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
				ImGui::PushID((int)w);
				if (ComboRow(weapon.name.c_str(), &selected, items[w].data(),
					(int)items[w].size(), 180.f, true))
				{
					SkinChanger::SetWantedWeaponSkin(weapon.key,
						selected > 0 ? weapon.skins[selected - 1] : std::string());
				}
				ImGui::PopID();
			}
		}
		EndCard();
	}
	ImGui::EndGroup();
}

static std::string FormatDistance(float meters)
{
	if (meters < 0.01f) return "0 m";
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.1f m", meters);
	return buf;
}

void DrawMiscServer()
{
	const float fullW = ImGui::GetContentRegionAvail().x;
	const auto snapshot = RobRunReader::GetSnapshot();
	if (!snapshot) {
		BeginCard("Players On Map", IconKind::Users, fullW, kAvailH, false);
		Note("No reader snapshot available.");
		EndCard();
		return;
	}

	static char serverSearch[64] = "";
	static int currentSort = 0;
	static bool filterAliveOnly = false;
	static bool filterExcludeParty = false;
	static bool filterExcludeSelf = false;

	int totalCount = 0, aliveCount = 0, deadCount = 0, partyCount = 0;
	Vector3 effectiveLocalPos = snapshot->localPosition;
	if (effectiveLocalPos.IsZero() && snapshot->localEntityId >= 0) {
		for (const auto& ent : snapshot->entities) {
			if (ent.entityId == snapshot->localEntityId && !ent.position.IsZero()) {
				effectiveLocalPos = ent.position;
				break;
			}
		}
	}

	struct PlayerItem {
		const RobRunReader::EntityData* ent = nullptr;
		float distance = 0.f;
		std::string name, clan, weapon;
		bool isLocal = false;
	};
	std::vector<PlayerItem> items;
	items.reserve(snapshot->entities.size());

	for (const auto& ent : snapshot->entities)
	{
		totalCount++;
		if (ent.isDead) deadCount++; else aliveCount++;
		if (ent.isPartyMember) partyCount++;

		PlayerItem item;
		item.ent = &ent;
		item.isLocal = (ent.entityId == snapshot->localEntityId);
		if (item.isLocal) item.distance = 0.f;
		else if (!effectiveLocalPos.IsZero() && !ent.position.IsZero()) {
			const float dx = ent.position.x - effectiveLocalPos.x;
			const float dy = ent.position.y - effectiveLocalPos.y;
			const float dz = ent.position.z - effectiveLocalPos.z;
			item.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		} else item.distance = 9999.f;

		item.name = RobRunMemory::WideToUtf8(ent.nickname);
		if (item.name.empty())
			item.name = item.isLocal ? "Local Player" : ("Player #" + std::to_string(ent.entityId));
		item.clan = RobRunMemory::WideToUtf8(ent.clanTag);
		item.weapon = RobRunMemory::WideToUtf8(ent.weaponName);
		if (item.weapon.empty()) item.weapon = "None";

		if (filterAliveOnly && ent.isDead) continue;
		if (filterExcludeParty && ent.isPartyMember && !item.isLocal) continue;
		if (filterExcludeSelf && item.isLocal) continue;
		if (serverSearch[0]) {
			std::string hay = item.name + " " + item.clan + " " + ent.roleName + " " + item.weapon;
			if (!CaseContains(hay.c_str(), serverSearch)) continue;
		}
		items.push_back(std::move(item));
	}

	static const char* sortModes[] = { "Distance (Nearest)", "Health (Lowest)", "Name (A-Z)" };
	switch (currentSort) {
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

	char titleBuf[64], metaBuf[96];
	std::snprintf(titleBuf, sizeof(titleBuf), "Players On Map (%d / %d)###srv_roster", (int)items.size(), totalCount);
	if (snapshot->spectatorAlert || snapshot->observerCount > 0)
		std::snprintf(metaBuf, sizeof(metaBuf), "%d alive | %d dead | %d party | %d spec",
			aliveCount, deadCount, partyCount, snapshot->observerCount);
	else
		std::snprintf(metaBuf, sizeof(metaBuf), "%d alive | %d dead | %d party",
			aliveCount, deadCount, partyCount);

	BeginCard(titleBuf, IconKind::Users, fullW, kAvailH, false, metaBuf, true);

	// Compact filter toolbar
	ImGui::SetNextItemWidth(200.f);
	ImGui::InputTextWithHint("##srv_search", "Filter name, clan, weapon...", serverSearch, sizeof(serverSearch));
	ImGui::SameLine(0.f, 8.f);
	ImGui::SetNextItemWidth(155.f);
	ImGui::Combo("##srv_sort", &currentSort, sortModes, IM_ARRAYSIZE(sortModes));

	// Inline pill filters on the right of the toolbar
	auto inlinePill = [&](const char* lbl, bool* val) {
		ImGui::SameLine(0.f, 12.f);
		ImGuiWindow* w = ImGui::GetCurrentWindow();
		const ImGuiID id = w->GetID(lbl);
		const float lw = TextW(font::inter_medium_small, lbl);
		const ImVec2 p = w->DC.CursorPos;
		const ImRect bb(p, p + ImVec2(lw + 38.f, 24.f));
		ImGui::ItemSize(bb);
		if (ImGui::ItemAdd(bb, id)) {
			bool h = false, hl = false;
			if (ImGui::ButtonBehavior(bb, id, &h, &hl)) *val = !*val;
			if (h) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			ImDrawList* dl = w->DrawList;
			dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
				ImVec2(p.x, RowTextY(p.y, 24.f, font::inter_medium_small)), C(*val || h ? kText : kTextDim), lbl);
			const ImRect tr(ImVec2(p.x + lw + 6.f, p.y + 4.f), ImVec2(p.x + lw + 36.f, p.y + 20.f));
			DrawPillSwitch(dl, tr, *val ? 1.f : 0.f);
		}
	};
	inlinePill("Alive", &filterAliveOnly);
	inlinePill("Hide Party", &filterExcludeParty);
	inlinePill("Hide Self", &filterExcludeSelf);

	ImGui::Separator();

	const float listH = ImMax(80.f, ImGui::GetContentRegionAvail().y);
	ImGui::BeginChild("##srv_list", ImVec2(0.f, listH), ImGuiChildFlags_None, ImGuiWindowFlags_NoSavedSettings);
	if (items.empty()) {
		if (totalCount == 0)
			Note("No player entities detected on the map. Join a match or attach DMA reader.");
		else
			Note("No players match the current filter.");
	} else {
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		ImDrawList* dl = window->DrawList;
		for (size_t idx = 0; idx < items.size(); ++idx)
		{
			const auto& item = items[idx];
			const auto& ent = *item.ent;
			constexpr float rowH = 46.f;
			const ImVec2 p = window->DC.CursorPos;
			const float w = ImGui::GetContentRegionAvail().x;
			const ImRect bb(p, p + ImVec2(w, rowH));
			ImGui::ItemSize(bb);
			const ImGuiID rowId = window->GetID((int)idx + 2000);
			if (!ImGui::ItemAdd(bb, rowId)) continue;

			const bool hov = ImGui::IsMouseHoveringRect(bb.Min, bb.Max);
			dl->AddRectFilled(bb.Min, bb.Max, C(hov ? kElevated : kBg), 6.f);
			if (item.isLocal)
				dl->AddRect(bb.Min, bb.Max, C(kAccent, 0.55f), 6.f, 0, 1.f);
			else if (ent.isAimingAtLocal)
				dl->AddRect(bb.Min, bb.Max, C(kRed, 0.55f), 6.f, 0, 1.f);
			else
				dl->AddRect(bb.Min, bb.Max, C(kStrokeSoft), 6.f, 0, 1.f);

			const float leftX = bb.Min.x + 10.f;
			const float rightX = bb.Max.x - 10.f;
			const float line1Y = bb.Min.y + 6.f;
			const float line2Y = bb.Min.y + 26.f;
			float curX = leftX;

			std::string roleStr = ent.roleName.empty() ? "UNKNOWN" : ent.roleName;
			ImVec4 roleColor = kTextDim;
			if (CaseContains(roleStr.c_str(), "crim")) roleColor = kRed;
			else if (CaseContains(roleStr.c_str(), "pol") || CaseContains(roleStr.c_str(), "cop")) roleColor = ImVec4(79 / 255.f, 148 / 255.f, 240 / 255.f, 1.f);
			else if (CaseContains(roleStr.c_str(), "civ")) roleColor = kGreen;

			auto drawBadge = [&](const char* txt, const ImVec4& col) {
				const float tw = TextW(font::inter_medium_small, txt) + 8.f;
				dl->AddRectFilled(ImVec2(curX, line1Y), ImVec2(curX + tw, line1Y + 15.f), C(col, 0.16f), 3.f);
				dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize * 0.9f,
					ImVec2(curX + 4.f, line1Y + 1.f), C(col), txt);
				curX += tw + 5.f;
			};

			drawBadge(roleStr.c_str(), roleColor);

			dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
				ImVec2(curX, line1Y - 1.f), C(kText), item.name.c_str());
			curX += TextW(font::inter_medium_widget, item.name.c_str()) + 5.f;

			if (!item.clan.empty()) {
				const std::string cStr = "<" + item.clan + ">";
				dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
					ImVec2(curX, line1Y + 1.f), C(kAccentBright), cStr.c_str());
				curX += TextW(font::inter_medium_small, cStr.c_str()) + 6.f;
			}
			if (item.isLocal) drawBadge("YOU", kAccent);
			else if (ent.isPartyMember) drawBadge("PARTY", kGreen);
			if (ent.isDead) drawBadge("DEAD", kRed);
			else if (ent.isKnockedDown || ent.isKnockedOut) drawBadge("KNOCKED", kAmber);
			if (ent.isAimingAtLocal) drawBadge("AIMING AT YOU", kRed);

			const bool isTrusted = ConfigInstance.Aim.IsTrusted(ent.publicId);
			if (isTrusted) drawBadge("TRUSTED", kTeal);

			// Right side: Trust toggle + Distance
			const std::string distStr = FormatDistance(item.distance);
			const float distW = TextW(font::inter_medium_small, distStr.c_str());
			ImU32 distCol = C(kTextDim);
			if (!item.isLocal) {
				if (item.distance < 50.f) distCol = C(kRed);
				else if (item.distance < 120.f) distCol = C(kAmber);
			}
			dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
				ImVec2(rightX - distW, line1Y + 1.f), distCol, distStr.c_str());

			if (!item.isLocal && ent.publicId) {
				const char* btnLabel = isTrusted ? "Untrust" : "Trust";
				const float btnW = TextW(font::inter_medium_small, btnLabel) + 12.f;
				const float btnX = rightX - distW - btnW - 8.f;
				const ImRect btnRect(ImVec2(btnX, line1Y - 1.f), ImVec2(btnX + btnW, line1Y + 16.f));
				const bool btnHov = ImGui::IsMouseHoveringRect(btnRect.Min, btnRect.Max);
				if (btnHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
				dl->AddRectFilled(btnRect.Min, btnRect.Max, C(btnHov ? kElevated : kControl), 4.f);
				dl->AddRect(btnRect.Min, btnRect.Max, C(isTrusted ? kTeal : kStroke), 4.f);
				dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize * 0.92f,
					ImVec2(btnRect.Min.x + 6.f, btnRect.Min.y + 2.f), C(isTrusted ? kTeal : kTextMid), btnLabel);
				if (btnHov && ImGui::IsMouseClicked(0)) {
					ConfigInstance.Aim.SetTrusted(ent.publicId, !isTrusted);
					g_dirty = true;
				}
			}

			// Line 2: Health, Armor, Weapon
			char hpBuf[64];
			if (ent.isDead) std::snprintf(hpBuf, sizeof(hpBuf), "DEAD");
			else if (ent.healthKnown && ent.health >= 0.f) {
				if (ent.hasArmor && ent.armor > 0.f)
					std::snprintf(hpBuf, sizeof(hpBuf), "HP %.0f/%.0f | AP %.0f",
						ent.health, ent.maxHealth > 0.f ? ent.maxHealth : 100.f, ent.armor);
				else
					std::snprintf(hpBuf, sizeof(hpBuf), "HP %.0f/%.0f",
						ent.health, ent.maxHealth > 0.f ? ent.maxHealth : 100.f);
			} else std::snprintf(hpBuf, sizeof(hpBuf), "HP ?");

			ImU32 hpCol = (ent.isDead || (ent.healthKnown && ent.health < 40.f)) ? C(kRed) : C(kTextDim);
			dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize, ImVec2(leftX, line2Y), hpCol, hpBuf);
			float l2X = leftX + TextW(font::inter_medium_small, hpBuf) + 14.f;
			const std::string wepStr = "Weapon: " + item.weapon;
			dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize, ImVec2(l2X, line2Y), C(kTextFaint), wepStr.c_str());
			if (ent.inVehicle) {
				l2X += TextW(font::inter_medium_small, wepStr.c_str()) + 10.f;
				dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize, ImVec2(l2X, line2Y), C(kAccentBright), "[Vehicle]");
			}
		}
	}
	ImGui::EndChild();
	EndCard();
}

// ===========================================================================
// TAB 5: SETTINGS
// ===========================================================================
void DrawSettingsOverlay()
{
	static const char* panicKeys[] = { "Off", "End", "F8", "F9", "F10", "F11", "F12" };
	static const char* reattachKeys[] = {
		"Off", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
		"Insert", "Home", "End", "Delete", "Page Up", "Page Down"
	};

	ImGui::BeginGroup();
	{
		BeginCard("Overlay & Display", IconKind::Settings, kCardW, 0.f, false, nullptr, true);
		bool transparent = Overlay::Transparent();
		if (ToggleRow("Transparent", &transparent)) Overlay::SetTransparent(transparent);
		bool clickThrough = Overlay::ClickThrough();
		if (ToggleRow("Click-Through When Closed", &clickThrough)) Overlay::SetClickThrough(clickThrough);
		bool onTop = Overlay::AlwaysOnTop();
		if (ToggleRow("Always On Top", &onTop)) Overlay::SetAlwaysOnTop(onTop);
		ToggleRow("High Contrast Menu", &ConfigInstance.Overlay.HighContrastMenu);
		ColorRow("Accent Colour", ConfigInstance.Overlay.AccentColour);

		SectionLabel("HUD & Performance");
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
		const int monCount = Overlay::MonitorCount();
		if (monCount > 0)
		{
			static char labels[8][64];
			static const char* ptrs[8];
			const int count = ImMin(monCount, 8);
			int current = Overlay::MonitorIndex();
			for (int i = 0; i < count; i++) {
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
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Hotkeys & Safety", IconKind::Shield, kCardW);
		ComboRow("Menu Key", &ConfigInstance.Overlay.MenuKey, kMenuKeyLabels, IM_ARRAYSIZE(kMenuKeyLabels));
		ToggleRow("Panic Mode", &ConfigInstance.Overlay.PanicMode);
		ComboRow("Panic Key", &ConfigInstance.Overlay.PanicKey, panicKeys, IM_ARRAYSIZE(panicKeys));
		ComboRow("Re-attach Key", &ConfigInstance.Overlay.ReattachKey, reattachKeys, IM_ARRAYSIZE(reattachKeys));
		Note("Panic instantly blanks the overlay and halts targeting.");
		EndCard();

		ImGui::Dummy(ImVec2(0.f, kCardGap - 7.f));
		BeginCard("Backdrop (Fuser / 2PC)", IconKind::Visuals, kCardW);
		bool vis = Backdrop::IsVisible();
		if (ToggleRow("Show Backdrop", &vis, false, false)) {
			if (vis) Backdrop::Show(); else Backdrop::Hide();
		}
		bool fs = Backdrop::IsFullscreen();
		if (ToggleRow("Fullscreen", &fs, false, false))
			Backdrop::SetFullscreen(fs);

		static int bdW = 1920, bdH = 1080;
		static bool customRes = false;
		ToggleRow("Custom Resolution", &customRes, false, false);
		if (customRes) {
			const float inW = 86.f;
			ImGui::SetNextItemWidth(inW);
			ImGui::InputInt("##bdw", &bdW, 0);
			ImGui::SameLine(0.f, 6.f);
			ImGui::SetNextItemWidth(inW);
			ImGui::InputInt("##bdh", &bdH, 0);
			ImGui::SameLine(0.f, 8.f);
			if (InlineButton("Apply", ImGui::GetContentRegionAvail().x, true))
				Backdrop::SetResolution(bdW, bdH);
		} else if (ButtonRow("", "Reset To Native", 132.f)) {
			Backdrop::SetFullscreen(true);
		}
		EndCard();
	}
	ImGui::EndGroup();
}

void DrawSettingsConfigs(bool attached, const std::string& statusText)
{
	static std::vector<std::string> configs;
	static int selectedIdx = 0;
	static char newConfigName[64] = "";
	static char renameConfigName[64] = "";
	static bool initialized = false;
	static bool confirmDelete = false;

	auto refreshList = [&](const std::string& preferSelect) {
		configs = ConfigFile::ListConfigs();
		selectedIdx = 0;
		for (size_t i = 0; i < configs.size(); ++i) {
			if (configs[i] == preferSelect) { selectedIdx = (int)i; break; }
		}
		if (!configs.empty()) {
			std::strncpy(renameConfigName, configs[selectedIdx].c_str(), sizeof(renameConfigName) - 1);
			renameConfigName[sizeof(renameConfigName) - 1] = 0;
		}
	};

	if (!initialized) {
		refreshList(ConfigFile::GetActiveConfigName());
		initialized = true;
	}

	ImGui::BeginGroup();
	{
		BeginCard("Config Profiles", IconKind::File, kCardW);
		const std::string activeName = ConfigFile::GetActiveConfigName();
		if (!configs.empty()) {
			std::vector<std::string> dispLabels;
			std::vector<const char*> ptrs;
			dispLabels.reserve(configs.size());
			ptrs.reserve(configs.size());
			for (const auto& c : configs) {
				dispLabels.push_back(c == activeName ? (c + " (active)") : c);
			}
			for (const auto& d : dispLabels) ptrs.push_back(d.c_str());

			int sel = ImClamp(selectedIdx, 0, (int)configs.size() - 1);
			if (ComboRow("Profile", &sel, ptrs.data(), (int)ptrs.size(), 185.f, false, false)) {
				selectedIdx = sel;
				confirmDelete = false;
				std::strncpy(renameConfigName, configs[sel].c_str(), sizeof(renameConfigName) - 1);
				renameConfigName[sizeof(renameConfigName) - 1] = 0;
			}
		}

		const std::string selName = (selectedIdx >= 0 && selectedIdx < (int)configs.size())
			? configs[selectedIdx] : activeName;
		const float gap = 6.f;
		const float delW = 78.f;
		const float actW = (ImGui::GetContentRegionAvail().x - delW - gap * 2.f) * 0.5f;

		if (InlineButton("Save", actW, true)) {
			const bool ok = ConfigFile::Save(selName);
			refreshList(selName);
			confirmDelete = false;
			if (ok) g_dirty = false;
			Overlay::Notify(ok ? "Config saved" : "Config save failed",
				ok ? IM_COL32(70, 194, 120, 255) : IM_COL32(229, 83, 83, 255), 2.f);
		}
		ImGui::SameLine(0.f, gap);
		if (InlineButton("Load", actW)) {
			const bool ok = ConfigFile::Load(selName);
			refreshList(ConfigFile::GetActiveConfigName());
			confirmDelete = false;
			if (ok) {
				g_dirty = false;
				g_aimVarsSyncNeeded = true;
				SyncFontFromConfig();
			}
			Overlay::Notify(ok ? "Config loaded" : "Config load failed",
				ok ? IM_COL32(70, 194, 120, 255) : IM_COL32(229, 83, 83, 255), 2.f);
		}
		ImGui::SameLine(0.f, gap);
		ImGui::BeginDisabled(configs.size() <= 1);
		if (InlineButton(confirmDelete ? "Confirm?" : "Delete", delW, false, confirmDelete)) {
			if (!confirmDelete) {
				confirmDelete = true;
			} else {
				const bool ok = ConfigFile::DeleteConfig(selName);
				refreshList(ConfigFile::GetActiveConfigName());
				confirmDelete = false;
				if (ok) {
					g_dirty = false;
					g_aimVarsSyncNeeded = true;
					SyncFontFromConfig();
				}
				Overlay::Notify(ok ? "Config deleted" : "Delete failed",
					ok ? IM_COL32(70, 194, 120, 255) : IM_COL32(229, 83, 83, 255), 2.f);
			}
		}
		ImGui::EndDisabled();

		SectionLabel("Rename Selected");
		{
			const float btnW = 80.f;
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - 8.f);
			ImGui::InputTextWithHint("##rename_cfg", "New name...", renameConfigName, sizeof(renameConfigName));
			ImGui::SameLine(0.f, 8.f);
			if (InlineButton("Rename", btnW)) {
				if (renameConfigName[0] && selectedIdx >= 0 && selectedIdx < (int)configs.size()) {
					const std::string newName = renameConfigName;
					if (ConfigFile::RenameConfig(configs[selectedIdx], newName)) {
						refreshList(newName);
						confirmDelete = false;
						Overlay::Notify("Config renamed", IM_COL32(70, 194, 120, 255), 2.f);
					} else {
						Overlay::Notify("Rename failed", IM_COL32(229, 83, 83, 255), 2.f);
					}
				}
			}
		}

		SectionLabel("New Profile");
		{
			const float btnW = 80.f;
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - 8.f);
			ImGui::InputTextWithHint("##new_cfg", "Config name...", newConfigName, sizeof(newConfigName));
			ImGui::SameLine(0.f, 8.f);
			if (InlineButton("Create", btnW)) {
				if (newConfigName[0]) {
					const std::string created = newConfigName;
					if (ConfigFile::CreateConfig(created)) {
						refreshList(created);
						newConfigName[0] = 0;
						confirmDelete = false;
						g_dirty = false;
						Overlay::Notify("Config created", IM_COL32(70, 194, 120, 255), 2.f);
					} else {
						Overlay::Notify("Create failed", IM_COL32(229, 83, 83, 255), 2.f);
					}
				}
			}
		}

		Note("%s\\%s.json | Auto-saved on close.",
			ConfigFile::GetConfigDirectory().c_str(), ConfigFile::GetActiveConfigName().c_str());
		EndCard();
	}
	ImGui::EndGroup();

	if (!g_singleColumn) ImGui::SameLine(0.f, kGap);

	ImGui::BeginGroup();
	{
		BeginCard("Connection Status", IconKind::Cpu, kCardW);
		{
			const bool dmaReady = (TargetProcess.vHandle != nullptr);
			const int gamePid = attached ? TargetProcess.GetPID() : 0;
			const auto snapshot = RobRunReader::GetSnapshot();
			const bool hasLiveSnap = attached && snapshot && snapshot->battleEntities != 0 && !snapshot->entities.empty();
			Note("DMA: %s", dmaReady ? "Connected" : "Offline");
			if (attached && gamePid > 0)
				Note("Game: Attached (PID %d)", gamePid);
			else
				Note("Game: %s", statusText.c_str());
			if (hasLiveSnap)
				Note("Match: Active (%zu entities | %u skeletons | %u failures)",
					snapshot->entities.size(), snapshot->skeletonCount, snapshot->failureCount);
			else if (attached)
				Note("Match: Waiting for entities (menu / loading)");
			else
				Note("Match: Not attached");
			Note("Overlay: %.0f FPS", ImGui::GetIO().Framerate);
		}
		if (ButtonRow("", "Re-attach Game", 130.f, true))
			RequestReattach();
		EndCard();

		ImGui::Dummy(ImVec2(0.f, kCardGap - 7.f));
		BeginCard("About", IconKind::Settings, kCardW);
		const int mkIdx = ImClamp(ConfigInstance.Overlay.MenuKey, 0, (int)IM_ARRAYSIZE(kMenuKeyLabels) - 1);
		Note("ROB RUN External DMA Overlay");
		Note("Menu Key: %s", kMenuKeyLabels[mkIdx]);
		Note("External DMA reader - never injects into the game process. Rows marked MEM WRITE modify game memory over DMA.");
		EndCard();
	}
	ImGui::EndGroup();
}

// ===========================================================================
// CHROME: Top Bar, Sub-Tab Bar, Footer Bar
// ===========================================================================
void DrawTopBar(const ImVec2& pos, ImRect& outTabsRect, ImRect& outSearchRect, ImRect& outCloseRect)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();

	// Top bar bottom border & logo right divider
	dl->AddLine(pos + ImVec2(0.f, kTop), pos + ImVec2(kW, kTop), C(kStroke), 1.f);
	dl->AddLine(pos + ImVec2(kLogoW, 0.f), pos + ImVec2(kLogoW, kTop), C(kStroke), 1.f);

	// Logo glyph ("R") centered in left cell
	{
		const char* letter = "R";
		const float tw = TextW(font::inter_medium_big, letter);
		dl->AddText(font::inter_medium_big, font::inter_medium_big->FontSize,
			ImVec2(pos.x + (kLogoW - tw) * 0.5f, pos.y + (kTop - font::inter_medium_big->FontSize) * 0.5f - 1.f),
			C(kAccent), letter);
	}

	// Primary Top Tabs
	float tx = pos.x + kLogoW + 14.f;
	const float ty = pos.y + (kTop - 34.f) * 0.5f;
	const float tabsStartX = tx;
	for (int i = 0; i < kTabCount; ++i)
	{
		const float lw = TextW(font::inter_medium_widget, kTabNames[i]);
		const float bw = 11.f + 15.f + 7.f + lw + 11.f;
		const ImRect bb(ImVec2(tx, ty), ImVec2(tx + bw, ty + 34.f));

		ImGui::SetCursorScreenPos(bb.Min);
		ImGui::PushID(i);
		const bool clicked = ImGui::InvisibleButton("##toptab", bb.GetSize());
		const bool hov = ImGui::IsItemHovered();
		if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		ImGui::PopID();
		if (clicked) {
			g_tab = i;
			g_searchDropdownOpen = false;
		}

		const bool active = (g_tab == i);
		if (active)
			dl->AddRectFilled(bb.Min, bb.Max, C(kElevated), 6.f);

		const ImU32 icCol = active ? C(kAccent) : (hov ? C(kTextDim) : C(kTextFaint));
		const ImU32 txCol = active ? C(kText) : (hov ? C(kTextMid) : C(kTextDim));
		DrawVectorIcon(dl, kTabIcons[i], ImVec2(bb.Min.x + 17.f, (bb.Min.y + bb.Max.y) * 0.5f), 6.f, icCol);
		dl->AddText(font::inter_medium_widget, font::inter_medium_widget->FontSize,
			ImVec2(bb.Min.x + 31.f, RowTextY(bb.Min.y, 34.f, font::inter_medium_widget)),
			txCol, kTabNames[i]);

		tx += bw + 6.f;
	}
	outTabsRect = ImRect(ImVec2(tabsStartX, ty), ImVec2(tx, ty + 34.f));

	// Right Close (X) button — no divider line
	constexpr float closeSz = 32.f;
	outCloseRect = ImRect(
		ImVec2(pos.x + kW - 14.f - closeSz, pos.y + (kTop - closeSz) * 0.5f),
		ImVec2(pos.x + kW - 14.f, pos.y + (kTop + closeSz) * 0.5f));
	ImGui::SetCursorScreenPos(outCloseRect.Min);
	const bool closeClicked = ImGui::InvisibleButton("##close_menu", outCloseRect.GetSize());
	const bool closeHov = ImGui::IsItemHovered();
	if (closeHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	if (closeHov)
		dl->AddRectFilled(outCloseRect.Min, outCloseRect.Max, C(kRed, 0.14f), 6.f);
	{
		const ImVec2 cc((outCloseRect.Min.x + outCloseRect.Max.x) * 0.5f,
			(outCloseRect.Min.y + outCloseRect.Max.y) * 0.5f);
		const ImU32 xc = closeHov ? C(kRed) : C(kTextDim);
		dl->AddLine(ImVec2(cc.x - 4.5f, cc.y - 4.5f), ImVec2(cc.x + 4.5f, cc.y + 4.5f), xc, 1.5f);
		dl->AddLine(ImVec2(cc.x + 4.5f, cc.y - 4.5f), ImVec2(cc.x - 4.5f, cc.y + 4.5f), xc, 1.5f);
	}
	if (closeClicked) Overlay::SetMenuOpen(false);

	// Inline Search Bar to the left of Close (X)
	const float maxSearchW = ImMax(110.f, outCloseRect.Min.x - 10.f - (tx + 8.f));
	const float searchW = ImClamp(200.f, 110.f, maxSearchW);
	outSearchRect = ImRect(
		ImVec2(outCloseRect.Min.x - 10.f - searchW, pos.y + (kTop - 32.f) * 0.5f),
		ImVec2(outCloseRect.Min.x - 10.f, pos.y + (kTop + 32.f) * 0.5f));

	if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false))
		ImGui::SetKeyboardFocusHere();

	const bool sFocused = ImGui::GetActiveID() == ImGui::GetID("##gsearch");
	dl->AddRectFilled(outSearchRect.Min, outSearchRect.Max, C(kCard), 6.f);
	dl->AddRect(outSearchRect.Min, outSearchRect.Max, C(sFocused ? kAccent : kStroke), 6.f, 0, 1.f);

	// Magnifier glyph
	const ImVec2 mag(outSearchRect.Min.x + 14.f, (outSearchRect.Min.y + outSearchRect.Max.y) * 0.5f - 1.f);
	dl->AddCircle(mag, 4.5f, C(kTextFaint), 14, 1.4f);
	dl->AddLine(ImVec2(mag.x + 3.2f, mag.y + 3.2f), ImVec2(mag.x + 6.8f, mag.y + 6.8f), C(kTextFaint), 1.4f);

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(28.f, 7.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Text, C(kText));
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, C(kTextFaint));
	ImGui::SetCursorScreenPos(outSearchRect.Min);
	ImGui::SetNextItemWidth(searchW - (g_search[0] ? 22.f : 0.f));
	const bool enterPressed = ImGui::InputTextWithHint("##gsearch", "Search...", g_search, sizeof(g_search),
		ImGuiInputTextFlags_EnterReturnsTrue);
	if (ImGui::IsItemActive() && g_search[0]) g_searchDropdownOpen = true;
	ImGui::PopStyleColor(5);
	ImGui::PopStyleVar(3);

	if (g_search[0])
	{
		const ImRect clr(ImVec2(outSearchRect.Max.x - 24.f, outSearchRect.Min.y + 7.f),
			ImVec2(outSearchRect.Max.x - 6.f, outSearchRect.Max.y - 7.f));
		ImGui::SetCursorScreenPos(clr.Min);
		const bool clrClick = ImGui::InvisibleButton("##gsearch_clr", clr.GetSize());
		const bool clrHov = ImGui::IsItemHovered();
		if (clrHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		const ImVec2 cc((clr.Min.x + clr.Max.x) * 0.5f, (clr.Min.y + clr.Max.y) * 0.5f);
		dl->AddLine(ImVec2(cc.x - 3.f, cc.y - 3.f), ImVec2(cc.x + 3.f, cc.y + 3.f), C(clrHov ? kText : kTextDim), 1.3f);
		dl->AddLine(ImVec2(cc.x + 3.f, cc.y - 3.f), ImVec2(cc.x - 3.f, cc.y + 3.f), C(clrHov ? kText : kTextDim), 1.3f);
		if (clrClick) { g_search[0] = 0; g_searchDropdownOpen = false; }
	}
	else
	{
		g_searchDropdownOpen = false;
	}

	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		g_searchDropdownOpen = false;

	// Jump to first hit on Enter
	if (enterPressed && g_search[0])
	{
		for (const auto& entry : kSearchIndex) {
			if (CaseContains(entry.label, g_search)) {
				g_tab = entry.tab;
				g_sub[g_tab] = entry.sub;
				g_flashLabel = entry.match ? entry.match : entry.label;
				g_flashTimer = 1.2f;
				g_searchDropdownOpen = false;
				break;
			}
		}
	}
}

void DrawSearchDropdown(const ImRect& searchRect)
{
	if (!g_searchDropdownOpen || !g_search[0]) return;

	std::vector<const SearchEntry*> hits;
	for (const auto& entry : kSearchIndex) {
		if (CaseContains(entry.label, g_search)) {
			hits.push_back(&entry);
			if (hits.size() >= 10) break;
		}
	}

	constexpr float dropW = 300.f;
	const float rowH = 26.f;
	const float dropH = hits.empty() ? 36.f : (12.f + (float)hits.size() * rowH);
	const ImVec2 dPos(searchRect.Max.x - dropW, searchRect.Max.y + 6.f);

	ImGui::SetNextWindowPos(dPos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(dropW, dropH), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f, 6.f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, C(kPopup));
	ImGui::PushStyleColor(ImGuiCol_Border, C(kStroke));

	if (ImGui::Begin("##search_drop", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing))
	{
		if (hits.empty()) {
			Note("No matching options.");
		} else {
			ImDrawList* dl = ImGui::GetWindowDrawList();
			for (size_t i = 0; i < hits.size(); ++i)
			{
				const auto* e = hits[i];
				int sc = 0;
				const SubTabDef* subs = GetSubs(e->tab, sc);
				const char* subName = (subs && e->sub >= 0 && e->sub < sc) ? subs[e->sub].name : "";

				const ImVec2 p = ImGui::GetCursorScreenPos();
				const float w = ImGui::GetContentRegionAvail().x;
				ImGui::PushID((int)i);
				const bool clicked = ImGui::InvisibleButton("##shit", ImVec2(w, rowH));
				const bool hov = ImGui::IsItemHovered();
				if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
				ImGui::PopID();

				if (hov)
					dl->AddRectFilled(p, p + ImVec2(w, rowH), C(kElevated), 5.f);

				dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
					ImVec2(p.x + 8.f, RowTextY(p.y, rowH, font::inter_medium_small)), C(kText), e->label);

				char pathBuf[64];
				std::snprintf(pathBuf, sizeof(pathBuf), "%s -> %s", kTabNames[e->tab], subName);
				TextRight(font::inter_medium_small, p.x + w - 8.f,
					RowTextY(p.y, rowH, font::inter_medium_small), C(kTextDim), pathBuf);

				if (clicked) {
					g_tab = e->tab;
					g_sub[g_tab] = e->sub;
					g_flashLabel = e->match ? e->match : e->label;
					g_flashTimer = 1.2f;
					g_searchDropdownOpen = false;
				}
			}
		}

		// Dismiss if clicked outside both search box and dropdown
		if (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
			!searchRect.Contains(ImGui::GetIO().MousePos))
		{
			g_searchDropdownOpen = false;
		}
		ImGui::End();
	}
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(3);
}

void DrawSubBar(const ImVec2& pos)
{
	int count = 0;
	const SubTabDef* subs = GetSubs(g_tab, count);
	if (!subs || count <= 0) return;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	float sx = pos.x + kPadX;
	const float sy = pos.y + kTop + 10.f;

	for (int i = 0; i < count; ++i)
	{
		const float lw = TextW(font::inter_medium_small, subs[i].name);
		const float bw = 11.f + 13.f + 7.f + lw + 12.f;
		const ImRect bb(ImVec2(sx, sy), ImVec2(sx + bw, sy + 30.f));

		ImGui::SetCursorScreenPos(bb.Min);
		ImGui::PushID(100 + i);
		const bool clicked = ImGui::InvisibleButton("##subtab", bb.GetSize());
		const bool hov = ImGui::IsItemHovered();
		if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		ImGui::PopID();
		if (clicked) g_sub[g_tab] = i;

		const bool active = (g_sub[g_tab] == i);
		if (active)
			dl->AddRectFilled(bb.Min, bb.Max, C(kCard), 6.f);

		const ImU32 icCol = active ? C(kAccent) : (hov ? C(kTextDim) : C(kTextFaint));
		const ImU32 txCol = active ? C(kText) : (hov ? C(kTextMid) : C(kTextDim));
		DrawVectorIcon(dl, subs[i].icon, ImVec2(bb.Min.x + 16.f, (bb.Min.y + bb.Max.y) * 0.5f), 5.2f, icCol);
		dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize,
			ImVec2(bb.Min.x + 29.f, RowTextY(bb.Min.y, 30.f, font::inter_medium_small)),
			txCol, subs[i].name);

		sx += bw + 6.f;
	}
}

void DrawFooter(const ImVec2& pos, bool attached)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float fy = pos.y + kH - kFooter;
	dl->AddLine(ImVec2(pos.x, fy), ImVec2(pos.x + kW, fy), C(kStroke), 1.f);

	const auto snapshot = RobRunReader::GetSnapshot();
	const bool dmaReady = (TargetProcess.vHandle != nullptr);
	const int gamePid = attached ? TargetProcess.GetPID() : 0;
	const bool hasLiveSnap = attached && snapshot && snapshot->battleEntities != 0 && !snapshot->entities.empty();

	const ImVec4 dotCol = !dmaReady ? kRed : (!attached ? kAmber : (hasLiveSnap ? kGreen : kAmber));
	const ImVec4 txtCol = !dmaReady ? kRed : (hasLiveSnap ? kTextMid : kAmber);
	const char* stLabel = !dmaReady ? "DMA Offline"
		: (!attached ? "Waiting for Game"
		: (hasLiveSnap ? "In Match" : "Attached (Menu)"));

	// Left: Connection-state pill + PID
	const float pillW = 24.f + TextW(font::inter_medium_small, stLabel) + 8.f;
	const ImRect pill(ImVec2(pos.x + 16.f, fy + 7.f), ImVec2(pos.x + 16.f + pillW, fy + kFooter - 7.f));
	dl->AddRectFilled(pill.Min, pill.Max, C(kCard), 5.f);
	dl->AddRect(pill.Min, pill.Max, C(kStroke), 5.f);
	dl->AddCircleFilled(ImVec2(pill.Min.x + 10.f, (pill.Min.y + pill.Max.y) * 0.5f), 3.f, C(dotCol));
	dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize * 0.92f,
		ImVec2(pill.Min.x + 19.f, RowTextY(pill.Min.y, pill.GetHeight(), font::inter_medium_small)),
		C(txtCol), stLabel);

	float leftBound = pill.Max.x + 10.f;
	if (attached && gamePid > 0) {
		char pidBuf[32];
		std::snprintf(pidBuf, sizeof(pidBuf), "PID %d", gamePid);
		dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize * 0.92f,
			ImVec2(leftBound, RowTextY(fy, kFooter, font::inter_medium_small)),
			C(kTextFaint), pidBuf);
		leftBound += TextW(font::inter_medium_small, pidBuf) * 0.92f + 12.f;
	}

	// Right: dynamic menu-key hint + Re-attach button
	constexpr float btnW = 78.f, btnH = 24.f;
	const ImRect rBtn(
		ImVec2(pos.x + kW - 16.f - btnW, fy + (kFooter - btnH) * 0.5f),
		ImVec2(pos.x + kW - 16.f, fy + (kFooter + btnH) * 0.5f));
	ImGui::SetCursorScreenPos(rBtn.Min);
	const bool rClick = ImGui::InvisibleButton("##footer_reattach", rBtn.GetSize());
	const bool rHov = ImGui::IsItemHovered();
	if (rHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	dl->AddRectFilled(rBtn.Min, rBtn.Max, C(rHov ? kElevated : kCard), 5.f);
	dl->AddRect(rBtn.Min, rBtn.Max, C(rHov ? kAccent : kStroke), 5.f);
	{
		const char* rLbl = "Re-attach";
		const float tw = TextW(font::inter_medium_small, rLbl);
		dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize * 0.92f,
			ImVec2((rBtn.Min.x + rBtn.Max.x - tw) * 0.5f, RowTextY(rBtn.Min.y, btnH, font::inter_medium_small)),
			C(rHov ? kText : kTextMid), rLbl);
	}
	if (rClick) RequestReattach();

	float rightBound = rBtn.Min.x - 12.f;
	if (kW >= 820.f) {
		const int mkIdx = ImClamp(ConfigInstance.Overlay.MenuKey, 0, (int)IM_ARRAYSIZE(kMenuKeyLabels) - 1);
		char hintBuf[48];
		std::snprintf(hintBuf, sizeof(hintBuf), "%s to close", kMenuKeyLabels[mkIdx]);
		TextRight(font::inter_medium_small, rightBound,
			RowTextY(fy, kFooter, font::inter_medium_small), C(kTextFaint), hintBuf);
		rightBound -= TextW(font::inter_medium_small, hintBuf) + 12.f;
	}

	// Center: FPS · ENT · activeConfig.json * (clipped safely between left & right bounds)
	if (rightBound > leftBound + 40.f) {
		char midBuf[128];
		std::snprintf(midBuf, sizeof(midBuf), "%.0f FPS  |  %zu ENT  |  %s.json%s",
			ImGui::GetIO().Framerate,
			snapshot ? snapshot->entities.size() : (size_t)0,
			ConfigFile::GetActiveConfigName().c_str(),
			g_dirty ? "  *" : "");
		const float midW = TextW(font::inter_medium_small, midBuf) * 0.92f;
		const float midX = ImClamp(pos.x + (kW - midW) * 0.5f, leftBound, ImMax(leftBound, rightBound - midW));
		dl->PushClipRect(ImVec2(leftBound, fy), ImVec2(rightBound, fy + kFooter), true);
		dl->AddText(font::inter_medium_small, font::inter_medium_small->FontSize * 0.92f,
			ImVec2(midX, RowTextY(fy, kFooter, font::inter_medium_small)),
			C(g_dirty ? kTextDim : kTextFaint), midBuf);
		dl->PopClipRect();
	}
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
	colors[ImGuiCol_Border] = kStroke;
	colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
	colors[ImGuiCol_FrameBg] = kControl;
	colors[ImGuiCol_FrameBgHovered] = kElevated;
	colors[ImGuiCol_FrameBgActive] = kElevated;
	colors[ImGuiCol_TitleBg] = kPopup;
	colors[ImGuiCol_TitleBgActive] = kPopup;
	colors[ImGuiCol_TitleBgCollapsed] = kPopup;
	colors[ImGuiCol_MenuBarBg] = kPopup;
	colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
	colors[ImGuiCol_ScrollbarGrab] = kTrack;
	colors[ImGuiCol_ScrollbarGrabHovered] = kAccent;
	colors[ImGuiCol_ScrollbarGrabActive] = kAccentBright;
	colors[ImGuiCol_CheckMark] = kAccent;
	colors[ImGuiCol_SliderGrab] = kAccent;
	colors[ImGuiCol_SliderGrabActive] = kAccentBright;
	colors[ImGuiCol_Button] = kControl;
	colors[ImGuiCol_ButtonHovered] = kElevated;
	colors[ImGuiCol_ButtonActive] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.5f);
	colors[ImGuiCol_Header] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.22f);
	colors[ImGuiCol_HeaderHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.32f);
	colors[ImGuiCol_HeaderActive] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.42f);
	colors[ImGuiCol_Separator] = kStrokeSoft;
	colors[ImGuiCol_SeparatorHovered] = kAccent;
	colors[ImGuiCol_SeparatorActive] = kAccentBright;
	colors[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
	colors[ImGuiCol_ResizeGripHovered] = kAccent;
	colors[ImGuiCol_ResizeGripActive] = kAccentBright;
	colors[ImGuiCol_NavCursor] = kAccent;
	colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
}

void Init()
{
	ImGuiIO& io = ImGui::GetIO();

	static ImWchar glyphRanges[] = { 0x0020, 0x00FF, 0x2000, 0x206F, 0xE000, 0xF8FF, 0 };

	ImFontConfig cfg;
	cfg.FontDataOwnedByAtlas = false;

	font::inter_medium_widget = io.Fonts->AddFontFromMemoryTTF(inter_medium, sizeof(inter_medium), 14.5f, &cfg, glyphRanges);
	font::inter_medium_small = io.Fonts->AddFontFromMemoryTTF(inter_medium, sizeof(inter_medium), 12.5f, &cfg, glyphRanges);
	font::inter_medium_big = io.Fonts->AddFontFromMemoryTTF(inter_bold, sizeof(inter_bold), 21.f, &cfg, glyphRanges);
	font::inter_bold = io.Fonts->AddFontFromMemoryTTF(inter_bold, sizeof(inter_bold), 15.f, &cfg, glyphRanges);
	font::icomoon = io.Fonts->AddFontFromMemoryTTF(icomoon, sizeof(icomoon), 17.f, &cfg, glyphRanges);
	font::icomoon_default = io.Fonts->AddFontFromMemoryTTF(icomoon, sizeof(icomoon), 20.f, &cfg, glyphRanges);

	if (font::inter_medium_widget)
		io.FontDefault = font::inter_medium_widget;

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
		for (auto& f : fontFiles) {
			if (std::filesystem::exists(f.path))
				g_fonts[f.idx].ptr = io.Fonts->AddFontFromFileTTF(f.path.c_str(), 14.5f, nullptr, glyphRanges);
		}
		SyncFontFromConfig();
	}

	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 10.f;
	style.ChildRounding = 8.f;
	style.FrameRounding = 5.f;
	style.PopupRounding = 8.f;
	style.GrabRounding = 4.f;
	style.ScrollbarRounding = 4.f;
	style.WindowBorderSize = 0.f;
	style.ChildBorderSize = 0.f;
	style.FrameBorderSize = 1.f;
	style.WindowPadding = ImVec2(0.f, 0.f);
	style.FramePadding = ImVec2(9.f, 5.f);
	style.ItemSpacing = ImVec2(0.f, 7.f);
	style.ItemInnerSpacing = ImVec2(6.f, 4.f);
	style.ScrollbarSize = 6.f;
	style.AntiAliasedLines = true;
	style.AntiAliasedFill = true;

	g_highContrast = ConfigInstance.Overlay.HighContrastMenu;
	ApplyPalette();
	ApplyStyleColors();
	g_lastAccent = kAccent;
}

void Draw(bool attached, const std::string& statusText)
{
	ImGuiIO& io = ImGui::GetIO();

	if (g_flashTimer > 0.f) {
		g_flashTimer = ImMax(0.f, g_flashTimer - io.DeltaTime);
		if (g_flashTimer <= 0.f) g_flashLabel.clear();
	}

	SyncAccent();
	const bool accentChanged =
		std::fabs(kAccent.x - g_lastAccent.x) > 0.002f ||
		std::fabs(kAccent.y - g_lastAccent.y) > 0.002f ||
		std::fabs(kAccent.z - g_lastAccent.z) > 0.002f;

	if (g_highContrast != ConfigInstance.Overlay.HighContrastMenu || accentChanged)
	{
		g_highContrast = ConfigInstance.Overlay.HighContrastMenu;
		ApplyPalette();
		ApplyStyleColors();
		g_lastAccent = kAccent;
	}

	ImGui::SetNextWindowSizeConstraints(ImVec2(kMinW, kMinH), io.DisplaySize);
	if (g_dragDelta.x != 0.f || g_dragDelta.y != 0.f)
	{
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
		g_placementApplied = true;
		const WindowPlacement& saved = ConfigInstance.Overlay.MenuWindow;
		if (saved.Saved) {
			const float w = ImClamp(saved.Width, kMinW, io.DisplaySize.x);
			const float h = ImClamp(saved.Height, kMinH, io.DisplaySize.y);
			const ImVec2 target(
				ImClamp(saved.X, -w * 0.5f, ImMax(io.DisplaySize.x - w * 0.5f, 0.f)),
				ImClamp(saved.Y, 0.f, ImMax(io.DisplaySize.y - kTop, 0.f)));
			ImGui::SetNextWindowPos(target, ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
		} else {
			ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - kW) * 0.5f, (io.DisplaySize.y - kH) * 0.5f), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize(ImVec2(kW, kH), ImGuiCond_FirstUseEver);
		}
	}

	ImGui::Begin("##ROBRUN", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);

	ImRect searchRect{};
	{
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		const ImVec2 winSize = window->Size;
		kW = ImMax(winSize.x, kMinW);
		kH = ImMax(winSize.y, kMinH);
		g_singleColumn = kW < kTwoColumnMin;

		const ImVec2 pos = ImGui::GetWindowPos();
		{
			WindowPlacement& place = ConfigInstance.Overlay.MenuWindow;
			place.Saved = true;
			place.X = pos.x;
			place.Y = pos.y;
			place.Width = winSize.x;
			place.Height = winSize.y;
		}

		ImDrawList* dl = ImGui::GetWindowDrawList();

		// Flat matte dark window base
		dl->AddRectFilled(pos, pos + ImVec2(kW, kH), C(kBg), 10.f);
		dl->AddRect(pos, pos + ImVec2(kW, kH), C(kStroke), 10.f, 0, 1.f);

		ImRect tabsRect{}, closeRect{};
		DrawTopBar(pos, tabsRect, searchRect, closeRect);
		DrawSubBar(pos);

		// Drag window by top bar (excluding tabs, search, and close button)
		{
			const ImRect dragZone(pos, pos + ImVec2(kW, kTop));
			const ImVec2 mouse = io.MousePos;
			const bool inZone = dragZone.Contains(mouse) &&
				!tabsRect.Contains(mouse) && !searchRect.Contains(mouse) && !closeRect.Contains(mouse);
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
				g_dragging = false;
			else if (inZone && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive())
				g_dragging = true;
			if (g_dragging)
				g_dragDelta += io.MouseDelta;
		}

		// Main Content Area (scrollbar auto-hides when content fits; allows scroll on overflow / narrow single-col)
		const float contentTop = kTop + kSubBar;
		const float contentH = ImMax(120.f, kH - contentTop - kFooter);
		const ImVec2 contentMin(pos.x, pos.y + contentTop);
		const ImVec2 contentSize(kW, contentH);

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPadX, kPadTop));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, kCardGap));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
		ImGui::SetCursorScreenPos(contentMin);
		ImGui::BeginChild("##content", contentSize, ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoSavedSettings);

		const float availW = ImGui::GetContentRegionAvail().x;
		kCardW = g_singleColumn ? availW : (availW - kGap) * 0.5f;
		kAvailH = ImMax(120.f, contentH - kPadTop - kPadBot);

		const int sub = g_sub[g_tab];
		switch (g_tab) {
		case 0:
			if (sub == 0) DrawAimGeneral();
			else if (sub == 1) DrawAimTargetsRecoil();
			else if (sub == 2) DrawAimTriggerbot();
			else DrawAimHardwarePad();
			break;
		case 1:
			if (sub == 0) DrawVisualsEsp();
			else if (sub == 1) DrawVisualsSkeleton();
			else DrawVisualsStates();
			break;
		case 2:
			if (sub == 0) DrawWorldAndLoot();
			else if (sub == 1) DrawWorldValueSizing();
			else DrawWorldCatalog();
			break;
		case 3:
			if (sub == 0) DrawRadar2DWeb();
			else DrawRadarInGameMap();
			break;
		case 4:
			if (sub == 0) DrawMiscGameplay();
			else if (sub == 1) DrawMiscSkins();
			else DrawMiscServer();
			break;
		case 5:
			if (sub == 0) DrawSettingsOverlay();
			else DrawSettingsConfigs(attached, statusText);
			break;
		default: break;
		}

		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);

		DrawFooter(pos, attached);

		// Subtle bottom-right resize grip diagonals
		{
			const ImVec2 c(pos.x + kW - 5.f, pos.y + kH - 5.f);
			const ImU32 col = C(kTextFaint, 0.7f);
			dl->AddLine(ImVec2(c.x - 9.f, c.y), ImVec2(c.x, c.y - 9.f), col, 1.3f);
			dl->AddLine(ImVec2(c.x - 4.f, c.y), ImVec2(c.x, c.y - 4.f), col, 1.3f);
		}

		g_lastPos = pos;
	}
	ImGui::End();

	DrawSearchDropdown(searchRect);
}

} // namespace VeoxMenu
