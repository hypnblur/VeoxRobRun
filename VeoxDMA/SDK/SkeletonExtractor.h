#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace Skeleton {

struct Vec { float x, y, z; };
struct BoneSegment { Vec a, b; bool visible = true; };

// Named aim points resolved from the same pose as the skeleton. Only valid when
// the rig exposed all four anchors.
struct BonePoints {
    Vec head{}, neck{}, chest{}, pelvis{};
    Vec leftShoulder{}, leftElbow{}, leftHand{};
    Vec rightShoulder{}, rightElbow{}, rightHand{};
    Vec leftHip{}, leftKnee{}, leftFoot{};
    Vec rightHip{}, rightKnee{}, rightFoot{};
    bool valid = false;
};

using ReadFn = std::function<bool(uint64_t, void*, std::size_t)>;

// Everything about a rig that stays constant while the character exists:
// the animation-matrix array, the bone parent table and which bones form the
// skeleton. Resolving this requires class-name and pointer walks, so it is
// cached per entity. Sampling then costs a single read of the matrix array.
struct RigLayout {
    std::uint64_t matrices = 0;
    std::int32_t count = 0;
    std::vector<std::int32_t> parent;
    std::vector<std::uint8_t> selected;
    std::vector<std::pair<std::int32_t, std::int32_t>> segments;
    std::int32_t head = -1;
    std::int32_t neck = -1;
    std::int32_t chest = -1;
    std::int32_t pelvis = -1;
    std::int32_t leftShoulder = -1, leftElbow = -1, leftHand = -1;
    std::int32_t rightShoulder = -1, rightElbow = -1, rightHand = -1;
    std::int32_t leftHip = -1, leftKnee = -1, leftFoot = -1;
    std::int32_t rightHip = -1, rightKnee = -1, rightFoot = -1;
};

// Resolves and validates the layout from a BattleCharacterEntityView.
bool BuildLayoutFromView(std::uint64_t view, const ReadFn& read, RigLayout& out);

// Reads the current animation pose and emits world-space bone segments.
// Returns false when the layout is no longer valid (freed/moved array,
// corrupt pose), in which case the caller should rebuild it.
bool SampleLayout(const RigLayout& layout, const ReadFn& read,
    std::vector<BoneSegment>& out, BonePoints* points = nullptr,
    Vec* localPosOut = nullptr);

// Convenience one-shot extraction (layout build + sample).
std::vector<BoneSegment> BuildFromView(
    std::uint64_t view, const ReadFn& read);

// Fallback used when the view pointer is unknown: walks the active updater to
// find the entity view, then extracts the rig.
std::vector<BoneSegment> BuildSkeleton(
    std::uint64_t game, std::uint64_t battleModel,
    std::int32_t entityId, const ReadFn& read);

} // namespace Skeleton
