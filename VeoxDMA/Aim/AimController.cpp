#include "pch.h"
#include "AimController.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
#include "RobRunSDK.h"
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "Memory.h"
#include "DeviceSession.h"
#include "InputManager.h"
#include "Overlay.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace Aim {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Keep in sync with the labels below. 0 means "always active".
const int kKeys[] = {
    VK_RBUTTON,      // 0: Right Mouse
    VK_LBUTTON,      // 1: Left Mouse
    VK_LMENU,        // 2: Left Alt
    VK_LSHIFT,       // 3: Left Shift
    VK_XBUTTON1,     // 4: Mouse 4
    VK_XBUTTON2,     // 5: Mouse 5
    VK_MBUTTON,      // 6: Middle Mouse
    VK_LCONTROL,     // 7: Left Ctrl
    VK_CAPITAL,      // 8: Caps Lock
    VK_SPACE,        // 9: Space
    VK_TAB,          // 10: Tab
    'C',             // 11: C
    'V',             // 12: V
    'E',             // 13: E
    'F',             // 14: F
    'Q',             // 15: Q
    'X',             // 16: X
    'Z',             // 17: Z
    0                // 18: Always
};
const char* const kKeyNames[] = {
    "Right Mouse",
    "Left Mouse",
    "Left Alt",
    "Left Shift",
    "Mouse 4",
    "Mouse 5",
    "Middle Mouse",
    "Left Ctrl",
    "Caps Lock",
    "Space",
    "Tab",
    "C",
    "V",
    "E",
    "F",
    "Q",
    "X",
    "Z",
    "Always"
};
constexpr int kKeyCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));

// The camera block is written as one scatter transaction. Ten milliseconds is
// slow enough to keep the read worker fed and still fast enough that the
// exponential response (20-90 ms) does not look steppy.
constexpr std::uint64_t kWriteIntervalMs = 16;

std::uint64_t g_observation = 0;
int g_lastTarget = -1;
std::uint64_t g_lastTargetSwitchMs = 0;
bool g_wasAiming = false;
DebugInfo g_debug{};

// Smoothing state. Reset whenever the aim key is released or the target
// changes, so a new pick starts from its own angle instead of sweeping across
// the world from the previous target.
bool g_smoothInit = false;
float g_curYaw = 0.f;
float g_curPitch = 0.f;
float g_carryYaw = 0.f;
float g_carryPitch = 0.f;
int g_smoothTarget = -1;
std::uint64_t g_lastWriteTick = 0;
Vector3 g_writtenForward{};
bool g_haveWrittenForward = false;
bool g_writeHealthy = false;
const char* g_lastVerdict = "-";

// Memory aim write gate. A level transition destroys the camera/input objects;
// writes stay blocked for a second after the reader reports a new session so
// the new objects can be re-resolved first.
std::uint64_t g_lastSessionGeneration = 0;
std::uint64_t g_writeBlockedUntil = 0;

// Previous feet position per entity, used for target leading. Kept on the
// render thread because the reader snapshot only carries one position.
struct MotionSample {
    Vector3 position{};
    std::uint64_t tickMs = 0;
    bool valid = false;
};
std::unordered_map<int, MotionSample> g_motion;

struct AimQuat {
    float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
};

bool Held(int keyIndex, const char** source)
{
    if (keyIndex < 0 || keyIndex >= kKeyCount) keyIndex = 0;
    const int vk = kKeys[keyIndex];
    if (vk == 0) {
        if (source) *source = "always";
        return true;
    }

    // Poll every source and OR the results. Short-circuiting on the DMA
    // keyboard was hiding a working device/host input: IsReady() only proves
    // the bitmap address was readable once, not that it points at this
    // session, so IsKeyDown() can legitimately stay false forever.
    if (Keyboard::IsReady() &&
        Keyboard::IsKeyDown(static_cast<std::uint32_t>(vk))) {
        if (source) *source = "DMA keyboard";
        return true;
    }

    bool deviceDown = false;
    if (Base::Hardware::MouseDown(vk, deviceDown) && deviceDown) {
        if (source) *source = "aim device";
        return true;
    }

    auto isHostDown = [](int k) -> bool {
        if ((GetAsyncKeyState(k) & 0x8000) != 0) return true;
        if (k == VK_LMENU || k == VK_RMENU) return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        if (k == VK_LSHIFT || k == VK_RSHIFT) return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (k == VK_LCONTROL || k == VK_RCONTROL) return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        return false;
    };

    if (isHostDown(vk)) {
        if (source) *source = "host keyboard";
        return true;
    }
    return false;
}

bool Finite(const Vector3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Role labels come from the reader's reflection pass. Mirror the ESP's id
// fallbacks so the aim filter and the overlay never disagree about a target.
// The teammate filter is party based: the reader resolves the local party's
// member ids and marks each entity, so same-role strangers stay targetable.
bool ShouldSkipByRole(const RobRunReader::EntityData& ent, const AimConfig& cfg)
{
    const std::string role = RobRunReader::RoleLabel(ent.role, ent.roleName);
    if (cfg.ExpIgnorePolice && role == "Police Officer") return true;
    if (cfg.ExpIgnoreCriminal && role == "Criminal") return true;
    if (cfg.ExpIgnoreCivilian && role == "Civilian") return true;
    if (cfg.ExpIgnoreTeammates && ent.isPartyMember) return true;
    if (ent.publicId && cfg.TrustedPlayers.count(ent.publicId)) return true;
    return false;
}

Vector3 BonePoint(const RobRunReader::EntityData& ent, int bone)
{
    static const float heights[] = { 1.62f, 1.45f, 1.25f, 0.95f };
    const int index = (bone >= 0 && bone < 4) ? bone : 0;

    const auto& points = ent.bonePoints;
    if (points.valid) {
        Vector3 point{};
        bool have = false;
        switch (bone) {
        case 0: point = points.head;   have = true; break;
        case 1: point = points.neck;   have = true; break;
        case 2: point = points.chest;  have = true; break;
        case 3: point = points.pelvis; have = true; break;
        default: break;
        }
        // A stale or mis-resolved rig layout can point a bone at the world
        // origin. Only trust a sample that is finite and actually sits on this
        // character (feet-to-head is under 2 m).
        if (have && Finite(point)) {
            const Vector3 root(ent.position.x, ent.position.y + 1.f, ent.position.z);
            if (Distance(point, root) <= 3.f) return point;
        }
    }
    // Fallback when the rig could not be sampled: body-relative heights above
    // the transform root (which sits at the feet).
    return Vector3(ent.position.x, ent.position.y + heights[index], ent.position.z);
}

void SendRelative(int dx, int dy)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(input));
}

float FovRadiusPixels(float fovDeg, float verticalFovDeg, float height)
{
    fovDeg = std::clamp(fovDeg, 5.f, 180.f);
    if (fovDeg >= 179.f) return 1.0e6f; // effectively everything on screen
    const float vfov = std::clamp(verticalFovDeg, 1.f, 179.f);
    return std::tan(fovDeg * 0.5f * kPi / 180.f)
        / std::tan(vfov * 0.5f * kPi / 180.f) * (height * 0.5f);
}

// Recovers the vertical FOV encoded in the row-vector view-projection matrix.
// The second column is the camera up axis scaled by 1/tan(vfov/2), so its
// length is the projection's y scale. This makes the aim cone match whatever
// matrix the ESP actually drew with, including the native Unity fallback.
float VerticalFovFromProjection(const Matrix4x4& viewProjection, float fallback)
{
    const float x = viewProjection.matrix[0][1];
    const float y = viewProjection.matrix[1][1];
    const float z = viewProjection.matrix[2][1];
    const float yScale = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(yScale) || yScale <= 1.0e-4f) return fallback;
    return 2.f * std::atan(1.f / yScale) * 180.f / kPi;
}

float ConeRadiusPixels(const Matrix4x4& viewProjection, float fovDeg,
    float fallbackVerticalFov, float height)
{
    return FovRadiusPixels(fovDeg,
        VerticalFovFromProjection(viewProjection, fallbackVerticalFov), height);
}

// -------- angle helpers (same convention as the reference memory aim) ------
// Unity left-handed: positive pitch is nose-down, so forward.y = -sin(pitch).

// Rotation matrix (columns right/up/forward) -> quaternion. The reference built
// the quaternion from Euler angles directly, but its z term did not reproduce
// its own forward vector, so the game's quaternion is derived from the same
// basis we write instead. This keeps the block internally consistent.
AimQuat QuatFromBasis(const Vector3& right, const Vector3& up, const Vector3& forward)
{
    const float m00 = right.x, m01 = up.x, m02 = forward.x;
    const float m10 = right.y, m11 = up.y, m12 = forward.y;
    const float m20 = right.z, m21 = up.z, m22 = forward.z;
    const float trace = m00 + m11 + m22;

    AimQuat q;
    if (trace > 0.f) {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }

    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length > 1.0e-6f) {
        q.x /= length; q.y /= length; q.z /= length; q.w /= length;
    }
    return q;
}

Vector3 EulerToForward(float yaw, float pitch)
{
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    return Vector3(std::sin(yaw) * cp, -sp, std::cos(yaw) * cp);
}

Vector3 RightFromYaw(float yaw)
{
    return Vector3(std::cos(yaw), 0.f, -std::sin(yaw));
}

Vector3 UpFromYawPitch(float yaw, float pitch)
{
    return Vector3(
        std::sin(yaw) * std::sin(pitch),
        std::cos(pitch),
        std::cos(yaw) * std::sin(pitch));
}

// Response time in milliseconds for the exponential gain. Smoothness 1 is the
// fastest step the reference exposes, 100 the slowest.
float ResponseForSmoothness(int smoothness)
{
    const float s = std::clamp((static_cast<float>(smoothness) - 1.f) / 99.f, 0.f, 1.f);
    const float step = 1.f + s * 9.f; // 1..10, matches the reference mapping
    return 12.f + 8.f * step;
}

// Writes the camera orientation, look quaternion and orthonormal basis in one
// scatter transaction. Returns false when the device rejected the write, which
// lets the caller fall back to cursor movement.
struct WriteOutcome {
    bool attempted = false;
    bool scatter = false;
};

bool WriteCameraAngles(std::uintptr_t battleModel, std::uintptr_t cameraModel,
    float yaw, float pitch,
    WriteOutcome& outcome, Vector3* liveForwardOut = nullptr,
    std::uintptr_t lookDirModel = 0, std::uintptr_t lookDirDirect = 0,
    std::uintptr_t playerRotInput = 0)
{
    outcome = WriteOutcome{};
    if (cameraModel <= 0x10000 || battleModel <= 0x10000) return false;

    Vector3 forward = EulerToForward(yaw, pitch);
    Vector3 right = RightFromYaw(yaw);
    Vector3 up = UpFromYawPitch(yaw, pitch);
    AimQuat q = QuatFromBasis(right, up, forward);

    // Validate every target in one scatter transaction before writing a byte.
    // After a level change the cached pointers can still satisfy the old
    // numeric checks (freed memory keeps the last values), so the only reliable
    // read-only proof is that BattleModel still points at this exact camera
    // object and that each auxiliary object still looks structurally valid.
    float liveQuat[4]{};
    Vector3 liveForward{};
    std::uintptr_t liveCamera = 0;
    Vector3 lookModelValue{};
    Vector3 lookDirectValue{};
    float lookDirectYaw = 0.f;
    float lookDirectPitch = 0.f;
    float rotYaw = 0.f;
    float rotPitch = 0.f;
    {
        Memory::ScopedScatter scatter(TargetProcess);
        if (!scatter.handle) return false;
        TargetProcess.AddScatterReadRequest(scatter,
            battleModel + rr::BattleModel::BattleCameraModel, &liveCamera,
            sizeof(liveCamera));
        TargetProcess.AddScatterReadRequest(scatter,
            cameraModel + rr::BattleCameraModelOffsets::Orientation,
            liveQuat, sizeof(liveQuat));
        TargetProcess.AddScatterReadRequest(scatter,
            cameraModel + rr::BattleCameraModelOffsets::Forward,
            &liveForward, sizeof(liveForward));
        if (lookDirModel > 0x10000)
            TargetProcess.AddScatterReadRequest(scatter,
                lookDirModel + rr::ReferenceData::Value, &lookModelValue,
                sizeof(lookModelValue));
        if (lookDirDirect > 0x10000) {
            TargetProcess.AddScatterReadRequest(scatter,
                lookDirDirect + rr::LookDirectionModel::Value, &lookDirectValue,
                sizeof(lookDirectValue));
            TargetProcess.AddScatterReadRequest(scatter,
                lookDirDirect + rr::LookDirectionModel::Yaw, &lookDirectYaw,
                sizeof(lookDirectYaw));
            TargetProcess.AddScatterReadRequest(scatter,
                lookDirDirect + rr::LookDirectionModel::Pitch, &lookDirectPitch,
                sizeof(lookDirectPitch));
        }
        if (playerRotInput > 0x10000) {
            TargetProcess.AddScatterReadRequest(scatter,
                playerRotInput + rr::PlayerRotationInputModel::Yaw, &rotYaw,
                sizeof(rotYaw));
            TargetProcess.AddScatterReadRequest(scatter,
                playerRotInput + rr::PlayerRotationInputModel::Pitch, &rotPitch,
                sizeof(rotPitch));
        }
        TargetProcess.ExecuteReadScatter(scatter);
    }
    if (liveForwardOut) *liveForwardOut = liveForward;

    // The camera object must still be the one BattleModel currently owns. This
    // is the hard gate that stops writes into a camera freed by a level change.
    if (liveCamera != cameraModel) return false;

    const float quatLength2 = liveQuat[0] * liveQuat[0] + liveQuat[1] * liveQuat[1] +
        liveQuat[2] * liveQuat[2] + liveQuat[3] * liveQuat[3];
    if (!std::isfinite(quatLength2) || std::fabs(quatLength2 - 1.f) > 0.15f) return false;
    if (!Finite(liveForward)) return false;
    const float forwardLength = std::sqrt(
        liveForward.x * liveForward.x + liveForward.y * liveForward.y +
        liveForward.z * liveForward.z);
    if (!std::isfinite(forwardLength) || forwardLength < 0.9f || forwardLength > 1.1f)
        return false;

    // Auxiliary input models are optional. If one no longer looks valid, drop
    // that target instead of writing into a reused object.
    const auto unitVector = [](const Vector3& v) {
        if (!Finite(v)) return false;
        const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
        return std::isfinite(len2) && len2 > 0.64f && len2 < 1.44f;
    };
    const auto angleOk = [](float angle) {
        return std::isfinite(angle) && std::fabs(angle) <= 7.0f;
    };
    const auto pitchOk = [](float angle) {
        return std::isfinite(angle) && std::fabs(angle) <= 1.75f;
    };
    if (lookDirModel > 0x10000 && !unitVector(lookModelValue))
        lookDirModel = 0;
    if (lookDirDirect > 0x10000 &&
        (!unitVector(lookDirectValue) || !angleOk(lookDirectYaw) ||
         !pitchOk(lookDirectPitch)))
        lookDirDirect = 0;
    if (playerRotInput > 0x10000 &&
        (!angleOk(rotYaw) || !pitchOk(rotPitch)))
        playerRotInput = 0;

    outcome.attempted = true;

    Memory::ScopedScatter scatter(TargetProcess);
    if (scatter.handle) {
        TargetProcess.AddScatterWriteRequest(scatter,
            cameraModel + rr::BattleCameraModelOffsets::Orientation, &q, sizeof(q));
        TargetProcess.AddScatterWriteRequest(scatter,
            cameraModel + rr::BattleCameraModelOffsets::LookQuat, &q, sizeof(q));
        TargetProcess.AddScatterWriteRequest(scatter,
            cameraModel + rr::BattleCameraModelOffsets::Forward, &forward, sizeof(forward));
        TargetProcess.AddScatterWriteRequest(scatter,
            cameraModel + rr::BattleCameraModelOffsets::Right, &right, sizeof(right));
        TargetProcess.AddScatterWriteRequest(scatter,
            cameraModel + rr::BattleCameraModelOffsets::Up, &up, sizeof(up));
        // Write the forward vector to the LookDirection model so the game's
        // input pipeline reads the same direction we set on the camera.
        if (lookDirModel > 0x10000)
            TargetProcess.AddScatterWriteRequest(scatter,
                lookDirModel + rr::ReferenceData::Value, &forward, sizeof(forward));
        // BCM+0x70 LookDirectionModel: _value(+0x10), _yaw(+0x1C), _pitch(+0x20)
        if (lookDirDirect > 0x10000) {
            TargetProcess.AddScatterWriteRequest(scatter,
                lookDirDirect + rr::LookDirectionModel::Value, &forward, sizeof(forward));
            TargetProcess.AddScatterWriteRequest(scatter,
                lookDirDirect + rr::LookDirectionModel::Yaw, &yaw, sizeof(yaw));
            TargetProcess.AddScatterWriteRequest(scatter,
                lookDirDirect + rr::LookDirectionModel::Pitch, &pitch, sizeof(pitch));
        }
        if (playerRotInput > 0x10000) {
            TargetProcess.AddScatterWriteRequest(scatter,
                playerRotInput + rr::PlayerRotationInputModel::Yaw, &yaw, sizeof(yaw));
            TargetProcess.AddScatterWriteRequest(scatter,
                playerRotInput + rr::PlayerRotationInputModel::Pitch, &pitch, sizeof(pitch));
        }
        if (TargetProcess.ExecuteScatterWrite(scatter)) {
            outcome.scatter = true;
            return true;
        }
    }

    bool ok = TargetProcess.Write(
        cameraModel + rr::BattleCameraModelOffsets::Orientation, &q, sizeof(q));
    ok = TargetProcess.Write(
        cameraModel + rr::BattleCameraModelOffsets::LookQuat, &q, sizeof(q)) && ok;
    ok = TargetProcess.Write(
        cameraModel + rr::BattleCameraModelOffsets::Forward, &forward, sizeof(forward)) && ok;
    ok = TargetProcess.Write(
        cameraModel + rr::BattleCameraModelOffsets::Right, &right, sizeof(right)) && ok;
    ok = TargetProcess.Write(
        cameraModel + rr::BattleCameraModelOffsets::Up, &up, sizeof(up)) && ok;
    if (lookDirModel > 0x10000)
        TargetProcess.Write(
            lookDirModel + rr::ReferenceData::Value, &forward, sizeof(forward));
    if (lookDirDirect > 0x10000) {
        TargetProcess.Write(lookDirDirect + rr::LookDirectionModel::Value, &forward, sizeof(forward));
        TargetProcess.Write(lookDirDirect + rr::LookDirectionModel::Yaw, &yaw, sizeof(yaw));
        TargetProcess.Write(lookDirDirect + rr::LookDirectionModel::Pitch, &pitch, sizeof(pitch));
    }
    if (playerRotInput > 0x10000) {
        TargetProcess.Write(playerRotInput + rr::PlayerRotationInputModel::Yaw, &yaw, sizeof(yaw));
        TargetProcess.Write(playerRotInput + rr::PlayerRotationInputModel::Pitch, &pitch, sizeof(pitch));
    }
    return ok;
}

// Verdict on the previous write: if the camera controller rebuilds the basis
// from its own input state, the forward vector no longer matches what we wrote.
// This is a diagnostic, not a failure path.
const char* CompareToWritten(const Vector3& written, const Vector3& live)
{
    if (!Finite(live)) return "unreadable";
    const float length = std::sqrt(live.x * live.x + live.y * live.y + live.z * live.z);
    if (!std::isfinite(length) || length < 1.0e-3f) return "unreadable";
    const float dot = (written.x * live.x + written.y * live.y + written.z * live.z) / length;
    return dot > 0.99f ? "held" : "reverted";
}

// Reference leading: sample window 4..100 ms, speed gate 0.35..12 m/s, lead
// capped at 65 ms / response time / 0.55 m per m/s. Y lead is scaled
// separately because jump and crouch arcs do not extrapolate linearly.
bool ApplyLeading(const Vector3& point, const Vector3& position,
    const Vector3& previous, std::uint64_t sampleMs, float responseMs,
    int strengthPercent, int yScalePercent, Vector3& out, int& leadMs)
{
    if (sampleMs == 0) return false;
    const float seconds = static_cast<float>(sampleMs) * 0.001f;
    const float vx = (position.x - previous.x) / seconds;
    const float vy = (position.y - previous.y) / seconds;
    const float vz = (position.z - previous.z) / seconds;

    const float speedXZ = std::hypot(vx, vz);
    const float speedY = std::fabs(vy);
    if (speedXZ < 0.35f && speedY < 0.35f) return false;

    const float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (!std::isfinite(speed) || speed > 12.f) return false;

    float lead = std::min(0.065f, responseMs * 0.001f);
    lead = std::min(lead, 0.55f / speed);
    const float strength = std::clamp(static_cast<float>(strengthPercent) / 100.f, 0.f, 2.f);
    lead *= strength;
    const float yScale = std::clamp(static_cast<float>(yScalePercent) / 100.f, 0.f, 1.f);

    out = Vector3(
        point.x + vx * lead,
        point.y + vy * lead * yScale,
        point.z + vz * lead);
    leadMs = static_cast<int>(lead * 1000.f);
    return true;
}

} // namespace

const char* const* KeyNames() { return kKeyNames; }
int KeyCount() { return kKeyCount; }
bool IsKeyHeld(int keyIndex)
{
    const char* source = nullptr;
    return Held(keyIndex, &source);
}
static std::atomic<float> g_displayWidth{ 1920.f };
static std::atomic<float> g_displayHeight{ 1080.f };
static std::mutex g_debugMutex;
static std::atomic<bool> g_aimWorkerStop{ false };
static std::thread g_aimWorkerThread;

void SetDisplaySize(float width, float height)
{
    if (width > 0.f && height > 0.f) {
        g_displayWidth.store(width, std::memory_order_relaxed);
        g_displayHeight.store(height, std::memory_order_relaxed);
    }
}

DebugInfo GetDebug()
{
    std::lock_guard<std::mutex> lock(g_debugMutex);
    return g_debug;
}

static void TickInstantLoot()
{
    if (!ConfigInstance.World.InstantLoot) return;
    const auto snap = RobRunReader::GetSnapshot();
    if (!snap || snap->battleModel <= 0x10000) return;

    const auto bm = snap->battleModel;

    // ProlongedInteractionDialog -> Progress (float at +0x18).
    // When the progress bar is moving (0 < p < 1), pin it to 1.0.
    const std::uintptr_t dialog = RobRunMemory::ReadPtr(
        bm + rr::BattleModel::ProlongedInteractionDialog);
    if (dialog > 0x10000) {
        const float progress = RobRunMemory::ReadFloat(
            dialog + rr::ProlongedInteractionDialogModel::Progress);
        if (progress > 0.01f && progress < 0.99f) {
            float done = 1.0f;
            Memory::ScopedScatter sc(TargetProcess);
            if (sc.handle) {
                TargetProcess.AddScatterWriteRequest(sc,
                    dialog + rr::ProlongedInteractionDialogModel::Progress,
                    &done, sizeof(done));
                TargetProcess.ExecuteWriteScatter(sc);
            }
        }
    }

    // DelayedUseAction -> set EndTs = StartTs so it completes this tick.
    const std::uintptr_t delayed = RobRunMemory::ReadPtr(
        bm + rr::BattleModel::DelayedUseAction);
    if (delayed > 0x10000) {
        float startTs = RobRunMemory::ReadFloat(
            delayed + rr::DelayedUseActionModel::StartTs);
        const float endTs = RobRunMemory::ReadFloat(
            delayed + rr::DelayedUseActionModel::EndTs);
        if (endTs > startTs && startTs > 0.f) {
            Memory::ScopedScatter sc(TargetProcess);
            if (sc.handle) {
                TargetProcess.AddScatterWriteRequest(sc,
                    delayed + rr::DelayedUseActionModel::EndTs,
                    &startTs, sizeof(startTs));
                TargetProcess.ExecuteWriteScatter(sc);
            }
        }
    }

    // Patch InventoryRevealViewDescriptions so loot slots appear instantly
    // after a container is opened (skip scan/shine/icon animation phases).
    {
        static std::uintptr_t cachedRevealDescs = 0;
        static std::uint64_t revealStamp = 0;
        const std::uint64_t now = GetTickCount64();
        if (now - revealStamp < 2000) return;
        revealStamp = now;

        if (!cachedRevealDescs) {
            const std::uintptr_t klass =
                Il2Cpp::FindClassByName("InventoryRevealViewDescriptions");
            if (klass) {
                const std::uintptr_t statics = Il2Cpp::StaticFields(klass);
                if (statics)
                    cachedRevealDescs = RobRunMemory::ReadPtr(statics);
            }
        }
        if (cachedRevealDescs <= 0x10000) { cachedRevealDescs = 0; return; }

        namespace rv = rr::InventoryRevealViewDescriptions;
        const std::uintptr_t items = RobRunMemory::ReadPtr(
            cachedRevealDescs + rv::Items);
        if (items <= 0x10000) { cachedRevealDescs = 0; return; }
        const int count = RobRunMemory::ReadInt32(items + rv::Size);
        if (count <= 0 || count > 64) { cachedRevealDescs = 0; return; }
        const std::uintptr_t data = RobRunMemory::ReadPtr(items + rv::Data);
        if (data <= 0x10000) { cachedRevealDescs = 0; return; }

        const float nearZero = 0.0001f;
        const float zero = 0.0f;
        for (int i = 0; i < count; i++) {
            const std::uintptr_t pair = data + 0x20 + (std::uintptr_t)i * rv::Stride;
            const std::uintptr_t desc = RobRunMemory::ReadPtr(pair + rv::PairDesc);
            if (desc <= 0x10000) continue;

            TargetProcess.Write(desc + rv::ScanTime, nearZero);

            const std::uintptr_t shine = RobRunMemory::ReadPtr(desc + rv::ShineView);
            if (shine > 0x10000)
                TargetProcess.Write(shine + rv::ShineDuration, zero);

            const std::uintptr_t icon = RobRunMemory::ReadPtr(desc + rv::IconView);
            if (icon > 0x10000)
                TargetProcess.Write(icon + rv::IconDuration, zero);
        }
    }
}

void StartAimWorker()
{
    StopAimWorker();
    StartRecoilWorker();
    g_aimWorkerStop.store(false, std::memory_order_relaxed);
    g_aimWorkerThread = std::thread([] {
        using clock = std::chrono::steady_clock;
        auto lastTick = clock::now() - std::chrono::seconds(1);
        auto lastLoot = clock::now() - std::chrono::seconds(1);
        while (!g_aimWorkerStop.load(std::memory_order_relaxed)) {
            const auto now = clock::now();
            try {
                // Camera refresh stays at the loop rate (one scatter read); the
                // full entity scan and the loot poke do not need 500 Hz. The
                // write path is throttled to 16 ms and the device to 6 ms, so
                // running the scan faster than that only burns CPU.
                RobRunReader::RefreshCamera();
                if (now - lastTick >= std::chrono::milliseconds(6)) {
                    lastTick = now;
                    Tick();
                    TickRecoilSpread();
                }
                if (now - lastLoot >= std::chrono::milliseconds(50)) {
                    lastLoot = now;
                    TickInstantLoot();
                }
            } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
}

void StopAimWorker()
{
    StopRecoilWorker();
    g_aimWorkerStop.store(true, std::memory_order_relaxed);
    if (g_aimWorkerThread.joinable())
        g_aimWorkerThread.join();
}

// The reference memory aim also writes the local input model, the player
// rotation-input model and the local look-direction model. Those offsets are
// not in the current SDK, so dump the runtime field tables once and let the
// caller wire them from real metadata instead of guessing.
static void DumpAimFieldsOnce()
{
    static bool done = false;
    if (done) return;
    done = true;

    std::error_code ec;
    std::filesystem::create_directories(L"C:\\VeoxDMA\\RobRun", ec);
    std::ofstream out("C:\\VeoxDMA\\RobRun\\aim_fields.txt", std::ios::trunc);
    if (!out) return;

    struct Entry {
        int index;
        const char* label;
    };
    const Entry entries[] = {
        { rr::ClassIndex::BattleModel, "BattleModel" },
        { rr::ClassIndex::BattleCharacterModel, "BattleCharacterModel" },
        { rr::ClassIndex::BattleEntitiesModel, "BattleEntitiesModel" },
        { rr::ClassIndex::BattleCameraModel, "BattleCameraModel" },
    };

    out << "ROB RUN field dump (read-only metadata walk)\n\n";
    for (const auto& entry : entries) {
        const std::uintptr_t klass = Il2Cpp::GetClassByIndex(entry.index);
        out << "=== " << entry.label << " index=" << entry.index
            << " klass=0x" << std::hex << klass << std::dec << " ===\n";
        if (!klass) {
            out << "  unresolved\n\n";
            continue;
        }
        for (const auto& field : Il2Cpp::Fields(klass)) {
            out << "  +0x" << std::hex << field.offset << std::dec
                << "  type=" << static_cast<int>(field.typeCode)
                << "  " << field.name << "\n";
        }
        out << "\n";
    }

    out << "Camera block written by the aim: Position 0x10, Orientation 0x1C,\n"
           "LookQuat 0x2C, Forward 0x3C, Right 0x48, Up 0x54.\n\n";

    // Walk the local character hierarchy to discover InputModel and
    // BMPlayerRotationInput class names and their field offsets.
    const auto snapshot = RobRunReader::GetSnapshot();
    if (snapshot && snapshot->localCharacter > 0x10000) {
        out << "=== Local character sub-model scan ===\n"
            << "localCharacter = 0x" << std::hex << snapshot->localCharacter << std::dec << "\n";
        auto ReadPtr = [](std::uintptr_t addr) -> std::uintptr_t {
            std::uintptr_t val = 0;
            if (addr > 0x10000)
                RobRunMemory::ReadBytes(addr, &val, sizeof(val));
            return val;
        };
        auto DumpModelAt = [&](std::uintptr_t ptr, const char* label) {
            if (ptr <= 0x10000) return;
            const std::uintptr_t klass = ReadPtr(ptr + rr::Object::klass);
            if (klass <= 0x10000) return;
            char name[128]{}; char ns[128]{};
            const std::uintptr_t namePtr = ReadPtr(klass + rr::Klass::name_ptr);
            const std::uintptr_t nsPtr = ReadPtr(klass + rr::Klass::namespace_ptr);
            if (namePtr > 0x10000) RobRunMemory::ReadBytes(namePtr, name, sizeof(name) - 1);
            if (nsPtr > 0x10000) RobRunMemory::ReadBytes(nsPtr, ns, sizeof(ns) - 1);
            out << "  " << label << " ptr=0x" << std::hex << ptr << std::dec
                << "  class=" << ns << "::" << name << "\n";
            for (const auto& field : Il2Cpp::Fields(klass)) {
                out << "    +0x" << std::hex << field.offset << std::dec
                    << "  type=" << static_cast<int>(field.typeCode)
                    << "  " << field.name << "\n";
            }
        };
        // Walk known offsets on BattleCharacterModel that might host input models.
        const std::uintptr_t lc = snapshot->localCharacter;
        const std::uintptr_t offsets[] = {
            0x70, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8,
            0x228, 0x300
        };
        for (auto off : offsets) {
            char label[64];
            std::snprintf(label, sizeof(label), "BCM+0x%X", (unsigned)off);
            DumpModelAt(ReadPtr(lc + off), label);
        }
        // BattleModel+0x40 PlayerRotationInput
        if (snapshot->battleModel > 0x10000)
            DumpModelAt(ReadPtr(snapshot->battleModel + rr::BattleModel::PlayerRotationInput), "BM+0x40 (PlayerRotationInput)");

        // Also check PlayerEntityModel::Client (0x18 on the player entity)
        if (snapshot->battleModel > 0x10000) {
            const std::uintptr_t battleEnts = ReadPtr(
                snapshot->battleModel + rr::BattleModel::BattleEntitiesModel);
            const std::uintptr_t localPlayer = ReadPtr(
                battleEnts + rr::BattleEntitiesModel::LocalPlayerEntityModel);
            if (localPlayer > 0x10000) {
                DumpModelAt(ReadPtr(localPlayer + rr::PlayerEntityModel::Client), "PlayerEntity.Client");
                const std::uintptr_t client = ReadPtr(localPlayer + rr::PlayerEntityModel::Client);
                if (client > 0x10000) {
                    const std::uintptr_t clientOffsets[] = {
                        0x28, 0x30, 0x38, 0x40, 0x298
                    };
                    for (auto off : clientOffsets) {
                        char label[64];
                        std::snprintf(label, sizeof(label), "Client+0x%X", (unsigned)off);
                        DumpModelAt(ReadPtr(client + off), label);
                    }
                }
            }
        }
        out << "\n";
    }
    out << "LookDirectionModel (CommonModel) = 0x" << std::hex
        << (snapshot ? snapshot->lookDirectionModel : 0) << std::dec << "\n";
    out << "LookDirDirect (BCM+0x70) = 0x" << std::hex
        << (snapshot ? snapshot->lookDirDirect : 0) << std::dec << "\n";
    out << "PlayerRotationInput (BM+0x40) = 0x" << std::hex
        << (snapshot ? snapshot->playerRotationInput : 0) << std::dec << "\n";
}

void Tick()
{
    const auto& cfg = ConfigInstance.Aim;
	DebugInfo dbg{};
	// Publish the finished DebugInfo under the lock GetDebug() uses. Every
	// path out of Tick(), including the early returns below, publishes once
	// through this destructor; unlocked writes raced the reader.
	struct PublishDebug {
		DebugInfo& info;
		~PublishDebug() {
			std::lock_guard<std::mutex> lock(g_debugMutex);
			g_debug = info;
		}
	} publishDebug{ dbg };
    dbg.enabled = cfg.Enabled;
    dbg.menuOpen = Overlay::MenuOpen();
    dbg.aimType = cfg.AimType;
    dbg.deviceReady = Base::Hardware::Ready();

    const auto stopAiming = [] {
        if (g_wasAiming) {
            Base::Hardware::Session().Cancel(Base::Hardware::Source::Aim);
            g_wasAiming = false;
        }
    };

    const auto resetSmoothing = [] {
        g_smoothInit = false;
        g_carryYaw = g_carryPitch = 0.f;
        g_lastWriteTick = 0;
        g_haveWrittenForward = false;
        g_writeHealthy = false;
        g_lastVerdict = "-";
    };

    if (!cfg.Enabled) {
        dbg.reason = "aim disabled";
        stopAiming();
        resetSmoothing();
        g_lastTarget = -1;
        return;
    }
    if (ConfigInstance.Overlay.PanicMode) {
        dbg.reason = "panic active";
        stopAiming();
        resetSmoothing();
        g_lastTarget = -1;
        return;
    }
    if (Overlay::MenuOpen()) {
        dbg.reason = "menu open";
        stopAiming();
        resetSmoothing();
        g_lastTarget = -1;
        return;
    }

    const char* keySource = "-";
    dbg.held = Held(cfg.AimKey, &keySource);
    dbg.keySource = keySource;
    if (!dbg.held) {
        dbg.reason = "aim key not held";
        stopAiming();
        resetSmoothing();
        g_lastTarget = -1;
        g_motion.clear();
        return;
    }

    const auto snapshot = RobRunReader::GetSnapshot();
    if (!snapshot || snapshot->entities.empty()) {
        dbg.reason = "no entities";
        stopAiming();
        return;
    }
    dbg.entities = static_cast<int>(snapshot->entities.size());

    // A scene/session change (level transition, new match) invalidates every
    // pointer the aim was holding. Reset smoothing and block memory writes
    // until the reader has published fresh, re-resolved objects.
    if (snapshot->sessionGeneration != g_lastSessionGeneration) {
        g_lastSessionGeneration = snapshot->sessionGeneration;
        resetSmoothing();
        g_lastTarget = -1;
        g_motion.clear();
        g_writeBlockedUntil = GetTickCount64() + 1000;
    }

    // One-off, cheap enough to do on the first attached tick: write the runtime
    // field tables that the reference memory aim writes through, so the missing
    // input/rotation offsets can be taken from real metadata.
    DumpAimFieldsOnce();

    const float dispX = g_displayWidth.load(std::memory_order_relaxed);
    const float dispY = g_displayHeight.load(std::memory_order_relaxed);
    const int width = static_cast<int>(dispX);
    const int height = static_cast<int>(dispY);
    if (width <= 0 || height <= 0) {
        dbg.reason = "no display size";
        return;
    }

    Matrix4x4 viewProjection{};
    if (!RobRunReader::ResolveViewProjection(viewProjection, width, height)) {
        dbg.reason = "no view projection";
        stopAiming();
        return;
    }

    // Solve angles from the same camera the projection above was built with.
    // snapshot->camera trails the render thread by up to one worker cycle, so
    // using it here biased yaw/pitch by the camera's motion since the snapshot.
    RobRunCamera::CameraState freshCamera{};
    const bool haveFreshCamera = RobRunReader::GetFreshCamera(freshCamera) &&
        freshCamera.valid;

    const float centerX = dispX * 0.5f;
    const float centerY = dispY * 0.5f;
    const float fovDeg = std::clamp(static_cast<float>(cfg.Fov), 5.f, 180.f);
    // Screen-space cone, exactly like the FOV circle and the ESP projection.
    // The previous angular dot-product gate used the worker's camera sample
    // while the projection used the fresher render-thread camera, so it could
    // reject every target while the ESP drew them on screen.
    const float verticalFov = haveFreshCamera
        ? freshCamera.verticalFov
        : (snapshot->camera.valid ? snapshot->camera.verticalFov : 60.f);
    const float radiusPixels = ConeRadiusPixels(viewProjection, fovDeg, verticalFov, dispY);

    bool found = false;
    int bestId = -1;
    Vector3 bestPoint{};
    Vector2 bestScreen{};
    float bestScore = 0.f;
    int bestLeadMs = 0;
    bool lastValid = false;
    Vector3 lastPoint{};
    Vector2 lastScreen{};
    int lastLeadMs = 0;
    // If the local transform could not be resolved the origin would reject
    // every target on distance, so only apply the range filter when we have a
    // real local position.
    const bool haveLocalPosition = !snapshot->localPosition.IsZero();
    const std::uint64_t nowMs = GetTickCount64();
    const float responseMs = ResponseForSmoothness(cfg.Smoothness);

    // Bounded sample table: entity ids churn across matches, so drop stale
    // history rather than letting the map grow for the whole session.
    if (g_motion.size() > 512) g_motion.clear();

    for (const auto& ent : snapshot->entities) {
        if (ent.entityId == snapshot->localEntityId) continue;
        if (ent.isDead) continue;
        if (cfg.SkipKnocked && (ent.isKnockedDown || ent.isKnockedOut)) continue;
        if (ShouldSkipByRole(ent, cfg)) continue;
        // Visibility and wall occlusion gate
        if (cfg.VisibleBonesOnly && !ent.visible) continue;
        if (haveLocalPosition &&
            Distance(ent.position, snapshot->localPosition) > cfg.MaxDistance) continue;

        int activeBone = cfg.TargetBone;
        if (cfg.DynamicBoneFallback) {
            // Drop to chest (bone 2) if low health or head occluded
            if (ent.healthKnown && ent.health <= 40.f) {
                activeBone = 2;
            } else if (RobRunReader::GetBvhStatus().loaded && RobRunReader::IsCameraOccluded(BonePoint(ent, activeBone))) {
                activeBone = 2;
            }
        }

        Vector3 point = BonePoint(ent, activeBone);
        if (cfg.VisibleBonesOnly && RobRunReader::GetBvhStatus().loaded) {
            if (RobRunReader::IsCameraOccluded(point)) {
                bool altFound = false;
                static const int altBones[] = { 0, 1, 2, 3 }; // Head, Neck, Chest, Pelvis
                for (int b : altBones) {
                    if (b == activeBone) continue;
                    Vector3 altPoint = BonePoint(ent, b);
                    if (!RobRunReader::IsCameraOccluded(altPoint)) {
                        point = altPoint;
                        altFound = true;
                        break;
                    }
                }
                if (!altFound) continue;
            }
        }
        int leadMs = 0;

        // Leading uses the previous feet sample. Update the sample after the
        // prediction step so the next tick differentiates against this one.
        MotionSample& sample = g_motion[ent.entityId];
        if (cfg.Prediction && sample.valid && sample.tickMs != 0) {
            const std::uint64_t sampleMs = nowMs - sample.tickMs;
            if (sampleMs >= 4 && sampleMs <= 100) {
                Vector3 lead;
                if (ApplyLeading(point, ent.position, sample.position,
                        sampleMs, responseMs, cfg.PredictionStrength,
                        cfg.PredictionYScale, lead, leadMs)) {
                    point = lead;
                }
            }
        }
        sample.position = ent.position;
        sample.tickMs = nowMs;
        sample.valid = true;

        Vector2 screen{};
        if (!RobRunReader::ProjectWorld(viewProjection, point, width, height, screen))
            continue;

        if (ent.entityId == g_lastTarget) {
            lastValid = true;
            lastScreen = screen;
            lastPoint = point;
            lastLeadMs = leadMs;
        }

        const float dx = screen.x - centerX;
        const float dy = screen.y - centerY;
        const float distance = std::hypot(dx, dy);

        // Hard lock: the sticky target keeps its pick even when it leaves the
        // FOV ring, mirroring the reference's locked-target behaviour.
        const bool isLocked = cfg.HardLock && ent.entityId == g_lastTarget;
        if (!isLocked && distance > radiusPixels) continue;
        ++dbg.candidates;

        if (isLocked) {
            found = true;
            bestId = ent.entityId;
            bestScreen = screen;
            bestPoint = point;
            bestLeadMs = leadMs;
            break;
        }

        float score = distance;
        if (cfg.TargetPriority == 1) {
            // 3D Proximity
            score = haveLocalPosition ? Distance(ent.position, snapshot->localPosition) : distance;
        } else if (cfg.TargetPriority == 2) {
            // Lowest Health finish priority
            score = (ent.healthKnown && ent.health > 0.f) ? ent.health : (10000.f + distance);
        }

        if (ent.entityId == g_lastTarget) score *= 0.75f;
        if (!found || score < bestScore) {
            found = true;
            bestId = ent.entityId;
            bestScreen = screen;
            bestPoint = point;
            bestScore = score;
            bestLeadMs = leadMs;
        }
    }

    if (!found) {
        dbg.reason = "no target in FOV";
        stopAiming();
        resetSmoothing();
        g_lastTarget = -1;
        return;
    }
    if (bestId != g_lastTarget && g_lastTarget >= 0 && cfg.TargetSwitchDelay > 0 &&
        lastValid &&
        (nowMs - g_lastTargetSwitchMs) < static_cast<std::uint64_t>(cfg.TargetSwitchDelay)) {
        bestId = g_lastTarget;
        bestScreen = lastScreen;
        bestPoint = lastPoint;
        bestLeadMs = lastLeadMs;
    }
    if (bestId != g_lastTarget)
        g_lastTargetSwitchMs = nowMs;
    g_lastTarget = bestId;
    dbg.target = bestId;
    dbg.leadMs = bestLeadMs;

    const float dx = bestScreen.x - centerX;
    const float dy = bestScreen.y - centerY;
    dbg.dx = dx;
    dbg.dy = dy;

    // Target angles are measured from the camera, not the local player: the
    // camera is what actually has to turn.
    const Vector3 origin = haveFreshCamera
        ? freshCamera.position
        : (snapshot->camera.valid ? snapshot->camera.position : snapshot->localPosition);
    if (origin.IsZero()) {
        dbg.reason = "no camera origin";
        stopAiming();
        return;
    }
    dbg.distance = Distance(origin, bestPoint);

    const Vector3 dir = Vector3(
        bestPoint.x - origin.x,
        bestPoint.y - origin.y,
        bestPoint.z - origin.z);
    const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (length < 1.0e-4f) {
        dbg.reason = "zero aim vector";
        return;
    }
    const float horizontal = std::hypot(dir.x, dir.z);
    const float targetYaw = std::atan2(dir.x, dir.z);
    const float targetPitch = std::atan2(-dir.y, horizontal);

    // -------- Memory Aim: write camera angles over DMA -------------------
    if (cfg.AimType == 0 && cfg.MemoryWrite) {
        stopAiming();
        dbg.writeMode = true;

        const std::uintptr_t cameraModel = snapshot->cameraModel;
        const std::uintptr_t battleModel = snapshot->battleModel;
        const std::uint64_t now = GetTickCount64();
        // Never write from a stale snapshot, and never while the roots are
        // missing or the reader has just reported a new scene. The write path
        // re-checks BattleModel -> camera itself, but gating here stops the
        // 16 ms stream during the teardown/rebuild window entirely.
        const bool freshSnapshot =
            snapshot->stampMs != 0 && now - snapshot->stampMs <= 500;
        const bool validRoots = cameraModel > 0x10000 &&
            battleModel > 0x10000 && snapshot->localCharacter > 0x10000;
        if (!validRoots) {
            g_writeHealthy = false;
            g_lastVerdict = "waiting for session";
        } else if (!freshSnapshot) {
            g_writeHealthy = false;
            g_lastVerdict = "stale snapshot";
        } else if (now < g_writeBlockedUntil) {
            g_writeHealthy = false;
            g_lastVerdict = "level transition";
        } else if (g_lastWriteTick == 0 || now - g_lastWriteTick >= kWriteIntervalMs) {
            const float dtMs = g_lastWriteTick == 0
                ? 16.f
                : std::clamp(static_cast<float>(now - g_lastWriteTick), 0.5f, 24.f);
            g_lastWriteTick = now;

            const float gain = 1.f - std::exp(-dtMs / responseMs);
            const float verticalGain = 1.f - std::exp(-dtMs / (responseMs * 0.8f));

            if (!g_smoothInit || g_smoothTarget != bestId) {
                g_curYaw = targetYaw;
                g_curPitch = targetPitch;
                g_carryYaw = g_carryPitch = 0.f;
                g_smoothInit = true;
                g_smoothTarget = bestId;
            } else {
                float dYaw = targetYaw - g_curYaw;
                while (dYaw > kPi) dYaw -= 2.f * kPi;
                while (dYaw < -kPi) dYaw += 2.f * kPi;
                const float dPitch = targetPitch - g_curPitch;

                if (dYaw * g_carryYaw < 0.f) g_carryYaw = 0.f;
                if (dPitch * g_carryPitch < 0.f) g_carryPitch = 0.f;

                const float stepYaw = dYaw * gain + g_carryYaw;
                const float stepPitch = dPitch * verticalGain + g_carryPitch;
                g_carryYaw = stepYaw - std::trunc(stepYaw * 1.0e4f) / 1.0e4f;
                g_carryPitch = stepPitch - std::trunc(stepPitch * 1.0e4f) / 1.0e4f;
                g_curYaw += stepYaw;
                g_curPitch += stepPitch;
            }

            WriteOutcome outcome;
            Vector3 liveForward{};
            g_writeHealthy = WriteCameraAngles(battleModel, cameraModel, g_curYaw,
                g_curPitch, outcome, &liveForward,
                snapshot->lookDirectionModel, snapshot->lookDirDirect,
                snapshot->playerRotationInput);
            dbg.tgtYawDeg = g_curYaw * 180.f / kPi;
            dbg.tgtPitchDeg = g_curPitch * 180.f / kPi;

            // Verdict on the previous write using the forward the scatter
            // validation read fetched (before the new write replaced it).
            if (g_haveWrittenForward && Finite(liveForward))
                g_lastVerdict = CompareToWritten(g_writtenForward, liveForward);

            if (!g_writeHealthy) {
                g_lastVerdict = "write failed";
            } else {
                g_writtenForward = EulerToForward(g_curYaw, g_curPitch);
                g_haveWrittenForward = true;
                if (g_lastVerdict[0] == '-' || g_lastVerdict[0] == '\0')
                    g_lastVerdict = "writing";
            }
        }

        dbg.writeOk = g_writeHealthy;
        dbg.writeState = g_lastVerdict;

        if (g_writeHealthy) {
            dbg.reason = "writing angles (memory)";
            return;
        }

        if (!cfg.CursorFallback) {
            dbg.reason = "memory write unavailable";
            return;
        }
        // Fall through to the cursor path below.
    }

    if (std::hypot(dx, dy) < 0.5f) {
        dbg.reason = "on target";
        return;
    }

    static auto s_lastTickTime = std::chrono::steady_clock::now();
    const auto nowTime = std::chrono::steady_clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(nowTime - s_lastTickTime).count(), 0.001f, 0.05f);
    s_lastTickTime = nowTime;

    Base::AimMotion::Settings settings;
    settings.smoothX = settings.smoothY = std::clamp(cfg.Smoothness, 1, 100);
    settings.countsPerPixelX = settings.countsPerPixelY = 1.f;
    settings.deadzonePixels = 0.5f;

    if (cfg.AimType == 0) {
        // Cursor fallback: closed-loop relative movement on this PC. Each frame
        // re-reads the target so the gain does not need to match the game's
        // sensitivity exactly. This only reaches a game running on the same PC;
        // a two-PC DMA setup needs Hardware Aim or the angle write above.
        stopAiming();
        const auto step = Base::AimMotion::Step({ dx, dy }, settings, dt);
        const int mx = static_cast<int>(std::lround(step.x));
        const int my = static_cast<int>(std::lround(step.y));
        dbg.stepX = mx;
        dbg.stepY = my;
        if (mx != 0 || my != 0) {
            SendRelative(mx, my);
            dbg.reason = "moving (cursor)";
        } else {
            dbg.reason = "inside deadzone";
        }
        return;
    }

    // Hardware Aim: the selected device smooths and quantizes the same pixel
    // error, then writes counts to the game PC. The response uses the same
    // millisecond curve as the memory write path; the old slider-only curve
    // was several times slower at high Smoothness, which made the device feel
    // unresponsive. The gain compensates for in-game sensitivity (one count is
    // not necessarily one screen pixel) and the higher cap lets flicks reach
    // the device's 127-count report limit instead of being clamped early.
    if (!Base::Hardware::Ready()) {
        dbg.reason = "hardware device not ready";
        stopAiming();
        return;
    }

    settings.responseMs = ResponseForSmoothness(cfg.Smoothness);
    settings.countsPerPixelX = settings.countsPerPixelY =
        std::clamp(cfg.HardwareGain, 0.25f, 5.f);
    settings.maxCountsPerSecond = 24000.f;

    Base::Hardware::MotionIntent intent;
    intent.value = { dx, dy };
    intent.settings = settings;
    intent.target = static_cast<std::uint64_t>(bestId);
    intent.observation = ++g_observation;
    intent.error = true;
    intent.intervalMs = 6;
    intent.expires = Base::Hardware::Clock::now() + std::chrono::milliseconds(50);
    if (Base::Hardware::Session().Submit(Base::Hardware::Source::Aim, intent)) {
        g_wasAiming = true;
        dbg.reason = "moving (hardware)";
    } else {
        dbg.reason = "hardware submit rejected";
    }
}

static std::uintptr_t g_cachedPlayerClient = 0;
static std::uint64_t g_lastRecoilGen = 0;

// ---- No Recoil: per-shot kick budget (dedicated worker) -------------------
static std::atomic<bool> g_recoilStop{true};
static std::thread g_recoilThread;

struct ShotBudget {
    float budget;
    float initialBudget;
    std::chrono::steady_clock::time_point shotTime;
};

static constexpr int kMaxBudgets = 8;
static constexpr auto kShotWindow = std::chrono::microseconds(70000);
static constexpr float kKickNoise = 1e-5f;
static constexpr float kInitKick = 0.01745f;  // 1 deg
static constexpr float kMinKick  = 0.00524f;  // 0.3 deg
static constexpr float kMaxKick  = 0.03491f;  // 2.0 deg

static std::uint64_t g_rcSessionGen = 0;
static std::uintptr_t g_rcClient = 0;
static std::uint64_t g_rcLastClientCheck = 0;
static int g_rcLastShotIndex = -1;
static bool g_rcWasFiring = false;
static float g_rcLastPitch = 0.f;
static bool g_rcPitchValid = false;
static ShotBudget g_rcBudgets[kMaxBudgets];
static int g_rcBudgetCount = 0;
static float g_rcKickEstimate = kInitKick;
static float g_rcBurstCancelled = 0.f;
static int g_rcBurstShots = 0;
static int g_rcBurstWrites = 0;

static void ExpireBudgets()
{
    const auto now = std::chrono::steady_clock::now();
    while (g_rcBudgetCount > 0) {
        auto& oldest = g_rcBudgets[0];
        if (now - oldest.shotTime < kShotWindow) break;
        const float used = oldest.initialBudget - oldest.budget;
        if (oldest.budget <= 0.f)
            g_rcKickEstimate *= 1.10f;
        else
            g_rcKickEstimate += 0.25f * (used - g_rcKickEstimate);
        g_rcKickEstimate = std::clamp(g_rcKickEstimate, kMinKick, kMaxKick);
        for (int i = 1; i < g_rcBudgetCount; i++)
            g_rcBudgets[i - 1] = g_rcBudgets[i];
        g_rcBudgetCount--;
    }
}

static void PushBudget()
{
    if (g_rcBudgetCount >= kMaxBudgets) {
        for (int i = 1; i < g_rcBudgetCount; i++)
            g_rcBudgets[i - 1] = g_rcBudgets[i];
        g_rcBudgetCount--;
    }
    const float b = g_rcKickEstimate * 1.15f;
    g_rcBudgets[g_rcBudgetCount++] = {b, b, std::chrono::steady_clock::now()};
}

static void TickRecoil()
{
    if (!ConfigInstance.Aim.NoRecoil) {
        g_rcPitchValid = false;
        return;
    }

    const auto snapshot = RobRunReader::GetSnapshot();
    if (!snapshot || snapshot->battleModel <= 0x10000) return;

    if (snapshot->sessionGeneration != g_rcSessionGen) {
        g_rcClient = 0;
        g_rcSessionGen = snapshot->sessionGeneration;
        g_rcPitchValid = false;
        g_rcLastShotIndex = -1;
        g_rcBudgetCount = 0;
        g_rcKickEstimate = kInitKick;
        g_rcWasFiring = false;
        g_rcBurstCancelled = 0.f;
        g_rcBurstShots = 0;
        g_rcBurstWrites = 0;
    }

    const std::uint64_t now = GetTickCount64();
    if (g_rcClient <= 0x10000 || now - g_rcLastClientCheck > 500) {
        g_rcLastClientCheck = now;
        const auto battleEnts = RobRunMemory::ReadPtr(
            snapshot->battleModel + rr::BattleModel::BattleEntitiesModel);
        if (battleEnts <= 0x10000) return;
        const auto localPlayer = RobRunMemory::ReadPtr(
            battleEnts + rr::BattleEntitiesModel::LocalPlayerEntityModel);
        if (localPlayer <= 0x10000) return;
        g_rcClient = RobRunMemory::ReadPtr(
            localPlayer + rr::PlayerEntityModel::Client);
        if (g_rcClient <= 0x10000) return;
    }

    const std::uintptr_t rotInput = snapshot->playerRotationInput;
    if (rotInput <= 0x10000 || g_rcClient <= 0x10000) return;

    const std::uintptr_t fireModel = RobRunMemory::ReadPtr(
        g_rcClient + rr::CharacterClientModel::FireModel);
    if (fireModel <= 0x10000) return;

    // Uncached reads — do not use ReadBytes (cached, up to 600ms stale)
    struct { std::uint8_t isFiring; std::uint8_t hasShot; std::uint8_t pad[2]; std::int32_t shotIndex; } fire{};
    float pitch = 0.f;
    if (!RobRunMemory::ReadBytesFresh(
            fireModel + rr::CharacterFireModel::IsFiring, &fire, sizeof(fire)))
        return;
    if (!RobRunMemory::ReadBytesFresh(
            rotInput + rr::PlayerRotationInputModel::Pitch, &pitch, sizeof(pitch)))
        return;

    if (!std::isfinite(pitch) || std::fabs(pitch) > 1.75f) {
        g_rcPitchValid = false;
        return;
    }

    if (!g_rcPitchValid) {
        g_rcLastPitch = pitch;
        g_rcLastShotIndex = fire.shotIndex;
        g_rcPitchValid = true;
        g_rcWasFiring = fire.isFiring != 0;
        return;
    }

    ExpireBudgets();

    // Detect new shot: ShotIndex changed and either increased, or weapon is
    // firing/has-shot (a reset to -1 after firing is not a shot).
    if (fire.shotIndex != g_rcLastShotIndex) {
        const bool isNewShot =
            fire.shotIndex > g_rcLastShotIndex ||
            (fire.shotIndex >= 0 && (fire.isFiring || fire.hasShot));
        if (isNewShot) {
            PushBudget();
            g_rcBurstShots++;
        }
        g_rcLastShotIndex = fire.shotIndex;
    }

    // Cancel kick: negative pitch delta (view goes up) during active budget
    const float delta = pitch - g_rcLastPitch;
    if (delta < -kKickNoise && g_rcBudgetCount > 0) {
        float toCancel = -delta;
        float corrected = 0.f;
        for (int i = 0; i < g_rcBudgetCount && toCancel > 0.f; i++) {
            const float take = (std::min)(toCancel, g_rcBudgets[i].budget);
            g_rcBudgets[i].budget -= take;
            toCancel -= take;
            corrected += take;
        }
        if (corrected > 0.f) {
            const float newPitch = pitch + corrected;
            TargetProcess.Write(
                rotInput + rr::PlayerRotationInputModel::Pitch, newPitch);
            g_rcLastPitch = newPitch;
            g_rcBurstCancelled += corrected;
            g_rcBurstWrites++;
        } else {
            g_rcLastPitch = pitch;
        }
    } else {
        g_rcLastPitch = pitch;
    }

    // Burst diagnostics (once firing stops and all windows expire)
    const bool firing = fire.isFiring != 0;
    if (!firing && g_rcWasFiring && g_rcBudgetCount == 0 && g_rcBurstShots > 0) {
        std::printf("[NoRecoil] burst: %d shots, kick cancelled %.3f deg, "
                    "%d writes, kick/shot %.3f deg\n",
            g_rcBurstShots,
            g_rcBurstCancelled * 57.2957795f,
            g_rcBurstWrites,
            g_rcKickEstimate * 57.2957795f);
        g_rcBurstCancelled = 0.f;
        g_rcBurstShots = 0;
        g_rcBurstWrites = 0;
    }
    g_rcWasFiring = firing;
}

void StartRecoilWorker()
{
    StopRecoilWorker();
    g_recoilStop.store(false, std::memory_order_relaxed);
    g_recoilThread = std::thread([] {
        while (!g_recoilStop.load(std::memory_order_relaxed)) {
            try { TickRecoil(); } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}

void StopRecoilWorker()
{
    g_recoilStop.store(true, std::memory_order_relaxed);
    if (g_recoilThread.joinable())
        g_recoilThread.join();
}

void TickRecoilSpread()
{
    const auto& cfg = ConfigInstance.Aim;
    if (!cfg.NoSpread && !cfg.DynamicRecoilCompensation)
        return;

    const auto snapshot = RobRunReader::GetSnapshot();
    if (!snapshot || snapshot->battleModel <= 0x10000) return;

    if (snapshot->sessionGeneration != g_lastRecoilGen) {
        g_cachedPlayerClient = 0;
        g_lastRecoilGen = snapshot->sessionGeneration;
    }

    // Revalidate client pointer every 500ms (respawn can free old one)
    static std::uint64_t g_lastClientCheck = 0;
    const std::uint64_t now = GetTickCount64();
    if (g_cachedPlayerClient <= 0x10000 || now - g_lastClientCheck > 500) {
        g_lastClientCheck = now;
        const std::uintptr_t battleEnts = RobRunMemory::ReadPtr(
            snapshot->battleModel + rr::BattleModel::BattleEntitiesModel);
        if (battleEnts <= 0x10000) return;
        const std::uintptr_t localPlayer = RobRunMemory::ReadPtr(
            battleEnts + rr::BattleEntitiesModel::LocalPlayerEntityModel);
        if (localPlayer <= 0x10000) return;
        g_cachedPlayerClient = RobRunMemory::ReadPtr(
            localPlayer + rr::PlayerEntityModel::Client);
        if (g_cachedPlayerClient <= 0x10000) return;
    }

    // --- Dynamic Recoil: scale RecoilOffset (Vector2, 8 bytes) ---
    if (cfg.DynamicRecoilCompensation) {
        const std::uintptr_t recoilModel = RobRunMemory::ReadPtr(
            g_cachedPlayerClient + rr::PlayerClient::Recoil);
        if (recoilModel > 0x10000) {
            struct { float x, y; } cur{};
            if (RobRunMemory::ReadBytes(recoilModel + rr::RecoilModel::OffsetVec, &cur, sizeof(cur))) {
                const float factor = std::clamp(1.f - cfg.RecoilStrength, 0.f, 1.f);
                cur.x *= factor;
                cur.y *= factor;
                TargetProcess.Write(recoilModel + rr::RecoilModel::OffsetVec,
                    reinterpret_cast<void*>(&cur), sizeof(cur));
            }
        }
    }

    // --- No Spread: zero per-frame outputs (limited effectiveness) ---
    if (cfg.NoSpread) {
        Memory::ScopedScatter scatter(TargetProcess);
        if (scatter.handle) {
            const std::uintptr_t spreadModel = RobRunMemory::ReadPtr(
                g_cachedPlayerClient + rr::PlayerClient::FakeSpread);
            if (spreadModel > 0x10000) {
                float zero = 0.f;
                TargetProcess.AddScatterWriteRequest(scatter,
                    spreadModel + rr::FakeSpreadModel::Spread, &zero, sizeof(zero));
                TargetProcess.AddScatterWriteRequest(scatter,
                    spreadModel + rr::FakeSpreadModel::Air, &zero, sizeof(zero));
                TargetProcess.AddScatterWriteRequest(scatter,
                    spreadModel + rr::FakeSpreadModel::Move, &zero, sizeof(zero));
            }
            TargetProcess.ExecuteScatterWrite(scatter);
        }
    }
}

void Draw()
{
    const auto& cfg = ConfigInstance.Aim;
    if (!cfg.Enabled || !cfg.ShowFov) return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.f || display.y <= 0.f) return;

    float verticalFov = 60.f;
    RobRunCamera::CameraState freshCamera{};
    if (RobRunReader::GetFreshCamera(freshCamera) && freshCamera.valid)
        verticalFov = freshCamera.verticalFov;
    else {
        const auto snapshot = RobRunReader::GetSnapshot();
        if (snapshot && snapshot->camera.valid) verticalFov = snapshot->camera.verticalFov;
    }

    Matrix4x4 viewProjection{};
    float radius;
    if (RobRunReader::ResolveViewProjection(viewProjection, (int)display.x, (int)display.y)) {
        radius = ConeRadiusPixels(viewProjection, static_cast<float>(cfg.Fov),
            verticalFov, display.y);
    } else {
        radius = FovRadiusPixels(static_cast<float>(cfg.Fov), verticalFov, display.y);
    }
    radius = (std::min)(radius, std::hypot(display.x * 0.5f, display.y * 0.5f));
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const ImVec2 center(display.x * 0.5f, display.y * 0.5f);
    drawList->AddCircle(center, radius, IM_COL32(30, 120, 255, 110), 96, 1.5f);
}

void DrawDebug()
{
    if (!ConfigInstance.Aim.ShowDebug) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.f || display.y <= 0.f) return;

    const DebugInfo d = GetDebug();
    char lines[8][160];
    std::snprintf(lines[0], sizeof(lines[0]), "Aim: %s", d.enabled ? "on" : "off");
    std::snprintf(lines[1], sizeof(lines[1]), "Menu: %s  Key: %s (%s)",
        d.menuOpen ? "open" : "closed", d.held ? "held" : "up", d.keySource);
    std::snprintf(lines[2], sizeof(lines[2]), "Entities: %d  Candidates: %d  Target: %d",
        d.entities, d.candidates, d.target);
    std::snprintf(lines[3], sizeof(lines[3]), "Delta: %.1f, %.1f  Step: %d, %d  Lead: %dms",
        d.dx, d.dy, d.stepX, d.stepY, d.leadMs);
    std::snprintf(lines[4], sizeof(lines[4]), "Type: %s  Device: %s",
        d.aimType == 0 ? "memory" : "hardware", d.deviceReady ? "ready" : "not ready");
    std::snprintf(lines[5], sizeof(lines[5]), "Write: %s  %s  yaw %.1f pitch %.1f dist %.0fm",
        d.writeMode ? (d.writeOk ? "ok" : "fail") : "off",
        d.writeState, d.tgtYawDeg, d.tgtPitchDeg, d.distance);
    std::snprintf(lines[6], sizeof(lines[6]), "Reason: %s", d.reason);
    {
        const auto cam = RobRunReader::GetCameraDebug();
        const float angle = cam.forwardDot > -1.f
            ? 57.2957795f * std::acos(std::clamp(cam.forwardDot, -1.f, 1.f))
            : -1.f;
        char pose[32] = "pose=--";
        if (cam.snapshotAgeMs >= 0)
            std::snprintf(pose, sizeof(pose), "pose=%lldms",
                static_cast<long long>(cam.snapshotAgeMs));
        if (cam.nativeValid)
            std::snprintf(lines[7], sizeof(lines[7]),
                "ESP cam: native d=%.2fm a=%.1f  %s", cam.positionError, angle,
                pose);
        else
            std::snprintf(lines[7], sizeof(lines[7]),
                "ESP cam: %s (native %s)  %s",
                RobRunReader::GetViewProjectionSource(),
                cam.nativeEnabled ? "not found" : "off", pose);
    }

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 origin(12.f, 12.f);
    for (int i = 0; i < 8; ++i) {
        dl->AddText(ImVec2(origin.x + 1.f, origin.y + i * 15.f + 1.f),
            IM_COL32(0, 0, 0, 200), lines[i]);
        dl->AddText(ImVec2(origin.x, origin.y + i * 15.f),
            IM_COL32(120, 200, 255, 235), lines[i]);
    }
}

} // namespace Aim
