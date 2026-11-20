#pragma once
#include <cstdint>
#include "Matrix4x4.h"
#include "../Misc/Vector.h"

namespace RobRunCamera {

struct CameraState {
    bool valid = false;
    Vector3 position{};
    float orientation[4]{}; // x y z w
    float look[4]{};        // x y z w
    Vector3 forward{};
    Vector3 right{};
    Vector3 up{};
    float distance = 0.f;
    float verticalFov = 60.f;
    bool fovFromCinemachine = false;
};

void Init(std::uintptr_t gameAssemblyBase);

// Reads BattleCameraModel and attaches the active Cinemachine vertical FOV.
CameraState Read(std::uintptr_t battleCameraModel);

// Reads CinemachineCore -> brain list -> active CinemachineBrain +0xB0.
// Returns 0 when the path is unavailable.
float ReadCinemachineFov();

// Builds a row-vector view-projection matrix matching RobRunReader's
// WorldToScreen convention. aspect = viewport width / viewport height.
bool BuildViewProjection(const CameraState& camera, float aspect,
    Matrix4x4& out, float nearPlane = 0.05f, float farPlane = 20000.f);

bool WorldToScreen(const Matrix4x4& viewProjection, const Vector3& world,
    int screenWidth, int screenHeight, Vector2& out);

} // namespace RobRunCamera
