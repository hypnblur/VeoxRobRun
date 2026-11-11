#include "pch.h"
#include "VeoxMenu.h"
#include "ConfigInstance.h"
#include "ConfigFile.h"
#include "RobRunReader.h"
#include "DeviceSession.h"
#include "InputManager.h"
#include "../Aim/AimController.h"
#include "../Overlay/Overlay.h"
#include "../ImGUI/imgui.h"
#include <cstdio>
#include <cstring>

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
	ImGui::Text("Animator / Cull Check");
	ImGui::Checkbox("Enable VisCheck", &ConfigInstance.Player.VisCheck);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(
			"Colors bones by whether the character's animator is still updating\n"
			"its pose. A frozen pose means Unity culled the character.\n\n"
			"This is NOT wall occlusion: geometry between the camera and the\n"
			"character is not visible to read-only memory, so an animating\n"
			"character behind cover still uses the active color.");
	if (ConfigInstance.Player.VisCheck)
	{
		auto visColour = [](const char* label, D2D1::ColorF& colour) {
			float v[4] = { colour.r, colour.g, colour.b, colour.a };
			if (ImGui::ColorEdit4(label, v,
				ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
				colour = D2D1::ColorF(v[0], v[1], v[2], v[3]);
			}
		};
		visColour("Active (animating)", ConfigInstance.Player.VisibleColour);
		visColour("Frozen (culled)", ConfigInstance.Player.HiddenColour);
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::Text("Wall Occlusion (Offline Raycast)");
	ImGui::Checkbox("Enable WallCheck", &ConfigInstance.Player.WallCheck);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(
			"Casts rays from the camera to each bone against pre-built\n"
			"collision geometry. Bones behind walls are marked hidden.\n\n"
			"Requires a .bvh file built by convert_scene.py.");
	{
		auto status = RobRunReader::GetBvhStatus();
		if (status.loaded)
			ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f),
				"BVH: %u nodes, %u tris", status.nodeCount, status.triCount);
		else
			ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "BVH: not loaded");
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::Text("World ESP");

	ImGui::Checkbox("Enable World ESP", &ConfigInstance.World.Enabled);
	ImGui::Checkbox("Show empty state", &ConfigInstance.World.ShowEmpty);
	ImGui::Checkbox("Show distance", &ConfigInstance.World.ShowDistance);
	ImGui::SliderInt("World distance", &ConfigInstance.World.MaxDistance, 0, 2000, "%dm");
	ImGui::SliderInt("World font", &ConfigInstance.World.FontSize, 1, 24, "%dpx");

	auto worldColour = [](const char* id, D2D1::ColorF& colour) {
		float values[4] = { colour.r, colour.g, colour.b, colour.a };
		if (ImGui::ColorEdit4(id, values,
			ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
			ImGuiColorEditFlags_AlphaBar)) {
			colour = D2D1::ColorF(values[0], values[1], values[2], values[3]);
		}
	};
	auto worldRow = [&](const char* label, bool& enabled, D2D1::ColorF& colour) {
		ImGui::Checkbox(label, &enabled);
		ImGui::SameLine();
		const std::string id = std::string("##") + label;
		worldColour(id.c_str(), colour);
	};

	ImGui::Spacing();
	ImGui::Text("Containers");
	worldRow("Containers (master)", ConfigInstance.World.Containers, ConfigInstance.World.ContainerColour);
	ImGui::Indent();
	worldRow("Loot Node", ConfigInstance.World.LootNodes, ConfigInstance.World.LootNodeColour);
	worldRow("Safe", ConfigInstance.World.SafeDoors, ConfigInstance.World.SafeDoorColour);
	worldRow("Storage Box", ConfigInstance.World.StorageBoxes, ConfigInstance.World.StorageBoxColour);
	worldRow("Shelving", ConfigInstance.World.Shelvings, ConfigInstance.World.ShelvingColour);
	worldRow("Collection Table", ConfigInstance.World.CollectionTables, ConfigInstance.World.CollectionTableColour);
	worldRow("Plot Container", ConfigInstance.World.PlotContainers, ConfigInstance.World.PlotContainerColour);
	ImGui::Unindent();

	ImGui::Spacing();
	ImGui::Text("Loot");
	worldRow("Loot (master)", ConfigInstance.World.Loot, ConfigInstance.World.LootColour);
	ImGui::Indent();
	worldRow("Dropped Bag", ConfigInstance.World.DroppedBags, ConfigInstance.World.DroppedBagColour);
	worldRow("Vehicle Loot", ConfigInstance.World.VehicleLoots, ConfigInstance.World.VehicleLootColour);
	worldRow("Cargo", ConfigInstance.World.Cargos, ConfigInstance.World.CargoColour);
	worldRow("Plot Loot", ConfigInstance.World.PlotLoots, ConfigInstance.World.PlotLootColour);
	worldRow("Furniture Loot", ConfigInstance.World.FurnitureLoots, ConfigInstance.World.FurnitureLootColour);
	worldRow("Building Loot", ConfigInstance.World.BuildingLoots, ConfigInstance.World.BuildingLootColour);
	ImGui::Unindent();

	ImGui::Spacing();
	ImGui::Text("Vehicles & Grenades");
	worldRow("Vehicles (master)", ConfigInstance.World.Vehicles, ConfigInstance.World.VehicleColour);
	worldRow("Grenades", ConfigInstance.World.Grenades, ConfigInstance.World.GrenadeColour);

	ImGui::Spacing();
	ImGui::Text("Valuables");
	worldRow("Valuables (master)", ConfigInstance.World.Valuables, ConfigInstance.World.ValuableColour);
	ImGui::Indent();
	worldRow("Cash Loot Node", ConfigInstance.World.CashLootNodes, ConfigInstance.World.CashLootNodeColour);
	worldRow("Settlement Cash", ConfigInstance.World.SettlementCash, ConfigInstance.World.SettlementCashColour);
	worldRow("Money Workbench", ConfigInstance.World.MoneyWorkbenches, ConfigInstance.World.MoneyWorkbenchColour);
	ImGui::Unindent();

	ImGui::Spacing();
	worldRow("Points of Interest", ConfigInstance.World.PointsOfInterest, ConfigInstance.World.PointOfInterestColour);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::Text("Diagnostics");

	const auto snapshot = RobRunReader::GetSnapshot();
	const int playerCount = snapshot ? static_cast<int>(snapshot->entities.size()) : 0;
	const int worldCount = snapshot ? static_cast<int>(snapshot->world.entities.size()) : 0;
	const int localId = snapshot ? snapshot->localEntityId : -1;
	const bool worldValid = snapshot && snapshot->world.valid;
	const unsigned skeletonCount = snapshot ? snapshot->skeletonCount : 0;
	const unsigned poseCount = snapshot ? snapshot->poseCount : 0;
	const unsigned poseFailures = snapshot ? snapshot->poseFailures : 0;
	ImGui::Text("Players: %d   World: %d%s   Local ID: %d",
		playerCount, worldCount, worldValid ? "" : " (root not found)", localId);
	ImGui::Text("Skeletons: %u/%d   Aim pose: %u/%d",
		skeletonCount, playerCount, poseCount, playerCount);
	if (poseFailures)
		ImGui::Text("Pose read retries: %u", poseFailures);
	ImGui::Text("DMA keyboard: %s", Keyboard::IsReady() ? "ready" : "unavailable");
	ImGui::Text("Aim device: %s",
		Base::Hardware::StateLabel(Base::Hardware::Session().GetStatus().state));

	ImGui::EndChild();
	ImGui::PopStyleVar();
}

static void DrawPanelDevices()
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

	static const char* deviceNames[] = { "Off", "Kmbox USB", "Kmbox Net", "Macku", "Macku Controller", "Ferrum", "Macku Medius", "Kmbox NB Pro" };
	static const DeviceKind deviceKinds[] = { DeviceKind::Off, DeviceKind::KmboxUsb, DeviceKind::KmboxNet, DeviceKind::Makcu, DeviceKind::Controller, DeviceKind::Ferrum, DeviceKind::Medius, DeviceKind::KmboxNbPro };
	static const int baudRates[] = { 115200, 921600, 1500000, 2000000, 3000000, 4000000 };
	static const char* baudLabels[] = { "115200", "921600", "1500000", "2000000", "3000000", "4000000" };

	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, CHROME_R);
	float pw = MENU_W - SIDE_W - 24.f;
	float ph = MENU_H - LOGO_H - 24.f;
	ImGui::SetCursorPos(ImVec2(SIDE_W + 8.f, LOGO_H + 8.f));
	ImGui::BeginChild("##aim_panel", ImVec2(pw, ph), true);

	ImGui::Text("Aim");
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::Checkbox("Enable aim", &ConfigInstance.Aim.Enabled);

	static const char* aimTypeNames[] = { "Memory Aim", "Hardware Aim" };
	ImGui::SetNextItemWidth(180.f);
	ImGui::Combo("Aim type", &ConfigInstance.Aim.AimType, aimTypeNames,
		IM_ARRAYSIZE(aimTypeNames));
	if (ConfigInstance.Aim.AimType == 0) {
		ImGui::Checkbox("Write camera angles (DMA)", &ConfigInstance.Aim.MemoryWrite);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"Writes the camera orientation, look quaternion and basis vectors\n"
				"directly into BattleCameraModel through one scatter write per step.\n\n"
				"This works on a two-PC DMA setup. If the game rebuilds the camera\n"
				"from its own input state every frame, the debug HUD reports the\n"
				"write as \"reverted\" and you can fall back to cursor movement.");
		if (ConfigInstance.Aim.MemoryWrite) {
			ImGui::Checkbox("Cursor fallback", &ConfigInstance.Aim.CursorFallback);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"If the DMA write is rejected (or the camera model is missing),\n"
					"drive the local cursor with SendInput instead. Only useful when\n"
					"the game runs on this PC.");
		} else {
			ImGui::TextDisabled("Closed-loop SendInput relative movement.");
		}
	} else {
		ImGui::TextDisabled("Motion is sent to the selected device.");
	}

	static const char* boneNames[] = { "Head", "Neck", "Chest", "Pelvis" };
	ImGui::SetNextItemWidth(180.f);
	ImGui::Combo("Target bone", &ConfigInstance.Aim.TargetBone, boneNames,
		IM_ARRAYSIZE(boneNames));

	ImGui::Checkbox("Visible bones only", &ConfigInstance.Aim.VisibleBonesOnly);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(
			"Only target characters whose animator is still updating its pose.\n"
			"This is the same per-character state the ESP colours as active.\n\n"
			"Read-only DMA cannot see geometry, so this is NOT wall occlusion:\n"
			"a character animating behind cover still counts as visible.");

	ImGui::SliderInt("Smoothness", &ConfigInstance.Aim.Smoothness, 1, 100);
	ImGui::SliderInt("FOV", &ConfigInstance.Aim.Fov, 5, 180, "%d deg");

	ImGui::Spacing();
	ImGui::Checkbox("Target prediction", &ConfigInstance.Aim.Prediction);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(
			"Leads a moving target using the velocity between two aim frames.\n"
			"Lead is capped at 65 ms and 0.55 m per m/s, so a target that\n"
			"teleports or desyncs cannot pull the aim point far away.");
	if (ConfigInstance.Aim.Prediction) {
		ImGui::Indent();
		ImGui::SliderInt("Lead strength", &ConfigInstance.Aim.PredictionStrength, 0, 200, "%d%%");
		ImGui::SliderInt("Vertical lead", &ConfigInstance.Aim.PredictionYScale, 0, 100, "%d%%");
		ImGui::Unindent();
	}

	ImGui::Checkbox("Hard lock", &ConfigInstance.Aim.HardLock);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(
			"Keep the current target even when it leaves the FOV ring, so a\n"
			"strafing player does not cause the pick to jump to someone else.\n"
			"The lock drops when the target dies or leaves the distance limit.");

	ImGui::SetNextItemWidth(180.f);
	ImGui::Combo("Aim key", &ConfigInstance.Aim.AimKey, Aim::KeyNames(),
		Aim::KeyCount());

	ImGui::SliderInt("Aim distance", &ConfigInstance.Aim.MaxDistance, 10, 1000, "%dm");
	ImGui::Checkbox("Show FOV circle", &ConfigInstance.Aim.ShowFov);
	ImGui::Checkbox("Show aim debug", &ConfigInstance.Aim.ShowDebug);

	{
		const Aim::DebugInfo info = Aim::GetDebug();
		ImGui::Spacing();
		ImGui::TextDisabled("Live: %s | key %s (%s)", info.reason,
			info.held ? "held" : "up", info.keySource);
		ImGui::TextDisabled("Targets %d/%d | target %d | delta %.1f, %.1f | device %s",
			info.candidates, info.entities, info.target, info.dx, info.dy,
			info.deviceReady ? "ready" : "not ready");
		if (info.writeMode)
			ImGui::TextDisabled("Write: %s (%s) | yaw %.1f pitch %.1f | %.0fm | lead %dms",
				info.writeOk ? "ok" : "fail", info.writeState,
				info.tgtYawDeg, info.tgtPitchDeg, info.distance, info.leadMs);
		if (info.menuOpen)
			ImGui::TextDisabled("Aim is paused while this menu is open.");
	}

	if (ConfigInstance.Aim.AimType == 1 && !Ready())
		ImGui::TextDisabled("Hardware Aim needs a connected device below.");

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::Text("Aim Device");
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::SetNextItemWidth(180.f);
	ImGui::Combo("Device", &deviceIndex, deviceNames, IM_ARRAYSIZE(deviceNames));

	const auto kind = deviceKinds[deviceIndex];
	const bool isNet = IsNetworkDevice(kind);
	const bool isSerial = kind != DeviceKind::Off && !isNet;

	if (isNet) {
		ImGui::SetNextItemWidth(140.f);
		ImGui::InputText("IP", ipAddr, sizeof(ipAddr));
		ImGui::SetNextItemWidth(80.f);
		ImGui::InputText("Port", netPort, sizeof(netPort));
		ImGui::SetNextItemWidth(100.f);
		ImGui::InputText("Device ID", macId, sizeof(macId));
	}

	if (isSerial) {
		if (needsRefresh) { Session().RefreshPorts(); needsRefresh = false; }
		auto ports = Session().GetPorts();
		ImGui::Checkbox("Manual COM port", &manualPort);
		if (manualPort || kind == DeviceKind::Ferrum) {
			ImGui::SetNextItemWidth(100.f);
			ImGui::InputText("COM Port", comPort, sizeof(comPort));
		} else if (!ports.empty()) {
			ImGui::SetNextItemWidth(200.f);
			if (ImGui::BeginCombo("COM Port", comPort[0] ? comPort : "Auto-detect")) {
				for (const auto& p : ports) {
					char label[128];
					std::snprintf(label, sizeof(label), "%s  %s", p.port.c_str(), p.name.c_str());
					if (ImGui::Selectable(label, p.port == comPort)) {
						std::strncpy(comPort, p.port.c_str(), sizeof(comPort) - 1);
						comPort[sizeof(comPort) - 1] = 0;
					}
				}
				ImGui::EndCombo();
			}
			if (ImGui::SmallButton("Refresh ports")) Session().RefreshPorts();
		} else {
			ImGui::TextDisabled("No COM ports found");
			if (ImGui::SmallButton("Refresh ports")) Session().RefreshPorts();
		}

		if (IsMakcu(kind)) {
			ImGui::SetNextItemWidth(120.f);
			ImGui::Combo("Baud", &baudIndex, baudLabels, IM_ARRAYSIZE(baudLabels));
		}
		if (kind == DeviceKind::Ferrum)
			ImGui::Checkbox("Ferrum controller mode", &ferrumController);
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	const auto status = Session().GetStatus();
	const bool connected = status.state == LinkState::Ready;
	const bool connecting = status.state == LinkState::Connecting || status.state == LinkState::Recovering;

	if (kind != DeviceKind::Off) {
		if (!connected && !connecting) {
			if (ImGui::Button("Connect", ImVec2(100, 0))) {
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
		} else {
			if (ImGui::Button("Disconnect", ImVec2(100, 0)))
				Session().Disconnect();
			ImGui::SameLine();
			if (connected && ImGui::Button("Test", ImVec2(60, 0)))
				Session().TestMovement();
		}
	}

	ImGui::Spacing();
	ImGui::Text("Status: %s", StateLabel(status.state));
	if (!status.endpoint.empty())
		ImGui::Text("Endpoint: %s", status.endpoint.c_str());
	if (status.protocolVerified)
		ImGui::TextColored(ImVec4(0.3f, 0.85f, 0.4f, 1.f), "Protocol verified");
	if (!status.error.empty())
		ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.f), "%s", status.error.c_str());
	if (status.retryInMs > 0)
		ImGui::TextDisabled("Retrying in %u ms", status.retryInMs);
	if (status.test != OutputResult::None) {
		const char* testLabels[] = { "", "Testing...", "OK", "Expired", "Cancelled", "Failed", "Unconfirmed" };
		ImGui::Text("Test: %s", testLabels[int(status.test)]);
	}
	if (status.writes)
		ImGui::TextDisabled("Writes: %llu | Failures: %llu", status.writes, status.failures);

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
		ImGui::Text("Skeletons: %u | Aim pose: %u | Pose retries: %u",
			snapshot->skeletonCount, snapshot->poseCount, snapshot->poseFailures);
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

	{
		const int monCount = Overlay::MonitorCount();
		int monIndex = Overlay::MonitorIndex();
		if (monCount <= 0) {
			ImGui::TextDisabled("No monitors detected.");
		} else {
			char preview[64] = "";
			Overlay::MonitorInfo info{};
			if (Overlay::GetMonitorInfo(monIndex, info))
				std::snprintf(preview, sizeof(preview), "%d  %dx%d%s",
					monIndex + 1, info.width, info.height, info.primary ? "  (primary)" : "");

			if (ImGui::BeginCombo("Display Monitor", preview)) {
				for (int i = 0; i < monCount; ++i) {
					char label[64];
					if (Overlay::GetMonitorInfo(i, info))
						std::snprintf(label, sizeof(label), "%d  %dx%d%s",
							i + 1, info.width, info.height, info.primary ? "  (primary)" : "");
					else
						std::snprintf(label, sizeof(label), "Monitor %d", i + 1);

					if (ImGui::Selectable(label, i == monIndex))
						Overlay::SetMonitor(i);
					if (i == monIndex)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}
	}

	ImGui::TextDisabled("Insert toggles the menu.");

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::Text("Config");
	if (ImGui::Button("Save config", ImVec2(110, 0)))
		ConfigFile::Save();
	ImGui::SameLine();
	if (ImGui::Button("Load config", ImVec2(110, 0)))
		ConfigFile::Load();
	ImGui::TextDisabled("C:\\VeoxDMA\\RobRun\\config.json");
	ImGui::TextDisabled("Also saved automatically when the menu closes.");

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

	static const char* tabs[] = { "ESP", "Device Connections", "Misc", "Settings" };
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
	case 1: DrawPanelDevices(); break;
	case 2: DrawPanelMisc(); break;
	case 3: DrawPanelSettings(attached, statusText); break;
	}

	ImGui::End();
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(2);
	PopBlueTheme();
}

} // namespace VeoxMenu
