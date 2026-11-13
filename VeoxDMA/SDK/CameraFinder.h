#pragma once
#include <cstdint>
#include "Matrix4x4.h"
#include "../Misc/Vector.h"

namespace CameraFinder {

void Init(uintptr_t unityPlayerBase);
uintptr_t GetTaggedNodes(uintptr_t s_Instance, int n5);
uintptr_t FindMainCamera();

struct CameraData {
    // Combined row-vector view-projection (clip = world * viewMatrix).
    Matrix4x4 viewMatrix;
    // The two halves, so callers can rebuild the projection for a different
    // aspect while keeping the camera's real vertical FOV.
    Matrix4x4 view;
    Matrix4x4 projection;
    bool valid = false;
    // Validation against the managed camera (the aim never writes its
    // position): distance in metres and forward-vector dot product. Negative
    // when unknown.
    float positionError = -1.f;
    float forwardDot = -1.f;
};

CameraData GetCameraMatrices();

// Finds the Unity camera that is actually rendering the scene and reads its
// world-to-camera and projection matrices. Unity's Camera field offsets move
// between builds, so the matrices are located structurally (orthonormal view
// basis, camera position extracted from the view matrix, perspective
// projection layout) and cross-checked against the managed camera position and
// forward vector. A camera that does not match is rejected, which keeps a
// wrong/preview camera from silently breaking ESP projection.
CameraData GetCameraMatricesValidated(const Vector3& expectedPosition,
    const Vector3& expectedForward, float expectedVerticalFovDegrees);

float ReadNativeFov();

// Fast path: scatter-reads view+projection in one device transaction,
// no managed validation. Use when native camera was valid last frame.
CameraData GetCameraMatricesFast();

} // namespace CameraFinder
