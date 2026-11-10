#include "pch.h"
#include "VeoxMenu.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
#include "../Overlay/Overlay.h"
#include "../ImGUI/imgui.h"

namespace VeoxMenu {

static int g_tab = 0;
static bool g_readCatalog = false;

static constexpr float MENU_W = 620.f;
static constexpr float MENU_H = 420.f;
static constexpr float SIDE_W = 150.f;
static constexpr float LOGO_H = 50.f;
static constexpr float SHELL_R = 12.f;
static constexpr float CHROME_R = 8.f;

// ---- Black / Blue palette ----
static constexpr ImU32 COL_BG_DARK     = IM_COL32(12, 12, 18, 255);
static constexpr ImU32 COL_BG_SIDEBAR  = IM_COL32(18, 20, 30, 255);
static constexpr ImU32 COL_BG_CHILD    = IM_COL32(22, 24, 36, 255);
static constexpr ImU32 COL_BORDER      = IM_COL32(40, 80, 160, 80);
static constexpr ImU32 COL_ACCENT      = IM_COL32(30, 120, 255, 255);
static constexpr ImU32 COL_ACCENT_HI   = IM_COL32(60, 145, 255, 255);
static constexpr ImU32 COL_ACCENT_DIM  = IM_COL32(30, 120, 255, 180);
static constexpr ImU32 COL_TEXT        = IM_COL32(220, 225, 235, 255);
static constexpr ImU32 COL_TEXT_DIM    = IM_COL32(100, 110, 130, 255);
static constexpr ImU32 COL_FRAME       = IM_COL32(30, 32, 48, 255);
static constexpr ImU32 COL_FRAME_HI    = IM_COL32(40, 45, 65, 255);
static constexpr ImU32 COL_BUTTON      = IM_COL32(28, 30, 45, 255);

static void PushBlueTheme()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, SHELL_R);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, CHROME_R);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, CHROME_R);
	ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, CHROME_R);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, SHELL_R);
	ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.f);
	ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 8.f);
	ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, CHROME_R);
	ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, CHROME_R);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f, 6.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 3.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 2.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(2.f, 1.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);

	ImGui::PushStyleColor(ImGuiCol_WindowBg, COL_BG_DARK);
	ImGui::PushStyleColor(ImGuiCol_Border, COL_BORDER);
	ImGui::PushStyleColor(ImGuiCol_BorderShadow, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Button, COL_BUTTON);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_ACCENT_DIM);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, COL_ACCENT);
	ImGui::PushStyleColor(ImGuiCol_Header, COL_BUTTON);
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, COL_ACCENT_DIM);
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, COL_ACCENT);
	ImGui::PushStyleColor(ImGuiCol_Tab, COL_BUTTON);
	ImGui::PushStyleColor(ImGuiCol_TabHovered, COL_ACCENT_DIM);
	ImGui::PushStyleColor(ImGuiCol_TabActive, COL_ACCENT);
	ImGui::PushStyleColor(ImGuiCol_CheckMark, COL_ACCENT);
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, COL_ACCENT);
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, COL_ACCENT_HI);
	ImGui::PushStyleColor(ImGuiCol_Text, COL_TEXT);
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, COL_TEXT_DIM);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, COL_BG_CHILD);
	ImGui::PushStyleColor(ImGuiCol_PopupBg, COL_BG_CHILD);
	ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, COL_BG_DARK);
	ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, COL_ACCENT);
	ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, COL_ACCENT_HI);
	ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, COL_ACCENT_HI);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, COL_FRAME);
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, COL_FRAME_HI);
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(30, 120, 255, 70));
	ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(255, 255, 255, 28));
	ImGui::PushStyleColor(ImGuiCol_SeparatorHovered, COL_ACCENT_DIM);
	ImGui::PushStyleColor(ImGuiCol_SeparatorActive, COL_ACCENT);
}

static constexpr int STYLE_VAR_COUNT = 15;
static constexpr int STYLE_COL_COUNT = 29;

static void PopBlueTheme()
{
	ImGui::PopStyleColor(STYLE_COL_COUNT);
	ImGui::PopStyleVar(STYLE_VAR_COUNT);
}

static bool SidebarButton(const char* label, bool selected, float width)
{
	if (selected) {
		ImGui::PushStyleColor(ImGuiCol_Button, COL_ACCENT);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_ACCENT);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, COL_ACCENT);
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
	} else {
		ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(30, 120, 255, 60));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, COL_ACCENT_DIM);
		ImGui::PushStyleColor(ImGuiCol_Text, COL_TEXT_DIM);
	}
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
	bool clicked = ImGui::Button(label, ImVec2(width - 12.f, 28.f));
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(4);
	return clicked;
}

static void DrawPanelESP()
{
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, CHROME_R);
	float pw = MENU_W - SIDE_W - 24.f;
	float ph = MENU_H - LOGO_H - 24.f;
	ImGui::SetCursorPos(ImVec2(SIDE_W + 8.f, LOGO_H + 8.f));
	ImGui::BeginChild("##esp_panel", ImVec2(pw, ph), true);

	ImGui::Text("ESP Settings");
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::Checkbox("Health", &ConfigInstance.Player.Health);
	ImGui::Checkbox("Distance", &ConfigInstance.Player.Distance);
	ImGui::Checkbox("Primary Weapon", &ConfigInstance.Player.PrimaryWeapon);
	ImGui::Checkbox("Secondary Weapon", &ConfigInstance.Player.SecondaryWeapon);
	ImGui::Spacing();
	ImGui::SliderInt("Max Distance", &ConfigInstance.Player.MaxDistance, 0, 1500, "%dm");
	ImGui::SliderInt("Font Size", &ConfigInstance.Player.FontSize, 1, 24, "%dpx");

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	float col[4] = {
		ConfigInstance.Player.TextColour.r,
		ConfigInstance.Player.TextColour.g,
		ConfigInstance.Player.TextColour.b,
		ConfigInstance.Player.TextColour.a
	};
	if (ImGui::ColorEdit4("ESP Color", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
	{
		ConfigInstance.Player.TextColour = D2D1::ColorF(col[0], col[1], col[2], col[3]);
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::Text("World ESP");

	ImGui::Checkbox("Enable World ESP", &ConfigInstance.World.Enabled);
	ImGui::Checkbox("Containers", &ConfigInstance.World.Containers);
	ImGui::Checkbox("Loot", &ConfigInstance.World.Loot);
	ImGui::Checkbox("Vehicles", &ConfigInstance.World.Vehicles);
	ImGui::Checkbox("Grenades", &ConfigInstance.World.Grenades);
	ImGui::Checkbox("Show empty state", &ConfigInstance.World.ShowEmpty);
	ImGui::Checkbox("Show distance", &ConfigInstance.World.ShowDistance);
	ImGui::SliderInt("World distance", &ConfigInstance.World.MaxDistance, 0, 2000, "%dm");
	ImGui::SliderInt("World font", &ConfigInstance.World.FontSize, 1, 24, "%dpx");

	auto worldColour = [](const char* label, D2D1::ColorF& colour) {
		float values[4] = { colour.r, colour.g, colour.b, colour.a };
		if (ImGui::ColorEdit4(label, values,
			ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
			colour = D2D1::ColorF(values[0], values[1], values[2], values[3]);
		}
	};
	worldColour("Container Color", ConfigInstance.World.ContainerColour);
	worldColour("Loot Color", ConfigInstance.World.LootColour);
	worldColour("Vehicle Color", ConfigInstance.World.VehicleColour);
	worldColour("Grenade Color", ConfigInstance.World.GrenadeColour);

	ImGui::EndChild();
	ImGui::PopStyleVar();
}

static void DrawPanelAimbot()
{
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, CHROME_R);
	float pw = MENU_W - SIDE_W - 24.f;
	float ph = MENU_H - LOGO_H - 24.f;
	ImGui::SetCursorPos(ImVec2(SIDE_W + 8.f, LOGO_H + 8.f));
	ImGui::BeginChild("##aim_panel", ImVec2(pw, ph), true);

	ImGui::Text("Aimbot");
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextDisabled("Aim device integration ready.");
	ImGui::TextDisabled("Configure aim device type and settings here.");

	ImGui::EndChild();
	ImGui::PopStyleVar();
}

static void DrawPanelMisc()
{
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, CHROME_R);
	float pw = MENU_W - SIDE_W - 24.f;
	float ph = MENU_H - LOGO_H - 24.f;
	ImGui::SetCursorPos(ImVec2(SIDE_W + 8.f, LOGO_H + 8.f));
	ImGui::BeginChild("##misc_panel", ImVec2(pw, ph), true);

	ImGui::Text("Misc");
	ImGui::Separator();
	ImGui::Spacing();

	if (ImGui::Checkbox("Read item catalog", &g_readCatalog))
		RobRunReader::SetReadCatalogEnabled(g_readCatalog);
	ImGui::TextDisabled("One-off bounded class scan. It can pause briefly.");

	const auto snapshot = RobRunReader::GetSnapshot();
	if (snapshot && snapshot->inventory.valid)
	{
		const auto& inventory = snapshot->inventory;
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Text("Inventory");
		ImGui::Text("Grid: %d columns x %d lines", inventory.columns, inventory.activeLines);
		ImGui::Text("Base %d + backpack %d / max %d",
			inventory.baseLines, inventory.backpackLines, inventory.maxLines);
		ImGui::Text("Slots: %zu", inventory.slots.size());
		if (!inventory.catalog.empty())
			ImGui::Text("Catalog entries: %zu", inventory.catalog.size());
	}

	ImGui::EndChild();
	ImGui::PopStyleVar();
}

static void DrawPanelSettings(bool attached, const std::string& statusText)
{
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, CHROME_R);
	float pw = MENU_W - SIDE_W - 24.f;
	float ph = MENU_H - LOGO_H - 24.f;
	ImGui::SetCursorPos(ImVec2(SIDE_W + 8.f, LOGO_H + 8.f));
	ImGui::BeginChild("##settings_panel", ImVec2(pw, ph), true);

	ImGui::Text("Settings / DMA Status");
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::Text("DMA Status: %s", attached ? "Connected" : "Disconnected");
	ImGui::TextDisabled("%s", statusText.c_str());
	ImGui::Spacing();
	ImGui::Text("Overlay FPS: %.0f", ImGui::GetIO().Framerate);

	const auto snapshot = RobRunReader::GetSnapshot();
	if (snapshot)
	{
		const auto& world = snapshot->world;
		ImGui::Text("Entities: %zu | C:%d L:%d V:%d G:%d",
			snapshot->entities.size(), world.containers, world.loots,
			world.vehicles, world.grenades);
		ImGui::Text("Read failures: %u", snapshot->failureCount);
	}
	else
	{
		ImGui::Text("Entities: 0");
		ImGui::Text("Read failures: 0");
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::Text("Overlay");

	bool transparent = Overlay::Transparent();
	if (ImGui::Checkbox("Transparent", &transparent))
		Overlay::SetTransparent(transparent);

	bool clickThrough = Overlay::ClickThrough();
	if (ImGui::Checkbox("Click-through when closed", &clickThrough))
		Overlay::SetClickThrough(clickThrough);

	bool onTop = Overlay::AlwaysOnTop();
	if (ImGui::Checkbox("Always on top", &onTop))
		Overlay::SetAlwaysOnTop(onTop);

	ImGui::Checkbox("Show FPS", &ConfigInstance.Overlay.FPSEnabled);

	int cap = Overlay::FrameCap();
	if (ImGui::SliderInt("Frame cap", &cap, 0, 360, cap > 0 ? "%d fps" : "off"))
		Overlay::SetFrameCap(cap);

	int monitor = Overlay::MonitorIndex();
	if (ImGui::SliderInt("Monitor", &monitor, 0, std::max(0, Overlay::MonitorCount() - 1)))
		Overlay::SetMonitor(monitor);

	ImGui::TextDisabled("Insert toggles the menu.");

	ImGui::EndChild();
	ImGui::PopStyleVar();
}

void Init()
{
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = SHELL_R;
	style.ChildRounding = CHROME_R;
	style.FrameRounding = CHROME_R;
	style.GrabRounding = CHROME_R;
	style.PopupRounding = SHELL_R;
	style.ScrollbarRounding = CHROME_R;
	style.TabRounding = CHROME_R;
	style.AntiAliasedLines = true;
	style.AntiAliasedFill = true;

	ImVec4* colors = style.Colors;
	colors[ImGuiCol_Text] = ImColor(COL_TEXT).Value;
	colors[ImGuiCol_TextDisabled] = ImColor(COL_TEXT_DIM).Value;
	colors[ImGuiCol_WindowBg] = ImColor(COL_BG_DARK).Value;
	colors[ImGuiCol_ChildBg] = ImColor(COL_BG_CHILD).Value;
	colors[ImGuiCol_PopupBg] = ImColor(COL_BG_CHILD).Value;
	colors[ImGuiCol_Border] = ImColor(COL_BORDER).Value;
	colors[ImGuiCol_FrameBg] = ImColor(COL_FRAME).Value;
	colors[ImGuiCol_FrameBgHovered] = ImColor(COL_FRAME_HI).Value;
	colors[ImGuiCol_FrameBgActive] = ImColor(IM_COL32(30, 120, 255, 70)).Value;
	colors[ImGuiCol_CheckMark] = ImColor(COL_ACCENT).Value;
	colors[ImGuiCol_SliderGrab] = ImColor(COL_ACCENT).Value;
	colors[ImGuiCol_SliderGrabActive] = ImColor(COL_ACCENT_HI).Value;
	colors[ImGuiCol_Button] = ImColor(COL_BUTTON).Value;
	colors[ImGuiCol_ButtonHovered] = ImColor(COL_ACCENT_DIM).Value;
	colors[ImGuiCol_ButtonActive] = ImColor(COL_ACCENT).Value;
	colors[ImGuiCol_Header] = ImColor(COL_BUTTON).Value;
	colors[ImGuiCol_HeaderHovered] = ImColor(COL_ACCENT_DIM).Value;
	colors[ImGuiCol_HeaderActive] = ImColor(COL_ACCENT).Value;
	colors[ImGuiCol_ScrollbarBg] = ImColor(COL_BG_DARK).Value;
	colors[ImGuiCol_ScrollbarGrab] = ImColor(COL_ACCENT).Value;
	colors[ImGuiCol_ScrollbarGrabHovered] = ImColor(COL_ACCENT_HI).Value;
	colors[ImGuiCol_ScrollbarGrabActive] = ImColor(COL_ACCENT_HI).Value;
}

void Draw(bool attached, const std::string& statusText)
{
	PushBlueTheme();

	ImGui::SetNextWindowSize(ImVec2(MENU_W, MENU_H), ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(80.f, 80.f), ImGuiCond_FirstUseEver);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));

	ImGui::Begin("##VeoxMenu", nullptr,
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoBackground);

	ImVec2 wpos = ImGui::GetWindowPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();

	// Main shell background
	dl->AddRectFilled(wpos, ImVec2(wpos.x + MENU_W, wpos.y + MENU_H), COL_BG_DARK, SHELL_R);
	// Sidebar background
	dl->AddRectFilled(wpos, ImVec2(wpos.x + SIDE_W, wpos.y + MENU_H), COL_BG_SIDEBAR, SHELL_R,
		ImDrawFlags_RoundCornersLeft);
	// Sidebar divider
	dl->AddLine(
		ImVec2(wpos.x + SIDE_W - 1.f, wpos.y + SHELL_R),
		ImVec2(wpos.x + SIDE_W - 1.f, wpos.y + MENU_H - SHELL_R),
		COL_BORDER, 1.f);
	// Shell border
	dl->AddRect(wpos, ImVec2(wpos.x + MENU_W, wpos.y + MENU_H), COL_BORDER, SHELL_R);

	// ---- Logo / Brand ----
	{
		const float cx = wpos.x + SIDE_W * 0.5f;
		const float cy = wpos.y + LOGO_H * 0.5f;
		dl->AddText(ImVec2(cx - 38.f, cy - 8.f), COL_ACCENT, "VEOX DMA");
	}
	dl->AddLine(
		ImVec2(wpos.x, wpos.y + LOGO_H),
		ImVec2(wpos.x + SIDE_W, wpos.y + LOGO_H),
		COL_BORDER, 1.f);

	// ---- Sidebar navigation ----
	ImGui::SetCursorPos(ImVec2(6.f, LOGO_H + 8.f));
	ImGui::BeginChild("##sidebar_nav", ImVec2(SIDE_W - 6.f, MENU_H - LOGO_H - 8.f), false,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);

	static const char* tabs[] = { "ESP", "Aimbot", "Misc", "Settings" };
	for (int i = 0; i < 4; i++)
	{
		if (SidebarButton(tabs[i], g_tab == i, SIDE_W - 6.f))
			g_tab = i;
		if (i < 3) ImGui::Spacing();
	}

	// Status indicator at bottom
	{
		float avail = ImGui::GetContentRegionAvail().y;
		if (avail > 30.f)
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail - 24.f);
		ImU32 dotCol = attached ? IM_COL32(50, 220, 100, 255) : IM_COL32(220, 60, 60, 255);
		ImVec2 cpos = ImGui::GetCursorScreenPos();
		dl->AddCircleFilled(ImVec2(cpos.x + 8.f, cpos.y + 8.f), 4.f, dotCol);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 18.f);
		ImGui::TextColored(ImColor(COL_TEXT_DIM).Value, attached ? "Online" : "Offline");
	}

	ImGui::EndChild();

	// ---- Content panels ----
	switch (g_tab)
	{
	case 0: DrawPanelESP(); break;
	case 1: DrawPanelAimbot(); break;
	case 2: DrawPanelMisc(); break;
	case 3: DrawPanelSettings(attached, statusText); break;
	}

	ImGui::End();
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(2);
	PopBlueTheme();
}

} // namespace VeoxMenu
