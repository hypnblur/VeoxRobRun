#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <windows.h>
#include "Matrix4x4.h"
#include "DebugLog.h"

namespace CharacterMesh {

struct Vertex {
    float x, y, z;
    std::uint8_t bones[4];
    float weights[4];
};

struct Mesh {
    std::uint32_t numBones = 0;
    std::vector<std::uint32_t> boneHashes;
    std::vector<Matrix4x4> bindPoses;
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
    bool valid = false;
};

inline std::string ResolvePath(const std::string& requested)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    if (!requested.empty() && fs::exists(requested, ec))
        return fs::absolute(requested, ec).string();

    std::string filename = fs::path(requested).filename().string();
    if (filename.empty()) filename = "character_mesh.bin";

    const std::vector<std::string> probes = {
        requested, filename,
        "tools/" + filename, "../tools/" + filename,
        "../../tools/" + filename, "../../../tools/" + filename
    };
    for (const auto& p : probes)
        if (!p.empty() && fs::exists(p, ec))
            return fs::absolute(p, ec).string();

    char buf[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH) > 0) {
        fs::path dir = fs::path(buf).parent_path();
        for (auto d = dir; d.has_parent_path() && d != d.parent_path(); d = d.parent_path()) {
            auto p = d / filename;
            if (fs::exists(p, ec)) return fs::absolute(p, ec).string();
            p = d / "tools" / filename;
            if (fs::exists(p, ec)) return fs::absolute(p, ec).string();
        }
    }
    return requested;
}

inline bool Load(const std::string& path, Mesh& mesh)
{
    DLOG("CharacterMesh::Load('%s')", path.c_str());
    std::string resolved;
    try {
        resolved = ResolvePath(path);
    } catch (const std::exception& e) {
        DLOG("  FAIL: ResolvePath threw: %s", e.what());
        return false;
    } catch (...) {
        DLOG("  FAIL: ResolvePath threw unknown exception");
        return false;
    }
    DLOG("  resolved='%s'", resolved.c_str());
    std::ifstream f(resolved, std::ios::binary);
    if (!f) { DLOG("  FAIL: could not open file"); return false; }

    char magic[4];
    f.read(magic, 4);
    if (magic[0] != 'C' || magic[1] != 'H' || magic[2] != 'R' || magic[3] != '1') {
        DLOG("  FAIL: bad magic %02x%02x%02x%02x", (unsigned char)magic[0], (unsigned char)magic[1], (unsigned char)magic[2], (unsigned char)magic[3]);
        return false;
    }

    std::uint32_t numBones, numVerts, numTris;
    f.read(reinterpret_cast<char*>(&numBones), 4);
    f.read(reinterpret_cast<char*>(&numVerts), 4);
    f.read(reinterpret_cast<char*>(&numTris), 4);
    DLOG("  header: bones=%u verts=%u tris=%u", numBones, numVerts, numTris);

    if (numBones > 256 || numVerts > 100000 || numTris > 100000) {
        DLOG("  FAIL: counts out of range");
        return false;
    }

    mesh.numBones = numBones;

    mesh.boneHashes.resize(numBones);
    f.read(reinterpret_cast<char*>(mesh.boneHashes.data()), numBones * 4);

    mesh.bindPoses.resize(numBones);
    f.read(reinterpret_cast<char*>(mesh.bindPoses.data()),
           numBones * sizeof(Matrix4x4));

    mesh.vertices.resize(numVerts);
    f.read(reinterpret_cast<char*>(mesh.vertices.data()),
           numVerts * sizeof(Vertex));

    mesh.indices.resize(numTris * 3);
    f.read(reinterpret_cast<char*>(mesh.indices.data()), numTris * 3 * 2);

    if (!f.good()) {
        DLOG("  FAIL: stream error (truncated file)");
        mesh.valid = false;
        return false;
    }

    // character_mesh.bin is a generated asset, so its indices are untrusted
    // input. Skin() and Draw() use them to index fixed-size arrays; a bone
    // index past numBones or a triangle index past numVerts reads out of
    // bounds on the render thread. Reject the file once, here.
    for (const Vertex& v : mesh.vertices) {
        for (int j = 0; j < 4; ++j) {
            // Skin() skips zero-weight slots, so only the bones it will
            // actually dereference have to be in range.
            if (v.weights[j] <= 1e-6f) continue;
            if (v.bones[j] >= numBones) {
                DLOG("  FAIL: bone index %u out of range (%u bones)",
                     (unsigned)v.bones[j], numBones);
                mesh.valid = false;
                return false;
            }
        }
    }
    for (std::uint16_t idx : mesh.indices) {
        if (idx >= numVerts) {
            DLOG("  FAIL: triangle index %u out of range (%u verts)",
                 (unsigned)idx, numVerts);
            mesh.valid = false;
            return false;
        }
    }

    mesh.valid = true;
    DLOG("  Load OK");
    return true;
}

// Skin all vertices given per-bone world matrices.
// boneWorld:  array of world-space 4x4 matrices, one per skeleton bone
//             (column-major: matrix[col][row], matching Matrix4x4).
// boneMap:    meshBoneIndex → skeletonBoneIndex (size = mesh.numBones).
//             -1 means unmapped (identity fallback).
// outPos:     output world positions (size = mesh.vertices.size()).
inline void Skin(const Mesh& mesh,
                 const Matrix4x4* boneWorld, int skeletonBoneCount,
                 const int* boneMap,
                 Vector3* outPos)
{
    // Called once per player per frame; reuse scratch instead of allocating
    // two vectors for every entity. numBones is validated at load time.
    thread_local std::vector<Matrix4x4> s_skin;
    thread_local std::vector<uint8_t> s_mapped;
    if (s_skin.size() < mesh.numBones) {
        s_skin.resize(mesh.numBones);
        s_mapped.resize(mesh.numBones);
    }
    Matrix4x4* skin = s_skin.data();
    std::uint8_t* mapped = s_mapped.data();
    std::memset(mapped, 0, mesh.numBones);
    for (std::uint32_t b = 0; b < mesh.numBones; ++b) {
        int skel = boneMap ? boneMap[b] : -1;
        if (skel < 0 || skel >= skeletonBoneCount) {
            std::memset(&skin[b], 0, sizeof(Matrix4x4));
            continue;
        }
        skin[b] = mesh.bindPoses[b] * boneWorld[skel];
        mapped[b] = 1;
    }
    // reject mis-mapped bones whose skin matrix drifts far from the pack
    {
        float rx = 0, ry = 0, rz = 0;
        bool hasRef = false;
        for (std::uint32_t b = 0; b < mesh.numBones; ++b) {
            if (!mapped[b]) continue;
            rx = skin[b].matrix[3][0];
            ry = skin[b].matrix[3][1];
            rz = skin[b].matrix[3][2];
            hasRef = true;
            break;
        }
        if (hasRef) {
            for (std::uint32_t b = 0; b < mesh.numBones; ++b) {
                if (!mapped[b]) continue;
                const float dx = skin[b].matrix[3][0] - rx;
                const float dy = skin[b].matrix[3][1] - ry;
                const float dz = skin[b].matrix[3][2] - rz;
                if (dx*dx + dy*dy + dz*dz > 9.0f) {
                    std::memset(&skin[b], 0, sizeof(Matrix4x4));
                    mapped[b] = 0;
                }
            }
        }
    }

    constexpr float NAN_VAL = std::numeric_limits<float>::quiet_NaN();
    for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
        const Vertex& vt = mesh.vertices[v];
        float mw = 0;
        for (int j = 0; j < 4; ++j)
            if (vt.weights[j] > 1e-6f && mapped[vt.bones[j]])
                mw += vt.weights[j];
        if (mw < 0.01f) { outPos[v] = {NAN_VAL, 0, 0}; continue; }

        float wx = 0, wy = 0, wz = 0;
        for (int j = 0; j < 4; ++j) {
            const float w = vt.weights[j];
            if (w < 1e-6f || !mapped[vt.bones[j]]) continue;
            const auto& M = skin[vt.bones[j]].matrix;
            wx += w * (M[0][0] * vt.x + M[1][0] * vt.y + M[2][0] * vt.z + M[3][0]);
            wy += w * (M[0][1] * vt.x + M[1][1] * vt.y + M[2][1] * vt.z + M[3][1]);
            wz += w * (M[0][2] * vt.x + M[1][2] * vt.y + M[2][2] * vt.z + M[3][2]);
        }
        const float inv = 1.0f / mw;
        outPos[v] = { wx * inv, wy * inv, wz * inv };
    }
    // cull spike vertices: rotational mismatches fling verts far from root
    const float rootX = boneWorld[0].matrix[3][0];
    const float rootY = boneWorld[0].matrix[3][1];
    const float rootZ = boneWorld[0].matrix[3][2];
    if (rootX == 0.f && rootY == 0.f && rootZ == 0.f) return;
    for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
        if (std::isnan(outPos[v].x)) continue;
        const float dx = outPos[v].x - rootX;
        const float dy = outPos[v].y - rootY;
        const float dz = outPos[v].z - rootZ;
        if (dx*dx + dy*dy + dz*dz > 9.0f)
            outPos[v].x = std::numeric_limits<float>::quiet_NaN();
    }
}

// Project skinned vertices to screen space and draw filled triangles.
// Returns true if anything was drawn.
inline bool Draw(ImDrawList* dl,
                 const Mesh& mesh,
                 const Vector3* worldPos,
                 const Matrix4x4& viewProj,
                 int screenW, int screenH,
                 ImU32 fillCol)
{
    const int nv = (int)mesh.vertices.size();
    const int ni = (int)mesh.indices.size();
    if (nv == 0 || ni == 0) return false;

    // Reusable scratch buffers to eliminate per-entity heap allocations during rendering
    thread_local std::vector<ImVec2> s_screen;
    thread_local std::vector<float> s_clipW;
    thread_local std::vector<uint8_t> s_visible;

    if ((int)s_screen.size() < nv) {
        s_screen.resize(nv);
        s_clipW.resize(nv);
        s_visible.resize(nv);
    }
    std::fill_n(s_visible.data(), nv, static_cast<uint8_t>(0));

    for (int i = 0; i < nv; ++i) {
        const Vector3& p = worldPos[i];
        if (std::isnan(p.x)) continue;
        const auto& M = viewProj.matrix;
        const float cw = M[0][3] * p.x + M[1][3] * p.y + M[2][3] * p.z + M[3][3];
        if (cw < 0.1f) continue;

        const float cx = M[0][0] * p.x + M[1][0] * p.y + M[2][0] * p.z + M[3][0];
        const float cy = M[0][1] * p.x + M[1][1] * p.y + M[2][1] * p.z + M[3][1];

        const float invW = 1.0f / cw;
        const float ndcX = cx * invW;
        const float ndcY = cy * invW;

        s_screen[i].x = (1.0f + ndcX) * 0.5f * (float)screenW;
        s_screen[i].y = (1.0f - ndcY) * 0.5f * (float)screenH;
        s_clipW[i] = cw;
        s_visible[i] = 1;
    }

    bool drew = false;
    for (int i = 0; i < ni; i += 3) {
        const int a = mesh.indices[i];
        const int b = mesh.indices[i + 1];
        const int c = mesh.indices[i + 2];

        if (!s_visible[a] || !s_visible[b] || !s_visible[c])
            continue;

        // Cull triangles entirely off-screen
        const float minX = std::min({s_screen[a].x, s_screen[b].x, s_screen[c].x});
        const float maxX = std::max({s_screen[a].x, s_screen[b].x, s_screen[c].x});
        const float minY = std::min({s_screen[a].y, s_screen[b].y, s_screen[c].y});
        const float maxY = std::max({s_screen[a].y, s_screen[b].y, s_screen[c].y});
        if (maxX < 0 || minX > screenW || maxY < 0 || minY > screenH)
            continue;
        if (maxX - minX > screenW * 1.5f || maxY - minY > screenH * 1.5f)
            continue;

        dl->AddTriangleFilled(s_screen[a], s_screen[b], s_screen[c], fillCol);
        drew = true;
    }
    return drew;
}

} // namespace CharacterMesh
