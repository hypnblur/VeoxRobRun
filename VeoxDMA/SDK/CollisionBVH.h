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
    int entityId = 0;
    Float3 center;
    Float3 halfExtents;
    float yawRad;
    bool isHelicopter;
    // Precomputed by InitCache() when dynamic occluders refresh (~2 Hz) so
    // per-ray occlusion checks avoid redundant sin/cos and reject distant OBBs
    // via a cheap segment-sphere test.
    float cosNegYaw = 1.0f;
    float sinNegYaw = 0.0f;
    float boundRadiusSq = 0.0f;
    bool cacheValid = false;

    void InitCache() {
        cosNegYaw = std::cos(-yawRad);
        sinNegYaw = std::sin(-yawRad);
        boundRadiusSq = halfExtents.x * halfExtents.x +
                        halfExtents.y * halfExtents.y +
                        halfExtents.z * halfExtents.z;
        cacheValid = true;
    }
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

inline bool RayAABBNear(
    const Float3& orig, const Float3& invDir,
    const Float3& mn, const Float3& mx, float tMax, float& tNearOut)
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

    if (tmax >= std::max(tmin, 0.f) && tmin < tMax) {
        tNearOut = tmin;
        return true;
    }
    return false;
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

// Ray-OBB intersection given pre-transformed local origin and precomputed yaw trig
inline bool RayVehicleOBBLocal(
    const Float3& localOrig, const Float3& dir,
    const VehicleOBB& v, float cosY, float sinY,
    float tMax, bool carCabinSolid, float tMin = 0.20f)
{
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

// Ray-OBB intersection for vehicles with optional car cabin solid mode
inline bool RayVehicleOBB(
    const Float3& orig, const Float3& dir,
    const VehicleOBB& v, float tMax, bool carCabinSolid, float tMin = 0.20f)
{
    const float cosY = v.cacheValid ? v.cosNegYaw : std::cos(-v.yawRad);
    const float sinY = v.cacheValid ? v.sinNegYaw : std::sin(-v.yawRad);

    const Float3 rel = orig - v.center;
    const Float3 localOrig = {
        rel.x * cosY - rel.z * sinY,
        rel.y,
        rel.x * sinY + rel.z * cosY
    };
    return RayVehicleOBBLocal(localOrig, dir, v, cosY, sinY, tMax, carCabinSolid, tMin);
}

inline bool IsOccluded(
    const Scene& scene,
    const Float3& origin, const Float3& target,
    const DynamicOccluders* dyn = nullptr,
    bool carCabinSolid = false,
    float tMin = 0.20f,
    int skipVehicleId1 = 0,
    int skipVehicleId2 = 0,
    bool skipAllVehicles = false)
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
        if (!skipAllVehicles) {
            for (const auto& v : dyn->vehicles) {
                if ((skipVehicleId1 > 0 && v.entityId == skipVehicleId1) ||
                    (skipVehicleId2 > 0 && v.entityId == skipVehicleId2))
                    continue;

                // Fast segment-to-bounding-sphere reject before rotating into OBB space
                const Float3 rel = origin - v.center;
                const float rSq = v.cacheValid
                    ? v.boundRadiusSq
                    : (v.halfExtents.x * v.halfExtents.x +
                       v.halfExtents.y * v.halfExtents.y +
                       v.halfExtents.z * v.halfExtents.z);
                const float tClosest = std::clamp(-dot(rel, d), 0.0f, tMax);
                const Float3 closest = rel + d * tClosest;
                if (dot(closest, closest) > rSq)
                    continue;

                const float cosY = v.cacheValid ? v.cosNegYaw : std::cos(-v.yawRad);
                const float sinY = v.cacheValid ? v.sinNegYaw : std::sin(-v.yawRad);
                const Float3 lo = { rel.x * cosY - rel.z * sinY, rel.y, rel.x * sinY + rel.z * cosY };
                // Skip vehicles the ray originates inside (self-occlusion)
                if (std::abs(lo.x) <= v.halfExtents.x &&
                    std::abs(lo.y) <= v.halfExtents.y &&
                    std::abs(lo.z) <= v.halfExtents.z)
                    continue;
                // Skip vehicles the target bone is sitting inside (occupant visibility)
                const Float3 relT = target - v.center;
                const Float3 lt = { relT.x * cosY - relT.z * sinY, relT.y, relT.x * sinY + relT.z * cosY };
                if (std::abs(lt.x) <= v.halfExtents.x + 0.15f &&
                    std::abs(lt.y) <= v.halfExtents.y + 0.15f &&
                    std::abs(lt.z) <= v.halfExtents.z + 0.15f)
                    continue;
                if (RayVehicleOBBLocal(lo, d, v, cosY, sinY, tMax, carCabinSolid, tMin))
                    return true;
            }
        }
    }

    // 2. Static BVH Scene Pass (near-child-first ordered traversal)
    if (!scene.valid || scene.nodes.empty()) return false;

    const Float3 invDir = {
        std::abs(d.x) > 1e-8f ? 1.0f / d.x : std::copysign(1e8f, d.x),
        std::abs(d.y) > 1e-8f ? 1.0f / d.y : std::copysign(1e8f, d.y),
        std::abs(d.z) > 1e-8f ? 1.0f / d.z : std::copysign(1e8f, d.z),
    };

    float rootT = 0.f;
    if (!RayAABBNear(origin, invDir, scene.nodes[0].aabbMin, scene.nodes[0].aabbMax, tMax, rootT))
        return false;

    uint32_t stack[128];
    int sp = 0;
    stack[sp++] = 0;

    const bool hasSurfaces = !scene.surfaceTypes.empty();
    const std::size_t surfSize = scene.surfaceTypes.size();

    while (sp > 0) {
        const BVHNode& node = scene.nodes[stack[--sp]];

        if (node.isLeaf()) {
            const uint32_t start = node.triStart();
            const uint32_t end = start + node.triCount();
            float t;
            for (uint32_t i = start; i < end; ++i) {
                if (RayTriangle(origin, d, scene.tris[i], tMax, t, tMin)) {
                    if (hasSurfaces && i < surfSize) {
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
            const uint32_t leftIdx = node.left();
            const uint32_t rightIdx = node.right();
            float tLeft = 0.f, tRight = 0.f;
            const bool hitLeft = RayAABBNear(origin, invDir,
                scene.nodes[leftIdx].aabbMin, scene.nodes[leftIdx].aabbMax, tMax, tLeft);
            const bool hitRight = RayAABBNear(origin, invDir,
                scene.nodes[rightIdx].aabbMin, scene.nodes[rightIdx].aabbMax, tMax, tRight);

            if (hitLeft && hitRight) {
                if (sp < 126) {
                    // Push farther child first so the nearer child pops next
                    if (tLeft <= tRight) {
                        stack[sp++] = rightIdx;
                        stack[sp++] = leftIdx;
                    } else {
                        stack[sp++] = leftIdx;
                        stack[sp++] = rightIdx;
                    }
                }
            } else if (hitLeft) {
                if (sp < 127) stack[sp++] = leftIdx;
            } else if (hitRight) {
                if (sp < 127) stack[sp++] = rightIdx;
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
