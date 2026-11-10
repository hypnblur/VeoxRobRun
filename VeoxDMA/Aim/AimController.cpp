#include "pch.h"
#include "AimController.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
#include "DeviceSession.h"
#include "InputManager.h"
#include "Overlay.h"
#include "imgui.h"
#include <cmath>
#include <cstdio>

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
DebugInfo g_debug{};

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

    if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
        if (source) *source = "host keyboard";
        return true;
    }
    return false;
}

bool Finite(const Vector3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
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
    static const float heights[] = { 1.62f, 1.45f, 1.25f, 0.95f };
    const int index = (bone >= 0 && bone < 4) ? bone : 0;

    const auto& points = ent.bonePoints;
    if (points.valid) {
        Vector3 point{};
        bool have = false;
        switch (bone) {
        case 0: point = Vector3(points.head.x, points.head.y, points.head.z); have = true; break;
        case 1: point = Vector3(points.neck.x, points.neck.y, points.neck.z); have = true; break;
        case 2: point = Vector3(points.chest.x, points.chest.y, points.chest.z); have = true; break;
        case 3: point = Vector3(points.pelvis.x, points.pelvis.y, points.pelvis.z); have = true; break;
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

} // namespace

const char* const* KeyNames() { return kKeyNames; }
int KeyCount() { return kKeyCount; }
DebugInfo GetDebug() { return g_debug; }

void Tick()
{
    const auto& cfg = ConfigInstance.Aim;
    g_debug = DebugInfo{};
    g_debug.enabled = cfg.Enabled;
    g_debug.menuOpen = Overlay::MenuOpen();
    g_debug.aimType = cfg.AimType;
    g_debug.deviceReady = Base::Hardware::Ready();

    const auto stopAiming = [] {
        if (g_wasAiming) {
            Base::Hardware::Session().Cancel(Base::Hardware::Source::Aim);
            g_wasAiming = false;
        }
    };

    if (!cfg.Enabled) {
        g_debug.reason = "aim disabled";
        stopAiming();
        g_lastTarget = -1;
        return;
    }
    if (Overlay::MenuOpen()) {
        g_debug.reason = "menu open";
        stopAiming();
        g_lastTarget = -1;
        return;
    }

    const char* keySource = "-";
    g_debug.held = Held(cfg.AimKey, &keySource);
    g_debug.keySource = keySource;
    if (!g_debug.held) {
        g_debug.reason = "aim key not held";
        stopAiming();
        return;
    }

    const auto snapshot = RobRunReader::GetSnapshot();
    if (!snapshot || snapshot->entities.empty()) {
        g_debug.reason = "no entities";
        stopAiming();
        return;
    }
    g_debug.entities = static_cast<int>(snapshot->entities.size());

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const int width = static_cast<int>(display.x);
    const int height = static_cast<int>(display.y);
    if (width <= 0 || height <= 0) {
        g_debug.reason = "no display size";
        return;
    }

    Matrix4x4 viewProjection{};
    if (!RobRunReader::GetViewProjection(viewProjection, width, height) &&
        !RobRunReader::GetFallbackViewProjection(viewProjection)) {
        g_debug.reason = "no view projection";
        stopAiming();
        return;
    }

    const float centerX = display.x * 0.5f;
    const float centerY = display.y * 0.5f;
    const float fovDeg = std::clamp(static_cast<float>(cfg.Fov), 5.f, 180.f);
    // Screen-space cone, exactly like the FOV circle and the ESP projection.
    // The previous angular dot-product gate used the worker's camera sample
    // while the projection used the fresher render-thread camera, so it could
    // reject every target while the ESP drew them on screen.
    const float verticalFov = snapshot->camera.valid
        ? snapshot->camera.verticalFov : 60.f;
    const float radiusPixels = ConeRadiusPixels(viewProjection, fovDeg, verticalFov, display.y);

    bool found = false;
    int bestId = -1;
    Vector2 bestScreen{};
    float bestScore = 0.f;
    // If the local transform could not be resolved the origin would reject
    // every target on distance, so only apply the range filter when we have a
    // real local position.
    const bool haveLocalPosition = !snapshot->localPosition.IsZero();

    for (const auto& ent : snapshot->entities) {
        if (ent.entityId == snapshot->localEntityId) continue;
        if (ent.isDead) continue;
        // Animator/cull gate: skip characters whose pose has frozen, which is
        // the closest read-only equivalent of "not visible". This is not wall
        // occlusion.
        if (cfg.VisibleBonesOnly && !ent.visible) continue;
        if (haveLocalPosition &&
            Distance(ent.position, snapshot->localPosition) > cfg.MaxDistance) continue;

        const Vector3 point = BonePoint(ent, cfg.TargetBone);
        Vector2 screen{};
        if (!RobRunReader::ProjectWorld(viewProjection, point, width, height, screen))
            continue;

        const float dx = screen.x - centerX;
        const float dy = screen.y - centerY;
        const float distance = std::hypot(dx, dy);
        if (distance > radiusPixels) continue;
        ++g_debug.candidates;

        float score = distance;
        if (ent.entityId == g_lastTarget) score *= 0.75f;
        if (!found || score < bestScore) {
            found = true;
            bestId = ent.entityId;
            bestScreen = screen;
            bestScore = score;
        }
    }

    if (!found) {
        g_debug.reason = "no target in FOV";
        stopAiming();
        g_lastTarget = -1;
        return;
    }
    g_lastTarget = bestId;
    g_debug.target = bestId;

    const float dx = bestScreen.x - centerX;
    const float dy = bestScreen.y - centerY;
    g_debug.dx = dx;
    g_debug.dy = dy;
    if (std::hypot(dx, dy) < 0.5f) {
        g_debug.reason = "on target";
        return;
    }

    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.001f, 0.05f);

    Base::AimMotion::Settings settings;
    settings.smoothX = settings.smoothY = std::clamp(cfg.Smoothness, 1, 100);
    settings.countsPerPixelX = settings.countsPerPixelY = 1.f;
    settings.deadzonePixels = 0.5f;

    if (cfg.AimType == 0) {
        // Memory Aim: closed-loop relative cursor movement on this PC. Each
        // frame re-reads the target so the gain does not need to match the
        // game's sensitivity exactly. This only reaches a game running on the
        // same PC; a two-PC DMA setup needs Hardware Aim.
        stopAiming();
        const auto step = Base::AimMotion::Step({ dx, dy }, settings, dt);
        const int mx = static_cast<int>(std::lround(step.x));
        const int my = static_cast<int>(std::lround(step.y));
        g_debug.stepX = mx;
        g_debug.stepY = my;
        if (mx != 0 || my != 0) {
            SendRelative(mx, my);
            g_debug.reason = "moving (memory)";
        } else {
            g_debug.reason = "inside deadzone";
        }
        return;
    }

    // Hardware Aim: the selected device smooths and quantizes the same pixel
    // error, then writes counts to the game PC.
    if (!Base::Hardware::Ready()) {
        g_debug.reason = "hardware device not ready";
        stopAiming();
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
    if (Base::Hardware::Session().Submit(Base::Hardware::Source::Aim, intent)) {
        g_wasAiming = true;
        g_debug.reason = "moving (hardware)";
    } else {
        g_debug.reason = "hardware submit rejected";
    }
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

    Matrix4x4 viewProjection{};
    float radius;
    if (RobRunReader::GetViewProjection(viewProjection, (int)display.x, (int)display.y) ||
        RobRunReader::GetFallbackViewProjection(viewProjection)) {
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
    std::snprintf(lines[3], sizeof(lines[3]), "Delta: %.1f, %.1f  Step: %d, %d",
        d.dx, d.dy, d.stepX, d.stepY);
    std::snprintf(lines[4], sizeof(lines[4]), "Type: %s  Device: %s",
        d.aimType == 0 ? "memory" : "hardware", d.deviceReady ? "ready" : "not ready");
    std::snprintf(lines[5], sizeof(lines[5]), "Reason: %s", d.reason);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 origin(12.f, 12.f);
    for (int i = 0; i < 6; ++i) {
        dl->AddText(ImVec2(origin.x + 1.f, origin.y + i * 15.f + 1.f),
            IM_COL32(0, 0, 0, 200), lines[i]);
        dl->AddText(ImVec2(origin.x, origin.y + i * 15.f),
            IM_COL32(120, 200, 255, 235), lines[i]);
    }
}

} // namespace Aim
