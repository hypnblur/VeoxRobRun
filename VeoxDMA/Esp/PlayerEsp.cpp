#include "pch.h"
#include "PlayerEsp.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
#include "RobRunMemory.h"
#include "EspDraw.h"
#include "WeaponSprites.h"
#include "CharacterMesh.h"
#include "DebugLog.h"
#include "../ImGUI/imgui.h"
#include <cfloat>
#include <chrono>
#include <cmath>
#include <unordered_map>

static CharacterMesh::Mesh g_charMesh;
static bool g_charMeshLoadAttempted = false;

static ImVec2 MeasureText(const char* text, float size)
{
	ImFont* font = ImGui::GetFont();
	if (!font) return ImGui::CalcTextSize(text);
	return font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
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

static bool ProjectWithRadius(const Matrix4x4& vp, const Vector3& world, float worldRadius,
	int width, int height, ImVec2& outScreen, float& outRadius)
{
	if (width <= 0 || height <= 0)
		return false;

	// Forward camera depth gate: reject points in the near-clipping frustum (< 0.40m)
	// where perspective projection divides by ~zero and explodes into massive screen coordinates.
	const float clipW = vp.matrix[0][3] * world.x + vp.matrix[1][3] * world.y +
		vp.matrix[2][3] * world.z + vp.matrix[3][3];
	if (!std::isfinite(clipW) || clipW < 0.40f)
		return false;

	if (!Project(vp, world, width, height, outScreen))
		return false;

	if (!std::isfinite(outScreen.x) || !std::isfinite(outScreen.y))
		return false;

	// Reject points far outside visible display margin
	constexpr float kMargin = 200.0f;
	if (outScreen.x < -kMargin || outScreen.x > (float)width + kMargin ||
		outScreen.y < -kMargin || outScreen.y > (float)height + kMargin)
		return false;

	// Analytical perspective radius: r = (worldRadius / clipW) * (height * 0.5f) * cot(fovY / 2)
	float fovScale = std::abs(vp.matrix[1][1]);
	if (fovScale < 0.5f || fovScale > 4.0f) fovScale = 1.0f;

	const float r = (worldRadius / clipW) * ((float)height * 0.5f) * fovScale;
	if (!std::isfinite(r) || r < 0.5f)
		return false;

	// Strict upper limit: no bone capsule or joint can exceed 20% of screen height
	const float maxRadius = (float)height * 0.20f;
	if (r > maxRadius)
		return false;

	outRadius = r;
	return true;
}

static inline bool IsValidBone(const Vector3& p, const Vector3& root)
{
	if (std::abs(p.x) < 0.1f && std::abs(p.y) < 0.1f && std::abs(p.z) < 0.1f)
		return false;
	if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
		return false;
	const float d = Distance(p, root);
	return d <= 3.0f;
}

static inline bool IsValidBoneSegment(const Vector3& a, const Vector3& b, const Vector3& root)
{
	if (!IsValidBone(a, root) || !IsValidBone(b, root))
		return false;
	const float len = Distance(a, b);
	return len >= 0.02f && len <= 1.25f;
}

static void DrawCapsuleSegment(ImDrawList* dl, const Matrix4x4& vp, int screenW, int screenH,
	const Vector3& a, float radiusA, const Vector3& b, float radiusB,
	const Vector3& root, ImU32 fillCol, bool outline, ImU32 outlineCol, float outlineThick,
	bool capA = true, bool capB = true)
{
	if (!IsValidBoneSegment(a, b, root))
		return;

	ImVec2 sa{}, sb{};
	float ra = 0.f, rb = 0.f;
	if (!ProjectWithRadius(vp, a, radiusA, screenW, screenH, sa, ra)) return;
	if (!ProjectWithRadius(vp, b, radiusB, screenW, screenH, sb, rb)) return;

	// Sanity limit: screen distance between two bone joints cannot exceed screen height
	const float dx = sb.x - sa.x;
	const float dy = sb.y - sa.y;
	const float distSq = dx * dx + dy * dy;
	const float maxDist = (float)screenH * 0.85f;
	if (distSq > maxDist * maxDist)
		return;

	Esp::DrawCapsule2D(dl, sa, ra, sb, rb, fillCol, outline, outlineCol, outlineThick, capA, capB);
}

static void DrawSpherePoint(ImDrawList* dl, const Matrix4x4& vp, int screenW, int screenH,
	const Vector3& p, float radius, const Vector3& root,
	ImU32 fillCol, bool outline, ImU32 outlineCol, float outlineThick)
{
	if (!IsValidBone(p, root))
		return;

	ImVec2 sp{};
	float r = 0.f;
	if (!ProjectWithRadius(vp, p, radius, screenW, screenH, sp, r)) return;

	Esp::DrawSphere2D(dl, sp, r, fillCol, outline, outlineCol, outlineThick);
}

static void DrawPlayerChams(ImDrawList* dl, const RobRunReader::EntityData& ent,
	const Matrix4x4& vp, int screenW, int screenH, const PlayerConfig& pcfg)
{
	if (!pcfg.Chams)
		return;

	const Vector3 root = ent.position;
	if (std::abs(root.x) < 0.1f && std::abs(root.y) < 0.1f && std::abs(root.z) < 0.1f)
		return;

	// Reject entities inside the near camera clipping plane
	const float rootClipW = vp.matrix[0][3] * root.x + vp.matrix[1][3] * root.y +
		vp.matrix[2][3] * root.z + vp.matrix[3][3];
	if (!std::isfinite(rootClipW) || rootClipW < 0.40f)
		return;

	ImU32 chamsCol = Esp::ShadeColour(pcfg.ChamsColour, pcfg.ChamsAlpha);
	if (pcfg.ChamsVisCheck) {
		chamsCol = ent.visible
			? Esp::ShadeColour(pcfg.ChamsVisibleColour, pcfg.ChamsAlpha)
			: Esp::ShadeColour(pcfg.ChamsHiddenColour, pcfg.ChamsAlpha);
	}

	const bool outline = pcfg.ChamsOutline;
	const ImU32 outlineCol = Esp::ShadeColour(pcfg.ChamsOutlineColour, std::min(1.0f, pcfg.ChamsAlpha * 1.5f));
	const float outlineThick = 1.0f;
	const float scale = std::clamp(pcfg.ChamsScale, 0.4f, 2.5f);

	// Mesh-based chams: skin the character mesh and draw filled triangles
	if (!g_charMeshLoadAttempted) {
		g_charMeshLoadAttempted = true;
		DLOG("PlayerEsp: attempting CharacterMesh::Load");
		try {
			bool loadOk = CharacterMesh::Load("character_mesh.bin", g_charMesh);
			DLOG("PlayerEsp: CharacterMesh::Load returned %d, valid=%d, bones=%u, verts=%zu, tris=%zu",
				loadOk, g_charMesh.valid, g_charMesh.numBones,
				g_charMesh.vertices.size(), g_charMesh.indices.size() / 3);
		} catch (const std::exception& e) {
			DLOG("PlayerEsp: CharacterMesh::Load EXCEPTION: %s", e.what());
			g_charMesh.valid = false;
		} catch (...) {
			DLOG("PlayerEsp: CharacterMesh::Load UNKNOWN EXCEPTION");
			g_charMesh.valid = false;
		}
	}
	if (g_charMesh.valid && ent.boneWorldMatrices.size() >= 2) {
		thread_local std::vector<Vector3> skinned;
		static bool loggedOnce = false;
		if (!loggedOnce) {
			DLOG("PlayerEsp: mesh chams path entered, boneWorldMatrices.size=%zu, boneMap.size=%zu",
				ent.boneWorldMatrices.size(), ent.boneMap.size());
			loggedOnce = true;
		}

		static const int kDefaultBoneMap[94] = {
			9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 116, 130, 132, 133, 140, 141, 142, 143, 144, 146, 147, 148, 149, 150
		};

		const int* activeBoneMap = nullptr;
		if (ent.boneMap.size() >= g_charMesh.numBones) {
			activeBoneMap = ent.boneMap.data();
		} else if (g_charMesh.numBones == 94) {
			activeBoneMap = kDefaultBoneMap;
		}

		skinned.resize(g_charMesh.vertices.size());

		const int safeBoneCount = static_cast<int>(ent.boneWorldMatrices.size());
		CharacterMesh::Skin(g_charMesh, ent.boneWorldMatrices.data(),
			safeBoneCount, activeBoneMap, skinned.data());

		if (CharacterMesh::Draw(dl, g_charMesh, skinned.data(), vp,
			screenW, screenH, chamsCol))
			return;
	}

	const auto& bp = ent.bonePoints;

	const bool haveHead = bp.hasHead && IsValidBone(bp.head, root);
	const bool haveNeck = bp.hasNeck && IsValidBone(bp.neck, root);
	const bool haveChest = bp.hasChest && IsValidBone(bp.chest, root);
	const bool havePelvis = bp.hasPelvis && IsValidBone(bp.pelvis, root);

	const bool haveLS = IsValidBone(bp.leftShoulder, root);
	const bool haveRS = IsValidBone(bp.rightShoulder, root);
	const bool haveLE = IsValidBone(bp.leftElbow, root);
	const bool haveRE = IsValidBone(bp.rightElbow, root);
	const bool haveLH = IsValidBone(bp.leftHand, root);
	const bool haveRH = IsValidBone(bp.rightHand, root);

	const bool haveLHip = IsValidBone(bp.leftHip, root);
	const bool haveRHip = IsValidBone(bp.rightHip, root);
	const bool haveLKnee = IsValidBone(bp.leftKnee, root);
	const bool haveRKnee = IsValidBone(bp.rightKnee, root);
	const bool haveLFoot = IsValidBone(bp.leftFoot, root);
	const bool haveRFoot = IsValidBone(bp.rightFoot, root);

	bool drawnAny = false;

	// ---- PASS 1: FILL everything (no outlines) ----
	// Draw all fills first so overlapping capsules create a solid body.

	// 1a. Torso capsules (always drawn — provides fill even when quad is used)
	if (haveNeck && haveChest)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.neck, 0.14f * scale,
			bp.chest, 0.24f * scale, root, chamsCol, false, 0, 0, false, false);
		drawnAny = true;
	}
	if (haveChest && havePelvis)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.chest, 0.24f * scale,
			bp.pelvis, 0.20f * scale, root, chamsCol, false, 0, 0, false, true);
		drawnAny = true;
	}
	if (haveLS && haveRS)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.leftShoulder, 0.14f * scale,
			bp.rightShoulder, 0.14f * scale, root, chamsCol, false, 0, 0, false, false);
		drawnAny = true;
	}
	if (haveLHip && haveRHip)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.leftHip, 0.14f * scale,
			bp.rightHip, 0.14f * scale, root, chamsCol, false, 0, 0, false, false);
		drawnAny = true;
	}

	// 1b. Torso quad overlay (extra fill, wider coverage at any angle)
	bool torsoQuadOk = false;
	ImVec2 sLS{}, sRS{}, sRHip{}, sLHip{};
	if (haveLS && haveRS && haveLHip && haveRHip)
	{
		float rLS = 0.f, rRS = 0.f, rRHip = 0.f, rLHip = 0.f;
		if (ProjectWithRadius(vp, bp.leftShoulder, 0.18f * scale, screenW, screenH, sLS, rLS) &&
			ProjectWithRadius(vp, bp.rightShoulder, 0.18f * scale, screenW, screenH, sRS, rRS) &&
			ProjectWithRadius(vp, bp.rightHip, 0.16f * scale, screenW, screenH, sRHip, rRHip) &&
			ProjectWithRadius(vp, bp.leftHip, 0.16f * scale, screenW, screenH, sLHip, rLHip))
		{
			const ImVec2 pts[4] = { sLS, sRS, sRHip, sLHip };
			bool isConvex = true;
			float sign = 0.f;
			for (int i = 0; i < 4; ++i) {
				const ImVec2& p0 = pts[i];
				const ImVec2& p1 = pts[(i + 1) % 4];
				const ImVec2& p2 = pts[(i + 2) % 4];
				const float cp = (p1.x - p0.x) * (p2.y - p1.y) - (p1.y - p0.y) * (p2.x - p1.x);
				if (std::abs(cp) < 1.0f) { isConvex = false; break; }
				if (sign == 0.f) sign = (cp > 0.f) ? 1.f : -1.f;
				else if ((cp > 0.f && sign < 0.f) || (cp < 0.f && sign > 0.f)) { isConvex = false; break; }
			}
			if (isConvex) {
				dl->AddQuadFilled(sLS, sRS, sRHip, sLHip, chamsCol);
				torsoQuadOk = true;
				drawnAny = true;
			}
		}
	}

	// 2a. Head fill
	if (haveHead)
	{
		DrawSpherePoint(dl, vp, screenW, screenH, bp.head, 0.14f * scale, root,
			chamsCol, false, 0, 0);
		drawnAny = true;
	}
	if (haveHead && haveNeck)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.head, 0.12f * scale,
			bp.neck, 0.12f * scale, root, chamsCol, false, 0, 0, false, false);
		drawnAny = true;
	}
	else if (haveHead && haveChest)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.head, 0.12f * scale,
			bp.chest, 0.16f * scale, root, chamsCol, false, 0, 0, false, false);
		drawnAny = true;
	}

	// 3a. Arm fills
	if (haveLS && haveLE)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.leftShoulder, 0.12f * scale,
			bp.leftElbow, 0.10f * scale, root, chamsCol, false, 0, 0, true, false);
		drawnAny = true;
	}
	if (haveLE && haveLH)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.leftElbow, 0.10f * scale,
			bp.leftHand, 0.08f * scale, root, chamsCol, false, 0, 0, false, true);
		drawnAny = true;
	}
	if (haveRS && haveRE)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.rightShoulder, 0.12f * scale,
			bp.rightElbow, 0.10f * scale, root, chamsCol, false, 0, 0, true, false);
		drawnAny = true;
	}
	if (haveRE && haveRH)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.rightElbow, 0.10f * scale,
			bp.rightHand, 0.08f * scale, root, chamsCol, false, 0, 0, false, true);
		drawnAny = true;
	}

	// 4a. Leg fills
	if (haveLHip && haveLKnee)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.leftHip, 0.15f * scale,
			bp.leftKnee, 0.12f * scale, root, chamsCol, false, 0, 0, true, false);
		drawnAny = true;
	}
	if (haveLKnee && haveLFoot)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.leftKnee, 0.12f * scale,
			bp.leftFoot, 0.09f * scale, root, chamsCol, false, 0, 0, false, true);
		drawnAny = true;
	}
	if (haveRHip && haveRKnee)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.rightHip, 0.15f * scale,
			bp.rightKnee, 0.12f * scale, root, chamsCol, false, 0, 0, true, false);
		drawnAny = true;
	}
	if (haveRKnee && haveRFoot)
	{
		DrawCapsuleSegment(dl, vp, screenW, screenH, bp.rightKnee, 0.12f * scale,
			bp.rightFoot, 0.09f * scale, root, chamsCol, false, 0, 0, false, true);
		drawnAny = true;
	}

	// ---- PASS 2: OUTLINE only (on top of all fills) ----
	if (outline && (outlineCol & IM_COL32_A_MASK) != 0)
	{
		const ImU32 noFill = IM_COL32(0, 0, 0, 0);

		// Torso quad outline
		if (torsoQuadOk) {
			dl->AddLine(sLS, sRS, outlineCol, outlineThick);
			dl->AddLine(sRS, sRHip, outlineCol, outlineThick);
			dl->AddLine(sRHip, sLHip, outlineCol, outlineThick);
			dl->AddLine(sLHip, sLS, outlineCol, outlineThick);
		}

		// Head outline
		if (haveHead)
			DrawSpherePoint(dl, vp, screenW, screenH, bp.head, 0.14f * scale, root,
				noFill, true, outlineCol, outlineThick);

		// Arm outlines (outer contour only — cap at shoulder and hand)
		if (haveLS && haveLE)
			DrawCapsuleSegment(dl, vp, screenW, screenH, bp.leftShoulder, 0.12f * scale,
				bp.leftElbow, 0.10f * scale, root, noFill, true, outlineCol, outlineThick, true, false);
		if (haveLE && haveLH)
			DrawCapsuleSegment(dl, vp, screenW, screenH, bp.leftElbow, 0.10f * scale,
				bp.leftHand, 0.08f * scale, root, noFill, true, outlineCol, outlineThick, false, true);
		if (haveRS && haveRE)
			DrawCapsuleSegment(dl, vp, screenW, screenH, bp.rightShoulder, 0.12f * scale,
				bp.rightElbow, 0.10f * scale, root, noFill, true, outlineCol, outlineThick, true, false);
		if (haveRE && haveRH)
			DrawCapsuleSegment(dl, vp, screenW, screenH, bp.rightElbow, 0.10f * scale,
				bp.rightHand, 0.08f * scale, root, noFill, true, outlineCol, outlineThick, false, true);

		// Leg outlines (cap at hip and foot)
		if (haveLHip && haveLKnee)
			DrawCapsuleSegment(dl, vp, screenW, screenH, bp.leftHip, 0.15f * scale,
				bp.leftKnee, 0.12f * scale, root, noFill, true, outlineCol, outlineThick, true, false);
		if (haveLKnee && haveLFoot)
			DrawCapsuleSegment(dl, vp, screenW, screenH, bp.leftKnee, 0.12f * scale,
				bp.leftFoot, 0.09f * scale, root, noFill, true, outlineCol, outlineThick, false, true);
		if (haveRHip && haveRKnee)
			DrawCapsuleSegment(dl, vp, screenW, screenH, bp.rightHip, 0.15f * scale,
				bp.rightKnee, 0.12f * scale, root, noFill, true, outlineCol, outlineThick, true, false);
		if (haveRKnee && haveRFoot)
			DrawCapsuleSegment(dl, vp, screenW, screenH, bp.rightKnee, 0.12f * scale,
				bp.rightFoot, 0.09f * scale, root, noFill, true, outlineCol, outlineThick, false, true);
	}

	// 5. Fallback: if named bone points didn't resolve, render validated ent.bones
	if (!drawnAny)
	{
		for (const auto& seg : ent.bones)
		{
			if (!IsValidBoneSegment(seg.a, seg.b, root))
				continue;

			ImU32 segChamsCol = chamsCol;
			if (pcfg.ChamsVisCheck) {
				segChamsCol = seg.visible
					? Esp::ShadeColour(pcfg.ChamsVisibleColour, pcfg.ChamsAlpha)
					: Esp::ShadeColour(pcfg.ChamsHiddenColour, pcfg.ChamsAlpha);
			}
			DrawCapsuleSegment(dl, vp, screenW, screenH, seg.a, 0.12f * scale,
				seg.b, 0.12f * scale, root, segChamsCol, outline, outlineCol, outlineThick, true, true);
		}
	}
}

static void DrawOffscreenArrow(ImDrawList* dl, int screenW, int screenH,
	const Vector3& targetPos, const RobRunCamera::CameraState& cam,
	float distMeters, ImU32 color, ImFont* font, float fontSize)
{
	const float cx = (float)screenW * 0.5f;
	const float cy = (float)screenH * 0.5f;
	Vector3 toTarget = targetPos - cam.position;
	const float toTargetLen = toTarget.Length();
	if (toTargetLen < 0.001f) return;
	toTarget = toTarget / toTargetLen;

	float x = Vector3::Dot(toTarget, cam.right);
	float y = Vector3::Dot(toTarget, cam.up);
	float z = Vector3::Dot(toTarget, cam.forward);

	if (z < 0.f) {
		x = -x;
		y = -y;
	}

	const float angle = std::atan2(-y, x);
	const float radius = std::min(cx, cy) * 0.82f;
	const ImVec2 arrowPos(cx + std::cos(angle) * radius, cy + std::sin(angle) * radius);

	constexpr float arrowLen = 14.f;
	constexpr float arrowWidth = 9.f;
	const ImVec2 tip = arrowPos;
	const ImVec2 baseCenter(arrowPos.x - std::cos(angle) * arrowLen, arrowPos.y - std::sin(angle) * arrowLen);
	const ImVec2 normal(-std::sin(angle), std::cos(angle));
	const ImVec2 left(baseCenter.x + normal.x * arrowWidth * 0.5f, baseCenter.y + normal.y * arrowWidth * 0.5f);
	const ImVec2 right(baseCenter.x - normal.x * arrowWidth * 0.5f, baseCenter.y - normal.y * arrowWidth * 0.5f);

	dl->AddTriangleFilled(tip, left, right, color);
	dl->AddTriangle(tip, left, right, IM_COL32(0, 0, 0, 200), 1.2f);

	char distBuf[16];
	std::snprintf(distBuf, sizeof(distBuf), "%.0fm", distMeters);
	ImVec2 txtSz = font ? font->CalcTextSizeA(fontSize - 2.f, FLT_MAX, 0.f, distBuf) : ImGui::CalcTextSize(distBuf);
	ImVec2 txtPos(arrowPos.x - txtSz.x * 0.5f, arrowPos.y + (std::sin(angle) >= 0.f ? 8.f : -8.f - txtSz.y));
	Esp::DrawOutlinedText(dl, txtPos, color, font, fontSize - 2.f, distBuf);
}

void DrawPlayers()
{
	static int drawCount = 0;
	++drawCount;
	if (drawCount <= 3 || (drawCount % 500) == 0)
		DLOG("DrawPlayers #%d", drawCount);

	try {

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const int screenW = (int)display.x;
	const int screenH = (int)display.y;
	if (screenW <= 0 || screenH <= 0)
		return;

	// One snapshot for the whole frame. No DMA reads happen here; bones and
	// positions were resolved by the reader worker.
	const auto snapshot = RobRunReader::GetSnapshot();
	if (!snapshot) {
		RobRunReader::ReportRenderState(false, FLT_MAX);
		return;
	}

	Matrix4x4 viewProjection{};
	if (!RobRunReader::ResolveViewProjection(viewProjection, screenW, screenH)) {
		RobRunReader::ReportRenderState(false, FLT_MAX);
		return;
	}

	const int32_t localId = snapshot->localEntityId;
	const Vector3 localPos = snapshot->localPosition;
	const bool haveLocalPosition = !localPos.IsZero();

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	const auto& pcfg = ConfigInstance.Player;
	const ImU32 espCol = Esp::D2DColorToImU32(pcfg.TextColour);
	const ImU32 boxCol = Esp::D2DColorToImU32(pcfg.BoxColour);
	const ImU32 nameCol = Esp::D2DColorToImU32(pcfg.NameColour);
	const ImU32 distCol = Esp::D2DColorToImU32(pcfg.DistanceColour);
	const ImU32 hpTxtCol = Esp::D2DColorToImU32(pcfg.HealthTextColour);
	const ImU32 weapCol = Esp::D2DColorToImU32(pcfg.WeaponColour);
	const ImU32 roleCol = Esp::D2DColorToImU32(pcfg.RoleColour);
	const ImU32 lookCol = Esp::D2DColorToImU32(pcfg.LookingColour);
	const ImU32 stateCol = Esp::D2DColorToImU32(pcfg.StateColour);
	const float fontSize = (float)pcfg.FontSize;
	ImFont* font = ImGui::GetFont();

	// --- pose ---------------------------------------------------------
	// The reader smooths the pose on the worker (EMA over consecutive valid
	// samples) and publishes one immutable snapshot per pass, so the overlay
	// projects exactly the points it is given. There is no render-thread
	// interpolation or extrapolation: extrapolating past the newest sample was
	// what made bones overshoot and spring back every frame.
	thread_local std::vector<std::string> lookingNames;
	lookingNames.clear();

	// Text segments are rebuilt every frame for every player. Keep the buffers
	// (and the joined-line scratch) alive across frames instead of allocating
	// them per entity per frame.
	struct TextSeg { std::string text; ImU32 col; };
	thread_local std::vector<TextSeg> topSegs;
	thread_local std::vector<TextSeg> stateSegs;
	thread_local std::vector<TextSeg> lookSeg;
	thread_local std::string segFull;

	bool anyEnemyOnScreen = false;
	float nearestOnScreenDist = FLT_MAX;

	for (const auto& ent : snapshot->entities)
	{
		if (ent.entityId == localId)
			continue;
		if (ent.isDead)
			continue;
		if (pcfg.HideTeammates && (ent.isPartyMember || ent.isClanMate))
			continue;

		const Vector3 entPos = ent.position;

		const float distF = Distance(entPos, localPos);
		if (haveLocalPosition && (int)distF > pcfg.MaxDistance)
			continue;

		// Filter out local player and near-camera intersecting models:
		// If localEntityId is unresolved (-1), local player has distF ~ 0.
		if (haveLocalPosition && distF < 0.75f)
			continue;

		// Camera proximity filter: entities clipping directly into camera
		if (snapshot->camera.valid) {
			const float camDist = Distance(entPos, snapshot->camera.position);
			if (camDist < 0.50f)
				continue;
		}

		ImVec2 screenPos{};
		const bool projected = Project(viewProjection, entPos, screenW, screenH, screenPos);
		const bool onScreen = projected &&
			screenPos.x >= 0.f && screenPos.x <= (float)screenW &&
			screenPos.y >= 0.f && screenPos.y <= (float)screenH;

		if (!onScreen)
		{
			if (pcfg.OffscreenArrows && snapshot->camera.valid)
			{
				ImU32 offCol = Esp::D2DColorToImU32(pcfg.OffscreenColour);
				if (pcfg.VisCheck || pcfg.WallCheck) {
					offCol = ent.visible
						? Esp::D2DColorToImU32(pcfg.VisibleColour)
						: Esp::D2DColorToImU32(pcfg.HiddenColour);
				}
				DrawOffscreenArrow(dl, screenW, screenH, entPos, snapshot->camera,
					distF, offCol, font, fontSize);
			}
			continue;
		}

		anyEnemyOnScreen = true;
		if (haveLocalPosition && distF < nearestOnScreenDist)
			nearestOnScreenDist = distF;

		// Only dim to HiddenAlpha when visibility/wall checking is actually
		// enabled; otherwise the fallback animator-motion heuristic dims idle
		// or LOD-throttled players to grey and causes a white/grey strobe.
		const bool visActive = pcfg.VisCheck || pcfg.WallCheck;
		const float alphaMult = (!visActive || ent.visible) ? 1.0f : std::clamp(pcfg.HiddenAlpha, 0.05f, 0.60f);
		const ImU32 boxCol = Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.BoxColour), alphaMult);
		const ImU32 nameCol = Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.NameColour), alphaMult);
		const ImU32 distCol = Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.DistanceColour), alphaMult);
		const ImU32 hpTxtCol = Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.HealthTextColour), alphaMult);
		const ImU32 weapCol = Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.WeaponColour), alphaMult);
		const ImU32 roleCol = Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.RoleColour), alphaMult);
		const ImU32 lookCol = Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.LookingColour), alphaMult);
		const ImU32 stateCol = Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.StateColour), alphaMult);
		const ImU32 espCol = Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.TextColour), alphaMult);

		// Vis Ring: 3D Ground Circle on floor (Y + 0.05m) (item 8)
		if (pcfg.VisRing)
		{
			constexpr int kRingSegments = 24;
			constexpr float kRingRadius = 0.50f;
			const Vector3 groundCenter(entPos.x, entPos.y + 0.05f, entPos.z);
			ImVec2 rpts[kRingSegments];
			bool rvalid[kRingSegments];
			int numValid = 0;
			for (int i = 0; i < kRingSegments; ++i) {
				const float angle = (float)i * (6.2831853f / (float)kRingSegments);
				const Vector3 p(groundCenter.x + std::cos(angle) * kRingRadius, groundCenter.y, groundCenter.z + std::sin(angle) * kRingRadius);
				rvalid[i] = Project(viewProjection, p, screenW, screenH, rpts[i]);
				if (rvalid[i]) ++numValid;
			}
			if (numValid >= 12) {
				const float pulse = 0.65f + 0.35f * std::sin((float)ImGui::GetTime() * 8.f);
				ImU32 ringCol;
				if (ent.visible) {
					ringCol = IM_COL32(
						(int)(pcfg.VisRingColour.r * 255.f),
						(int)(pcfg.VisRingColour.g * 255.f),
						(int)(pcfg.VisRingColour.b * 255.f),
						(int)(255.f * pulse));
				} else {
					ringCol = IM_COL32(130, 130, 130, (int)(80.f * alphaMult));
				}
				for (int i = 0; i < kRingSegments; ++i) {
					int next = (i + 1) % kRingSegments;
					if (rvalid[i] && rvalid[next]) {
						dl->AddLine(rpts[i], rpts[next], ringCol, ent.visible ? 2.0f : 1.0f);
					}
				}
			}
		}

		// Status Ring: Subtle screen-space halo around torso/head centroid when visible (item 8)
		if (pcfg.StatusRing && ent.visible)
		{
			const Vector3 centroid = ent.bonePoints.hasChest ? ent.bonePoints.chest : Vector3(entPos.x, entPos.y + 1.25f, entPos.z);
			ImVec2 screenCentroid;
			if (Project(viewProjection, centroid, screenW, screenH, screenCentroid)) {
				const float pulse = 0.70f + 0.30f * std::sin((float)ImGui::GetTime() * 10.f);
				const ImU32 statusCol = IM_COL32(
					(int)(pcfg.StatusRingColour.r * 255.f),
					(int)(pcfg.StatusRingColour.g * 255.f),
					(int)(pcfg.StatusRingColour.b * 255.f),
					(int)(230.f * pulse));
				dl->AddCircle(screenCentroid, 18.f, statusCol, 0, 1.8f);
			}
		}

		// --- chams ---
		if (pcfg.Chams)
		{
			DrawPlayerChams(dl, ent, viewProjection, screenW, screenH, pcfg);
		}

		// --- skeleton bones + box ---
		bool boxValid = false;
		float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;

		for (const auto& seg : ent.bones)
		{
			if (!IsValidBoneSegment(seg.a, seg.b, entPos))
				continue;

			ImVec2 a{}, b{};
			if (!Project(viewProjection, seg.a, screenW, screenH, a)) continue;
			if (!Project(viewProjection, seg.b, screenW, screenH, b)) continue;

			if (pcfg.Skeleton)
			{
				ImU32 segCol = espCol;
				if (pcfg.VisCheck || pcfg.WallCheck) {
					segCol = seg.visible
						? Esp::D2DColorToImU32(pcfg.VisibleColour)
						: Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.HiddenColour), alphaMult);
				}
				dl->AddLine(a, b, segCol, pcfg.SkeletonThickness);
			}
			minX = std::min(minX, std::min(a.x, b.x));
			maxX = std::max(maxX, std::max(a.x, b.x));
			minY = std::min(minY, std::min(a.y, b.y));
			maxY = std::max(maxY, std::max(a.y, b.y));
			boxValid = true;
		}

		if (pcfg.HeadCircle && pcfg.Skeleton &&
			ent.bonePoints.hasHead && ent.bonePoints.hasNeck &&
			IsValidBone(ent.bonePoints.head, entPos) &&
			IsValidBone(ent.bonePoints.neck, entPos))
		{
			const Vector3& head = ent.bonePoints.head;
			const Vector3& neck = ent.bonePoints.neck;
			const float headRadius = Distance(head, neck) * 0.5f;
			const Vector3 center(
				(head.x + neck.x) * 0.5f,
				(head.y + neck.y) * 0.5f,
				(head.z + neck.z) * 0.5f);
			ImVec2 sc{}; float sr = 0.f;
			if (headRadius > 0.02f &&
				ProjectWithRadius(viewProjection, center, headRadius,
					screenW, screenH, sc, sr))
			{
				ImU32 headCol = espCol;
				if (pcfg.VisCheck || pcfg.WallCheck) {
					headCol = ent.visible
						? Esp::D2DColorToImU32(pcfg.VisibleColour)
						: Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.HiddenColour), alphaMult);
				}
				dl->AddCircle(sc, sr, headCol, 0, 2.f);
			}
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
			ImU32 currentBoxCol = boxCol;
			if (pcfg.VisCheck || pcfg.WallCheck) {
				currentBoxCol = ent.visible
					? Esp::D2DColorToImU32(pcfg.VisibleColour)
					: Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.HiddenColour), alphaMult);
			}
			Esp::DrawCornerBox(dl, ImVec2(minX - 4.f, minY - 4.f),
				ImVec2(maxX + 4.f, maxY + 4.f), currentBoxCol, 1.5f, 25.f, true);

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

				char hpNum[16];
				std::snprintf(hpNum, sizeof(hpNum), "%.0f", ent.health);
				ImVec2 numSz = font ? font->CalcTextSizeA(fontSize - 2.f, FLT_MAX, 0.f, hpNum) : ImGui::CalcTextSize(hpNum);
				Esp::DrawOutlinedText(dl, ImVec2(barX - numSz.x - 2.f, maxY - numSz.y),
					barCol, font, fontSize - 2.f, hpNum);
			}
		}

		// --- snaplines ---
		if (pcfg.Snaplines)
		{
			ImVec2 origin(screenW * 0.5f, (float)screenH);
			if (pcfg.SnaplineOrigin == 1) {
				origin = ImVec2(screenW * 0.5f, screenH * 0.5f);
			} else if (pcfg.SnaplineOrigin == 2) {
				origin = ImVec2(screenW * 0.5f, 0.f);
			}

			const ImVec2 snapTarget = boxValid
				? ImVec2((minX + maxX) * 0.5f, maxY + 4.f)
				: screenPos;

			ImU32 snapCol = Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.SnaplineColour), alphaMult);
			if (pcfg.VisCheck || pcfg.WallCheck) {
				snapCol = ent.visible
					? Esp::D2DColorToImU32(pcfg.VisibleColour)
					: Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.HiddenColour), alphaMult);
			}
			dl->AddLine(origin, snapTarget, snapCol, 1.2f);
		}

		// --- head dot ---
		if (pcfg.HeadDot)
		{
			Vector3 headPos = ent.bonePoints.hasHead && IsValidBone(ent.bonePoints.head, entPos)
				? ent.bonePoints.head
				: Vector3(entPos.x, entPos.y + 1.70f, entPos.z);

			ImVec2 headScreen{};
			if (Project(viewProjection, headPos, screenW, screenH, headScreen))
			{
				ImU32 headCol = Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.HeadDotColour), alphaMult);
				if (pcfg.VisCheck || pcfg.WallCheck) {
					headCol = ent.visible
						? Esp::D2DColorToImU32(pcfg.VisibleColour)
						: Esp::ScaleAlpha(Esp::D2DColorToImU32(pcfg.HiddenColour), alphaMult);
				}
				dl->AddCircleFilled(headScreen, pcfg.HeadDotRadius, headCol);
				dl->AddCircle(headScreen, pcfg.HeadDotRadius, IM_COL32(0, 0, 0, 180), 0, 1.0f);
			}
		}

		// --- colored text segments ---

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
			segFull.clear();
			for (const auto& s : segs) segFull += s.text;
			if (segFull.empty()) return y;
			ImVec2 fullSize = MeasureText(segFull.c_str(), fontSize);
			float cx = textX - fullSize.x * 0.5f;
			for (const auto& s : segs) {
				ImVec2 sz = MeasureText(s.text.c_str(), fontSize);
				Esp::DrawOutlinedText(dl, ImVec2(cx, y), s.col, font, fontSize, s.text.c_str());
				cx += sz.x;
			}
			return y + fullSize.y;
		};

		// Resolve role color once — used for name and role tag
		const std::string roleLabel =
			RobRunReader::RoleLabel(ent.role, ent.roleName);
		ImU32 roleBasedCol = nameCol;
		if (pcfg.NameByRole && !roleLabel.empty()) {
			if (roleLabel == "Criminal")
				roleBasedCol = Esp::D2DColorToImU32(pcfg.CriminalColour);
			else if (roleLabel == "Police Officer")
				roleBasedCol = Esp::D2DColorToImU32(pcfg.PoliceColour);
			else if (roleLabel == "Civilian")
				roleBasedCol = Esp::D2DColorToImU32(pcfg.CivilianColour);
			else if (roleLabel == "Hubber")
				roleBasedCol = Esp::D2DColorToImU32(pcfg.HubberColour);
			if (alphaMult < 1.f)
				roleBasedCol = Esp::ScaleAlpha(roleBasedCol, alphaMult);
		}

		// Top: name + clan tag
		topSegs.clear();
		if (!ent.nicknameUtf8.empty())
			topSegs.push_back({ ent.nicknameUtf8, roleBasedCol });
		if (!ent.clanTagUtf8.empty())
			topSegs.push_back({ " <" + ent.clanTagUtf8 + ">", roleBasedCol });

		float topY = boxValid ? (minY - 6.f) : screenPos.y;
		if (!topSegs.empty()) {
			segFull.clear();
			for (const auto& s : topSegs) segFull += s.text;
			ImVec2 sz = MeasureText(segFull.c_str(), fontSize);
			topY -= sz.y;
			drawSegLine(topSegs, topY);
		}

		// Bottom: weapon sprite + weapon name + distance
		float botY = boxValid ? (maxY + 8.f) : (screenPos.y + 14.f);
		{
			const std::string& weapStr = ent.weaponNameUtf8;
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
		stateSegs.clear();
		if (pcfg.VisCheck || pcfg.WallCheck)
		{
			const ImU32 visBadgeCol = ent.visible
				? Esp::D2DColorToImU32(pcfg.VisibleColour)
				: Esp::D2DColorToImU32(pcfg.HiddenColour);
			stateSegs.push_back({ ent.visible ? "[VIS]" : "[WALL]", visBadgeCol });
		}
		if (pcfg.Health && ent.healthKnown)
			stateSegs.push_back({ "[" + std::to_string((int)ent.health) + "hp]", hpTxtCol });
		if (pcfg.ShowFiring && ent.isFiring)
			stateSegs.push_back({ "[FIRE]", fireCol });
		if (pcfg.ShowReloading && ent.isReloading)
			stateSegs.push_back({ "[RLD]", reloadCol });
		if (pcfg.ShowArmor && (ent.hasArmor || ent.armor > 0.f))
		{
			if (pcfg.ShowArmorValue && ent.armor > 0.f)
				stateSegs.push_back({ "[ARM:" + std::to_string((int)ent.armor) + "]", armorCol });
			else
				stateSegs.push_back({ "[ARM]", armorCol });
		}
		if (pcfg.ShowStamina && ent.stamina >= 0.f)
		{
			const ImU32 stmCol = Esp::D2DColorToImU32(pcfg.StaminaColour);
			stateSegs.push_back({ "[STM:" + std::to_string((int)ent.stamina) + "]", stmCol });
		}
		if (ent.isAimingAtLocal)
			stateSegs.push_back({ "[AIMING]", lookCol });
		if (ent.isKnockedDown) stateSegs.push_back({ "[KD]", stateCol });
		if (ent.isKnockedOut) stateSegs.push_back({ "[KO]", stateCol });
		if (ent.isSurrendered) stateSegs.push_back({ "[SUR]", stateCol });
		if (ent.isHandcuffed) stateSegs.push_back({ "[HC]", stateCol });
		if (pcfg.HiddenPlayerEsp && snapshot->world &&
			snapshot->world->hiddenEntityIds.count(ent.entityId))
			stateSegs.push_back({ "[HIDING]", IM_COL32(255, 165, 0, 255) });
		if (pcfg.ShowRole)
		{
			ImU32 roleTextCol = roleCol;
			if (pcfg.ExpRoleColors && !roleLabel.empty()) {
				if (roleLabel == "Criminal")
					roleTextCol = Esp::D2DColorToImU32(pcfg.CriminalColour);
				else if (roleLabel == "Police Officer")
					roleTextCol = Esp::D2DColorToImU32(pcfg.PoliceColour);
				else if (roleLabel == "Civilian")
					roleTextCol = Esp::D2DColorToImU32(pcfg.CivilianColour);
				else if (roleLabel == "Hubber")
					roleTextCol = Esp::D2DColorToImU32(pcfg.HubberColour);
				if (alphaMult < 1.f)
					roleTextCol = Esp::ScaleAlpha(roleTextCol, alphaMult);
			}
			if (!roleLabel.empty())
				stateSegs.push_back({ "[" + roleLabel + "]", roleTextCol });
		}
		if (ent.bagValue > 0) {
			const char* bagTag = (ent.role != 0 && ent.roleName == "Police Officer")
				? "CBAG" : "RBAG";
			stateSegs.push_back({ "[" + std::string(bagTag) + ":$"
				+ std::to_string(ent.bagValue) + "]",
				IM_COL32(120, 255, 140, 255) });
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
						lookSeg.clear();
						lookSeg.push_back({ "[LOOKING]", lookCol });
						botY = drawSegLine(lookSeg, botY);
					}
					if (pcfg.LookingAlert) {
						if (ent.nicknameUtf8.empty())
							lookingNames.push_back("Entity " + std::to_string(ent.entityId));
						else
							lookingNames.push_back(ent.nicknameUtf8);
					}
				}
			}
		}
	}

	RobRunReader::ReportRenderState(anyEnemyOnScreen, nearestOnScreenDist);

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

		const ImU32 alertCol = Esp::D2DColorToImU32(pcfg.LookingColour);
		for (const auto& name : lookingNames)
		{
			std::string line = name + " is looking at you!";
			ImGui::TextColored(ImColor(alertCol).Value, "%s", line.c_str());
		}

		ImGui::End();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(4);
	}

	// Wallet / bag value HUD. Values are read by the inventory worker once a
	// second, so this window only formats the published state. Suppressed in
	// battle mode so only player ESP remains on screen.
	if (pcfg.ExpMoneyHud && !ConfigInstance.Overlay.BattleMode &&
		snapshot->inventory && snapshot->inventory->valid)
	{
		const auto& inv = *snapshot->inventory;
		ImGui::SetNextWindowPos(ImVec2(20.f, 106.f), ImGuiCond_FirstUseEver);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 16, 26, 225));
		ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 200, 75, 190));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 8.f));

		ImGui::Begin("##MoneyCountHud", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoNav);

		ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.30f, 1.0f), "COINS : $%d", inv.coins);
		if (inv.cryptoCoins > 0)
			ImGui::TextColored(ImVec4(0.45f, 0.88f, 1.0f, 1.0f), "CRYPTO : %d", inv.cryptoCoins);
		ImGui::TextColored(ImVec4(0.60f, 0.96f, 0.65f, 0.95f),
			"BAG VALUE : $%d (Cash $%d / Items $%d)",
			inv.inventoryTotalValue, inv.inventoryCashValue, inv.inventoryItemsValue);
		if (inv.equipValue > 0 || inv.vehicleTotalValue > 0)
			ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.92f, 0.90f),
				"EQUIP : $%d  |  VEH : $%d", inv.equipValue, inv.vehicleTotalValue);

		ImGui::End();
		ImGui::PopStyleVar(3);
		ImGui::PopStyleColor(2);
	}

	} catch (const std::exception& e) {
		DLOG("DrawPlayers EXCEPTION #%d: %s", drawCount, e.what());
	} catch (...) {
		DLOG("DrawPlayers UNKNOWN EXCEPTION #%d", drawCount);
	}
}

void DrawBvhDebug()
{
	if (!ConfigInstance.Player.ShowBvh) return;

	const auto& scene = RobRunReader::GetBvhScene();
	if (!scene.valid || scene.nodes.empty()) return;

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const int screenW = (int)display.x;
	const int screenH = (int)display.y;
	if (screenW <= 0 || screenH <= 0) return;

	Matrix4x4 vp{};
	if (!RobRunReader::ResolveViewProjection(vp, screenW, screenH)) return;

	const auto snapshot = RobRunReader::GetSnapshot();
	if (!snapshot) return;
	const Vector3 cam = snapshot->localPosition;
	const float maxDist = (float)ConfigInstance.Player.BvhDrawDist;
	const float maxDistSq = maxDist * maxDist;

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	const ImU32 col = IM_COL32(0, 255, 255, 80);

	for (const auto& node : scene.nodes) {
		if (!node.isLeaf()) continue;

		const float cx = (node.aabbMin.x + node.aabbMax.x) * 0.5f;
		const float cy = (node.aabbMin.y + node.aabbMax.y) * 0.5f;
		const float cz = (node.aabbMin.z + node.aabbMax.z) * 0.5f;
		const float dx = cx - cam.x, dy = cy - cam.y, dz = cz - cam.z;
		if (dx*dx + dy*dy + dz*dz > maxDistSq) continue;

		Vector3 corners[8] = {
			{node.aabbMin.x, node.aabbMin.y, node.aabbMin.z},
			{node.aabbMax.x, node.aabbMin.y, node.aabbMin.z},
			{node.aabbMax.x, node.aabbMax.y, node.aabbMin.z},
			{node.aabbMin.x, node.aabbMax.y, node.aabbMin.z},
			{node.aabbMin.x, node.aabbMin.y, node.aabbMax.z},
			{node.aabbMax.x, node.aabbMin.y, node.aabbMax.z},
			{node.aabbMax.x, node.aabbMax.y, node.aabbMax.z},
			{node.aabbMin.x, node.aabbMax.y, node.aabbMax.z},
		};

		Vector2 sc[8];
		bool allOk = true;
		for (int i = 0; i < 8; ++i) {
			if (!RobRunReader::ProjectWorld(vp, corners[i], screenW, screenH, sc[i])) {
				allOk = false; break;
			}
		}
		if (!allOk) continue;

		static const int edges[12][2] = {
			{0,1},{1,2},{2,3},{3,0},
			{4,5},{5,6},{6,7},{7,4},
			{0,4},{1,5},{2,6},{3,7},
		};
		for (const auto& e : edges)
			dl->AddLine(ImVec2(sc[e[0]].x, sc[e[0]].y),
			            ImVec2(sc[e[1]].x, sc[e[1]].y), col, 1.0f);
	}
}
