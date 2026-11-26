#pragma once
#include <cstddef>
#include <cstdint>

namespace Base::ABCurves {
inline constexpr std::size_t kRendererBlobSize = 44484;
extern const std::uint8_t kRendererBlob[kRendererBlobSize];
inline constexpr std::size_t kDefaultProfileTicks = 256;
extern const std::int16_t kDefaultProfile[kDefaultProfileTicks][2];
} // namespace Base::ABCurves
