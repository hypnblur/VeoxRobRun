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

enum SurfaceType : uint16_t {
    Surface_DefaultOpaque  = 0,
    Surface_HouseGlass     = 1,
    Surface_VehicleGlass   = 2,
    Surface_ThinWood       = 3
};

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
    std::vector<uint16_t> surfaceTypes; // Optional surface tags (BVH2)
    std::string loadedPath;
    bool valid = false;
};

// Dynamic volume definitions for secondary occlusion passes
struct VehicleOBB {
    Float3 center;
    Float3 halfExtents;
    float yawRad;
    bool isHelicopter;
};

struct SmokeSphere {
    Float3 center;
    float radius;
};

struct DynamicOccluders {
    std::vector<VehicleOBB> vehicles;
    std::vector<SmokeSphere> smokes;
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
    const bool isBvh1 = (magic[0]=='B' && magic[1]=='V' && magic[2]=='H' && magic[3]=='1');
    const bool isBvh2 = (magic[0]=='B' && magic[1]=='V' && magic[2]=='H' && magic[3]=='2');
    if (!isBvh1 && !isBvh2)
        return false;

    uint32_t numNodes = 0, numTris = 0;
    f.read(reinterpret_cast<char*>(&numNodes), 4);
    f.read(reinterpret_cast<char*>(&numTris), 4);
    if (!f.good() || numNodes == 0 || numNodes > 16000000u || numTris > 32000000u)
        return false;

    scene.nodes.resize(numNodes);
    f.read(reinterpret_cast<char*>(scene.nodes.data()),
           static_cast<std::streamsize>(static_cast<std::size_t>(numNodes) * sizeof(BVHNode)));

    scene.tris.resize(numTris);
    f.read(reinterpret_cast<char*>(scene.tris.data()),
           static_cast<std::streamsize>(static_cast<std::size_t>(numTris) * sizeof(Triangle)));

    if (isBvh2) {
        scene.surfaceTypes.resize(numTris);
        f.read(reinterpret_cast<char*>(scene.surfaceTypes.data()),
               static_cast<std::streamsize>(static_cast<std::size_t>(numTris) * sizeof(uint16_t)));
    } else {
        scene.surfaceTypes.clear();
    }

    if (!f.good()) {
        scene.valid = false;
        return false;
    }

    // Validate all BVH node/triangle indices once at load time so traversal
    // cannot read out of bounds on a corrupt file.
    for (const BVHNode& node : scene.nodes) {
        if (node.isLeaf()) {
            const uint64_t end = static_cast<uint64_t>(node.triStart()) + node.triCount();
            if (end > numTris) {
                scene.valid = false;
                return false;
            }
        } else {
            if (node.left() >= numNodes || node.right() >= numNodes) {
                scene.valid = false;
                return false;
            }
        }
    }

    scene.valid = true;
    scene.loadedPath = resolved;
    return true;
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
    const Float3& mn, const Float3& mx, float tMax, float tMin = 0.f)
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

    return tmax >= std::max(tmin, tMin) && tmin < tMax;
}

// Ray-Sphere intersection for spherical smoke volume decay checks
inline bool RaySphere(
    const Float3& orig, const Float3& dir,
    const Float3& center, float radius, float tMax)
{
    const Float3 oc = orig - center;
    const float b = dot(oc, dir);
    const float c = dot(oc, oc) - radius * radius;
    if (c <= 0.0f) return true; // Origin inside smoke volume
    if (b > 0.0f) return false;
    const float disc = b * b - c;
    if (disc < 0.0f) return false;
    const float t = -b - std::sqrt(disc);
    return t >= 0.0f && t <= tMax;
}

// Ray-OBB intersection for vehicles with optional car cabin solid mode
inline bool RayVehicleOBB(
    const Float3& orig, const Float3& dir,
    const VehicleOBB& v, float tMax, bool carCabinSolid, float tMin = 0.20f)
{
    const float cosY = std::cos(-v.yawRad);
    const float sinY = std::sin(-v.yawRad);

    const Float3 rel = orig - v.center;
    const Float3 localOrig = {
        rel.x * cosY - rel.z * sinY,
        rel.y,
        rel.x * sinY + rel.z * cosY
    };
    const Float3 localDir = {
        dir.x * cosY - dir.z * sinY,
        dir.y,
        dir.x * sinY + dir.z * cosY
    };

    const Float3 invDir = {
        std::abs(localDir.x) > 1e-8f ? 1.0f / localDir.x : std::copysign(1e8f, localDir.x),
        std::abs(localDir.y) > 1e-8f ? 1.0f / localDir.y : std::copysign(1e8f, localDir.y),
        std::abs(localDir.z) > 1e-8f ? 1.0f / localDir.z : std::copysign(1e8f, localDir.z)
    };

    // If cabin is solid or target is a helicopter, test the full outer bounding box
    if (carCabinSolid || v.isHelicopter) {
        const Float3 mn = { -v.halfExtents.x, -v.halfExtents.y, -v.halfExtents.z };
        const Float3 mx = {  v.halfExtents.x,  v.halfExtents.y,  v.halfExtents.z };
        return RayAABB(localOrig, invDir, mn, mx, tMax, tMin);
    }

    // Car Cabin Not Solid: split vehicle into solid chassis/engine block and roof plate,
    // allowing rays to pass through the window plane (y between ~0.35 and 0.85 above center)
    const float hx = v.halfExtents.x;
    const float hy = v.halfExtents.y;
    const float hz = v.halfExtents.z;

    // 1. Lower chassis / engine block (solid base)
    const Float3 baseMn = { -hx, -hy, -hz };
    const Float3 baseMx = {  hx, -hy * 0.15f, hz };
    if (RayAABB(localOrig, invDir, baseMn, baseMx, tMax, tMin))
        return true;

    // 2. Roof plate (top surface)
    const Float3 roofMn = { -hx * 0.85f, hy * 0.75f, -hz * 0.55f };
    const Float3 roofMx = {  hx * 0.85f, hy,          hz * 0.45f };
    if (RayAABB(localOrig, invDir, roofMn, roofMx, tMax, tMin))
        return true;

    return false;
}

inline bool IsOccluded(
    const Scene& scene,
    const Float3& origin, const Float3& target,
    const DynamicOccluders* dyn = nullptr,
    bool carCabinSolid = false,
    float tMin = 0.20f)
{
    const Float3 dir = target - origin;
    const float len = std::sqrt(dot(dir, dir));
    if (len < 1e-6f) return false;

    const float invLen = 1.0f / len;
    const Float3 d = dir * invLen;
    const float tMax = len - 0.05f;
    if (tMax <= tMin) return false;

    // 1. Dynamic Occlusion Pass (Smokes & Vehicles)
    if (dyn) {
        for (const auto& smk : dyn->smokes) {
            if (RaySphere(origin, d, smk.center, smk.radius, tMax))
                return true;
        }
        for (const auto& v : dyn->vehicles) {
            // Skip vehicles the ray originates inside (self-occlusion)
            const Float3 rel = origin - v.center;
            const float cosY = std::cos(-v.yawRad);
            const float sinY = std::sin(-v.yawRad);
            const Float3 lo = { rel.x*cosY - rel.z*sinY, rel.y, rel.x*sinY + rel.z*cosY };
            if (std::abs(lo.x) <= v.halfExtents.x &&
                std::abs(lo.y) <= v.halfExtents.y &&
                std::abs(lo.z) <= v.halfExtents.z)
                continue;
            if (RayVehicleOBB(origin, d, v, tMax, carCabinSolid, tMin))
                return true;
        }
    }

    // 2. Static BVH Scene Pass
    if (!scene.valid || scene.nodes.empty()) return false;

    const Float3 invDir = {
        std::abs(d.x) > 1e-8f ? 1.0f / d.x : std::copysign(1e8f, d.x),
        std::abs(d.y) > 1e-8f ? 1.0f / d.y : std::copysign(1e8f, d.y),
        std::abs(d.z) > 1e-8f ? 1.0f / d.z : std::copysign(1e8f, d.z),
    };

    uint32_t stack[128];
    int sp = 0;
    stack[sp++] = 0;

    const bool hasSurfaces = !scene.surfaceTypes.empty();

    while (sp > 0) {
        const BVHNode& node = scene.nodes[stack[--sp]];

        if (!RayAABB(origin, invDir, node.aabbMin, node.aabbMax, tMax))
            continue;

        if (node.isLeaf()) {
            const uint32_t start = node.triStart();
            const uint32_t count = node.triCount();
            float t;
            for (uint32_t i = start; i < start + count; ++i) {
                if (RayTriangle(origin, d, scene.tris[i], tMax, t, tMin)) {
                    if (hasSurfaces && i < scene.surfaceTypes.size()) {
                        const uint16_t surf = scene.surfaceTypes[i];
                        if (surf == Surface_VehicleGlass && !carCabinSolid) {
                            // Vehicle glass see-through allowed when car cabin solid mode is disabled
                            continue;
                        }
                    }
                    return true;
                }
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

// Overload for backward compatibility
inline bool IsOccluded(
    const Scene& scene,
    const Float3& origin, const Float3& target,
    float tMin)
{
    return IsOccluded(scene, origin, target, nullptr, false, tMin);
}

} // namespace CollisionBVH
