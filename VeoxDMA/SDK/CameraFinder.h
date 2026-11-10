#pragma once
#include <cstdint>
#include "Matrix4x4.h"

namespace CameraFinder {

void Init(uintptr_t unityPlayerBase);
uintptr_t GetTaggedNodes(uintptr_t s_Instance, int n5);
uintptr_t FindMainCamera();

struct CameraData {
    Matrix4x4 viewMatrix;
    bool valid;
};

CameraData GetCameraMatrices();

} // namespace CameraFinder
