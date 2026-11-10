#include "pch.h"
#include "RobRunCamera.h"
#include "RobRunSDK.h"
#include "Il2Cpp.h"
#include "Memory.h"
#include <cmath>

namespace RobRunCamera {

static std::uintptr_t gaBase = 0;

static bool ReadBytes(std::uintptr_t address, void* buffer, std::size_t size)
{
    if (!address || !buffer || !size) return false;
    return TargetProcess.Read(address, buffer, size);
}

static std::uintptr_t ReadPtr(std::uintptr_t address)
{
    std::uintptr_t value = 0;
    return ReadBytes(address, &value, sizeof(value)) ? value : 0;
}

static bool Finite(const Vector3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static bool Normalize(Vector3& v)
{
    const float lengthSq = v.x * v.x + v.y * v.y + v.z * v.z;
    if (!std::isfinite(lengthSq) || lengthSq < 1e-8f) return false;
    const float inv = 1.f / std::sqrt(lengthSq);
    v.x *= inv; v.y *= inv; v.z *= inv;
    return Finite(v);
}

static Vector3 Cross(const Vector3& a, const Vector3& b)
{
    return Vector3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x);
}

static Vector3 Rotate(const float q[4], const Vector3& v)
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const Vector3 t{
        y * v.z - z * v.y,
        z * v.x - x * v.z,
        x * v.y - y * v.x
    };
    const Vector3 t2{ t.x * 2.f, t.y * 2.f, t.z * 2.f };
    const Vector3 cross{
        y * t2.z - z * t2.y,
        z * t2.x - x * t2.z,
        x * t2.y - y * t2.x
    };
    return Vector3(
        v.x + w * t2.x + cross.x,
        v.y + w * t2.y + cross.y,
        v.z + w * t2.z + cross.z);
}

static bool ReadVector3(std::uintptr_t address, Vector3& out)
{
    if (!ReadBytes(address, &out, sizeof(out))) return false;
    return Finite(out);
}

static bool ReadFloat(std::uintptr_t address, float& out)
{
    if (!ReadBytes(address, &out, sizeof(out))) return false;
    return std::isfinite(out);
}

static bool ReadQuat(std::uintptr_t address, float out[4])
{
    if (!ReadBytes(address, out, sizeof(float) * 4)) return false;
    return std::isfinite(out[0]) && std::isfinite(out[1]) &&
        std::isfinite(out[2]) && std::isfinite(out[3]);
}

void Init(std::uintptr_t gameAssemblyBase)
{
    gaBase = gameAssemblyBase;
    Il2Cpp::Init(gameAssemblyBase);
}

float ReadCinemachineFov()
{
    if (!gaBase) return 0.f;

    const std::uintptr_t klass = Il2Cpp::GetClassByIndex(rr::ClassIndex::CinemachineCore);
    if (!klass) return 0.f;

    const std::uintptr_t statics = Il2Cpp::StaticFields(klass);
    if (!statics) return 0.f;

    const std::uintptr_t core = ReadPtr(statics + rr::Cinemachine::CoreInstance);
    if (!core) return 0.f;

    const std::uintptr_t brainList = ReadPtr(core + rr::Cinemachine::BrainList);
    if (!brainList) return 0.f;

    const std::uintptr_t items = ReadPtr(brainList + rr::List::items);
    std::int32_t count = 0;
    if (!items || !ReadBytes(brainList + rr::List::count, &count, sizeof(count)))
        return 0.f;
    if (count < 1 || count > 32) return 0.f;

    for (int i = 0; i < count; ++i) {
        const std::uintptr_t brain = ReadPtr(items + rr::Array::vector +
            static_cast<std::uintptr_t>(i) * rr::Array::stride);
        if (!brain || !Il2Cpp::IsClass(brain, "CinemachineBrain")) continue;
        float fov = 0.f;
        if (ReadFloat(brain + rr::Cinemachine::BrainFov, fov) &&
            fov >= 1.f && fov <= 179.f) {
            return fov;
        }
    }
    return 0.f;
}

CameraState Read(std::uintptr_t battleCameraModel)
{
    CameraState camera{};
    if (battleCameraModel <= 0x10000) return camera;

    if (!ReadVector3(battleCameraModel + rr::BattleCameraModelOffsets::Position, camera.position))
        return camera;

    ReadQuat(battleCameraModel + rr::BattleCameraModelOffsets::Orientation, camera.orientation);
    ReadQuat(battleCameraModel + rr::BattleCameraModelOffsets::LookQuat, camera.look);
    ReadFloat(battleCameraModel + rr::BattleCameraModelOffsets::Distance, camera.distance);

    const bool hasForward = ReadVector3(battleCameraModel + rr::BattleCameraModelOffsets::Forward, camera.forward);
    const bool hasRight = ReadVector3(battleCameraModel + rr::BattleCameraModelOffsets::Right, camera.right);
    const bool hasUp = ReadVector3(battleCameraModel + rr::BattleCameraModelOffsets::Up, camera.up);

    if (!hasForward || !Normalize(camera.forward))
        camera.forward = Rotate(camera.look, Vector3(0.f, 0.f, 1.f));
    if (!Normalize(camera.forward)) return camera;

    const bool rightOk = hasRight && Normalize(camera.right);
    const bool upOk = hasUp && Normalize(camera.up);
    if (!rightOk || !upOk) {
        // Rebuild an orthonormal basis around forward. Unity is left-handed
        // with +Y up, so right = up x forward and up = forward x right.
        const Vector3 upHint = upOk ? camera.up : Vector3(0.f, 1.f, 0.f);
        Vector3 right = Cross(upHint, camera.forward);
        if (!Normalize(right)) return camera;
        Vector3 up = Cross(camera.forward, right);
        if (!Normalize(up)) return camera;
        camera.right = right;
        camera.up = up;
    }

    const float fov = ReadCinemachineFov();
    if (fov > 0.f) {
        camera.verticalFov = fov;
        camera.fovFromCinemachine = true;
    }

    camera.valid = true;
    return camera;
}

bool BuildViewProjection(const CameraState& camera, float aspect,
    Matrix4x4& out, float nearPlane, float farPlane)
{
    if (!camera.valid || !std::isfinite(aspect) || aspect <= 0.f) return false;
    if (!(nearPlane > 0.f) || !(farPlane > nearPlane)) return false;

    const Vector3& p = camera.position;
    const Vector3& r = camera.right;
    const Vector3& u = camera.up;
    const Vector3& f = camera.forward;

    // Row-vector convention: clip = [x y z 1] * view * projection, matching
    // WorldToScreen's column reads and CameraFinder's native-matrix fallback.
    // Unity stores matrices column-major, so this is the transposed view and
    // projection in memory order.
    const float dotRP = r.x * p.x + r.y * p.y + r.z * p.z;
    const float dotUP = u.x * p.x + u.y * p.y + u.z * p.z;
    const float dotFP = f.x * p.x + f.y * p.y + f.z * p.z;

    Matrix4x4 view{};
    view.matrix[0][0] = r.x;    view.matrix[0][1] = u.x;    view.matrix[0][2] = -f.x; view.matrix[0][3] = 0.f;
    view.matrix[1][0] = r.y;    view.matrix[1][1] = u.y;    view.matrix[1][2] = -f.y; view.matrix[1][3] = 0.f;
    view.matrix[2][0] = r.z;    view.matrix[2][1] = u.z;    view.matrix[2][2] = -f.z; view.matrix[2][3] = 0.f;
    view.matrix[3][0] = -dotRP; view.matrix[3][1] = -dotUP; view.matrix[3][2] = dotFP; view.matrix[3][3] = 1.f;

    const float fovRadians = camera.verticalFov * 3.14159265358979323846f / 180.f;
    const float yScale = 1.f / std::tan(fovRadians * 0.5f);
    const float xScale = yScale / aspect;
    const float a = (farPlane + nearPlane) / (nearPlane - farPlane);
    const float b = (2.f * farPlane * nearPlane) / (nearPlane - farPlane);

    Matrix4x4 projection{};
    projection.matrix[0][0] = xScale;
    projection.matrix[1][1] = yScale;
    projection.matrix[2][2] = a;
    projection.matrix[2][3] = -1.f;
    projection.matrix[3][2] = b;

    out = view * projection;
    return true;
}

bool WorldToScreen(const Matrix4x4& viewProjection, const Vector3& world,
    int screenWidth, int screenHeight, Vector2& out)
{
    if (screenWidth <= 0 || screenHeight <= 0) return false;

    const float clipX = viewProjection.matrix[0][0] * world.x +
        viewProjection.matrix[1][0] * world.y +
        viewProjection.matrix[2][0] * world.z +
        viewProjection.matrix[3][0];
    const float clipY = viewProjection.matrix[0][1] * world.x +
        viewProjection.matrix[1][1] * world.y +
        viewProjection.matrix[2][1] * world.z +
        viewProjection.matrix[3][1];
    const float clipW = viewProjection.matrix[0][3] * world.x +
        viewProjection.matrix[1][3] * world.y +
        viewProjection.matrix[2][3] * world.z +
        viewProjection.matrix[3][3];

    if (!std::isfinite(clipX) || !std::isfinite(clipY) ||
        !std::isfinite(clipW) || clipW <= 0.001f) {
        return false;
    }

    const float ndcX = clipX / clipW;
    const float ndcY = clipY / clipW;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) return false;

    out.x = (screenWidth * 0.5f) * (1.f + ndcX);
    out.y = (screenHeight * 0.5f) * (1.f - ndcY);
    return true;
}

} // namespace RobRunCamera
