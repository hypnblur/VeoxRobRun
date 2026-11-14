#pragma once
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <windows.h>

namespace CollisionBVH {

struct Float3 {
    float x, y, z;
    Float3 operator-(const Float3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Float3 operator+(const Float3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Float3 operator*(float s) const { return {x*s, y*s, z*s}; }
};

inline float dot(const Float3& a, const Float3& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}
inline Float3 cross(const Float3& a, const Float3& b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}

struct Triangle {
    Float3 v0, v1, v2;
};

// Binary layout per node: aabbMin(3f) aabbMax(3f) a(u32) b(u32) = 32 bytes.
// Leaf:     a = triStart,    b = triCount | 0x80000000
// Interior: a = left child,  b = right child  (high bit clear)
struct BVHNode {
    Float3 aabbMin;
    Float3 aabbMax;
    uint32_t a;
    uint32_t b;

    bool isLeaf() const { return (b & 0x80000000u) != 0; }
    uint32_t triStart() const { return a; }
    uint32_t triCount() const { return b & 0x7FFFFFFFu; }
    uint32_t left() const { return a; }
    uint32_t right() const { return b; }
};

struct Scene {
    std::vector<BVHNode> nodes;
    std::vector<Triangle> tris;
    std::string loadedPath;
    bool valid = false;
};

inline std::string ResolvePath(const std::string& requestedPath)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    // 1. Explicit path check
    if (!requestedPath.empty() && fs::exists(requestedPath, ec))
        return fs::absolute(requestedPath, ec).string();

    std::string filename = fs::path(requestedPath).filename().string();
    if (filename.empty()) filename = "city_colliders.bvh";

    // 2. Relative working directory probes
    const std::vector<std::string> relativeProbes = {
        requestedPath,
        filename,
        "tools/" + filename,
        "../tools/" + filename,
        "../../tools/" + filename,
        "../../../tools/" + filename
    };

    for (const auto& probe : relativeProbes) {
        if (!probe.empty() && fs::exists(probe, ec))
            return fs::absolute(probe, ec).string();
    }

    // 3. Executable-relative directory probes via GetModuleFileNameA
    char exeBuf[MAX_PATH] = { 0 };
    if (GetModuleFileNameA(nullptr, exeBuf, MAX_PATH) > 0) {
        fs::path exeDir = fs::path(exeBuf).parent_path();
        const std::vector<fs::path> exeProbes = {
            exeDir / filename,
            exeDir / "tools" / filename,
            exeDir.parent_path() / "tools" / filename,
            exeDir.parent_path().parent_path() / "tools" / filename,
            exeDir.parent_path().parent_path().parent_path() / "tools" / filename
        };

        for (const auto& probe : exeProbes) {
            if (fs::exists(probe, ec))
                return fs::absolute(probe, ec).string();
        }
    }

    return requestedPath;
}

inline bool Load(const std::string& path, Scene& scene) {
    const std::string resolved = ResolvePath(path);
    std::ifstream f(resolved, std::ios::binary);
    if (!f) return false;

    char magic[4];
    f.read(magic, 4);
    if (magic[0]!='B' || magic[1]!='V' || magic[2]!='H' || magic[3]!='1')
        return false;

    uint32_t numNodes, numTris;
    f.read(reinterpret_cast<char*>(&numNodes), 4);
    f.read(reinterpret_cast<char*>(&numTris), 4);

    scene.nodes.resize(numNodes);
    f.read(reinterpret_cast<char*>(scene.nodes.data()),
           numNodes * sizeof(BVHNode));

    scene.tris.resize(numTris);
    f.read(reinterpret_cast<char*>(scene.tris.data()),
           numTris * sizeof(Triangle));

    scene.valid = f.good() && numNodes > 0;
    if (scene.valid)
        scene.loadedPath = resolved;
    return scene.valid;
}

inline bool RayTriangle(
    const Float3& orig, const Float3& dir,
    const Triangle& tri, float tMax, float& tOut, float tMin = 0.20f)
{
    const Float3 e1 = tri.v1 - tri.v0;
    const Float3 e2 = tri.v2 - tri.v0;
    const Float3 h = cross(dir, e2);
    const float a = dot(e1, h);
    if (a > -1e-7f && a < 1e-7f) return false;

    const float inv = 1.0f / a;
    const Float3 s = orig - tri.v0;
    const float u = inv * dot(s, h);
    if (u < 0.f || u > 1.f) return false;

    const Float3 q = cross(s, e1);
    const float v = inv * dot(dir, q);
    if (v < 0.f || u + v > 1.f) return false;

    const float t = inv * dot(e2, q);
    if (t < tMin || t > tMax) return false;

    tOut = t;
    return true;
}

inline bool RayAABB(
    const Float3& orig, const Float3& invDir,
    const Float3& mn, const Float3& mx, float tMax)
{
    float t1 = (mn.x - orig.x) * invDir.x;
    float t2 = (mx.x - orig.x) * invDir.x;
    float tmin = std::min(t1, t2);
    float tmax = std::max(t1, t2);

    t1 = (mn.y - orig.y) * invDir.y;
    t2 = (mx.y - orig.y) * invDir.y;
    tmin = std::max(tmin, std::min(t1, t2));
    tmax = std::min(tmax, std::max(t1, t2));

    t1 = (mn.z - orig.z) * invDir.z;
    t2 = (mx.z - orig.z) * invDir.z;
    tmin = std::max(tmin, std::min(t1, t2));
    tmax = std::min(tmax, std::max(t1, t2));

    return tmax >= std::max(tmin, 0.f) && tmin < tMax;
}

inline bool IsOccluded(const Scene& scene,
    const Float3& origin, const Float3& target, float tMin = 0.20f)
{
    if (!scene.valid || scene.nodes.empty()) return false;

    const Float3 dir = target - origin;
    const float len = std::sqrt(dot(dir, dir));
    if (len < 1e-6f) return false;

    const float invLen = 1.0f / len;
    const Float3 d = dir * invLen;
    const Float3 invDir = {
        std::abs(d.x) > 1e-8f ? 1.0f / d.x : std::copysign(1e8f, d.x),
        std::abs(d.y) > 1e-8f ? 1.0f / d.y : std::copysign(1e8f, d.y),
        std::abs(d.z) > 1e-8f ? 1.0f / d.z : std::copysign(1e8f, d.z),
    };

    const float tMax = len - 0.05f;
    if (tMax <= tMin) return false;

    uint32_t stack[128];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0) {
        const BVHNode& node = scene.nodes[stack[--sp]];

        if (!RayAABB(origin, invDir, node.aabbMin, node.aabbMax, tMax))
            continue;

        if (node.isLeaf()) {
            const uint32_t start = node.triStart();
            const uint32_t count = node.triCount();
            float t;
            for (uint32_t i = start; i < start + count; ++i) {
                if (RayTriangle(origin, d, scene.tris[i], tMax, t, tMin))
                    return true;
            }
        } else {
            if (sp < 126) {
                stack[sp++] = node.right();
                stack[sp++] = node.left();
            }
        }
    }
    return false;
}

} // namespace CollisionBVH
