#include "pch.h"
#include "WorldEsp.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
#include "EspDraw.h"
#include "../ImGUI/imgui.h"
#include <cfloat>
#include <cmath>

static ImU32 D2DColorToImU32(const D2D1::ColorF& c)
{
	return IM_COL32(
		(int)(c.r * 255.f),
		(int)(c.g * 255.f),
		(int)(c.b * 255.f),
		(int)(c.a * 255.f));
}

static bool ResolveWorldViewProjection(Matrix4x4& viewProjection, int width, int height)
{
	if (RobRunReader::GetViewProjection(viewProjection, width, height))
		return true;
	return RobRunReader::GetFallbackViewProjection(viewProjection);
}

static bool IsEnabled(RobRunWorld::Kind kind)
{
	switch (kind)
	{
	case RobRunWorld::Kind::Container:
		return ConfigInstance.World.Containers;
	case RobRunWorld::Kind::Loot:
	case RobRunWorld::Kind::StaticLoot:
	case RobRunWorld::Kind::RandomLoot:
		return ConfigInstance.World.Loot;
	case RobRunWorld::Kind::Vehicle:
	case RobRunWorld::Kind::Car:
	case RobRunWorld::Kind::Helicopter:
		return ConfigInstance.World.Vehicles;
	case RobRunWorld::Kind::Grenade:
		return ConfigInstance.World.Grenades;
	}
	return false;
}

static ImU32 ColourFor(RobRunWorld::Kind kind)
{
	switch (kind)
	{
	case RobRunWorld::Kind::Container:
		return D2DColorToImU32(ConfigInstance.World.ContainerColour);
	case RobRunWorld::Kind::Loot:
	case RobRunWorld::Kind::StaticLoot:
	case RobRunWorld::Kind::RandomLoot:
		return D2DColorToImU32(ConfigInstance.World.LootColour);
	case RobRunWorld::Kind::Vehicle:
	case RobRunWorld::Kind::Car:
	case RobRunWorld::Kind::Helicopter:
		return D2DColorToImU32(ConfigInstance.World.VehicleColour);
	case RobRunWorld::Kind::Grenade:
		return D2DColorToImU32(ConfigInstance.World.GrenadeColour);
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
	if (!snapshot)
		return;

	Matrix4x4 viewProjection{};
	if (!ResolveWorldViewProjection(viewProjection, screenW, screenH))
		return;

	const Vector3 localPos = snapshot->localPosition;
	const bool haveLocalPosition = !localPos.IsZero();
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	ImFont* font = ImGui::GetFont();
	const float fontSize = (float)ConfigInstance.World.FontSize;

	for (const auto& entity : snapshot->world.entities)
	{
		if (!IsEnabled(entity.kind))
			continue;

		const float dx = entity.position.x - localPos.x;
		const float dy = entity.position.y - localPos.y;
		const float dz = entity.position.z - localPos.z;
		const int distance = (int)std::sqrt(dx * dx + dy * dy + dz * dz);
		if (haveLocalPosition && distance > ConfigInstance.World.MaxDistance)
			continue;

		Vector2 screen;
		if (!RobRunReader::ProjectWorld(
			viewProjection, entity.position, screenW, screenH, screen))
			continue;

		const ImU32 colour = ColourFor(entity.kind);
		dl->AddCircleFilled(ImVec2(screen.x, screen.y), 2.5f, colour);

		std::string label = entity.label;
		if (entity.kind == RobRunWorld::Kind::Container &&
			ConfigInstance.World.ShowEmpty && entity.hasEmptyFlag) {
			label += entity.empty ? " [empty]" : " [loot]";
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
