#include "pch.h"
#include "CameraFinder.h"
#include "Memory.h"

namespace CameraFinder {

static uintptr_t unityPlayer = 0;

void Init(uintptr_t unityPlayerBase) {
    unityPlayer = unityPlayerBase;
}

uintptr_t GetTaggedNodes(uintptr_t s_Instance, int n5) {
    if (!s_Instance)
        return 0;

    uint32_t v3;
    uint32_t v4;
    uint64_t v5;
    uint32_t v6;
    uintptr_t i_1;
    uint32_t n8;
    uintptr_t i;

    const uint32_t mask = TargetProcess.Read<uint32_t>(s_Instance + 0x08);
    const uintptr_t table = TargetProcess.Read<uint64_t>(s_Instance);

    if (!table)
        return 0;

    const uint32_t n = static_cast<uint32_t>(n5);
    const uint32_t a = 4097u * n + 2127912214u;

    v3 = a ^ (a >> 19) ^ 0xC761C23Cu;
    v4 = (33u * v3 + 374761393u) << 9;

    const uint32_t b =
        9u * (v4 ^ (33u * v3 - 369570787u)) - 42973499u;

    const uint32_t hash =
        b ^ (b >> 16) ^ 0xB55A4F09u;

    v5 = hash & mask;
    v6 = hash & 0xFFFFFFFCu;

    i_1 = table + 3ull * v5;

    if (TargetProcess.Read<uint32_t>(i_1) != v6 ||
        n5 != static_cast<int32_t>(TargetProcess.Read<uint32_t>(i_1 + 8))) {
        n8 = 8;

        if (TargetProcess.Read<uint32_t>(i_1) == 0xFFFFFFFFu)
            return 0;

        // Bounded probe: a corrupt/partial table must not spin forever.
        const uint32_t probeLimit = (mask > 0xFFFFu) ? 0x10000u : (mask + 8u);
        for (uint32_t probe = 0; probe < probeLimit; ++probe) {
            v5 = mask & static_cast<uint32_t>(n8 + v5);
            i_1 = table + 2ull * v5 + static_cast<uint32_t>(v5);

            if (TargetProcess.Read<uint32_t>(i_1) == v6 &&
                n5 == static_cast<int32_t>(TargetProcess.Read<uint32_t>(i_1 + 8))) {
                break;
            }

            n8 += 8;

            if (TargetProcess.Read<uint32_t>(i_1) == 0xFFFFFFFFu)
                return 0;
        }
    }

    for (
        i = table + 3ull * (static_cast<uint64_t>(mask) + 8ull);
        i_1 < i;
        i_1 += 24ull
    ) {
        if (TargetProcess.Read<uint32_t>(i_1) < 0xFFFFFFFEu)
            break;
    }

    if (table + 3ull * (static_cast<uint64_t>(mask) + 8ull) == i_1)
        return 0;

    return TargetProcess.Read<uint64_t>(i_1 + 16);
}

uintptr_t FindMainCamera() {
    if (!unityPlayer)
        return 0;

    uintptr_t gom = TargetProcess.Read<uintptr_t>(unityPlayer + 0x2230768);
    if (!gom)
        return 0;

    uintptr_t taggedNodes = GetTaggedNodes(gom, 5);
    if (!taggedNodes)
        return 0;

    uintptr_t node = TargetProcess.Read<uintptr_t>(taggedNodes + 0x08);

    uint32_t range = TargetProcess.Read<uint32_t>(unityPlayer + 0x21150C4);
    uint32_t base = TargetProcess.Read<uint32_t>(unityPlayer + 0x21150C0);

    // Bounded walk: a cyclic tagged-node list must not hang the reader.
    for (int guard = 0; node && node != taggedNodes && guard < 65536; ++guard) {
        uintptr_t entry = TargetProcess.Read<uintptr_t>(node + 0x10);
        if (entry) {
            uintptr_t begin = TargetProcess.Read<uintptr_t>(entry + 0x20);
            uintptr_t count = TargetProcess.Read<uintptr_t>(entry + 0x30);

            for (uintptr_t i = 0; begin && i < count && i < 4096; ++i) {
                uintptr_t element = begin + i * 0x10;
                uint32_t identifier = TargetProcess.Read<uint32_t>(element);

                if (uint32_t(identifier - base) < range) {
                    uintptr_t object = TargetProcess.Read<uintptr_t>(element + 0x08);
                    if (object)
                        return object;
                    break;
                }
            }
        }
        node = TargetProcess.Read<uintptr_t>(node + 0x08);
    }

    return 0;
}

CameraData GetCameraMatrices() {
    CameraData data{};
    data.valid = false;

    uintptr_t cameraNode = FindMainCamera();
    if (!cameraNode)
        return data;

    auto view = TargetProcess.Read<Matrix4x4>(cameraNode + 0x70);
    auto projection = TargetProcess.Read<Matrix4x4>(cameraNode + 0xB0);

    data.viewMatrix = view * projection;
    data.valid = true;
    return data;
}

} // namespace CameraFinder
