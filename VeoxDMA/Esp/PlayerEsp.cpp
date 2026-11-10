#include "pch.h"
#include "PlayerEsp.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
#include "RobRunMemory.h"
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

static ImVec2 MeasureText(const char* text, float size)
{
	ImFont* font = ImGui::GetFont();
	if (!font) return ImGui::CalcTextSize(text);
	return font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
}

static bool ResolveViewProjection(Matrix4x4& viewProjection, int width, int height)
{
	if (RobRunReader::GetViewProjection(viewProjection, width, height))
		return true;
	return RobRunReader::GetFallbackViewProjection(viewProjection);
}

static bool Project(const Matrix4x4& viewProjection, const Vector3& world,
	int width, int height, ImVec2& out)
{
	Vector2 screen;
	if (!RobRunReader::ProjectWorld(viewProjection, world, width, height, screen))
		return false;
	out = ImVec2(screen.x, screen.y);
	return true;
}

static float DistanceTo(const Vector3& a, const Vector3& b)
{
	const float dx = a.x - b.x;
	const float dy = a.y - b.y;
	const float dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void DrawPlayers()
{
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const int screenW = (int)display.x;
	const int screenH = (int)display.y;
	if (screenW <= 0 || screenH <= 0)
		return;

	// One snapshot for the whole frame. No DMA reads happen here; bones and
	// positions were resolved by the reader worker.
	const auto snapshot = RobRunReader::GetSnapshot();
	if (!snapshot)
		return;

	Matrix4x4 viewProjection{};
	if (!ResolveViewProjection(viewProjection, screenW, screenH))
		return;

	const int32_t localId = snapshot->localEntityId;
	const Vector3 localPos = snapshot->localPosition;
	const bool haveLocalPosition = !localPos.IsZero();

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	const ImU32 espCol = D2DColorToImU32(ConfigInstance.Player.TextColour);
	const float fontSize = (float)ConfigInstance.Player.FontSize;
	ImFont* font = ImGui::GetFont();

	for (const auto& ent : snapshot->entities)
	{
		if (ent.entityId == localId)
			continue;
		if (ent.isDead)
			continue;

		const int distance = (int)DistanceTo(ent.position, localPos);
		if (haveLocalPosition && distance > ConfigInstance.Player.MaxDistance)
			continue;

		ImVec2 screenPos{};
		if (!Project(viewProjection, ent.position, screenW, screenH, screenPos))
			continue;

		// --- skeleton bones + box ---
		bool boxValid = false;
		float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;

		for (const auto& seg : ent.bones)
		{
			ImVec2 a{}, b{};
			if (!Project(viewProjection, Vector3(seg.a.x, seg.a.y, seg.a.z),
					screenW, screenH, a)) continue;
			if (!Project(viewProjection, Vector3(seg.b.x, seg.b.y, seg.b.z),
					screenW, screenH, b)) continue;

			ImU32 segCol = espCol;
			if (ConfigInstance.Player.VisCheck) {
				segCol = seg.visible
					? D2DColorToImU32(ConfigInstance.Player.VisibleColour)
					: D2DColorToImU32(ConfigInstance.Player.HiddenColour);
			}
			dl->AddLine(a, b, segCol, 2.f);
			minX = std::min(minX, std::min(a.x, b.x));
			maxX = std::max(maxX, std::max(a.x, b.x));
			minY = std::min(minY, std::min(a.y, b.y));
			maxY = std::max(maxY, std::max(a.y, b.y));
			boxValid = true;
		}

		if (!boxValid)
		{
			auto projector = [&](const Vector3& world, float& sx, float& sy) {
				Vector2 screen;
				if (!RobRunReader::ProjectWorld(viewProjection, world, screenW, screenH, screen))
					return false;
				sx = screen.x;
				sy = screen.y;
				return true;
			};
			// Transform position is the character root at the feet; centre the
			// box half a body height above it.
			const Vector3 center(ent.position.x, ent.position.y + 0.95f, ent.position.z);
			ImVec2 mn{}, mx{};
			if (Esp::ProjectWorldAABB(projector, center, 0.35f, 0.95f, mn, mx))
			{
				minX = mn.x; minY = mn.y; maxX = mx.x; maxY = mx.y;
				boxValid = true;
			}
		}

		if (boxValid)
		{
			Esp::DrawCornerBox(dl, ImVec2(minX - 4.f, minY - 4.f),
				ImVec2(maxX + 4.f, maxY + 4.f), espCol, 1.5f, 25.f, true);

			if (ConfigInstance.Player.Health && ent.healthKnown && ent.maxHealth > 0.f)
			{
				const float pct = std::clamp(ent.health / ent.maxHealth, 0.f, 1.f);
				const ImU32 barCol = IM_COL32(
					(int)(255.f * (1.f - pct)), (int)(255.f * pct), 60, 230);
				const float barX = minX - 9.f;
				dl->AddRectFilled(ImVec2(barX, minY - 4.f),
					ImVec2(barX + 3.f, maxY + 4.f), IM_COL32(0, 0, 0, 180));
				dl->AddRectFilled(ImVec2(barX, maxY + 4.f - (maxY - minY + 8.f) * pct),
					ImVec2(barX + 3.f, maxY + 4.f), barCol);
			}
		}

		// --- text info ---
		std::string info;
		if (!ent.nickname.empty())
			info += RobRunMemory::WideToUtf8(ent.nickname);
		if (!ent.clanTag.empty())
			info += " <" + RobRunMemory::WideToUtf8(ent.clanTag) + ">";
		if (ConfigInstance.Player.Distance)
			info += "[" + std::to_string(distance) + "m]";
		if (ConfigInstance.Player.Health && ent.healthKnown)
			info += "[" + std::to_string((int)ent.health) + "hp]";

		const float textX = boxValid
			? (minX + maxX) * 0.5f
			: screenPos.x;
		float textY = boxValid ? (minY - 6.f) : screenPos.y;

		if (!info.empty())
		{
			ImVec2 textSize = MeasureText(info.c_str(), fontSize);
			textY -= textSize.y;
			Esp::DrawOutlinedText(dl,
				ImVec2(textX - textSize.x * 0.5f, textY),
				espCol, font, fontSize, info.c_str());
		}

		std::string stateStr;
		if (ent.inVehicle) stateStr = "[V]";
		else if (ent.isCrouching) stateStr = "[C]";
		else if (ent.isSprinting) stateStr = "[S]";
		if (ent.healthKnown && ent.criticalHealth > 0.f && ent.health <= ent.criticalHealth)
			stateStr += "[!]";
		if (!stateStr.empty())
		{
			ImVec2 textSize = MeasureText(stateStr.c_str(), fontSize);
			const float stateY = boxValid ? (maxY + 8.f) : (screenPos.y + 14.f);
			Esp::DrawOutlinedText(dl,
				ImVec2(textX - textSize.x * 0.5f, stateY),
				espCol, font, fontSize, stateStr.c_str());
		}
	}
}
