#include "pch.h"
#include "CameraFinder.h"
#include "Memory.h"
#include <atomic>
#include <cmath>
#include <cfloat>
#include <vector>

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

namespace {

constexpr uintptr_t kGomSlot = 0x2230768;
constexpr uintptr_t kTagRange = 0x21150C4;
constexpr uintptr_t kTagBase = 0x21150C0;

// Collects the objects attached to the MainCamera tag. Normally there is one,
// but a wrong/preview camera must not be silently used, so callers can inspect
// every candidate.
void CollectTaggedObjects(std::vector<uintptr_t>& out, std::size_t max) {
    out.clear();
    if (!unityPlayer)
        return;

    const uintptr_t gom = TargetProcess.Read<uintptr_t>(unityPlayer + kGomSlot);
    if (!gom)
        return;

    const uintptr_t taggedNodes = GetTaggedNodes(gom, 5);
    if (!taggedNodes)
        return;

    const uint32_t range = TargetProcess.Read<uint32_t>(unityPlayer + kTagRange);
    const uint32_t base = TargetProcess.Read<uint32_t>(unityPlayer + kTagBase);

    uintptr_t node = TargetProcess.Read<uintptr_t>(taggedNodes + 0x08);
    for (int guard = 0; node && node != taggedNodes && guard < 65536; ++guard) {
        const uintptr_t entry = TargetProcess.Read<uintptr_t>(node + 0x10);
        if (entry) {
            const uintptr_t begin = TargetProcess.Read<uintptr_t>(entry + 0x20);
            const uintptr_t count = TargetProcess.Read<uintptr_t>(entry + 0x30);
            for (uintptr_t i = 0; begin && i < count && i < 4096; ++i) {
                const uintptr_t element = begin + i * 0x10;
                const uint32_t identifier = TargetProcess.Read<uint32_t>(element);
                if (uint32_t(identifier - base) < range) {
                    const uintptr_t object = TargetProcess.Read<uintptr_t>(element + 0x08);
                    if (object) {
                        out.push_back(object);
                        if (out.size() >= max)
                            return;
                    }
                    break;
                }
            }
        }
        node = TargetProcess.Read<uintptr_t>(node + 0x08);
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
constexpr float kMaxPositionError = 5.f;   // metres
constexpr float kMinForwardDot = 0.5f;     // 60 degrees

// Cached render camera and the structurally discovered matrix offsets. Offsets
// are stable for a Unity build, so they survive scene changes; the node does
// not, and is re-resolved when it stops matching the managed camera.
std::atomic<std::uintptr_t> g_cameraNode{ 0 };
std::atomic<std::uintptr_t> g_viewOff{ 0 };
std::atomic<std::uintptr_t> g_projOff{ 0 };
std::atomic<std::uint64_t> g_lastResolveTick{ 0 };

bool ReadMatricesAt(std::uintptr_t node, std::uintptr_t viewOff,
    std::uintptr_t projOff, CameraData& data) {
    if (!node || !viewOff || !projOff)
        return false;

    Matrix4x4 view{};
    Matrix4x4 projection{};
    if (!TargetProcess.Read(node + viewOff, &view, sizeof(view)))
        return false;
    if (!TargetProcess.Read(node + projOff, &projection, sizeof(projection)))
        return false;

    Vector3 position{};
    Vector3 forward{};
    if (!ParseViewMatrix(&view.matrix[0][0], position, forward))
        return false;
    if (!ParseProjectionMatrix(&projection.matrix[0][0]))
        return false;

    data.view = view;
    data.projection = projection;
    data.viewMatrix = view * projection;
    data.valid = true;
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

    const float error = Vector3::Distance(position, expectedPosition);
    const float dot = Vector3::Dot(forward, expectedForward);
    if (error > kMaxPositionError || dot < kMinForwardDot)
        return false;

    return ProjectionFovMatches(data.projection, expectedVerticalFovDegrees);
}

bool DiscoverLayout(std::uintptr_t node, const Vector3& expectedPosition,
    const Vector3& expectedForward, float expectedVerticalFovDegrees,
    bool haveExpected, MatrixLayout& layout) {
    std::vector<unsigned char> buffer(kScanBytes);
    if (!TargetProcess.Read(node, buffer.data(), buffer.size()))
        return false;

    const float* floats = reinterpret_cast<const float*>(buffer.data());
    const std::size_t floatCount = buffer.size() / sizeof(float);
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
            const float error = Vector3::Distance(position, expectedPosition);
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

    const std::uintptr_t cachedNode = g_cameraNode.load(std::memory_order_acquire);
    const std::uintptr_t viewOff = g_viewOff.load(std::memory_order_acquire);
    const std::uintptr_t projOff = g_projOff.load(std::memory_order_acquire);

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
                        Vector3::Distance(position, expectedPosition);
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
            candidate.positionError = Vector3::Distance(position, expectedPosition);
            candidate.forwardDot = Vector3::Dot(forward, expectedForward);
        }

        g_cameraNode.store(node, std::memory_order_release);
        g_viewOff.store(layout.viewOff, std::memory_order_release);
        g_projOff.store(layout.projOff, std::memory_order_release);
        return candidate;
    }

    g_cameraNode.store(0, std::memory_order_release);
    g_viewOff.store(0, std::memory_order_release);
    g_projOff.store(0, std::memory_order_release);
    return data;
}

float ReadNativeFov()
{
    std::uintptr_t node = g_cameraNode.load(std::memory_order_acquire);
    std::uintptr_t projOff = g_projOff.load(std::memory_order_acquire);

    if (!node) {
        node = FindMainCamera();
        if (!node) return 0.f;
        projOff = 0xB0;
    }
    if (!projOff) projOff = 0xB0;

    const float yScale = TargetProcess.Read<float>(
        node + projOff + sizeof(float) * 5);
    if (!std::isfinite(yScale) || yScale <= 0.f)
        return 0.f;

    const float fovRad = 2.f * std::atan(1.f / yScale);
    const float fovDeg = fovRad * 180.f / 3.14159265358979323846f;
    if (fovDeg < 1.f || fovDeg > 179.f) return 0.f;
    return fovDeg;
}

} // namespace CameraFinder
