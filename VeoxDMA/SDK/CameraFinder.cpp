#include "pch.h"
#include "CameraFinder.h"
#include "Memory.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace CameraFinder {

static std::atomic<uintptr_t> unityPlayer{ 0 };

void Init(uintptr_t unityPlayerBase) {
    unityPlayer.store(unityPlayerBase, std::memory_order_release);
    InvalidateCache();
}

uintptr_t GetTaggedNodes(uintptr_t s_Instance, int n5) {
    if (!s_Instance)
        return 0;

    auto readFreshU32 = [](uintptr_t addr) -> uint32_t {
        uint32_t v = 0;
        return TargetProcess.ReadFresh(addr, &v, sizeof(v)) ? v : 0;
    };
    auto readFreshU64 = [](uintptr_t addr) -> uintptr_t {
        uintptr_t v = 0;
        return TargetProcess.ReadFresh(addr, &v, sizeof(v)) ? v : 0;
    };

    uint32_t v3;
    uint32_t v4;
    uint64_t v5;
    uint32_t v6;
    uintptr_t i_1;
    uint32_t n8;
    uintptr_t i;

    const uint32_t mask = readFreshU32(s_Instance + 0x08);
    const uintptr_t table = readFreshU64(s_Instance);

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

    if (readFreshU32(i_1) != v6 ||
        n5 != static_cast<int32_t>(readFreshU32(i_1 + 8))) {
        n8 = 8;

        if (readFreshU32(i_1) == 0xFFFFFFFFu)
            return 0;

        // Bounded probe: a corrupt/partial table must not spin forever.
        const uint32_t probeLimit = (mask > 0xFFFFu) ? 0x10000u : (mask + 8u);
        for (uint32_t probe = 0; probe < probeLimit; ++probe) {
            v5 = mask & static_cast<uint32_t>(n8 + v5);
            i_1 = table + 2ull * v5 + static_cast<uint32_t>(v5);

            if (readFreshU32(i_1) == v6 &&
                n5 == static_cast<int32_t>(readFreshU32(i_1 + 8))) {
                break;
            }

            n8 += 8;

            if (readFreshU32(i_1) == 0xFFFFFFFFu)
                return 0;
        }
    }

    for (
        i = table + 3ull * (static_cast<uint64_t>(mask) + 8ull);
        i_1 < i;
        i_1 += 24ull
    ) {
        if (readFreshU32(i_1) < 0xFFFFFFFEu)
            break;
    }

    if (table + 3ull * (static_cast<uint64_t>(mask) + 8ull) == i_1)
        return 0;

    return readFreshU64(i_1 + 16);
}

namespace {

constexpr uintptr_t kGomSlot = 0x2230768;
constexpr uintptr_t kTagRange = 0x21150C4;
constexpr uintptr_t kTagBase = 0x21150C0;

// Collects the objects attached to the MainCamera tag. Normally there is one,
// but a wrong/preview camera must not be silently used, so callers can inspect
// every candidate.
void CollectTaggedObjects(std::vector<uintptr_t>& out, std::size_t max) {
    out.clear();
    const uintptr_t up = unityPlayer.load(std::memory_order_acquire);
    if (!up)
        return;

    auto readFreshU64 = [](uintptr_t addr) -> uintptr_t {
        uintptr_t v = 0;
        return TargetProcess.ReadFresh(addr, &v, sizeof(v)) ? v : 0;
    };
    auto readFreshU32 = [](uintptr_t addr) -> uint32_t {
        uint32_t v = 0;
        return TargetProcess.ReadFresh(addr, &v, sizeof(v)) ? v : 0;
    };

    const uintptr_t gom = readFreshU64(up + kGomSlot);
    if (!gom)
        return;

    const uintptr_t taggedNodes = GetTaggedNodes(gom, 5);
    if (!taggedNodes)
        return;

    const uint32_t range = readFreshU32(up + kTagRange);
    const uint32_t base = readFreshU32(up + kTagBase);

    uintptr_t node = readFreshU64(taggedNodes + 0x08);
    for (int guard = 0; node && node != taggedNodes && guard < 65536; ++guard) {
        const uintptr_t entry = readFreshU64(node + 0x10);
        if (entry) {
            const uintptr_t begin = readFreshU64(entry + 0x20);
            const uintptr_t count = readFreshU64(entry + 0x30);
            for (uintptr_t i = 0; begin && i < count && i < 4096; ++i) {
                const uintptr_t element = begin + i * 0x10;
                const uint32_t identifier = readFreshU32(element);
                if (uint32_t(identifier - base) < range) {
                    const uintptr_t object = readFreshU64(element + 0x08);
                    if (object) {
                        out.push_back(object);
                        if (out.size() >= max)
                            return;
                    }
                    break;
                }
            }
        }
        node = readFreshU64(node + 0x08);
    }
}

// A Unity world-to-camera matrix read from column-major memory lands in the
// matrix as its transpose: rows are the camera right/up/-forward axes and the
// last row is (tx, ty, tz, 1). The camera position is -R^T * t.
bool ParseViewMatrix(const float* m, Vector3& position, Vector3& forward) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i]))
            return false;

    const float* r0 = m;
    const float* r1 = m + 4;
    const float* r2 = m + 8;
    const float* r3 = m + 12;

    if (std::fabs(r3[3] - 1.f) > 1e-2f)
        return false;

    const float l0 = r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2];
    const float l1 = r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2];
    const float l2 = r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2];
    if (std::fabs(l0 - 1.f) > 2e-2f || std::fabs(l1 - 1.f) > 2e-2f ||
        std::fabs(l2 - 1.f) > 2e-2f)
        return false;

    const float d01 = r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2];
    const float d02 = r0[0] * r2[0] + r0[1] * r2[1] + r0[2] * r2[2];
    const float d12 = r1[0] * r2[0] + r1[1] * r2[1] + r1[2] * r2[2];
    if (std::fabs(d01) > 2e-2f || std::fabs(d02) > 2e-2f ||
        std::fabs(d12) > 2e-2f)
        return false;

    // p = -(R^T * t): row i of the stored 3x3 dotted with t.
    position.x = -(r3[0] * r0[0] + r3[1] * r0[1] + r3[2] * r0[2]);
    position.y = -(r3[0] * r1[0] + r3[1] * r1[1] + r3[2] * r1[2]);
    position.z = -(r3[0] * r2[0] + r3[1] * r2[1] + r3[2] * r2[2]);

    // Row 2 of the stored matrix is -forward.
    forward.x = -r2[0];
    forward.y = -r2[1];
    forward.z = -r2[2];

    return std::isfinite(position.x) && std::isfinite(position.y) &&
        std::isfinite(position.z) && std::fabs(position.x) < 1e6f &&
        std::fabs(position.y) < 1e6f && std::fabs(position.z) < 1e6f;
}

// Unity's projectionMatrix is a perspective matrix in GL convention. Stored
// column-major and read as rows it looks like:
//   [ x 0 0 0 ] [ 0 y 0 0 ] [ 0 0 a -1 ] [ 0 0 b 0 ]
bool ParseProjectionMatrix(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i]))
            return false;

    if (!(m[0] > 0.f) || !(m[5] > 0.f))
        return false;
    if (std::fabs(m[11] + 1.f) > 1e-2f)   // m[2][3]
        return false;
    if (std::fabs(m[15]) > 1e-2f)         // m[3][3]
        return false;
    if (std::fabs(m[14]) < 1e-6f)         // m[3][2]
        return false;
    if (std::fabs(m[1]) > 1e-4f || std::fabs(m[3]) > 1e-4f ||
        std::fabs(m[4]) > 1e-4f || std::fabs(m[7]) > 1e-4f ||
        std::fabs(m[8]) > 1e-4f || std::fabs(m[9]) > 1e-4f)
        return false;

    return true;
}

struct MatrixLayout {
    std::uintptr_t viewOff = 0;
    std::uintptr_t projOff = 0;
};

constexpr std::size_t kScanBytes = 0x1000;
constexpr float kMaxPositionError = 16.f;  // metres (tolerates vehicles & custom camera zoom)
constexpr float kMinForwardDot = 0.5f;     // 60 degrees

std::atomic<std::uintptr_t> g_fovNode{ 0 };
std::atomic<std::uint64_t> g_fovNodeResolveTick{ 0 };

// Cached render camera and the structurally discovered matrix offsets. Offsets
// are stable for a Unity build, so they survive scene changes; the node does
// not, and is re-resolved when it stops matching the managed camera.
//
// Node and offsets are one value: as three separate atomics a reader could
// pair a freshly resolved node with the previous node's offsets and read
// matrices from the wrong place.
struct CameraNodeCache {
    std::uintptr_t node = 0;
    std::uintptr_t viewOff = 0;
    std::uintptr_t projOff = 0;
};
std::atomic<CameraNodeCache> g_cameraCache{ CameraNodeCache{} };
std::atomic<std::uint64_t> g_lastResolveTick{ 0 };

// Cached virtual->physical translation for the contiguous view+projection block
// when it fits inside one 4 KB page. Direct LcRead on the physical address
// skips VMMDLL's process/TLB locks and 4 KB page alignment for sub-ms reads.
struct PhysCamCache {
    std::uintptr_t va = 0;
    std::size_t span = 0;
    std::uint64_t pa = 0;
    std::uint64_t stampMs = 0;
};
std::mutex g_physCamMutex;
PhysCamCache g_physCam{};
bool ReadMatricesAt(std::uintptr_t node, std::uintptr_t viewOff,
    std::uintptr_t projOff, CameraData& data) {
    if (!node || !viewOff || !projOff)
        return false;

    Matrix4x4 view{};
    Matrix4x4 projection{};
    Vector3 position{};
    Vector3 forward{};
    bool parsed = false;
    bool usedPhys = false;

    const std::uintptr_t minOff = (std::min)(viewOff, projOff);
    const std::uintptr_t maxEnd = (std::max)(viewOff, projOff) + sizeof(Matrix4x4);
    const std::size_t span = static_cast<std::size_t>(maxEnd - minOff);
    const std::uintptr_t va = node + minOff;

    // Fastest path: contiguous block within one 4 KB page -> cached PA + direct
    // LeechCore physical read (~150-300 us, single 128-byte TLP).
    const bool singlePage = (span <= 512) &&
        ((va & ~static_cast<std::uintptr_t>(0xFFF)) ==
         ((va + span - 1) & ~static_cast<std::uintptr_t>(0xFFF)));
    if (singlePage) {
        std::uint64_t pa = 0;
        const std::uint64_t nowMs = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(g_physCamMutex);
            if (g_physCam.va == va && g_physCam.span == span &&
                g_physCam.pa != 0 && (nowMs - g_physCam.stampMs) < 3000) {
                pa = g_physCam.pa;
            }
        }
        if (!pa) {
            std::uint64_t translated = 0;
            if (TargetProcess.VirtToPhys(va, translated) && translated != 0) {
                pa = translated;
                std::lock_guard<std::mutex> lock(g_physCamMutex);
                g_physCam = PhysCamCache{ va, span, pa, nowMs };
            }
        }
        if (pa) {
            unsigned char raw[512];
            if (TargetProcess.ReadPhysicalFast(pa, raw, span)) {
                std::memcpy(&view, raw + (viewOff - minOff), sizeof(view));
                std::memcpy(&projection, raw + (projOff - minOff), sizeof(projection));
                if (ParseViewMatrix(&view.matrix[0][0], position, forward) &&
                    ParseProjectionMatrix(&projection.matrix[0][0])) {
                    parsed = true;
                    usedPhys = true;
                }
            }
            if (!parsed) {
                // Physical frame may have migrated or scene changed: drop the
                // cached PA and fall through to a virtual uncached read.
                std::lock_guard<std::mutex> lock(g_physCamMutex);
                g_physCam = PhysCamCache{};
            }
        }
    }

    if (!parsed) {
        if (span <= 512) {
            unsigned char raw[512];
            if (!TargetProcess.ReadFresh(va, raw, span))
                return false;
            std::memcpy(&view, raw + (viewOff - minOff), sizeof(view));
            std::memcpy(&projection, raw + (projOff - minOff), sizeof(projection));
        } else {
            Memory::ScopedScatter scatter(TargetProcess);
            if (!scatter) return false;
            TargetProcess.AddScatterReadRequest(scatter, node + viewOff, &view, sizeof(view));
            TargetProcess.AddScatterReadRequest(scatter, node + projOff, &projection, sizeof(projection));
            if (!TargetProcess.ExecuteReadScatter(scatter))
                return false;
        }
        if (!ParseViewMatrix(&view.matrix[0][0], position, forward))
            return false;
        if (!ParseProjectionMatrix(&projection.matrix[0][0]))
            return false;
    }

    data.view = view;
    data.projection = projection;
    data.viewMatrix = view * projection;
    data.position = position;
    data.forward = forward;
    data.valid = true;
    data.usedPhysical = usedPhys;
    data.positionError = -1.f;
    data.forwardDot = -1.f;
    return true;
}

// The projection's y scale must describe roughly the same vertical FOV as the
// managed camera. A structurally valid but unrelated matrix (or a stale
// projection) is rejected here.
bool ProjectionFovMatchesRaw(const float* m, float expectedVerticalFovDegrees) {
    if (!(expectedVerticalFovDegrees > 1.f) ||
        !(expectedVerticalFovDegrees < 179.f))
        return true;

    const float yScale = m[5];
    if (!std::isfinite(yScale) || yScale <= 0.f)
        return false;

    const float radians =
        expectedVerticalFovDegrees * 3.14159265358979323846f / 180.f;
    const float expected = 1.f / std::tan(radians * 0.5f);
    if (!std::isfinite(expected) || expected <= 0.f)
        return true;

    const float ratio = yScale / expected;
    return ratio > 0.5f && ratio < 2.f;
}

bool ProjectionFovMatches(const Matrix4x4& projection,
    float expectedVerticalFovDegrees) {
    return ProjectionFovMatchesRaw(&projection.matrix[0][0],
        expectedVerticalFovDegrees);
}

bool MatchesManaged(const CameraData& data, const Vector3& expectedPosition,
    const Vector3& expectedForward, float expectedVerticalFovDegrees,
    bool haveExpected) {
    if (!haveExpected)
        return true;

    Vector3 position{};
    Vector3 forward{};
    if (!ParseViewMatrix(&data.view.matrix[0][0], position, forward))
        return false;

    const float error = Distance(position, expectedPosition);
    const float dot = Vector3::Dot(forward, expectedForward);
    if (error > kMaxPositionError || dot < kMinForwardDot)
        return false;

    return ProjectionFovMatches(data.projection, expectedVerticalFovDegrees);
}

bool DiscoverLayout(std::uintptr_t node, const Vector3& expectedPosition,
    const Vector3& expectedForward, float expectedVerticalFovDegrees,
    bool haveExpected, MatrixLayout& layout) {
    // Fast probe of the standard Unity Camera matrix layout (0x70 / 0xB0):
    // only 128 bytes, so it never fails if node + 0x1000 crosses into an
    // unmapped guard page at the end of a heap allocation.
    {
        CameraData quick{};
        if (ReadMatricesAt(node, 0x70, 0xB0, quick) &&
            MatchesManaged(quick, expectedPosition, expectedForward,
                expectedVerticalFovDegrees, haveExpected)) {
            layout.viewOff = 0x70;
            layout.projOff = 0xB0;
            return true;
        }
    }

    // Safe chunked scan up to kScanBytes so an unmapped page past the object
    // does not abort discovery of matrices near the start of the struct.
    std::vector<unsigned char> buffer(kScanBytes, 0);
    std::size_t validBytes = 0;
    constexpr std::size_t kChunk = 0x200;
    for (std::size_t off = 0; off < kScanBytes; off += kChunk) {
        if (!TargetProcess.ReadFresh(node + off, buffer.data() + off, kChunk))
            break;
        validBytes = off + kChunk;
    }
    if (validBytes < 0x100)
        return false;

    const float* floats = reinterpret_cast<const float*>(buffer.data());
    const std::size_t floatCount = validBytes / sizeof(float);
    const std::size_t kMatrixFloats = 16;

    struct Candidate {
        std::size_t offset;
        float score;
    };
    std::vector<Candidate> views;

    for (std::size_t o = 8; o + kMatrixFloats <= floatCount; ++o) {
        Vector3 position{};
        Vector3 forward{};
        if (!ParseViewMatrix(floats + o, position, forward))
            continue;

        float score = 0.f;
        if (haveExpected) {
            const float error = Distance(position, expectedPosition);
            const float dot = Vector3::Dot(forward, expectedForward);
            if (error > kMaxPositionError * 4.f || dot < 0.f)
                continue;
            score = error + (1.f - dot) * 2.f;
        }
        views.push_back({ o, score });
    }

    if (views.empty())
        return false;

    // Prefer a view matrix that is immediately followed by a projection matrix:
    // Unity stores them next to each other, and the pairing rejects unrelated
    // orthonormal matrices elsewhere in the object.
    const std::size_t kPairWindow = 0x100 / sizeof(float);
    bool found = false;
    float bestScore = FLT_MAX;

    for (const Candidate& view : views) {
        std::size_t projOffset = 0;
        for (std::size_t o = view.offset + kMatrixFloats;
            o + kMatrixFloats <= floatCount && o <= view.offset + kPairWindow; ++o) {
            if (ParseProjectionMatrix(floats + o) &&
                ProjectionFovMatchesRaw(floats + o, expectedVerticalFovDegrees)) {
                projOffset = o;
                break;
            }
        }

        float score = view.score;
        if (!projOffset)
            score += 100.f;

        if (score < bestScore) {
            bestScore = score;
            layout.viewOff = view.offset * sizeof(float);
            layout.projOff = projOffset ? projOffset * sizeof(float) : 0;
            found = true;
        }
    }

    if (!found)
        return false;

    if (!layout.projOff) {
        // No adjacent projection: accept any structural projection in the
        // object rather than losing the native camera entirely.
        for (std::size_t o = 8; o + kMatrixFloats <= floatCount; ++o) {
            if (ParseProjectionMatrix(floats + o) &&
                ProjectionFovMatchesRaw(floats + o, expectedVerticalFovDegrees)) {
                layout.projOff = o * sizeof(float);
                break;
            }
        }
    }

    return layout.viewOff != 0 && layout.projOff != 0;
}

bool ExpectedFromManaged(const Vector3& expectedPosition,
    const Vector3& expectedForward) {
    if (expectedPosition.IsZero())
        return false;
    if (!std::isfinite(expectedPosition.x) || !std::isfinite(expectedPosition.y) ||
        !std::isfinite(expectedPosition.z))
        return false;
    if (!std::isfinite(expectedForward.x) || !std::isfinite(expectedForward.y) ||
        !std::isfinite(expectedForward.z))
        return false;
    return expectedForward.LengthSqr() > 0.5f;
}

} // namespace

uintptr_t FindMainCamera() {
    std::vector<uintptr_t> cameras;
    CollectTaggedObjects(cameras, 1);
    return cameras.empty() ? 0 : cameras[0];
}

CameraData GetCameraMatrices() {
    CameraData data{};

    const uintptr_t cameraNode = FindMainCamera();
    if (!cameraNode)
        return data;

    auto view = TargetProcess.Read<Matrix4x4>(cameraNode + 0x70);
    auto projection = TargetProcess.Read<Matrix4x4>(cameraNode + 0xB0);

    data.viewMatrix = view * projection;
    data.view = view;
    data.projection = projection;
    data.valid = true;
    return data;
}

CameraData GetCameraMatricesValidated(const Vector3& expectedPosition,
    const Vector3& expectedForward, float expectedVerticalFovDegrees) {
    CameraData data{};

    const bool haveExpected = ExpectedFromManaged(expectedPosition, expectedForward);
    const std::uint64_t now = GetTickCount64();

    const CameraNodeCache cache = g_cameraCache.load(std::memory_order_acquire);
    const std::uintptr_t cachedNode = cache.node;
    const std::uintptr_t viewOff = cache.viewOff;
    const std::uintptr_t projOff = cache.projOff;

    if (cachedNode && viewOff && projOff) {
        CameraData cached{};
        if (ReadMatricesAt(cachedNode, viewOff, projOff, cached) &&
            MatchesManaged(cached, expectedPosition, expectedForward,
                expectedVerticalFovDegrees, haveExpected)) {
            if (haveExpected) {
                Vector3 position{};
                Vector3 forward{};
                if (ParseViewMatrix(&cached.view.matrix[0][0], position, forward)) {
                    cached.positionError =
                        Distance(position, expectedPosition);
                    cached.forwardDot = Vector3::Dot(forward, expectedForward);
                }
            }
            return cached;
        }
    }

    // Re-resolving walks the tagged-node list, so rate-limit it when the game
    // is on a menu/loading screen or the camera cannot be identified.
    if (now < g_lastResolveTick.load(std::memory_order_relaxed))
        return data;
    g_lastResolveTick.store(now + 500, std::memory_order_relaxed);

    std::vector<uintptr_t> cameras;
    CollectTaggedObjects(cameras, 8);
    for (std::uintptr_t node : cameras) {
        MatrixLayout layout;
        if (!DiscoverLayout(node, expectedPosition, expectedForward,
                expectedVerticalFovDegrees, haveExpected, layout))
            continue;

        CameraData candidate{};
        if (!ReadMatricesAt(node, layout.viewOff, layout.projOff, candidate))
            continue;

        if (haveExpected) {
            Vector3 position{};
            Vector3 forward{};
            if (!ParseViewMatrix(&candidate.view.matrix[0][0], position, forward))
                continue;
            candidate.positionError = Distance(position, expectedPosition);
            candidate.forwardDot = Vector3::Dot(forward, expectedForward);
        }

        g_cameraCache.store(CameraNodeCache{ node, layout.viewOff, layout.projOff },
            std::memory_order_release);
        return candidate;
    }

    g_cameraCache.store(CameraNodeCache{}, std::memory_order_release);
    static std::uint64_t lastMissLogMs = 0;
    if (now - lastMissLogMs >= 10000) {
        lastMissLogMs = now;
        uintptr_t gom = 0;
        const uintptr_t up = unityPlayer.load(std::memory_order_acquire);
        if (up) TargetProcess.ReadFresh(up + kGomSlot, &gom, sizeof(gom));
        const uintptr_t tagged = gom ? GetTaggedNodes(gom, 5) : 0;
        std::printf("[CameraFinder] native miss: gom=0x%llX tagged=0x%llX cams=%zu haveExpected=%d\n",
            (unsigned long long)gom, (unsigned long long)tagged, cameras.size(), (int)haveExpected);
    }
    return data;
}

float ReadNativeFov()
{
    const CameraNodeCache cache = g_cameraCache.load(std::memory_order_acquire);
    std::uintptr_t node = cache.node;
    std::uintptr_t projOff = cache.projOff;

    if (!node) {
        node = g_fovNode.load(std::memory_order_acquire);
        const std::uint64_t now = GetTickCount64();
        if (!node && now >= g_fovNodeResolveTick.load(std::memory_order_relaxed)) {
            g_fovNodeResolveTick.store(now + 2000, std::memory_order_relaxed);
            node = FindMainCamera();
            if (node)
                g_fovNode.store(node, std::memory_order_release);
        }
        if (!node) return 0.f;
        projOff = 0xB0;
    }
    if (!projOff) projOff = 0xB0;

    float yScale = 0.f;
    if (!TargetProcess.ReadFresh(node + projOff + sizeof(float) * 5, &yScale, sizeof(yScale)) ||
        !std::isfinite(yScale) || yScale <= 0.f) {
        if (!cache.node) g_fovNode.store(0, std::memory_order_release);
        return 0.f;
    }

    const float fovRad = 2.f * std::atan(1.f / yScale);
    const float fovDeg = fovRad * 180.f / 3.14159265358979323846f;
    if (fovDeg < 1.f || fovDeg > 179.f) {
        if (!cache.node) g_fovNode.store(0, std::memory_order_release);
        return 0.f;
    }
    return fovDeg;
}

void InvalidateCache()
{
    g_cameraCache.store(CameraNodeCache{}, std::memory_order_release);
    g_lastResolveTick.store(0, std::memory_order_relaxed);
    g_fovNode.store(0, std::memory_order_release);
    g_fovNodeResolveTick.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_physCamMutex);
    g_physCam = PhysCamCache{};
}

CameraData GetCameraMatricesFast()
{
    CameraData data{};
    const CameraNodeCache cache = g_cameraCache.load(std::memory_order_acquire);
    const std::uintptr_t node = cache.node;
    const std::uintptr_t viewOff = cache.viewOff;
    const std::uintptr_t projOff = cache.projOff;
    if (!node || !viewOff || !projOff) return data;

    ReadMatricesAt(node, viewOff, projOff, data);
    return data;
}

} // namespace CameraFinder
