#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>

namespace Skeleton {

struct Vec { float x, y, z; };
struct BoneSegment { Vec a, b; };

using ReadFn = std::function<bool(uint64_t, void*, std::size_t)>;

std::vector<BoneSegment> BuildSkeleton(
    uint64_t game, uint64_t battleModel,
    int32_t entityId, const ReadFn& read);

std::vector<BoneSegment> BuildFromView(
    uint64_t view, const ReadFn& read);

} // namespace Skeleton
