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
	const auto& pcfg = ConfigInstance.Player;
	const ImU32 espCol = D2DColorToImU32(pcfg.TextColour);
	const ImU32 boxCol = D2DColorToImU32(pcfg.BoxColour);
	const ImU32 nameCol = D2DColorToImU32(pcfg.NameColour);
	const ImU32 distCol = D2DColorToImU32(pcfg.DistanceColour);
	const ImU32 hpTxtCol = D2DColorToImU32(pcfg.HealthTextColour);
	const ImU32 weapCol = D2DColorToImU32(pcfg.WeaponColour);
	const ImU32 roleCol = D2DColorToImU32(pcfg.RoleColour);
	const ImU32 lookCol = D2DColorToImU32(pcfg.LookingColour);
	const ImU32 stateCol = D2DColorToImU32(pcfg.StateColour);
	const float fontSize = (float)pcfg.FontSize;
	ImFont* font = ImGui::GetFont();

	std::vector<std::string> lookingNames;

	for (const auto& ent : snapshot->entities)
	{
		if (ent.entityId == localId)
			continue;
		if (ent.isDead)
			continue;

		const int distance = (int)DistanceTo(ent.position, localPos);
		if (haveLocalPosition && distance > pcfg.MaxDistance)
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
			if (pcfg.VisCheck || pcfg.WallCheck) {
				segCol = seg.visible
					? D2DColorToImU32(pcfg.VisibleColour)
					: D2DColorToImU32(pcfg.HiddenColour);
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

		if (boxValid && pcfg.BoxEsp)
		{
			Esp::DrawCornerBox(dl, ImVec2(minX - 4.f, minY - 4.f),
				ImVec2(maxX + 4.f, maxY + 4.f), boxCol, 1.5f, 25.f, true);

			if (pcfg.Health && ent.healthKnown && ent.maxHealth > 0.f)
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

		// --- colored text segments ---
		struct TextSeg { std::string text; ImU32 col; };

		// Top line: name, clan, distance, health, weapon
		std::vector<TextSeg> topSegs;
		if (!ent.nickname.empty())
			topSegs.push_back({ RobRunMemory::WideToUtf8(ent.nickname), nameCol });
		if (!ent.clanTag.empty())
			topSegs.push_back({ " <" + RobRunMemory::WideToUtf8(ent.clanTag) + ">", nameCol });
		if (pcfg.Distance)
			topSegs.push_back({ "[" + std::to_string(distance) + "m]", distCol });
		if (pcfg.Health && ent.healthKnown)
			topSegs.push_back({ "[" + std::to_string((int)ent.health) + "hp]", hpTxtCol });
		if (pcfg.WeaponEsp && !ent.weaponName.empty())
			topSegs.push_back({ "[" + RobRunMemory::WideToUtf8(ent.weaponName) + "]", weapCol });

		// Bottom line: role, looking, state
		std::vector<TextSeg> botSegs;
		if (pcfg.ShowRole && ent.role != 0)
		{
			if (ent.role == 1257638368)
				botSegs.push_back({ "[ROB]", roleCol });
			else
				botSegs.push_back({ "[Role:" + std::to_string(ent.role) + "]", roleCol });
		}
		if ((pcfg.LookingAtYou || pcfg.LookingAlert) && haveLocalPosition)
		{
			const float dx = localPos.x - ent.position.x;
			const float dy = localPos.y - ent.position.y;
			const float dz = localPos.z - ent.position.z;
			const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (len > 0.01f)
			{
				const float invLen = 1.f / len;
				const float dot = ent.lookDirection.x * dx * invLen
					+ ent.lookDirection.y * dy * invLen
					+ ent.lookDirection.z * dz * invLen;
				if (dot > 0.90f)
				{
					if (pcfg.LookingAtYou)
						botSegs.push_back({ "[LOOKING]", lookCol });
					if (pcfg.LookingAlert) {
						std::string name = ent.nickname.empty()
							? ("Entity " + std::to_string(ent.entityId))
							: RobRunMemory::WideToUtf8(ent.nickname);
						lookingNames.push_back(std::move(name));
					}
				}
			}
		}
		if (ent.inVehicle) botSegs.push_back({ "[V]", stateCol });
		else if (ent.isCrouching) botSegs.push_back({ "[C]", stateCol });
		else if (ent.isSprinting) botSegs.push_back({ "[S]", stateCol });
		if (ent.healthKnown && ent.criticalHealth > 0.f && ent.health <= ent.criticalHealth)
			botSegs.push_back({ "[!]", stateCol });

		const float textX = boxValid
			? (minX + maxX) * 0.5f
			: screenPos.x;

		auto drawSegLine = [&](const std::vector<TextSeg>& segs, float y) {
			std::string full;
			for (const auto& s : segs) full += s.text;
			if (full.empty()) return y;
			ImVec2 fullSize = MeasureText(full.c_str(), fontSize);
			float cx = textX - fullSize.x * 0.5f;
			for (const auto& s : segs) {
				ImVec2 sz = MeasureText(s.text.c_str(), fontSize);
				Esp::DrawOutlinedText(dl, ImVec2(cx, y), s.col, font, fontSize, s.text.c_str());
				cx += sz.x;
			}
			return y + fullSize.y;
		};

		float topY = boxValid ? (minY - 6.f) : screenPos.y;
		if (!topSegs.empty()) {
			std::string full;
			for (const auto& s : topSegs) full += s.text;
			ImVec2 sz = MeasureText(full.c_str(), fontSize);
			topY -= sz.y;
			drawSegLine(topSegs, topY);
		}

		if (!botSegs.empty()) {
			float botY = boxValid ? (maxY + 8.f) : (screenPos.y + 14.f);
			drawSegLine(botSegs, botY);
		}
	}

	if (pcfg.LookingAlert && !lookingNames.empty())
	{
		ImGui::SetNextWindowSize(ImVec2(280.f, 0.f), ImGuiCond_FirstUseEver);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 0, 0, 200));
		ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 50, 50, 200));
		ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(140, 20, 20, 220));
		ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(180, 30, 30, 230));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);

		ImGui::Begin("Looking Alert", nullptr,
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

		const ImU32 alertCol = D2DColorToImU32(pcfg.LookingColour);
		for (const auto& name : lookingNames)
		{
			std::string line = name + " is looking at you!";
			ImGui::TextColored(ImColor(alertCol).Value, "%s", line.c_str());
		}

		ImGui::End();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(4);
	}
}
