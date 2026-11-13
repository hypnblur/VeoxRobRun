#include "pch.h"
#include "PlayerEsp.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
#include "RobRunMemory.h"
#include "EspDraw.h"
#include "WeaponSprites.h"
#include "../ImGUI/imgui.h"
#include <cfloat>
#include <chrono>
#include <cmath>
#include <unordered_map>

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

	// --- pose ---------------------------------------------------------
	// The reader smooths the pose on the worker (EMA over consecutive valid
	// samples) and publishes one immutable snapshot per pass, so the overlay
	// projects exactly the points it is given. There is no render-thread
	// interpolation or extrapolation: extrapolating past the newest sample was
	// what made bones overshoot and spring back every frame.
	std::vector<std::string> lookingNames;

	for (const auto& ent : snapshot->entities)
	{
		if (ent.entityId == localId)
			continue;
		if (ent.isDead)
			continue;

		const Vector3 entPos = ent.position;

		const float distF = DistanceTo(entPos, localPos);
		if (haveLocalPosition && (int)distF > pcfg.MaxDistance)
			continue;

		ImVec2 screenPos{};
		if (!Project(viewProjection, entPos, screenW, screenH, screenPos))
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
			const Vector3 center(entPos.x, entPos.y + 0.95f, entPos.z);
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

		// Pulse multiplier for active state indicators (0.5..1.0 alpha cycle)
		const float pulse = 0.6f + 0.4f * std::sin((float)ImGui::GetTime() * 8.f);
		auto PulseCol = [&](const D2D1::ColorF& c) -> ImU32 {
			return IM_COL32(
				(int)(c.r * 255.f),
				(int)(c.g * 255.f),
				(int)(c.b * 255.f),
				(int)(c.a * 255.f * pulse));
		};
		const ImU32 fireCol = PulseCol(pcfg.FiringColour);
		const ImU32 reloadCol = PulseCol(pcfg.ReloadColour);
		const ImU32 armorCol = PulseCol(pcfg.ArmorColour);

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

		// Top: name + clan tag
		std::vector<TextSeg> topSegs;
		if (!ent.nickname.empty())
			topSegs.push_back({ RobRunMemory::WideToUtf8(ent.nickname), nameCol });
		if (!ent.clanTag.empty())
			topSegs.push_back({ " <" + RobRunMemory::WideToUtf8(ent.clanTag) + ">", nameCol });

		float topY = boxValid ? (minY - 6.f) : screenPos.y;
		if (!topSegs.empty()) {
			std::string full;
			for (const auto& s : topSegs) full += s.text;
			ImVec2 sz = MeasureText(full.c_str(), fontSize);
			topY -= sz.y;
			drawSegLine(topSegs, topY);
		}

		// Bottom: weapon sprite + weapon name + distance
		float botY = boxValid ? (maxY + 8.f) : (screenPos.y + 14.f);
		{
			const std::string weapStr = ent.weaponName.empty()
				? std::string()
				: RobRunMemory::WideToUtf8(ent.weaponName);
			char distBuf[32]{};
			if (pcfg.Distance)
				snprintf(distBuf, sizeof(distBuf), "%.1f m", distF);

			float spriteW = 0.f, spriteH = 0.f;
			WeaponSprites::SpriteInfo sprite{};
			if (pcfg.WeaponEsp && pcfg.WeaponSprite && !weapStr.empty()) {
				sprite = WeaponSprites::Get(weapStr);
				if (sprite.srv) {
					spriteH = fontSize + 2.f;
					spriteW = spriteH * ((float)sprite.width / (float)sprite.height);
				}
			}

			// Measure the full bottom line to center it
			float totalW = 0.f;
			if (sprite.srv) totalW += spriteW + 3.f;
			ImVec2 weapSz{};
			if (pcfg.WeaponEsp && !weapStr.empty()) {
				weapSz = MeasureText(weapStr.c_str(), fontSize);
				totalW += weapSz.x;
			}
			ImVec2 distSz{};
			if (distBuf[0]) {
				distSz = MeasureText(distBuf, fontSize);
				if (totalW > 0.f) totalW += 6.f;
				totalW += distSz.x;
			}

			if (totalW > 0.f) {
				float cx = textX - totalW * 0.5f;
				if (sprite.srv) {
					dl->AddImage((ImTextureID)sprite.srv,
						ImVec2(cx, botY), ImVec2(cx + spriteW, botY + spriteH));
					cx += spriteW + 3.f;
				}
				if (pcfg.WeaponEsp && !weapStr.empty()) {
					Esp::DrawOutlinedText(dl, ImVec2(cx, botY), weapCol,
						font, fontSize, weapStr.c_str());
					cx += weapSz.x;
				}
				if (distBuf[0]) {
					if (pcfg.WeaponEsp && !weapStr.empty()) cx += 6.f;
					Esp::DrawOutlinedText(dl, ImVec2(cx, botY), distCol,
						font, fontSize, distBuf);
				}
				botY += std::max(spriteH, (float)fontSize) + 2.f;
			}
		}

		// State indicators line
		std::vector<TextSeg> stateSegs;
		if (pcfg.Health && ent.healthKnown)
			stateSegs.push_back({ "[" + std::to_string((int)ent.health) + "hp]", hpTxtCol });
		if (pcfg.ShowFiring && ent.isFiring)
			stateSegs.push_back({ "[FIRE]", fireCol });
		if (pcfg.ShowReloading && ent.isReloading)
			stateSegs.push_back({ "[RLD]", reloadCol });
		if (pcfg.ShowArmor && ent.hasArmor)
			stateSegs.push_back({ "[ARM]", armorCol });
		if (ent.isKnockedDown) stateSegs.push_back({ "[KD]", stateCol });
		if (ent.isKnockedOut) stateSegs.push_back({ "[KO]", stateCol });
		if (ent.isSurrendered) stateSegs.push_back({ "[SUR]", stateCol });
		if (ent.isHandcuffed) stateSegs.push_back({ "[HC]", stateCol });
		if (pcfg.ShowRole && ent.role != 0)
		{
			if (ent.role == 1257638368)
				stateSegs.push_back({ "[ROB]", roleCol });
			else
				stateSegs.push_back({ "[Role:" + std::to_string(ent.role) + "]", roleCol });
		}
		if (ent.inVehicle) stateSegs.push_back({ "[V]", stateCol });
		else if (ent.isCrouching) stateSegs.push_back({ "[C]", stateCol });
		else if (ent.isSprinting) stateSegs.push_back({ "[S]", stateCol });
		if (ent.healthKnown && ent.criticalHealth > 0.f && ent.health <= ent.criticalHealth)
			stateSegs.push_back({ "[!]", stateCol });
		if (!stateSegs.empty())
			botY = drawSegLine(stateSegs, botY);

		// Looking-at-you detection
		if ((pcfg.LookingAtYou || pcfg.LookingAlert) && haveLocalPosition)
		{
			const float dx = localPos.x - entPos.x;
			const float dy = localPos.y - entPos.y;
			const float dz = localPos.z - entPos.z;
			const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (len > 0.01f)
			{
				const float invLen = 1.f / len;
				const float dot = ent.lookDirection.x * dx * invLen
					+ ent.lookDirection.y * dy * invLen
					+ ent.lookDirection.z * dz * invLen;
				if (dot > 0.90f)
				{
					if (pcfg.LookingAtYou) {
						std::vector<TextSeg> lookSeg = { { "[LOOKING]", lookCol } };
						botY = drawSegLine(lookSeg, botY);
					}
					if (pcfg.LookingAlert) {
						std::string name = ent.nickname.empty()
							? ("Entity " + std::to_string(ent.entityId))
							: RobRunMemory::WideToUtf8(ent.nickname);
						lookingNames.push_back(std::move(name));
					}
				}
			}
		}
	}

	if (pcfg.LookingAlert && !lookingNames.empty())
	{
		const WindowPlacement& savedAlert = ConfigInstance.Overlay.AlertWindow;
		if (savedAlert.Saved)
			ImGui::SetNextWindowPos(ImVec2(savedAlert.X, savedAlert.Y), ImGuiCond_FirstUseEver);
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

		// Remember where the alert was dragged so it reopens there.
		{
			WindowPlacement& place = ConfigInstance.Overlay.AlertWindow;
			const ImVec2 wp = ImGui::GetWindowPos();
			place.Saved = true;
			place.X = wp.x;
			place.Y = wp.y;
		}

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
