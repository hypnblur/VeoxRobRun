#include "pch.h"
#include "AimController.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
#include "DeviceSession.h"
#include "InputManager.h"
#include "Overlay.h"
#include "imgui.h"
#include <cmath>

namespace Aim {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Keep in sync with the labels below. 0 means "always active".
const int kKeys[] = {
    VK_RBUTTON, VK_LBUTTON, VK_LMENU, VK_LSHIFT, VK_XBUTTON1, VK_XBUTTON2, 0
};
const char* const kKeyNames[] = {
    "Right Mouse", "Left Mouse", "Left Alt", "Left Shift", "Mouse 4", "Mouse 5", "Always"
};
constexpr int kKeyCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));

std::uint64_t g_observation = 0;
int g_lastTarget = -1;
bool g_wasAiming = false;

bool Held(int keyIndex)
{
    if (keyIndex < 0 || keyIndex >= kKeyCount) keyIndex = 0;
    const int vk = kKeys[keyIndex];
    if (vk == 0) return true;
    return Keyboard::IsKeyDown(static_cast<std::uint32_t>(vk));
}

float Distance(const Vector3& a, const Vector3& b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Vector3 BonePoint(const RobRunReader::EntityData& ent, int bone)
{
    const auto& points = ent.bonePoints;
    if (points.valid) {
        switch (bone) {
        case 0: return Vector3(points.head.x, points.head.y, points.head.z);
        case 1: return Vector3(points.neck.x, points.neck.y, points.neck.z);
        case 2: return Vector3(points.chest.x, points.chest.y, points.chest.z);
        case 3: return Vector3(points.pelvis.x, points.pelvis.y, points.pelvis.z);
        default: break;
        }
    }
    // Fallback when the rig could not be sampled: body-relative heights above
    // the transform root (which sits at the feet).
    static const float heights[] = { 1.62f, 1.45f, 1.25f, 0.95f };
    const int index = (bone >= 0 && bone < 4) ? bone : 0;
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

} // namespace

const char* const* KeyNames() { return kKeyNames; }
int KeyCount() { return kKeyCount; }

void Tick()
{
    const auto& cfg = ConfigInstance.Aim;
    if (!cfg.Enabled || Overlay::MenuOpen()) {
        if (g_wasAiming) {
            Base::Hardware::Session().Cancel(Base::Hardware::Source::Aim);
            g_wasAiming = false;
        }
        g_lastTarget = -1;
        return;
    }

    if (!Held(cfg.AimKey)) {
        if (g_wasAiming) {
            Base::Hardware::Session().Cancel(Base::Hardware::Source::Aim);
            g_wasAiming = false;
        }
        return;
    }

    const auto snapshot = RobRunReader::GetSnapshot();
    if (!snapshot || snapshot->entities.empty()) return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const int width = static_cast<int>(display.x);
    const int height = static_cast<int>(display.y);
    if (width <= 0 || height <= 0) return;

    Matrix4x4 viewProjection{};
    if (!RobRunReader::GetViewProjection(viewProjection, width, height) &&
        !RobRunReader::GetFallbackViewProjection(viewProjection))
        return;

    const float centerX = display.x * 0.5f;
    const float centerY = display.y * 0.5f;
    const float fovDeg = std::clamp(static_cast<float>(cfg.Fov), 5.f, 180.f);
    const float halfFov = fovDeg * 0.5f * kPi / 180.f;
    const bool angularFov = snapshot->camera.valid;
    const Vector3 cameraPosition = snapshot->camera.position;
    const Vector3 cameraForward = snapshot->camera.forward;
    const float radiusPixels = FovRadiusPixels(fovDeg, snapshot->camera.verticalFov,
        display.y);

    bool found = false;
    int bestId = -1;
    Vector2 bestScreen{};
    float bestScore = 0.f;

    for (const auto& ent : snapshot->entities) {
        if (ent.entityId == snapshot->localEntityId) continue;
        if (ent.isDead) continue;
        if (Distance(ent.position, snapshot->localPosition) > cfg.MaxDistance) continue;

        const Vector3 point = BonePoint(ent, cfg.TargetBone);
        Vector2 screen{};
        if (!RobRunReader::ProjectWorld(viewProjection, point, width, height, screen))
            continue;

        if (angularFov) {
            const Vector3 d(point.x - cameraPosition.x, point.y - cameraPosition.y,
                point.z - cameraPosition.z);
            const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (length < 1e-3f) continue;
            float dot = (d.x * cameraForward.x + d.y * cameraForward.y
                + d.z * cameraForward.z) / length;
            dot = std::clamp(dot, -1.f, 1.f);
            if (std::acos(dot) > halfFov) continue;
        } else {
            const float dx = screen.x - centerX;
            const float dy = screen.y - centerY;
            if (std::hypot(dx, dy) > radiusPixels) continue;
        }

        float score = std::hypot(screen.x - centerX, screen.y - centerY);
        if (ent.entityId == g_lastTarget) score *= 0.75f;
        if (!found || score < bestScore) {
            found = true;
            bestId = ent.entityId;
            bestScreen = screen;
            bestScore = score;
        }
    }

    if (!found) {
        if (g_wasAiming) {
            Base::Hardware::Session().Cancel(Base::Hardware::Source::Aim);
            g_wasAiming = false;
        }
        g_lastTarget = -1;
        return;
    }
    g_lastTarget = bestId;

    const float dx = bestScreen.x - centerX;
    const float dy = bestScreen.y - centerY;
    if (std::hypot(dx, dy) < 0.5f) return;

    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.001f, 0.05f);

    Base::AimMotion::Settings settings;
    settings.smoothX = settings.smoothY = std::clamp(cfg.Smoothness, 1, 100);
    settings.countsPerPixelX = settings.countsPerPixelY = 1.f;
    settings.deadzonePixels = 0.5f;

    if (cfg.AimType == 0) {
        // Memory Aim: closed-loop relative cursor movement on this PC. Each
        // frame re-reads the target so the gain does not need to match the
        // game's sensitivity exactly.
        if (g_wasAiming) {
            Base::Hardware::Session().Cancel(Base::Hardware::Source::Aim);
            g_wasAiming = false;
        }
        const auto step = Base::AimMotion::Step({ dx, dy }, settings, dt);
        const int mx = static_cast<int>(std::lround(step.x));
        const int my = static_cast<int>(std::lround(step.y));
        if (mx != 0 || my != 0) SendRelative(mx, my);
        return;
    }

    // Hardware Aim: the selected device smooths and quantizes the same pixel
    // error, then writes counts to the game PC.
    if (!Base::Hardware::Ready()) {
        if (g_wasAiming) {
            Base::Hardware::Session().Cancel(Base::Hardware::Source::Aim);
            g_wasAiming = false;
        }
        return;
    }

    Base::Hardware::MotionIntent intent;
    intent.value = { dx, dy };
    intent.settings = settings;
    intent.target = static_cast<std::uint64_t>(bestId);
    intent.observation = ++g_observation;
    intent.error = true;
    intent.intervalMs = 8;
    intent.expires = Base::Hardware::Clock::now() + std::chrono::milliseconds(50);
    if (Base::Hardware::Session().Submit(Base::Hardware::Source::Aim, intent))
        g_wasAiming = true;
}

void Draw()
{
    const auto& cfg = ConfigInstance.Aim;
    if (!cfg.Enabled || !cfg.ShowFov) return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.f || display.y <= 0.f) return;

    float verticalFov = 60.f;
    const auto snapshot = RobRunReader::GetSnapshot();
    if (snapshot && snapshot->camera.valid) verticalFov = snapshot->camera.verticalFov;

    const float radius = (std::min)(
        FovRadiusPixels(static_cast<float>(cfg.Fov), verticalFov, display.y),
        std::hypot(display.x * 0.5f, display.y * 0.5f));
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const ImVec2 center(display.x * 0.5f, display.y * 0.5f);
    drawList->AddCircle(center, radius, IM_COL32(30, 120, 255, 110), 96, 1.5f);
}

} // namespace Aim
