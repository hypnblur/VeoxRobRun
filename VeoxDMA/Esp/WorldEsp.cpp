#include "pch.h"
#include "WorldEsp.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
#include "../SDK/GrenadePrediction.h"
#include "EspDraw.h"
#include "IconSprites.h"
#include "Diag.h"
#include "../SDK/RobRunMemory.h"
#include "../ImGUI/imgui.h"
#include <cfloat>
#include <cmath>
#include <cstring>
#include <unordered_set>

void DrawUnrevealedLootLabels(const RobRunReader::Snapshot* snapshot)
{
	if (!ConfigInstance.World.UnrevealedLootLabels || !snapshot)
		return;
	const auto current = RobRunReader::GetLootPreview();
	if (!current) return;
	const auto& preview = *current;
	if (preview.sessionGeneration != snapshot->sessionGeneration ||
		GetTickCount64() - preview.stampMs >= RobRunReader::LootPreview::MaxAgeMs)
		return;

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	if (display.x <= 0.f || display.y <= 0.f)
		return;
	auto* draw = ImGui::GetBackgroundDrawList();
	auto* font = ImGui::GetFont();
	const ImVec2 viewportMin(std::max(0.f, preview.viewportMin.x * display.x),
		std::max(0.f, preview.viewportMin.y * display.y));
	const ImVec2 viewportMax(std::min(display.x, preview.viewportMax.x * display.x),
		std::min(display.y, preview.viewportMax.y * display.y));
	for (const auto& label : preview.labels) {
		const ImVec2 min(label.min.x * display.x, label.min.y * display.y);
		const ImVec2 max(label.max.x * display.x, label.max.y * display.y);
		const ImVec2 clipMin(std::max(min.x, viewportMin.x), std::max(min.y, viewportMin.y));
		const ImVec2 clipMax(std::min(max.x, viewportMax.x), std::min(max.y, viewportMax.y));
		const float visibleHeight = clipMax.y - clipMin.y;
		const float width = clipMax.x - clipMin.x - 6.f;
		const float size = std::min(std::clamp((max.y - min.y) * 0.28f, 10.f, 16.f), visibleHeight - 2.f);
		if (width < 10.f || size < 8.f)
			continue;

		const char* text = label.name.c_str();
		std::string clipped;
		auto textSize = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
		if (textSize.x > width) {
			const float suffixWidth = font->CalcTextSizeA(size, FLT_MAX, 0.f, "...").x;
			if (width <= suffixWidth)
				continue;
			const char* end = nullptr;
			font->CalcTextSizeA(size, width - suffixWidth, 0.f, text, nullptr, &end);
			if (end == text)
				continue;
			clipped.assign(text, end);
			clipped += "...";
			text = clipped.c_str();
			textSize = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
		}

		const ImVec2 pos((clipMin.x + clipMax.x - textSize.x) * 0.5f,
			(clipMin.y + clipMax.y - textSize.y) * 0.5f);
		draw->PushClipRect(clipMin, clipMax, true);
		Esp::DrawOutlinedText(draw, pos, RarityColorU32(label.rarity), font, size, text);
		draw->PopClipRect();
	}
}

static bool IsEnabled(RobRunWorld::Kind kind)
{
	const auto& world = ConfigInstance.World;
	if (!world.Enabled) return false;
	switch (kind)
	{
	case RobRunWorld::Kind::Container:
		return world.LegacyContainers && world.Containers;
	case RobRunWorld::Kind::LootNode:
		return world.LegacyContainers && world.LootNodes;
	case RobRunWorld::Kind::SafeDoor:
		return world.LegacyContainers && world.SafeDoors;
	case RobRunWorld::Kind::StorageBox:
		return world.LegacyContainers && world.StorageBoxes;
	case RobRunWorld::Kind::Shelving:
		return world.LegacyContainers && world.Shelvings;
	case RobRunWorld::Kind::CollectionTable:
		return world.LegacyContainers && world.CollectionTables;
	case RobRunWorld::Kind::PlotContainer:
		return world.LegacyContainers && world.PlotContainers;
	case RobRunWorld::Kind::Loot:
	case RobRunWorld::Kind::StaticLoot:
	case RobRunWorld::Kind::RandomLoot:
		return world.LegacyLoot && world.Loot;
	case RobRunWorld::Kind::DroppedBag:
		return world.LegacyLoot && world.DroppedBags;
	case RobRunWorld::Kind::VehicleLoot:
		return world.LegacyLoot && world.VehicleLoots;
	case RobRunWorld::Kind::Cargo:
		return world.LegacyCargo && world.Cargos;
	case RobRunWorld::Kind::CargoBush:
		return world.LegacyCargo && world.CargoBushes;
	case RobRunWorld::Kind::CargoCrystal:
		return world.LegacyCargo && world.CargoCrystals;
	case RobRunWorld::Kind::CargoDrug:
		return world.LegacyCargo && world.CargoDrugs;
	case RobRunWorld::Kind::CargoCash:
		return world.LegacyCargo && world.CargoCash;
	case RobRunWorld::Kind::CargoExchange:
		return world.LegacyCargo && world.CargoExchange;
	case RobRunWorld::Kind::CargoBox:
		return world.LegacyCargo && world.CargoBoxes;
	case RobRunWorld::Kind::CargoGoods:
		return world.LegacyCargo && world.CargoGoods;
	case RobRunWorld::Kind::PlotLoot:
		return world.LegacyLoot && world.PlotLoots;
	case RobRunWorld::Kind::FurnitureLoot:
		return world.LegacyLoot && world.FurnitureLoots;
	case RobRunWorld::Kind::BuildingLoot:
		return world.LegacyLoot && world.BuildingLoots;
	case RobRunWorld::Kind::Vehicle:
	case RobRunWorld::Kind::Car:
	case RobRunWorld::Kind::Helicopter:
		return world.Vehicles;
	case RobRunWorld::Kind::Grenade:
		return world.Grenades;
	case RobRunWorld::Kind::CashLootNode:
		return world.LegacyValuables && world.CashLootNodes;
	case RobRunWorld::Kind::SettlementCash:
		return world.LegacyValuables && world.SettlementCash;
	case RobRunWorld::Kind::MoneyWorkbench:
		return world.LegacyValuables && world.MoneyWorkbenches;
	case RobRunWorld::Kind::CryptoWorkbench:
		return world.LegacyValuables && world.CryptoWorkbenches;
	case RobRunWorld::Kind::SecurityAlarm:
	case RobRunWorld::Kind::LocationSecurityAlarm:
	case RobRunWorld::Kind::SecurityPanel:
	case RobRunWorld::Kind::MetalDetector:
		return world.SecurityAlarms;
	case RobRunWorld::Kind::FirstAidCabinet:
		return world.FirstAidCabinets;
	case RobRunWorld::Kind::VehicleTrunk:
		return world.LegacyContainers && world.VehicleTrunks;
	case RobRunWorld::Kind::DrugWorkbench:
		return world.DrugWorkbenches;
	case RobRunWorld::Kind::DefenseBlock:
		return world.DefenseBlocks;
	}
	return true;
}

static ImU32 ColourFor(RobRunWorld::Kind kind)
{
	const auto& world = ConfigInstance.World;
	switch (kind)
	{
	case RobRunWorld::Kind::Container:
		return Esp::D2DColorToImU32(world.ContainerColour);
	case RobRunWorld::Kind::LootNode:
		return Esp::D2DColorToImU32(world.LootNodeColour);
	case RobRunWorld::Kind::SafeDoor:
		return Esp::D2DColorToImU32(world.SafeDoorColour);
	case RobRunWorld::Kind::StorageBox:
		return Esp::D2DColorToImU32(world.StorageBoxColour);
	case RobRunWorld::Kind::Shelving:
		return Esp::D2DColorToImU32(world.ShelvingColour);
	case RobRunWorld::Kind::CollectionTable:
		return Esp::D2DColorToImU32(world.CollectionTableColour);
	case RobRunWorld::Kind::PlotContainer:
		return Esp::D2DColorToImU32(world.PlotContainerColour);
	case RobRunWorld::Kind::Loot:
	case RobRunWorld::Kind::StaticLoot:
	case RobRunWorld::Kind::RandomLoot:
		return Esp::D2DColorToImU32(world.LootColour);
	case RobRunWorld::Kind::DroppedBag:
		return Esp::D2DColorToImU32(world.DroppedBagColour);
	case RobRunWorld::Kind::VehicleLoot:
		return Esp::D2DColorToImU32(world.VehicleLootColour);
	case RobRunWorld::Kind::Cargo:
		return Esp::D2DColorToImU32(world.CargoColour);
	case RobRunWorld::Kind::CargoBush:
		return Esp::D2DColorToImU32(world.CargoBushColour);
	case RobRunWorld::Kind::CargoCrystal:
		return Esp::D2DColorToImU32(world.CargoCrystalColour);
	case RobRunWorld::Kind::CargoDrug:
		return Esp::D2DColorToImU32(world.CargoDrugColour);
	case RobRunWorld::Kind::CargoCash:
		return Esp::D2DColorToImU32(world.CargoCashColour);
	case RobRunWorld::Kind::CargoExchange:
		return Esp::D2DColorToImU32(world.CargoExchangeColour);
	case RobRunWorld::Kind::CargoBox:
		return Esp::D2DColorToImU32(world.CargoBoxColour);
	case RobRunWorld::Kind::CargoGoods:
		return Esp::D2DColorToImU32(world.CargoGoodsColour);
	case RobRunWorld::Kind::PlotLoot:
		return Esp::D2DColorToImU32(world.PlotLootColour);
	case RobRunWorld::Kind::FurnitureLoot:
		return Esp::D2DColorToImU32(world.FurnitureLootColour);
	case RobRunWorld::Kind::BuildingLoot:
		return Esp::D2DColorToImU32(world.BuildingLootColour);
	case RobRunWorld::Kind::Vehicle:
	case RobRunWorld::Kind::Car:
	case RobRunWorld::Kind::Helicopter:
		return Esp::D2DColorToImU32(world.VehicleColour);
	case RobRunWorld::Kind::Grenade:
		return Esp::D2DColorToImU32(world.GrenadeColour);
	case RobRunWorld::Kind::CashLootNode:
		return Esp::D2DColorToImU32(world.CashLootNodeColour);
	case RobRunWorld::Kind::SettlementCash:
		return Esp::D2DColorToImU32(world.SettlementCashColour);
	case RobRunWorld::Kind::MoneyWorkbench:
		return Esp::D2DColorToImU32(world.MoneyWorkbenchColour);
	case RobRunWorld::Kind::CryptoWorkbench:
		return Esp::D2DColorToImU32(world.CryptoWorkbenchColour);
	case RobRunWorld::Kind::SecurityAlarm:
	case RobRunWorld::Kind::LocationSecurityAlarm:
	case RobRunWorld::Kind::SecurityPanel:
	case RobRunWorld::Kind::MetalDetector:
		return Esp::D2DColorToImU32(world.SecurityAlarmColour);
	case RobRunWorld::Kind::FirstAidCabinet:
		return Esp::D2DColorToImU32(world.FirstAidColour);
	case RobRunWorld::Kind::VehicleTrunk:
		return Esp::D2DColorToImU32(world.VehicleTrunkColour);
	case RobRunWorld::Kind::DrugWorkbench:
		return Esp::D2DColorToImU32(world.DrugWorkbenchColour);
	case RobRunWorld::Kind::DefenseBlock:
		return Esp::D2DColorToImU32(world.DefenseBlockColour);
	}
	return IM_COL32(255, 255, 255, 255);
}

static const char* WorkbenchName(RobRunWorld::WorkbenchKind kind)
{
	switch (kind) {
	case RobRunWorld::WorkbenchKind::GrowthTable: return "Growth Table";
	case RobRunWorld::WorkbenchKind::LabFreezer: return "Lab Freezer";
	case RobRunWorld::WorkbenchKind::ChemicalReactor: return "Chemical Reactor";
	}
	return "Workbench";
}

static std::string WorkbenchCargoLabel(const RobRunWorld::WorkbenchCargo& item)
{
	const auto* def = ConfigInstance.LootFilter.FindItemForEntity(RobRunWorld::Kind::Cargo, item.id);
	const bool known = def && (item.id == def->id || item.id == def->labelMatch);
	std::string label = std::to_string(item.count) + "x " + (known ? def->displayName : item.id);
	const bool numberedVariant = item.id.starts_with("bush_growing_") ||
		item.id.starts_with("bush_mature_") || item.id.starts_with("soft_crystal_") ||
		item.id.starts_with("hard_crystal_");
	if (known && numberedVariant && item.id.back() >= '1' && item.id.back() <= '3') {
		label += " (Variant ";
		label += item.id.back();
		label += ')';
	}

	static constexpr const char* rarityNames[] = {
		"", "Common", "Uncommon", "Rare", "Epic", "Legendary", "Exotic"
	};
	if (item.rarity > 0 && item.rarity < (int)std::size(rarityNames)) {
		label += " [";
		label += rarityNames[item.rarity];
		label += ']';
	}
	return label;
}

static void DrawWorkbench(ImDrawList* dl, ImVec2& cursor, ImFont* font, float fs,
	const RobRunWorld::WorkbenchInfo& workbench, bool showContents)
{
	Esp::DrawOutlinedText(dl, cursor, IM_COL32(175, 220, 255, 255), font, fs,
		WorkbenchName(workbench.kind));
	cursor.y += fs + 2.f;
	if (!showContents) return;

	const auto drawContent = [&](const char* text, ImU32 color) {
		Esp::DrawOutlinedText(dl, ImVec2(cursor.x + 10.f, cursor.y), color, font, fs, text);
		cursor.y += fs + 2.f;
	};
	for (const auto& item : workbench.cargo) {
		const std::string label = WorkbenchCargoLabel(item);
		drawContent(label.c_str(), IM_COL32(235, 235, 235, 255));
	}
	if (!workbench.contentsAvailable)
		drawContent(workbench.cargo.empty() ? "Contents unavailable" : "Some contents unavailable",
			IM_COL32(255, 200, 110, 255));
	else if (workbench.cargo.empty())
		drawContent("Empty", IM_COL32(190, 190, 190, 255));
}

static int CapWorldDistance(int range)
{
	const int global = ConfigInstance.LootFilter.GlobalMaxDistance;
	if (global <= 0) return range;
	return range > 0 ? std::min(range, global) : global;
}

static Vector3 WorldRangeOrigin(const Vector3& player)
{
	if (RobRunMemory::Finite(player) && !player.IsZero()) return player;
	RobRunCamera::CameraState camera{};
	if (RobRunReader::GetFreshCamera(camera) && RobRunMemory::Finite(camera.position))
		return camera.position;
	return {};
}

void DrawGrenadePrediction(const RobRunReader::Snapshot* snapshot)
{
	if (!snapshot || !ConfigInstance.World.Enabled || !ConfigInstance.World.GrenadePrediction) return;
	const auto preview = RobRunReader::GetGrenadePrediction();
	if (!preview || preview->sessionGeneration != snapshot->sessionGeneration ||
		GetTickCount64() - preview->stampMs >= RobRunGrenades::Preview::MaxAgeMs) return;

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const int screenW = static_cast<int>(display.x);
	const int screenH = static_cast<int>(display.y);
	Matrix4x4 viewProjection{};
	if (!RobRunReader::ResolveViewProjection(viewProjection, screenW, screenH)) return;

	const Vector3 localPosition = WorldRangeOrigin(snapshot->localPosition);
	ImDrawList* draw = ImGui::GetBackgroundDrawList();
	const float range = static_cast<float>(CapWorldDistance(0));
	const bool limitDistance = range > 0.f && !localPosition.IsZero();
	const ImU32 color = Esp::D2DColorToImU32(ConfigInstance.World.GrenadeColour);
	const ImU32 outline = IM_COL32(0, 0, 0, 210);
	auto project = [&](const CollisionBVH::Float3& point, Vector2& screen) {
		return RobRunReader::ProjectWorld(viewProjection, { point.x, point.y, point.z }, screenW, screenH, screen);
	};
	auto drawLine = [&](const Vector2& from, const Vector2& to, float thickness) {
		draw->AddLine(ImVec2(from.x, from.y), ImVec2(to.x, to.y), outline, thickness + 2.f);
		draw->AddLine(ImVec2(from.x, from.y), ImVec2(to.x, to.y), color, thickness);
	};
	auto drawEffectRing = [&](const CollisionBVH::Float3& center, float radius, bool dashed) {
		constexpr int segments = 48;
		constexpr float radPerSegment = 6.2831853f / segments;
		Vector2 previous{};
		bool previousVisible = project({ center.x + radius, center.y + 0.04f, center.z }, previous);
		for (int i = 1; i <= segments; ++i) {
			const float angle = i * radPerSegment;
			const CollisionBVH::Float3 point{ center.x + radius * std::cos(angle),
				center.y + 0.04f, center.z + radius * std::sin(angle) };
			Vector2 current{};
			const bool visible = project(point, current);
			if (visible && previousVisible && (!dashed || ((i - 1) / 2) % 2 == 0))
				drawLine(previous, current, 1.5f);
			previous = current;
			previousVisible = visible;
		}
	};
	auto drawPath = [&](const RobRunGrenades::Path& path, bool dashed, const char* marker, float radius) {
		if (path.points.empty()) return;
		Vector2 previous{};
		bool previousVisible = project(path.points.front(), previous);
		for (std::size_t i = 1; i < path.points.size(); ++i) {
			Vector2 current{};
			const bool visible = project(path.points[i], current);
			const bool inDash = !dashed || ((i - 1) / 2) % 2 == 0;
			if (visible && previousVisible && inDash) drawLine(previous, current, 2.f);
			previous = current;
			previousVisible = visible;
		}
		if (path.hitsSurface && radius > 0.f)
			drawEffectRing(path.points.back(), radius, path.estimatedImpact);
		if (path.hitsSurface && previousVisible && previous.x >= 0 && previous.y >= 0 &&
			previous.x < screenW && previous.y < screenH) {
			const ImVec2 center(previous.x, previous.y);
			draw->AddCircleFilled(center, 8.f, outline);
			draw->AddCircle(center, 8.f, color, 0, 1.5f);
			const ImVec2 size = ImGui::CalcTextSize(marker);
			draw->AddText(ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f),
				IM_COL32_WHITE, marker);
		}
	};
	for (const auto& trajectory : preview->trajectories) {
		const auto& position = trajectory.flight.points.front();
		if (limitDistance && Distance(localPosition, { position.x, position.y, position.z }) > range)
			continue;
		const bool reboundLanding = trajectory.rebound.hitsSurface;
		drawPath(trajectory.flight, false, "1", reboundLanding ? 0.f : trajectory.effectRadius);
		drawPath(trajectory.rebound, true, "2", reboundLanding ? trajectory.effectRadius : 0.f);
	}
}

static std::unordered_set<int> DrawWorkbenchLabels(const RobRunWorld::PlotState& plots,
	const Vector3& localPosition, const Matrix4x4& viewProjection, int screenW, int screenH,
	ImDrawList* dl, ImFont* font)
{
	std::unordered_set<int> drawn;
	if (!plots.valid || !RobRunMemory::Finite(localPosition) || localPosition.IsZero()) return drawn;

	const auto& config = ConfigInstance.World;
	const float range = static_cast<float>(CapWorldDistance(config.HouseMaxDistance));
	for (const auto& plot : plots.plots) {
		if (range > 0.f && Distance(plot.position, localPosition) > range) continue;
		for (const auto& room : plot.outbuildings) {
			for (const auto& workbench : room.workbenches) {
				if (!workbench.hasPosition || (range > 0.f && Distance(workbench.position, localPosition) > range) ||
					(workbench.entityId >= 0 && drawn.contains(workbench.entityId))) continue;
				Vector2 screen;
				if (!RobRunReader::ProjectWorld(viewProjection, workbench.position, screenW, screenH, screen) ||
					screen.x < 0.f || screen.x >= screenW || screen.y < 0.f || screen.y >= screenH) continue;

				ImVec2 cursor(screen.x + 6.f, screen.y);
				DrawWorkbench(dl, cursor, font, static_cast<float>(config.FontSize), workbench, config.ShowWorkbenchContents);
				if (workbench.entityId >= 0) drawn.insert(workbench.entityId);
			}
		}
	}
	return drawn;
}

void DrawWorldEntities(const RobRunReader::Snapshot* snapshot)
{
	if (!ConfigInstance.World.Enabled)
		return;

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const int screenW = (int)display.x;
	const int screenH = (int)display.y;
	if (screenW <= 0 || screenH <= 0)
		return;

	if (!snapshot)
		return;

	Matrix4x4 viewProjection{};
	if (!RobRunReader::ResolveViewProjection(viewProjection, screenW, screenH))
		return;

	const Vector3 localPos = WorldRangeOrigin(snapshot->localPosition);
	const bool haveLocalPosition = !localPos.IsZero();
	const char* rangeOrigin = !haveLocalPosition ? "unavailable" :
		(RobRunMemory::Finite(snapshot->localPosition) && !snapshot->localPosition.IsZero() ? "player" : "camera");
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	ImFont* font = ImGui::GetFont();
	std::unordered_set<int> detailedWorkbenches;
	if (ConfigInstance.World.HouseEsp && ConfigInstance.World.ShowWorkbenchLabels && snapshot->plots) {
		detailedWorkbenches = DrawWorkbenchLabels(*snapshot->plots, localPos,
			viewProjection, screenW, screenH, dl, font);
	}
	if (!snapshot->world) return;

	// What the snapshot actually contains, once every few seconds. This is how we
	// tell "cargo is filtered out" from "cargo never reached the entity list".
	if (kProbes) {
		static ULONGLONG s_lastProbe = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - s_lastProbe > 5000) {
			s_lastProbe = now;
			int cargo = 0, containers = 0, staticLoot = 0, randomLoot = 0, vehicles = 0, loots = 0;
			std::string cargoLabels, lootLabels;
			for (const auto& e : snapshot->world->entities) {
				switch (e.kind) {
				case RobRunWorld::Kind::Cargo:
					++cargo;
					if (cargoLabels.size() < 90) cargoLabels += "[" + e.label + "]";
					break;
				case RobRunWorld::Kind::Container:  ++containers; break;
				case RobRunWorld::Kind::StaticLoot: ++staticLoot; break;
				case RobRunWorld::Kind::RandomLoot: ++randomLoot; break;
				case RobRunWorld::Kind::Car:
				case RobRunWorld::Kind::Helicopter:
				case RobRunWorld::Kind::Vehicle:    ++vehicles; break;
				default:                            ++loots; break;
				}
				if (e.kind == RobRunWorld::Kind::StaticLoot && lootLabels.size() < 90)
					lootLabels += "[" + e.label + "]";
			}
			VeoxDiag("probe: entities=%zu cargo=%d containers=%d static=%d random=%d vehicles=%d other=%d "
				"cargoLabels=%s staticLabels=%s",
				snapshot->world->entities.size(), cargo, containers, staticLoot, randomLoot, vehicles, loots,
				cargoLabels.c_str(), lootLabels.c_str());
		}
	}

	if (kProbes) {
		static std::uint64_t lastCensus = 0;
		const std::uint64_t nowCensus = GetTickCount64();
		if (nowCensus - lastCensus > 3000) {
			lastCensus = nowCensus;
			const auto& dbg = ConfigInstance.LootFilter;
			std::string randomLabels, otherLabels;
			int random = 0, randomMatched = 0, other = 0, otherMatched = 0;
			for (const auto& e : snapshot->world->entities) {
				const auto* def = dbg.FindItemForEntity(e.kind, e.label);
				if (e.kind == RobRunWorld::Kind::RandomLoot) {
					++random;
					if (def) ++randomMatched;
					if (randomLabels.size() < 160) randomLabels += e.label + (def ? "*" : "!") + " ";
				} else if (e.kind == RobRunWorld::Kind::Loot ||
				           e.kind == RobRunWorld::Kind::StaticLoot ||
				           e.kind == RobRunWorld::Kind::DroppedBag) {
					++other;
					if (def) ++otherMatched;
					if (otherLabels.size() < 160) otherLabels += e.label + (def ? "*" : "!") + " ";
				}
			}
			VeoxDiag("esp labels: random %d/%d [%s] | loot %d/%d [%s]",
				randomMatched, random, randomLabels.c_str(), otherMatched, other, otherLabels.c_str());
		}
	}

	// Find the vehicle currently occupied by the local player so we don't draw a
	// 3D car icon/price directly on top of our own vehicle while driving.
	int localVehicleId = -1;
	for (const auto& pe : snapshot->entities) {
		if (pe.entityId == snapshot->localEntityId) {
			if (pe.inVehicle && pe.vehicleId > 0)
				localVehicleId = pe.vehicleId;
			break;
		}
	}

	for (const auto& entity : snapshot->world->entities)
	{
		if (entity.kind == RobRunWorld::Kind::DrugWorkbench && detailedWorkbenches.contains(entity.id))
			continue;

		const auto& lf = ConfigInstance.LootFilter;
		const LootFilter::ItemDef* itemDef = nullptr;
		const ItemSettings* itemCfg = nullptr;

		const bool isVehicleEntity =
			entity.kind == RobRunWorld::Kind::Car ||
			entity.kind == RobRunWorld::Kind::Helicopter ||
			entity.kind == RobRunWorld::Kind::Vehicle;
		if (isVehicleEntity && (!ConfigInstance.World.Vehicles ||
			(localVehicleId > 0 && entity.id == localVehicleId)))
			continue;

		if (lf.Enabled) {
			itemDef = lf.FindItemForEntity(entity.kind, entity.label);
			if (itemDef) {
				itemCfg = &lf.GetItem(itemDef->id);
				if (!itemCfg->visible) continue;
			}
		}

		// After the world and vehicle switches, choose the item rules:
		//   filter off  -> the legacy per-kind switches
		//   filter on   -> the catalog, and when nothing matched, the generic row
		//                  for that kind. Never the legacy switches: those would
		//                  silently drop loot the catalog simply could not name.
		if (!lf.Enabled) {
			if (!IsEnabled(entity.kind)) continue;
		} else if (!itemDef) {
			itemDef = lf.FindItemForEntity(entity.kind, "");
			if (itemDef) itemCfg = &lf.GetItem(itemDef->id);
		}
		if (itemDef && !itemCfg)
			itemCfg = &lf.GetItem(itemDef->id);
		if (lf.Enabled && itemCfg && !itemCfg->visible)
			continue;

		// Use 80-500 Hz live vehicle position from fastVehicles so moving cars
		// and helicopters stay synced instead of lagging behind the 1 Hz world worker.
		Vector3 entityPos = entity.position;
		if (isVehicleEntity && !snapshot->fastVehicles.empty()) {
			for (const auto& fv : snapshot->fastVehicles) {
				if (fv.id == entity.id) {
					if (std::isfinite(fv.position.x) && std::isfinite(fv.position.y) && std::isfinite(fv.position.z))
						entityPos = fv.position;
					break;
				}
			}
		}

		const float dx = entityPos.x - localPos.x;
		const float dy = entityPos.y - localPos.y;
		const float dz = entityPos.z - localPos.z;
		const float distSq = dx * dx + dy * dy + dz * dz;
		const bool hasItemRange = itemCfg && !itemCfg->useGlobalDistance && itemCfg->maxDistance > 0;

		const int markerRange = !lf.Enabled ? ConfigInstance.World.MaxDistance :
			(hasItemRange ? itemCfg->maxDistance :
				lf.ResolvedMaxDistance(itemDef ? itemDef->category : LootFilter::Category::Misc));
		const int maxDist = CapWorldDistance(markerRange);

		// Contents of a trunk or table draw only when their row opts in.
		if (entity.inTable &&
			(!lf.Enabled || !itemCfg || (itemCfg->flags & LootFilter::RenderInTable) == 0))
			continue;
		// A trunk item draws only when its own row opts in.
		if (entity.inVehicle &&
			(!lf.Enabled || !itemCfg || (itemCfg->flags & LootFilter::RenderInVehicle) == 0))
			continue;

		// Keep items with an explicit limit hidden until an origin is available.
		const float maxDistF = static_cast<float>(maxDist);
		const bool outsideRange = maxDist > 0 &&
			(!haveLocalPosition || distSq > maxDistF * maxDistF);
		if (kProbes && (entity.label == "perfume_3" || (itemDef && std::strcmp(itemDef->id, "perfume_3") == 0))) {
			static std::unordered_set<std::string> logged;
			const std::string key = std::to_string(entity.id) + ":" + std::to_string(maxDist) +
				":" + rangeOrigin + ":" + std::to_string(hasItemRange) + ":" + std::to_string(outsideRange);
			if (logged.size() < 64 && logged.insert(key).second)
				VeoxDiag("trio range: placement=%d row='%s' origin=%s distance=%.1f limit=%d perItem=%d culled=%d",
					entity.id, itemDef ? itemDef->id : "", rangeOrigin,
					haveLocalPosition ? std::sqrt(distSq) : 0.f, maxDist,
					hasItemRange, outsideRange);
		}
		if (outsideRange)
			continue;

		if (lf.Enabled && lf.FilterByMinPrice && lf.MinPrice > 0) {
			if (entity.hasCashAmount && entity.cashAmount < lf.MinPrice)
				continue;
		} else if (ConfigInstance.World.MinValueFilter > 0) {
			if (entity.hasCashAmount && entity.cashAmount < ConfigInstance.World.MinValueFilter)
				continue;
		}

		// HideEmpty only applies to rows that opted in: a growing bush reports
		// empty while it regrows and must stay on screen.
		if (lf.Enabled && lf.HideEmpty && entity.hasEmptyFlag && entity.empty &&
			(!itemDef || itemDef->hideWhenEmpty))
			continue;

		Vector2 screen;
		if (!RobRunReader::ProjectWorld(
			viewProjection, entityPos, screenW, screenH, screen))
			continue;

		// Skip off-screen world items before allocating/measuring text or
		// emitting draw commands.
		if (screen.x < -120.f || screen.x > (float)screenW + 120.f ||
			screen.y < -60.f || screen.y > (float)screenH + 60.f)
			continue;

		const bool filterOn = lf.Enabled;
		// A missing catalog row must not drop back to the legacy fixed sizes:
		// dot, font and icon scale stay under the loot filter's controls.
		std::uint8_t flags = itemCfg
			? itemCfg->flags
			: (std::uint8_t)(LootFilter::RenderDot | LootFilter::RenderName);
		// A row with every flag cleared draws absolutely nothing, which is
		// indistinguishable from a broken ESP. Treat "no flags" as "show the
		// basics" so a zeroed config can never make an entity invisible.
		if (filterOn && flags == 0)
			flags = (std::uint8_t)(LootFilter::RenderDot | LootFilter::RenderName);
		const LootFilter::Category sizingCat = itemDef ? itemDef->category : LootFilter::Category::Misc;
		const float labelSize = filterOn
			? (float)lf.ResolvedFontSize(sizingCat)
			: (float)ConfigInstance.World.FontSize;

		// Marker colour: the row's own colour; the value tiers still tint it so
		// a high-value find reads at a glance.
		ImU32 markerCol = itemCfg
			? Esp::D2DColorToImU32(itemCfg->color)
			: ColourFor(entity.kind);
		if (lf.Enabled && entity.hasCashAmount) {
			if (entity.cashAmount >= lf.HighValueThreshold)
				markerCol = Esp::D2DColorToImU32(lf.HighValueColour);
			else if (entity.cashAmount >= lf.MidValueThreshold)
				markerCol = Esp::D2DColorToImU32(lf.MidValueColour);
		}

		const bool wantIcon  = filterOn && (flags & LootFilter::RenderIcon);
		const bool wantDot   = !filterOn || (flags & LootFilter::RenderDot);
		const bool wantName  = !filterOn || (flags & LootFilter::RenderName);
		const bool wantPrice = !filterOn || (flags & LootFilter::RenderPrice);
		const bool wantDist  = filterOn
			? (flags & LootFilter::RenderDistance) != 0
			: (ConfigInstance.World.ShowDistance != false);   // legacy world ESP

		// Icon pass: any catalogue sprite, served from OrbitResources.dll.
		// Vehicles label themselves with their exact model+paint id, so try
		// that sprite first and fall back to the family row's sprite.
		float iconPx = 0.f;
		if (wantIcon) {
			const char* spriteName = nullptr;
			if (isVehicleEntity && !entity.label.empty() &&
				IconSprites::Get(entity.label.c_str()).srv) {
				spriteName = entity.label.c_str();
			} else if (itemDef && itemDef->iconSprite[0]) {
				spriteName = itemDef->iconSprite;
			}
			if (spriteName) {
				const auto icon = IconSprites::Get(spriteName);
				if (icon.srv) {
					iconPx = (float)lf.ResolvedIconSize(sizingCat);
					// Keep the sprite's aspect: a 512x256 art is drawn 2:1, not
					// stretched into a square.
					float drawW = iconPx;
					float drawH = iconPx;
					if (icon.width > 0 && icon.height > 0) {
						if (icon.width >= icon.height)
							drawH = iconPx * (float)icon.height / (float)icon.width;
						else
							drawW = iconPx * (float)icon.width / (float)icon.height;
					}
					// Sits above the dot so the dot still marks the world point.
					const float bottom = screen.y - (wantDot ? lf.ResolvedDotSize(sizingCat) : 0.f) - 1.f;
					dl->AddImage(reinterpret_cast<ImTextureID>(icon.srv),
						ImVec2(screen.x - drawW * 0.5f, bottom - drawH),
						ImVec2(screen.x + drawW * 0.5f, bottom));
					iconPx = drawH;
				}
			}
		}

		if (wantDot) {
			const float dotSize = filterOn ? lf.ResolvedDotSize(sizingCat) : 2.5f;
			dl->AddCircleFilled(ImVec2(screen.x, screen.y), dotSize, markerCol);
		}

		// Text stack under the marker: distance, name (rarity colour), price.
		// Matches the reference layout: icon above the dot, then the lines.
		float lineY = screen.y + (wantDot ? (filterOn ? lf.ResolvedDotSize(sizingCat) : 2.5f) : 0.f) + 2.f;
		ImFont* useFont = font;
		auto drawLine = [&](const std::string& text, ImU32 col) {
			if (text.empty()) return;
			ImVec2 size = useFont
				? useFont->CalcTextSizeA(labelSize, FLT_MAX, 0.f, text.c_str())
				: ImGui::CalcTextSize(text.c_str());
			Esp::DrawOutlinedText(dl,
				ImVec2(screen.x - size.x * 0.5f, lineY),
				col, useFont, labelSize, text.c_str());
			lineY += size.y + 1.f;
		};

		if (wantDist && haveLocalPosition)
			drawLine(std::to_string(static_cast<int>(std::sqrt(distSq))) + "m", IM_COL32(205, 205, 205, 235));

		if (wantName) {
			// The row owns the display name: the entity label is often the raw
			// item id ("phones_3"), which is not what belongs on screen.
			const bool unresolvedRandom = entity.kind == RobRunWorld::Kind::RandomLoot &&
				entity.label.empty();
			std::string label = (filterOn && itemDef && itemDef->displayName[0])
				? itemDef->displayName
				: entity.label;
			if (unresolvedRandom)
				label = "Loot - placement " + std::to_string(entity.id);
			else if (filterOn && itemCfg && !itemCfg->customName.empty())
				label = itemCfg->customName;

			if (entity.label.starts_with("alcohol_")) {
				static std::unordered_set<std::string> logged;
				const std::string key = std::to_string(static_cast<int>(entity.kind)) + ":" +
					std::to_string(entity.id) + ":" + entity.label + ":" + label;
				if (logged.size() < 64 && logged.insert(key).second) {
					VeoxDiag("bottle identity: kind=%d placement=%d pos=(%.1f,%.1f,%.1f) id='%s' row='%s' name='%s'",
						static_cast<int>(entity.kind), entity.id, entity.position.x,
						entity.position.y, entity.position.z, entity.label.c_str(),
						itemDef ? itemDef->id : "", label.c_str());
				}
			}

			// Rename "Bush" → "Weed" in display labels + optional hide
			{
				bool isBush = false;
				for (std::size_t p = 0; (p = label.find("Bush", p)) != std::string::npos; p += 4) {
					label.replace(p, 4, "Weed");
					isBush = true;
				}
				if (isBush && !lf.Enabled && !ConfigInstance.World.Weed)
					continue;
			}

			if (entity.kind == RobRunWorld::Kind::Container &&
				ConfigInstance.World.ShowEmpty && entity.hasEmptyFlag) {
				label += entity.empty ? " [empty]" : " [loot]";
			} else if (entity.kind == RobRunWorld::Kind::DefenseBlock && entity.hasEmptyFlag) {
				label += entity.empty ? " [empty]" : " [armed]";
			}

			// Game rarity (RarityItem: 1 Common .. 5 Legendary, 6 Exotic) wins;
			// the catalog rarity is the fallback when the description is unread.
			LootFilter::Rarity nameRarity = filterOn && itemDef
				? itemDef->rarity
				: LootFilter::Rarity::Common;
			if (entity.rarity > 0) {
				nameRarity = LootRarityFromGame(entity.rarity, LootFilter::Rarity::Legendary);
			}
			const ImU32 nameCol = filterOn ? RarityColorU32(nameRarity) : markerCol;
			drawLine(label, nameCol);
		}

		if (wantPrice) {
			std::string price;
			if (entity.hasCashAmount) {
				price = "$" + FormatCash(entity.cashAmount);
				if (entity.kind == RobRunWorld::Kind::MoneyWorkbench &&
					entity.cleanCash > 0 && entity.dirtyCash > 0) {
					price += " (C:$" + FormatCash(entity.cleanCash) + " D:$" + FormatCash(entity.dirtyCash) + ")";
				}
			} else {
				// Which number to show: the live sell price the game pays, the
				// catalog (buy) price, or the unlock price when we have it.
				const int mode = std::clamp(lf.PriceMode, 0, 2);
				int amount = 0;
				if (mode == 2)
					amount = 0;                       // unlock price not known yet
				else if (mode == 0 && entity.hasValue && entity.value > 0) {
					const bool isCargo =
						entity.kind == RobRunWorld::Kind::Cargo ||
						entity.kind == RobRunWorld::Kind::CargoBush ||
						entity.kind == RobRunWorld::Kind::CargoCrystal ||
						entity.kind == RobRunWorld::Kind::CargoDrug ||
						entity.kind == RobRunWorld::Kind::CargoCash ||
						entity.kind == RobRunWorld::Kind::CargoExchange ||
						entity.kind == RobRunWorld::Kind::CargoBox ||
						entity.kind == RobRunWorld::Kind::CargoGoods;
					const float bonus = isCargo
						? LootFilterConfig::CargoRevenueBonus(lf.AuthorityLevel)
						: LootFilterConfig::LootRevenueBonus(lf.AuthorityLevel);
					amount = (int)std::lround((double)entity.value * (1.0 + (double)bonus));
				}
				else if (mode == 1 && filterOn && itemDef && itemDef->price > 0)
					amount = (int)itemDef->price;
				else if (mode == 0 && filterOn && itemDef && itemDef->price > 0)
					amount = (int)itemDef->price;   // no live price yet: catalog fallback
				price = amount > 0 ? ("$" + FormatCash(amount)) : std::string();
			}
			// No price known: draw nothing rather than a placeholder line.
			if (!price.empty())
				drawLine(price, IM_COL32(235, 235, 235, 240));
		}
	}
}

// --- House info popup panel -----------------------------------------------

static void DrawBuildingLine(ImDrawList* dl, ImVec2& cursor, float indent,
	const RobRunWorld::BuildingInfo& b, ImFont* font, float fs)
{
	if (b.name.empty()) return;
	std::string line = b.name;
	if (b.hasDefense) {
		line += b.isFusesEmpty ? "  [no fuses]" : "  [armed]";
		if (b.isRaided) line += "  RAIDED";
	}
	if (b.doorCount > 0)
		line += "  doors:" + std::to_string(b.doorsOpen) + "/" + std::to_string(b.doorCount);

	const ImU32 col = b.isRaided ? IM_COL32(255, 80, 80, 255) :
		(b.isFusesEmpty ? IM_COL32(255, 200, 60, 255) : IM_COL32(200, 255, 200, 255));
	Esp::DrawOutlinedText(dl, ImVec2(cursor.x + indent, cursor.y), col, font, fs, line.c_str());
	cursor.y += fs + 2.f;
}

void DrawHouseInfo(const RobRunReader::Snapshot* snapshot)
{
	const auto& cfg = ConfigInstance.World;
	if (!cfg.Enabled || !cfg.HouseEsp)
		return;

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const int screenW = (int)display.x;
	const int screenH = (int)display.y;
	if (screenW <= 0 || screenH <= 0) return;

	if (!snapshot || !snapshot->plots || !snapshot->plots->valid) return;

	Matrix4x4 viewProjection{};
	if (!RobRunReader::ResolveViewProjection(viewProjection, screenW, screenH))
		return;

	const Vector3 localPos = WorldRangeOrigin(snapshot->localPosition);
	const bool haveLocal = !localPos.IsZero();
	const int range = CapWorldDistance(cfg.HouseMaxDistance);
	if (range > 0 && !haveLocal) return;
	const float cx = display.x * 0.5f;
	const float cy = display.y * 0.5f;
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	ImFont* font = ImGui::GetFont();
	const float fs = (float)cfg.FontSize;

	// Find the house closest to screen center within range
	const RobRunWorld::PlotInfo* best = nullptr;
	float bestDist2 = FLT_MAX;
	Vector2 bestScreen{};
	int bestMetres = 0;

	for (const auto& plot : snapshot->plots->plots) {
		const float dx = plot.position.x - localPos.x;
		const float dy = plot.position.y - localPos.y;
		const float dz = plot.position.z - localPos.z;
		const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (range > 0 && distance > static_cast<float>(range)) continue;
		const int metres = static_cast<int>(distance);

		Vector2 screen;
		if (!RobRunReader::ProjectWorld(viewProjection, plot.position, screenW, screenH, screen))
			continue;

		const float sx = screen.x - cx;
		const float sy = screen.y - cy;
		const float d2 = sx * sx + sy * sy;
		if (d2 < bestDist2) {
			bestDist2 = d2;
			best = &plot;
			bestScreen = screen;
			bestMetres = metres;
		}
	}

	if (!best) return;
	// Only show when looking roughly at the house (within 15% of screen)
	const float threshold = display.x * 0.15f;
	if (bestDist2 > threshold * threshold) return;

	// Resolve owner name from player list
	std::string ownerLabel;
	if (!best->ownerName.empty() && best->ownerName[0] >= '0' && best->ownerName[0] <= '9') {
		const std::uint64_t pubId = std::stoull(best->ownerName);
		for (const auto& e : snapshot->entities) {
			if (e.publicId == pubId && !e.nickname.empty()) {
				ownerLabel = RobRunMemory::WideToUtf8(e.nickname);
				break;
			}
		}
	} else if (!best->ownerName.empty()) {
		ownerLabel = best->ownerName;
	}
	if (ownerLabel.empty()) ownerLabel = "No owner";

	// --- Draw the panel ---
	const float panelX = bestScreen.x + 20.f;
	float panelY = bestScreen.y - 20.f;
	const float lineH = fs + 2.f;
	const float indent = 10.f;
	ImVec2 cursor(panelX, panelY);

	// Header: slot / owner / distance
	{
		std::string header = "Slot " + std::to_string(best->slotIndex);
		if (!ownerLabel.empty()) header += " | " + ownerLabel;
		header += " [" + std::to_string(bestMetres) + "m]";
		Esp::DrawOutlinedText(dl, cursor, IM_COL32(255, 255, 255, 255), font, fs + 2.f, header.c_str());
		cursor.y += fs + 6.f;
	}

	// DEFENCE
	{
		bool anyRaided = false;
		bool allEmpty = true;
		int totalDoors = 0, totalOpen = 0;

		auto accum = [&](const RobRunWorld::BuildingInfo& b) {
			if (b.hasDefense) {
				if (b.isRaided) anyRaided = true;
				if (!b.isFusesEmpty) allEmpty = false;
			}
			totalDoors += b.doorCount;
			totalOpen += b.doorsOpen;
		};
		accum(best->mainBuilding);
		accum(best->compoundWall);
		for (const auto& ob : best->outbuildings) accum(ob);

		Esp::DrawOutlinedText(dl, cursor, IM_COL32(255, 160, 60, 255), font, fs, "DEFENCE");
		cursor.y += lineH;

		std::string raidStatus = anyRaided ? "Raided" : "Not raided";
		std::string fuseStatus = allEmpty ? "No fuses" : "Fuses armed";
		std::string doorStatus = "Doors: " + std::to_string(totalOpen) + "/" + std::to_string(totalDoors) + " open";

		Esp::DrawOutlinedText(dl, ImVec2(cursor.x + indent, cursor.y),
			anyRaided ? IM_COL32(255, 80, 80, 255) : IM_COL32(200, 255, 200, 255),
			font, fs, raidStatus.c_str());
		cursor.y += lineH;

		Esp::DrawOutlinedText(dl, ImVec2(cursor.x + indent, cursor.y),
			allEmpty ? IM_COL32(255, 200, 60, 255) : IM_COL32(200, 255, 200, 255),
			font, fs, fuseStatus.c_str());
		cursor.y += lineH;

		if (best->mainBuilding.hasDefense && best->mainBuilding.raidLockTs > 0) {
			Esp::DrawOutlinedText(dl, ImVec2(cursor.x + indent, cursor.y),
				IM_COL32(255, 120, 120, 255), font, fs, "Raid locked");
			cursor.y += lineH;
		}

		Esp::DrawOutlinedText(dl, ImVec2(cursor.x + indent, cursor.y),
			IM_COL32(220, 220, 220, 255), font, fs, doorStatus.c_str());
		cursor.y += lineH + 2.f;
	}

	// BUILDINGS
	{
		Esp::DrawOutlinedText(dl, cursor, IM_COL32(100, 200, 255, 255), font, fs, "BUILDINGS");
		cursor.y += lineH;

		DrawBuildingLine(dl, cursor, indent, best->mainBuilding, font, fs);
		if (!best->compoundWall.name.empty())
			DrawBuildingLine(dl, cursor, indent, best->compoundWall, font, fs);
		for (const auto& ob : best->outbuildings) {
			DrawBuildingLine(dl, cursor, indent, ob, font, fs);
			ImVec2 workbenchCursor(cursor.x + indent * 2.f, cursor.y);
			for (const auto& workbench : ob.workbenches)
				DrawWorkbench(dl, workbenchCursor, font, fs, workbench, cfg.ShowWorkbenchContents);
			if (cfg.ShowWorkbenchContents && ob.workbenchesUnavailable) {
				const char* status = ob.workbenches.empty() ? "Contents unavailable" : "Some contents unavailable";
				Esp::DrawOutlinedText(dl, workbenchCursor, IM_COL32(255, 200, 110, 255), font, fs, status);
				workbenchCursor.y += lineH;
			} else if (cfg.ShowWorkbenchContents && ob.isWorkbenchRoom && ob.workbenches.empty()) {
				Esp::DrawOutlinedText(dl, workbenchCursor, IM_COL32(190, 190, 190, 255), font, fs,
					"No workbenches found");
				workbenchCursor.y += lineH;
			}
			cursor.y = workbenchCursor.y;
		}
		cursor.y += 2.f;
	}
}
