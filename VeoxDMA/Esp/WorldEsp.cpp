#include "pch.h"
#include "WorldEsp.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
#include "EspDraw.h"
#include "../ImGUI/imgui.h"
#include <cfloat>
#include <cmath>

static bool IsEnabled(RobRunWorld::Kind kind)
{
	const auto& world = ConfigInstance.World;
	if (!world.Enabled) return false;
	switch (kind)
	{
	case RobRunWorld::Kind::Container:
		return world.Containers;
	case RobRunWorld::Kind::LootNode:
		return world.Containers || world.LootNodes;
	case RobRunWorld::Kind::SafeDoor:
		return world.Containers || world.SafeDoors;
	case RobRunWorld::Kind::StorageBox:
		return world.Containers || world.StorageBoxes;
	case RobRunWorld::Kind::Shelving:
		return world.Containers || world.Shelvings;
	case RobRunWorld::Kind::CollectionTable:
		return world.Containers || world.CollectionTables;
	case RobRunWorld::Kind::PlotContainer:
		return world.Containers || world.PlotContainers;
	case RobRunWorld::Kind::Loot:
	case RobRunWorld::Kind::StaticLoot:
	case RobRunWorld::Kind::RandomLoot:
		return world.Loot;
	case RobRunWorld::Kind::DroppedBag:
		return world.Loot || world.DroppedBags;
	case RobRunWorld::Kind::VehicleLoot:
		return world.Loot || world.VehicleLoots;
	case RobRunWorld::Kind::Cargo:
		return world.Loot || world.Cargos;
	case RobRunWorld::Kind::PlotLoot:
		return world.Loot || world.PlotLoots;
	case RobRunWorld::Kind::FurnitureLoot:
		return world.Loot || world.FurnitureLoots;
	case RobRunWorld::Kind::BuildingLoot:
		return world.Loot || world.BuildingLoots;
	case RobRunWorld::Kind::Vehicle:
	case RobRunWorld::Kind::Car:
	case RobRunWorld::Kind::Helicopter:
		return world.Vehicles;
	case RobRunWorld::Kind::Grenade:
		return world.Grenades;
	case RobRunWorld::Kind::CashLootNode:
		return world.Valuables || world.CashLootNodes;
	case RobRunWorld::Kind::SettlementCash:
		return world.Valuables || world.SettlementCash;
	case RobRunWorld::Kind::MoneyWorkbench:
		return world.Valuables || world.MoneyWorkbenches;
	case RobRunWorld::Kind::CryptoWorkbench:
		return world.Valuables || world.CryptoWorkbenches;
	case RobRunWorld::Kind::SecurityAlarm:
	case RobRunWorld::Kind::LocationSecurityAlarm:
		return world.SecurityAlarms;
	case RobRunWorld::Kind::FirstAidCabinet:
		return world.FirstAidCabinets;
	case RobRunWorld::Kind::VehicleTrunk:
		return world.Containers || world.VehicleTrunks;
	case RobRunWorld::Kind::PointOfInterest:
		return world.PointsOfInterest;
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
		return Esp::D2DColorToImU32(world.SecurityAlarmColour);
	case RobRunWorld::Kind::FirstAidCabinet:
		return Esp::D2DColorToImU32(world.FirstAidColour);
	case RobRunWorld::Kind::VehicleTrunk:
		return Esp::D2DColorToImU32(world.VehicleTrunkColour);
	case RobRunWorld::Kind::PointOfInterest:
		return Esp::D2DColorToImU32(world.PointOfInterestColour);
	case RobRunWorld::Kind::DrugWorkbench:
		return Esp::D2DColorToImU32(world.DrugWorkbenchColour);
	case RobRunWorld::Kind::DefenseBlock:
		return Esp::D2DColorToImU32(world.DefenseBlockColour);
	}
	return IM_COL32(255, 255, 255, 255);
}

void DrawWorldEntities()
{
	if (!ConfigInstance.World.Enabled)
		return;

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const int screenW = (int)display.x;
	const int screenH = (int)display.y;
	if (screenW <= 0 || screenH <= 0)
		return;

	const auto snapshot = RobRunReader::GetSnapshot();
	if (!snapshot || !snapshot->world)
		return;

	Matrix4x4 viewProjection{};
	if (!RobRunReader::ResolveViewProjection(viewProjection, screenW, screenH))
		return;

	const Vector3 localPos = snapshot->localPosition;
	const bool haveLocalPosition = !localPos.IsZero();
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	ImFont* font = ImGui::GetFont();
	const float fontSize = (float)ConfigInstance.World.FontSize;

	for (const auto& entity : snapshot->world->entities)
	{
		if (!IsEnabled(entity.kind))
			continue;

		const float dx = entity.position.x - localPos.x;
		const float dy = entity.position.y - localPos.y;
		const float dz = entity.position.z - localPos.z;
		const int distance = (int)std::sqrt(dx * dx + dy * dy + dz * dz);
		if (haveLocalPosition && distance > ConfigInstance.World.MaxDistance)
			continue;

		if (ConfigInstance.LootFilter.Enabled && ConfigInstance.LootFilter.FilterByMinPrice && ConfigInstance.LootFilter.MinPrice > 0)
		{
			if (entity.hasCashAmount && entity.cashAmount < ConfigInstance.LootFilter.MinPrice)
				continue;
		}
		else if (ConfigInstance.World.MinValueFilter > 0)
		{
			if (entity.hasCashAmount && entity.cashAmount < ConfigInstance.World.MinValueFilter)
				continue;
		}

		Vector2 screen;
		if (!RobRunReader::ProjectWorld(
			viewProjection, entity.position, screenW, screenH, screen))
			continue;

		ImU32 colour = ColourFor(entity.kind);
		if (ConfigInstance.LootFilter.Enabled && entity.hasCashAmount)
		{
			if (entity.cashAmount >= ConfigInstance.LootFilter.HighValueThreshold)
				colour = Esp::D2DColorToImU32(ConfigInstance.LootFilter.HighValueColour);
			else if (entity.cashAmount >= ConfigInstance.LootFilter.MidValueThreshold)
				colour = Esp::D2DColorToImU32(ConfigInstance.LootFilter.MidValueColour);
		}
		dl->AddCircleFilled(ImVec2(screen.x, screen.y), 2.5f, colour);

		std::string label = entity.label;
		if (entity.hasCashAmount) {
			label += " | $" + FormatCash(entity.cashAmount);
			if (entity.kind == RobRunWorld::Kind::MoneyWorkbench && entity.cleanCash > 0 && entity.dirtyCash > 0) {
				label += " (C:$" + FormatCash(entity.cleanCash) + " D:$" + FormatCash(entity.dirtyCash) + ")";
			}
		} else if (entity.kind == RobRunWorld::Kind::Container &&
			ConfigInstance.World.ShowEmpty && entity.hasEmptyFlag) {
			label += entity.empty ? " [empty]" : " [loot]";
		} else if (entity.kind == RobRunWorld::Kind::DefenseBlock && entity.hasEmptyFlag) {
			label += entity.empty ? " [empty]" : " [armed]";
		}
		if (ConfigInstance.World.ShowDistance)
			label += " [" + std::to_string(distance) + "m]";

		ImFont* useFont = font;
		ImVec2 textSize = useFont
			? useFont->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label.c_str())
			: ImGui::CalcTextSize(label.c_str());

		Esp::DrawOutlinedText(dl,
			ImVec2(screen.x - textSize.x * 0.5f, screen.y - textSize.y - 4.f),
			colour, useFont, fontSize, label.c_str());
	}
}
