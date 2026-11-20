#include "pch.h"
#include "EspDraw.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

namespace Esp {

void LabelStack::FromBox(float x0, float y0, float x1, float y1)
{
	left = x0; right = x1;
	boxL = x0; boxR = x1;
	topY = y0; bottomY = y1;
	centerX = (x0 + x1) * 0.5f;
	aboveY = topY - 2.f;
	belowY = bottomY;
	leftY = rightY = topY;
}

void LabelStack::FromPoint(float x, float y, float halfHeight)
{
	left = right = centerX = x;
	boxL = x - 15.f; boxR = x + 15.f;
	topY = y - halfHeight;
	bottomY = y + halfHeight;
	aboveY = topY - 2.f;
	belowY = bottomY;
	leftY = rightY = topY;
}

namespace {

ImVec2 Snap(ImVec2 p)
{
	return ImVec2(std::floor(p.x + 0.5f), std::floor(p.y + 0.5f));
}

} // namespace

ImU32 ShadeColour(const D2D1::ColorF& c, float extraAlpha)
{
	const float a = std::clamp(c.a * extraAlpha, 0.f, 1.f);
	return IM_COL32(
		(int)(std::clamp(c.r, 0.f, 1.f) * 255.f + 0.5f),
		(int)(std::clamp(c.g, 0.f, 1.f) * 255.f + 0.5f),
		(int)(std::clamp(c.b, 0.f, 1.f) * 255.f + 0.5f),
		(int)(a * 255.f + 0.5f));
}

ImU32 D2DColorToImU32(const D2D1::ColorF& c)
{
	return IM_COL32(
		(int)(c.r * 255.f),
		(int)(c.g * 255.f),
		(int)(c.b * 255.f),
		(int)(c.a * 255.f));
}

void DrawOutlinedText(ImDrawList* dl, const ImVec2& pos, ImU32 col,
	ImFont* font, float size, const char* text)
{
	if (!text || !text[0] || !font) return;
	const ImVec2 p = Snap(pos);
	const ImU32 outline = IM_COL32(0, 0, 0, 220);
	for (int dy = -1; dy <= 1; ++dy)
		for (int dx = -1; dx <= 1; ++dx)
			if (dx || dy)
				dl->AddText(font, size, ImVec2(p.x + dx, p.y + dy), outline, text);
	dl->AddText(font, size, p, col, text);
}

void DrawFullBox(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
	ImU32 col, float thickness, bool outline)
{
	if (outline) {
		dl->AddRect(ImVec2(mn.x - 1.f, mn.y - 1.f),
			ImVec2(mx.x + 1.f, mx.y + 1.f),
			IM_COL32(0, 0, 0, 255), 0.f, 0, thickness + 1.f);
	}
	dl->AddRect(mn, mx, col, 0.f, 0, thickness);
}

void DrawCornerBox(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
	ImU32 col, float thickness, float pct, bool outline)
{
	const float w = mx.x - mn.x, h = mx.y - mn.y;
	const float side = std::min(w, h);
	if (side <= 2.f) return;
	const float pctClamped = std::clamp(pct, 10.f, 50.f) * 0.01f;
	const float c = std::clamp(side * pctClamped, 2.f, side * 0.5f);
	auto line = [&](ImVec2 a, ImVec2 b) {
		if (outline)
			dl->AddLine(a, b, IM_COL32(0, 0, 0, 255), thickness + 1.f);
		dl->AddLine(a, b, col, thickness);
	};
	// top-left
	line(mn, ImVec2(mn.x + c, mn.y));
	line(mn, ImVec2(mn.x, mn.y + c));
	// top-right
	line(ImVec2(mx.x - c, mn.y), ImVec2(mx.x, mn.y));
	line(ImVec2(mx.x, mn.y), ImVec2(mx.x, mn.y + c));
	// bottom-left
	line(ImVec2(mn.x, mx.y - c), ImVec2(mn.x, mx.y));
	line(ImVec2(mn.x, mx.y), ImVec2(mn.x + c, mx.y));
	// bottom-right
	line(ImVec2(mx.x - c, mx.y), ImVec2(mx.x, mx.y));
	line(ImVec2(mx.x, mx.y - c), ImVec2(mx.x, mx.y));
}

void DrawFilledBox(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
	ImU32 col, float fillAlpha)
{
	const float a = std::clamp(fillAlpha, 0.f, 1.f);
	if (a <= 0.f) return;
	const unsigned alpha = (unsigned)(a * 255.f + 0.5f);
	const ImU32 c = (col & 0x00FFFFFFu) | (alpha << IM_COL32_A_SHIFT);
	dl->AddRectFilled(mn, mx, c);
}

void DrawStackedText(ImDrawList* dl, LabelStack& st, int side,
	ImFont* font, float size, const char* text, ImU32 col,
	EspRect* outRect)
{
	if (!text || !text[0] || !font) return;
	const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
	constexpr float kGap = 2.f;
	constexpr float kSideGap = 4.f;
	float x = 0, y = 0;
	switch (side) {
	case StackLeft:
		x = st.left - kSideGap - sz.x;
		y = st.leftY;
		st.leftY += sz.y + kGap;
		break;
	case StackRight:
		x = st.right + kSideGap;
		y = st.rightY;
		st.rightY += sz.y + kGap;
		break;
	case StackTop:
		st.aboveY -= sz.y + kGap;
		x = st.centerX - sz.x * 0.5f;
		y = st.aboveY;
		break;
	case StackBottom:
	default:
		x = st.centerX - sz.x * 0.5f;
		y = st.belowY + kGap;
		st.belowY = y + sz.y;
		break;
	}
	DrawOutlinedText(dl, ImVec2(x, y), col, font, size, text);
	if (outRect)
		outRect->Set(x, y, x + sz.x, y + sz.y);
}

void DrawStackedBar(ImDrawList* dl, LabelStack& st, int side,
	float fill, ImU32 col, EspRect* outRect)
{
	constexpr float kBar = 3.f;
	constexpr float kSideGap = 4.f;
	constexpr float kGap = 3.f;
	const float f = std::clamp(fill, 0.f, 1.f);
	ImVec2 mn, mx;
	bool vertical = true;
	switch (side) {
	case StackLeft:
		mn = ImVec2(st.left - kSideGap - kBar, st.topY);
		st.left -= kSideGap + kBar;
		break;
	case StackRight:
		mn = ImVec2(st.right + kSideGap, st.topY);
		st.right += kSideGap + kBar;
		break;
	case StackTop:
		vertical = false;
		st.aboveY -= kBar + kGap;
		mn = ImVec2(st.boxL, st.aboveY);
		break;
	case StackBottom:
	default:
		vertical = false;
		mn = ImVec2(st.boxL, st.belowY + kGap);
		st.belowY = mn.y + kBar;
		break;
	}
	if (vertical)
		mx = ImVec2(mn.x + kBar, mn.y + std::max(8.f, st.bottomY - st.topY));
	else
		mx = ImVec2(mn.x + std::max(8.f, st.boxR - st.boxL), mn.y + kBar);

	dl->AddRectFilled(ImVec2(mn.x - 1.f, mn.y - 1.f),
		ImVec2(mx.x + 1.f, mx.y + 1.f),
		IM_COL32(0, 0, 0, 220));
	if (vertical)
		dl->AddRectFilled(ImVec2(mn.x, mx.y - (mx.y - mn.y) * f), mx, col);
	else
		dl->AddRectFilled(mn, ImVec2(mn.x + (mx.x - mn.x) * f, mx.y), col);
	if (outRect)
		outRect->Set(mn.x - 1.f, mn.y - 1.f, mx.x + 1.f, mx.y + 1.f);
}

bool ProjectWorldAABB(const Projector& project, const Vector3& worldPos,
	float halfWidth, float halfHeight,
	ImVec2& outMin, ImVec2& outMax, float minPx)
{
	if (!project) return false;
	if (!std::isfinite(worldPos.x) || !std::isfinite(worldPos.y) || !std::isfinite(worldPos.z)) return false;
	if (!std::isfinite(halfWidth) || !std::isfinite(halfHeight) || halfWidth <= 0.f || halfHeight <= 0.f) return false;
	// Unity is Y-up: halfHeight spans the vertical axis, halfWidth spans X/Z.
	const Vector3 corners[8] = {
		Vector3(worldPos.x - halfWidth, worldPos.y - halfHeight, worldPos.z - halfWidth),
		Vector3(worldPos.x + halfWidth, worldPos.y - halfHeight, worldPos.z - halfWidth),
		Vector3(worldPos.x - halfWidth, worldPos.y + halfHeight, worldPos.z - halfWidth),
		Vector3(worldPos.x + halfWidth, worldPos.y + halfHeight, worldPos.z - halfWidth),
		Vector3(worldPos.x - halfWidth, worldPos.y - halfHeight, worldPos.z + halfWidth),
		Vector3(worldPos.x + halfWidth, worldPos.y - halfHeight, worldPos.z + halfWidth),
		Vector3(worldPos.x - halfWidth, worldPos.y + halfHeight, worldPos.z + halfWidth),
		Vector3(worldPos.x + halfWidth, worldPos.y + halfHeight, worldPos.z + halfWidth),
	};
	float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
	for (const Vector3& c : corners) {
		float sx = 0.f, sy = 0.f;
		if (!project(c, sx, sy)) return false;
		if (sx < minX) minX = sx;
		if (sx > maxX) maxX = sx;
		if (sy < minY) minY = sy;
		if (sy > maxY) maxY = sy;
	}
	outMin = ImVec2(minX, minY);
	outMax = ImVec2(maxX, maxY);
	return (maxX - minX) >= minPx && (maxY - minY) >= minPx;
}

void DrawSphere2D(ImDrawList* dl, const ImVec2& center, float radius,
	ImU32 fillCol, bool outline, ImU32 outlineCol, float outlineThickness)
{
	if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(radius)) return;
	if (radius <= 0.5f || radius > 250.0f) return;
	const int segments = std::clamp((int)(radius * 1.5f), 12, 36);
	dl->AddCircleFilled(center, radius, fillCol, segments);
	if (outline && (outlineCol & IM_COL32_A_MASK) != 0) {
		dl->AddCircle(center, radius, outlineCol, segments, outlineThickness);
	}
}

void DrawCapsule2D(ImDrawList* dl, const ImVec2& a, float radiusA,
	const ImVec2& b, float radiusB, ImU32 fillCol,
	bool outline, ImU32 outlineCol, float outlineThickness,
	bool capA, bool capB)
{
	if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y) ||
		!std::isfinite(radiusA) || !std::isfinite(radiusB)) return;

	radiusA = std::clamp(radiusA, 0.5f, 250.0f);
	radiusB = std::clamp(radiusB, 0.5f, 250.0f);

	const float dx = b.x - a.x;
	const float dy = b.y - a.y;
	const float distSq = dx * dx + dy * dy;
	const float dist = std::sqrt(distSq);

	if (dist < 0.5f) {
		DrawSphere2D(dl, a, std::max(radiusA, radiusB), fillCol, outline, outlineCol, outlineThickness);
		return;
	}

	const float ux = dx / dist;
	const float uy = dy / dist;
	const float nx = -uy;
	const float ny = ux;

	const float dr = radiusA - radiusB;

	if (std::abs(dr) >= dist) {
		if (radiusA >= radiusB)
			DrawSphere2D(dl, a, radiusA, fillCol, outline, outlineCol, outlineThickness);
		else
			DrawSphere2D(dl, b, radiusB, fillCol, outline, outlineCol, outlineThickness);
		return;
	}

	const float sinPhi = std::clamp(dr / dist, -0.9999f, 0.9999f);
	const float cosPhi = std::sqrt(std::max(0.0f, 1.0f - sinPhi * sinPhi));

	const float n1x = nx * cosPhi + ux * sinPhi;
	const float n1y = ny * cosPhi + uy * sinPhi;

	const float n2x = -nx * cosPhi + ux * sinPhi;
	const float n2y = -ny * cosPhi + uy * sinPhi;

	const float beta = std::acos(sinPhi);
	constexpr int kArcSteps = 6;

	std::vector<ImVec2> poly;
	poly.reserve(kArcSteps * 2 + 6);

	const ImVec2 tA1(a.x + radiusA * n1x, a.y + radiusA * n1y);
	const ImVec2 tB1(b.x + radiusB * n1x, b.y + radiusB * n1y);
	const ImVec2 tB2(b.x + radiusB * n2x, b.y + radiusB * n2y);
	const ImVec2 tA2(a.x + radiusA * n2x, a.y + radiusA * n2y);

	poly.push_back(tA1);
	poly.push_back(tB1);

	std::vector<ImVec2> arcB;
	if (capB) {
		arcB.reserve(kArcSteps + 1);
		arcB.push_back(tB1);
		for (int i = 1; i < kArcSteps; ++i) {
			const float t = -1.0f + 2.0f * ((float)i / (float)kArcSteps);
			const float psi = t * beta;
			const float cosPsi = std::cos(psi);
			const float sinPsi = std::sin(psi);
			const float vx = ux * cosPsi - nx * sinPsi;
			const float vy = uy * cosPsi - ny * sinPsi;
			const ImVec2 pt(b.x + radiusB * vx, b.y + radiusB * vy);
			poly.push_back(pt);
			arcB.push_back(pt);
		}
		arcB.push_back(tB2);
	}
	poly.push_back(tB2);
	poly.push_back(tA2);

	std::vector<ImVec2> arcA;
	if (capA) {
		arcA.reserve(kArcSteps + 1);
		arcA.push_back(tA2);
		const float gamma = 3.14159265f - beta;
		for (int i = 1; i < kArcSteps; ++i) {
			const float t = -1.0f + 2.0f * ((float)i / (float)kArcSteps);
			const float psi = t * gamma;
			const float cosPsi = std::cos(psi);
			const float sinPsi = std::sin(psi);
			const float wx = -ux * cosPsi + nx * sinPsi;
			const float wy = -uy * cosPsi + ny * sinPsi;
			const ImVec2 pt(a.x + radiusA * wx, a.y + radiusA * wy);
			poly.push_back(pt);
			arcA.push_back(pt);
		}
		arcA.push_back(tA1);
	}

	dl->AddConvexPolyFilled(poly.data(), (int)poly.size(), fillCol);

	if (outline && (outlineCol & IM_COL32_A_MASK) != 0) {
		// Stroke lateral edge 1
		dl->AddLine(tA1, tB1, outlineCol, outlineThickness);
		// Stroke lateral edge 2
		dl->AddLine(tB2, tA2, outlineCol, outlineThickness);
		// Stroke outer extremity arcs without drawing interior seam circles
		if (capB && arcB.size() >= 2) {
			dl->AddPolyline(arcB.data(), (int)arcB.size(), outlineCol, 0, outlineThickness);
		}
		if (capA && arcA.size() >= 2) {
			dl->AddPolyline(arcA.data(), (int)arcA.size(), outlineCol, 0, outlineThickness);
		}
	}
}

} // namespace Esp
