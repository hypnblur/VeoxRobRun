#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include "../Misc/Vector.h"
#include "Matrix4x4.h"

namespace Skeleton {

using Vec = Vector3;
struct BoneSegment {
    Vec a, b;
    bool visible = true;
    bool startsVisible = true;
    // Each split is a fraction along a -> b. A split at 1 means the colour
    // stays the same through the end of the segment.
    float visibilitySplits[2] = { 1.f, 1.f };
};

template <typename IsVisible>
void SampleBoneVisibility(BoneSegment& segment, IsVisible isVisible)
{
    const bool samples[] = {
        isVisible(segment.a),
        isVisible(Vec::Lerp(segment.a, segment.b, 0.5f)),
        isVisible(segment.b)
    };
    segment.visible = samples[0] || samples[1] || samples[2];
    segment.startsVisible = samples[0];
    segment.visibilitySplits[0] = 1.f;
    segment.visibilitySplits[1] = 1.f;

    int splitCount = 0;
    for (int half = 0; half < 2; ++half) {
        if (samples[half] == samples[half + 1]) continue;
        float lo = half * 0.5f;
        float hi = lo + 0.5f;
        for (int step = 0; step < 2; ++step) {
            const float mid = (lo + hi) * 0.5f;
            if (isVisible(Vec::Lerp(segment.a, segment.b, mid)) == samples[half])
                lo = mid;
            else
                hi = mid;
        }
        segment.visibilitySplits[splitCount++] = (lo + hi) * 0.5f;
    }
}

inline bool IsValidBone(const Vec& point, const Vec& root, float maxDist = 3.0f)
{
    if (std::abs(point.x) < 0.1f && std::abs(point.y) < 0.1f && std::abs(point.z) < 0.1f)
        return false;
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
        return false;
    return Distance(point, root) <= maxDist;
}

inline bool IsValidBoneSegment(const Vec& a, const Vec& b, const Vec& root, float maxDist = 3.0f)
{
    if (!IsValidBone(a, root, maxDist) || !IsValidBone(b, root, maxDist))
        return false;
    const float len = Distance(a, b);
    return len >= 0.02f && len <= 1.25f;
}

// Full local pose of one bone. Animator-activity detection needs the rotation:
// Unity skeletal animation changes local rotations while local positions often
// stay constant, so a position-only comparison misses most movement.
struct BoneLocal {
    Vec position{};
    float rotation[4]{}; // x y z w
    Vec scale{1.f, 1.f, 1.f};
};

// Named aim points resolved from the same pose as the skeleton.
struct BonePoints {
    Vec head{}, neck{}, chest{}, pelvis{};
    Vec leftShoulder{}, leftElbow{}, leftHand{};
    Vec rightShoulder{}, rightElbow{}, rightHand{};
    Vec leftHip{}, leftKnee{}, leftFoot{};
    Vec rightHip{}, rightKnee{}, rightFoot{};
    bool hasHead = false, hasNeck = false, hasChest = false, hasPelvis = false, valid = false;
    std::uint32_t available = 0;
};

using ReadFn = std::function<bool(uint64_t, void*, std::size_t)>;

// Everything about a rig that stays constant while the character exists:
// the animation-matrix array, the bone parent table and which bones form the
// skeleton. Resolving this requires class-name and pointer walks, so it is
// cached per entity. Sampling then costs a single read of the matrix array.
struct RigLayout {
    std::uint64_t matrices = 0;
    std::uint64_t nativeTransform = 0;
    std::uint64_t hierarchy = 0;
    std::int32_t count = 0;
    std::vector<std::int32_t> parent;
    std::vector<std::uint8_t> selected;
    std::vector<std::pair<std::int32_t, std::int32_t>> segments;
    std::vector<std::int32_t> boneMap;
    std::int32_t head = -1;
    std::int32_t neck = -1;
    std::int32_t chest = -1;
    std::int32_t pelvis = -1;
    std::int32_t leftShoulder = -1, leftElbow = -1, leftHand = -1;
    std::int32_t rightShoulder = -1, rightElbow = -1, rightHand = -1;
    std::int32_t leftHip = -1, leftKnee = -1, leftFoot = -1;
    std::int32_t rightHip = -1, rightKnee = -1, rightFoot = -1;
};

struct AimBone {
    const char* name;
    Vec BonePoints::*point;
    std::int32_t RigLayout::*index;
};

// Keep the order stable. Aim profiles store one bit per entry.
inline constexpr AimBone kAimBones[] = {
    { "Head", &BonePoints::head, &RigLayout::head },
    { "Neck", &BonePoints::neck, &RigLayout::neck },
    { "Chest (Spine)", &BonePoints::chest, &RigLayout::chest },
    { "Pelvis", &BonePoints::pelvis, &RigLayout::pelvis },
    { "Left Shoulder", &BonePoints::leftShoulder, &RigLayout::leftShoulder },
    { "Left Elbow", &BonePoints::leftElbow, &RigLayout::leftElbow },
    { "Left Hand", &BonePoints::leftHand, &RigLayout::leftHand },
    { "Right Shoulder", &BonePoints::rightShoulder, &RigLayout::rightShoulder },
    { "Right Elbow", &BonePoints::rightElbow, &RigLayout::rightElbow },
    { "Right Hand", &BonePoints::rightHand, &RigLayout::rightHand },
    { "Left Hip", &BonePoints::leftHip, &RigLayout::leftHip },
    { "Left Knee", &BonePoints::leftKnee, &RigLayout::leftKnee },
    { "Left Foot", &BonePoints::leftFoot, &RigLayout::leftFoot },
    { "Right Hip", &BonePoints::rightHip, &RigLayout::rightHip },
    { "Right Knee", &BonePoints::rightKnee, &RigLayout::rightKnee },
    { "Right Foot", &BonePoints::rightFoot, &RigLayout::rightFoot },
};
inline constexpr int kAimBoneCount = sizeof(kAimBones) / sizeof(kAimBones[0]);
inline constexpr std::uint32_t kDefaultAimBones = 0x0f;
inline constexpr std::uint32_t kAllAimBones = (1u << kAimBoneCount) - 1u;

// Live transform chain of one character, re-navigated from the entity view.
// The view is the per-entity registry entry (BattleEntitiesUpdater ->
// _characterEntityViews); the root transform hangs off its movement view
// (MovementView / CharacterMovementView._transform). Unity recreates the
// hierarchy and its arrays on weapon/armour changes, pooling and respawns, so
// the array pointers must never be trusted across passes.
struct RigTarget {
    std::uint64_t view = 0;
    std::uint64_t movementView = 0;
    std::uint64_t managedTransform = 0;
    std::uint64_t nativeTransform = 0;
    std::uint64_t hierarchy = 0;
    std::uint64_t matrices = 0;
    std::int32_t count = 0;
};

// Resolves and validates the layout from a BattleCharacterEntityView.
bool BuildLayoutFromView(std::uint64_t view, const ReadFn& read, RigLayout& out);

// World position of the rig root (the parentless node of the transform
// hierarchy). Views only exist for streamed players and are not reliably keyed
// by entity id, so the reader compares this with each player's replicated
// position to find which player a view belongs to.
bool RootWorldPosition(std::uint64_t view, const ReadFn& read, Vec& out);

// Walks view -> movement view -> managed Transform -> native Transform ->
// hierarchy and returns the current array pointers. Cheap enough to run for a
// few entities per pass; no class-name reads on this path.
bool ResolveRigTarget(std::uint64_t view, const ReadFn& read, RigTarget& out);

// True when a cached layout still describes the target's live arrays.
bool MatchesRigTarget(const RigLayout& layout, const RigTarget& target);

// Quick check: re-reads hierarchy pointer from the native transform and
// verifies it still matches. Returns false when Unity reallocated the
// hierarchy (weapon equip, armour change), meaning the cached matrices
// pointer is stale and the layout must be rebuilt.
bool ValidateHierarchy(const RigLayout& layout, const ReadFn& read);

// Name-based fallback: walks the native transform hierarchy and matches
// bones by GameObject name against known humanoid names. Used when the
// weapon/limb IK view is temporarily unbound (holster, reload, vehicle).
bool BuildLayoutByName(std::uint64_t view, const ReadFn& read, RigLayout& out);

struct PoseStats {
    int total = 0;
    int invalid = 0;
    int normZero = 0;    // quaternion norm ~0 (unread/zeroed data)
    int normBad = 0;     // norm outside [0.5,1.5] but > 0.01
    int posInf = 0;      // non-finite position or scale
    int chainDrop = 0;   // valid bone but parent invalid
};

// Reads the current animation pose and emits world-space bone segments.
// Returns false when the layout is no longer valid (freed/moved array,
// corrupt pose), in which case the caller should rebuild it.
bool SampleLayout(const RigLayout& layout, const ReadFn& read,
    std::vector<BoneSegment>& out, BonePoints* points = nullptr,
    BoneLocal* localOut = nullptr, Matrix4x4* worldMatricesOut = nullptr,
    PoseStats* stats = nullptr);

// Processes pre-read pose data (from a scatter read) instead of issuing its
// own DMA read. The buffer must hold layout.count * 48 bytes (one TRS per bone).
bool ProcessPoseData(const RigLayout& layout, const void* poseData,
    std::size_t poseSize, std::vector<BoneSegment>& out,
    BonePoints* points = nullptr, BoneLocal* localOut = nullptr,
    Matrix4x4* worldMatricesOut = nullptr,
    PoseStats* stats = nullptr);

// Returns the byte size of the pose buffer needed for a layout.
inline std::size_t PoseDataSize(const RigLayout& layout) {
    return static_cast<std::size_t>(layout.count) * 48; // sizeof(TRS)
}

// Convenience one-shot extraction (layout build + sample).
std::vector<BoneSegment> BuildFromView(
    std::uint64_t view, const ReadFn& read);

// Looks up the BattleCharacterEntityView pointer for an entityId via the
// active BattleEntitiesUpdater dictionary. Returns 0 if not found.
std::uint64_t FindEntityView(
    std::uint64_t game, std::uint64_t battleModel,
    std::int32_t entityId, const ReadFn& read);

// Fallback used when the view pointer is unknown: walks the active updater to
// find the entity view, then extracts the rig.
std::vector<BoneSegment> BuildSkeleton(
    std::uint64_t game, std::uint64_t battleModel,
    std::int32_t entityId, const ReadFn& read);

} // namespace Skeleton
