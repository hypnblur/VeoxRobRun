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
    const std::string resolved = ResolvePath(path);
    std::ifstream f(resolved, std::ios::binary);
    if (!f) return false;

    char magic[4];
    f.read(magic, 4);
    if (magic[0] != 'C' || magic[1] != 'H' || magic[2] != 'R' || magic[3] != '1')
        return false;

    std::uint32_t numBones, numVerts, numTris;
    f.read(reinterpret_cast<char*>(&numBones), 4);
    f.read(reinterpret_cast<char*>(&numVerts), 4);
    f.read(reinterpret_cast<char*>(&numTris), 4);

    if (numBones > 256 || numVerts > 100000 || numTris > 100000)
        return false;

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

    mesh.valid = f.good();
    return mesh.valid;
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
    // Precompute skin matrices: skinMat[i] = boneWorld[map[i]] * invBindPose[i]
    std::vector<Matrix4x4> skin(mesh.numBones);
    for (std::uint32_t b = 0; b < mesh.numBones; ++b) {
        const int skel = boneMap[b];
        if (skel < 0 || skel >= skeletonBoneCount) {
            std::memset(&skin[b], 0, sizeof(Matrix4x4));
            skin[b].matrix[0][0] = skin[b].matrix[1][1] =
                skin[b].matrix[2][2] = skin[b].matrix[3][3] = 1.0f;
            continue;
        }
        skin[b] = boneWorld[skel] * mesh.bindPoses[b];
    }

    // Transform each vertex
    for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
        const Vertex& vt = mesh.vertices[v];
        float wx = 0, wy = 0, wz = 0;

        for (int j = 0; j < 4; ++j) {
            const float w = vt.weights[j];
            if (w < 1e-6f) continue;

            const auto& M = skin[vt.bones[j]].matrix;
            // column-major: M[col][row]
            // pos = M * [x,y,z,1]
            wx += w * (M[0][0] * vt.x + M[1][0] * vt.y + M[2][0] * vt.z + M[3][0]);
            wy += w * (M[0][1] * vt.x + M[1][1] * vt.y + M[2][1] * vt.z + M[3][1]);
            wz += w * (M[0][2] * vt.x + M[1][2] * vt.y + M[2][2] * vt.z + M[3][2]);
        }
        outPos[v] = { wx, wy, wz };
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

    // Project all vertices to screen
    std::vector<ImVec2> screen(nv);
    std::vector<float> clipW(nv);
    std::vector<bool> visible(nv, false);

    for (int i = 0; i < nv; ++i) {
        const Vector3& p = worldPos[i];
        const auto& M = viewProj.matrix;
        const float cw = M[0][3] * p.x + M[1][3] * p.y + M[2][3] * p.z + M[3][3];
        if (cw < 0.1f) continue;

        const float cx = M[0][0] * p.x + M[1][0] * p.y + M[2][0] * p.z + M[3][0];
        const float cy = M[0][1] * p.x + M[1][1] * p.y + M[2][1] * p.z + M[3][1];

        const float invW = 1.0f / cw;
        const float ndcX = cx * invW;
        const float ndcY = cy * invW;

        screen[i].x = (1.0f + ndcX) * 0.5f * (float)screenW;
        screen[i].y = (1.0f - ndcY) * 0.5f * (float)screenH;
        clipW[i] = cw;
        visible[i] = true;
    }

    bool drew = false;
    for (int i = 0; i < ni; i += 3) {
        const int a = mesh.indices[i];
        const int b = mesh.indices[i + 1];
        const int c = mesh.indices[i + 2];

        if (!visible[a] || !visible[b] || !visible[c])
            continue;

        // Cull triangles entirely off-screen
        const float minX = std::min({screen[a].x, screen[b].x, screen[c].x});
        const float maxX = std::max({screen[a].x, screen[b].x, screen[c].x});
        const float minY = std::min({screen[a].y, screen[b].y, screen[c].y});
        const float maxY = std::max({screen[a].y, screen[b].y, screen[c].y});
        if (maxX < 0 || minX > screenW || maxY < 0 || minY > screenH)
            continue;
        if (maxX - minX > screenW * 0.5f || maxY - minY > screenH * 0.5f)
            continue;

        dl->AddTriangleFilled(screen[a], screen[b], screen[c], fillCol);
        drew = true;
    }
    return drew;
}

} // namespace CharacterMesh
