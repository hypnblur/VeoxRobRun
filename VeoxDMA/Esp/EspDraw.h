#pragma once
// Shared ESP drawing primitives, ported from PureDMADBD's EspDraw. Pure
// drawing only: no DMA reads, no config writes, no locks. Safe to call from
// the overlay thread every frame.

#include "ImGUI/imgui.h"
#include "Misc/Vector.h"
#include <d2d1.h>
#include <functional>

namespace Esp {

// Stack side for labels around a box. The int form matches the persisted
// profile value (0..3).
enum StackSide : int { StackTop = 0, StackBottom = 1, StackLeft = 2, StackRight = 3 };

// Screen rectangle an ESP element covered. The preview panel can use these to
// hit-test drags; the live overlay passes nullptr and pays nothing.
struct EspRect {
	bool valid = false;
	float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
	void Set(float x0, float y0, float x1, float y1)
	{
		valid = true; minX = x0; minY = y0; maxX = x1; maxY = y1;
	}
};

// Cursors around a box so every side stacks independently. Seed from the box
// rect (`FromBox`) or from a point (`FromPoint`) before stacking.
struct LabelStack {
	float left = 0.f, right = 0.f;         // vertical edge X
	float topY = 0.f, bottomY = 0.f;       // horizontal edge Y (seed)
	float centerX = 0.f;                   // horizontal centre for top/bottom text
	float aboveY = 0.f, belowY = 0.f;      // running Y for top / bottom sides
	float leftY = 0.f, rightY = 0.f;       // running Y for left / right sides
	float boxL = 0.f, boxR = 0.f;          // original box edges (top/bottom bars)

	void FromBox(float x0, float y0, float x1, float y1);
	void FromPoint(float x, float y, float halfHeight = 10.f);
};

// 8-direction outline + colour pass. `col` is the fill. The outline is the
// same opaque-black cross+diagonals the source uses so letters stay readable
// on top of bright textures. `pos` is the top-left; the function pixel-snaps.
void DrawOutlinedText(ImDrawList* dl, const ImVec2& pos, ImU32 col,
	ImFont* font, float size, const char* text);

// Full-outline box with optional 1-px black backing outline.
void DrawFullBox(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
	ImU32 col, float thickness, bool outline);

// Four-corner bracket box. `pct` is the corner length as a percentage of the
// box's shorter edge, clamped to [10, 50].
void DrawCornerBox(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
	ImU32 col, float thickness, float pct, bool outline);

// Semi-transparent fill under the box. `fillAlpha` is 0..1 and is folded into
// the colour's alpha channel.
void DrawFilledBox(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
	ImU32 col, float fillAlpha);

// Stack one line of outlined text against the given side of the label stack.
// Advances the stack's cursor so the next call on the same side stacks under
// (or above) this one.
void DrawStackedText(ImDrawList* dl, LabelStack& st, int side,
	ImFont* font, float size, const char* text, ImU32 col,
	EspRect* outRect = nullptr);

// Progress bar stacked against any side. Left/right bars are 3 px wide and as
// tall as the box; top/bottom bars are 3 px tall and as wide as the box.
// `fill` in [0,1]. The bar advances that side's cursor so labels on the same
// side sit beyond it.
void DrawStackedBar(ImDrawList* dl, LabelStack& st, int side,
	float fill, ImU32 col, EspRect* outRect = nullptr);

// Project a world-space axis-aligned box centred on `worldPos` and return the
// smallest screen-space AABB that contains it. Half extents are in the same
// world units as `worldPos`; `halfHeight` is vertical (Unity Y-up) and
// `halfWidth` spans X/Z. `project` returns false for points behind the near
// plane. Returns false when any corner fails or the result is smaller than
// `minPx`.
using Projector = std::function<bool(const Vector3& world, float& sx, float& sy)>;
bool ProjectWorldAABB(const Projector& project, const Vector3& worldPos,
	float halfWidth, float halfHeight,
	ImVec2& outMin, ImVec2& outMax, float minPx = 6.f);

// Shade a D2D1 colour to ImU32, with an optional extra alpha multiplier.
ImU32 ShadeColour(const D2D1::ColorF& c, float extraAlpha = 1.f);

// Convert a D2D1 colour into the ImGui packed ImU32 colour. Shared by every
// ESP module (player boxes, world entities, radar, menu previews) so the whole
// overlay cannot carry private copies of the same conversion.
ImU32 D2DColorToImU32(const D2D1::ColorF& c);

} // namespace Esp
