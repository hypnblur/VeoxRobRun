#include "pch.h"
#include "AimController.h"
#include "MotionFit.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
#include "FovChanger.h"
#include "RobRunSDK.h"
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "Memory.h"
#include "DeviceSession.h"
#include "CtlState.h"
#include "InputManager.h"
#include "Overlay.h"
#include "imgui.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <unordered_map>
#include <deque>

namespace Aim {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kAlwaysKeyIndex = 18;
constexpr int kStickDeflectKey = 0x1021;

// Keep indices 0..18 stable so saved configs keep their bindings.
// Gamepad keys start at index 19 using CTL_VK_* codes + kStickDeflectKey.
const int kKeys[] = {
    VK_RBUTTON,        // 0: Right Mouse
    VK_LBUTTON,        // 1: Left Mouse
    VK_LMENU,          // 2: Left Alt
    VK_LSHIFT,         // 3: Left Shift
    VK_XBUTTON1,       // 4: Mouse 4
    VK_XBUTTON2,       // 5: Mouse 5
    VK_MBUTTON,        // 6: Middle Mouse
    VK_LCONTROL,       // 7: Left Ctrl
    VK_CAPITAL,        // 8: Caps Lock
    VK_SPACE,          // 9: Space
    VK_TAB,            // 10: Tab
    'C',               // 11: C
    'V',               // 12: V
    'E',               // 13: E
    'F',               // 14: F
    'Q',               // 15: Q
    'X',               // 16: X
    'Z',               // 17: Z
    0,                 // 18: Always
    CTL_VK_LT,         // 19: Gamepad LT
    CTL_VK_RT,         // 20: Gamepad RT
    CTL_VK_LB,         // 21: Gamepad LB
    CTL_VK_RB,         // 22: Gamepad RB
    CTL_VK_L3,         // 23: Gamepad L3
    CTL_VK_R3,         // 24: Gamepad R3
    CTL_VK_A,          // 25: Gamepad A
    CTL_VK_B,          // 26: Gamepad B
    CTL_VK_X,          // 27: Gamepad X
    CTL_VK_Y,          // 28: Gamepad Y
    CTL_VK_DPAD_UP,    // 29: Gamepad D-Up
    CTL_VK_DPAD_DOWN,  // 30: Gamepad D-Down
    CTL_VK_DPAD_LEFT,  // 31: Gamepad D-Left
    CTL_VK_DPAD_RIGHT, // 32: Gamepad D-Right
    CTL_VK_BACK,       // 33: Gamepad Back
    CTL_VK_START,      // 34: Gamepad Start
    kStickDeflectKey   // 35: Gamepad Stick
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
    "Always",
    "Gamepad LT",
    "Gamepad RT",
    "Gamepad LB",
    "Gamepad RB",
    "Gamepad L3",
    "Gamepad R3",
    "Gamepad A",
    "Gamepad B",
    "Gamepad X",
    "Gamepad Y",
    "Gamepad D-Up",
    "Gamepad D-Down",
    "Gamepad D-Left",
    "Gamepad D-Right",
    "Gamepad Back",
    "Gamepad Start",
    "Gamepad Stick"
};
constexpr int kKeyCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));
static_assert(sizeof(kKeys) / sizeof(kKeys[0]) == sizeof(kKeyNames) / sizeof(kKeyNames[0]),
    "kKeys and kKeyNames must stay in sync");

const char* const kWeaponClassNames[] = {
    "Rifle",
    "SMG",
    "Shotgun",
    "Pistol",
    "DMR / Sniper"
};
constexpr int kWeaponClassCount = static_cast<int>(sizeof(kWeaponClassNames) / sizeof(kWeaponClassNames[0]));
static_assert(kWeaponClassCount == AimConfig::kWeaponProfileCount, "weapon profile count mismatch");

// The camera block is written as one scatter transaction (validation read +
// write). 8 ms (~125 Hz max) keeps the exponential response (20-90 ms) smooth
// without flooding the DMA bus. All write timing uses steady_clock in
// microseconds: GetTickCount64 only advances in ~15.6 ms steps, which made a
// 16 ms gate fire every second step (~32 Hz) and fed dt-based smoothing with
// wrong time deltas.
constexpr std::uint64_t kWriteIntervalUs = 8000;
constexpr std::chrono::milliseconds kTickInterval{ 4 };

inline std::uint64_t SteadyUs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t g_observation = 0;
int g_lastTarget = -1;
std::uint64_t g_lastTargetSwitchMs = 0;
bool g_wasAiming = false;
DebugInfo g_debug{};
std::atomic<int> g_activeFov{ 40 };

// Smoothing state. Reset whenever the aim key is released or the target
// changes, so a new pick starts from its own angle instead of sweeping across
// the world from the previous target.
bool g_smoothInit = false;
float g_curYaw = 0.f;
float g_curPitch = 0.f;
float g_carryYaw = 0.f;
float g_carryPitch = 0.f;
int g_smoothTarget = -1;
float g_bezierT = 0.f;
float g_bezierBendSign = 1.f;
Base::AimMotion::CurveState g_cursorCurve{};
std::uint64_t g_lastWriteTick = 0;
std::uint64_t g_targetLockUs = 0;
Vector3 g_writtenForward{};
bool g_haveWrittenForward = false;
bool g_writeHealthy = false;
const char* g_lastVerdict = "-";

struct HumanizeState {
    int entityId = -1;
    int chosenBone = 2; // 0=Head, 1=Neck, 2=Chest
    std::uint64_t nextBoneRollUs = 0;
    Vector3 curOffset{};
    Vector3 targetOffset{};
    std::uint64_t nextOffsetRollUs = 0;
    std::uint64_t lastStepUs = 0;
    std::uint32_t rng = 0x9E3779B9u;

    float NextUnit() {
        // xorshift32 -> [-1, 1]
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (static_cast<float>(rng & 0xFFFFu) / 32767.5f) - 1.0f;
    }

    int PickHeadOrBodyBone(int entId, std::uint64_t nowUs) {
        if (entityId != entId || nowUs >= nextBoneRollUs) {
            entityId = entId;
            // Weighted: ~45% Chest, ~25% Neck, ~30% Head
            const float u = (NextUnit() + 1.0f) * 0.5f;
            chosenBone = (u < 0.45f) ? 2 : (u < 0.70f ? 1 : 0);
            const float holdSec = 1.6f + ((NextUnit() + 1.0f) * 0.5f) * 1.0f; // 1.6..2.6 s
            nextBoneRollUs = nowUs + static_cast<std::uint64_t>(holdSec * 1.0e6f);
        }
        return chosenBone;
    }

    Vector3 SampleOffset(int entId, std::uint64_t nowUs, float maxCm) {
        if (maxCm <= 0.05f) {
            curOffset = Vector3{};
            return curOffset;
        }
        const float maxM = std::clamp(maxCm, 0.f, 15.f) * 0.01f;
        if (entityId != entId || nowUs >= nextOffsetRollUs) {
            entityId = entId;
            targetOffset = Vector3(
                NextUnit() * maxM,
                NextUnit() * maxM * 0.85f,
                NextUnit() * maxM);
            const float holdSec = 0.9f + ((NextUnit() + 1.0f) * 0.5f) * 0.8f; // 0.9..1.7 s
            nextOffsetRollUs = nowUs + static_cast<std::uint64_t>(holdSec * 1.0e6f);
        }
        const float dt = (lastStepUs != 0 && nowUs > lastStepUs)
            ? std::clamp(static_cast<float>(nowUs - lastStepUs) * 1.0e-6f, 0.001f, 0.05f)
            : 0.008f;
        lastStepUs = nowUs;
        const float a = 1.f - std::exp(-dt * 2.5f); // smooth ~0.4 s wander, zero jitter
        curOffset.x += (targetOffset.x - curOffset.x) * a;
        curOffset.y += (targetOffset.y - curOffset.y) * a;
        curOffset.z += (targetOffset.z - curOffset.z) * a;
        return curOffset;
    }
};
HumanizeState g_humanize{};

// Memory aim write gate. A level transition destroys the camera/input objects;
// writes stay blocked for a second after the reader reports a new session so
// the new objects can be re-resolved first.
std::uint64_t g_lastSessionGeneration = 0;
std::uint64_t g_writeBlockedUntil = 0;

struct MotionHistoryEntry {
    Vector3 position{};
    std::uint64_t tickUs = 0;   // snapshot publish time, steady_clock microseconds
    bool valid = false;
};
struct MotionHistory {
    std::deque<MotionHistoryEntry> samples{};
    std::uint64_t lastUpdateMs = 0;
    std::uint64_t lastObservationUs = 0;
    bool valid = false;
    bool estimateValid = false;
    bool quadraticValid = false;
    bool smoothingInitialized = false;
    Vector3 smoothedVelocity{};
    Vector3 smoothedAcceleration{};
};
std::unordered_map<int, MotionHistory> g_motionHistory;

struct AimQuat {
    float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
};

// ---- Unified Gamepad Input (Hardware Device + Host XInput) ----------------
struct HostXInputGamepad {
    std::uint16_t wButtons;
    std::uint8_t bLeftTrigger;
    std::uint8_t bRightTrigger;
    std::int16_t sThumbLX;
    std::int16_t sThumbLY;
    std::int16_t sThumbRX;
    std::int16_t sThumbRY;
};
struct HostXInputState {
    std::uint32_t dwPacketNumber;
    HostXInputGamepad Gamepad;
};
static_assert(sizeof(HostXInputState) == 16, "XINPUT_STATE ABI mismatch");

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, HostXInputState*);

XInputGetStateFn ResolveXInputGetState()
{
    static XInputGetStateFn fn = []() -> XInputGetStateFn {
        HMODULE mod = LoadLibraryA("xinput1_4.dll");
        if (!mod) mod = LoadLibraryA("xinput9_1_0.dll");
        if (!mod) mod = LoadLibraryA("xinput1_3.dll");
        if (!mod) return nullptr;
        return reinterpret_cast<XInputGetStateFn>(GetProcAddress(mod, "XInputGetState"));
    }();
    return fn;
}

std::uint16_t MapXInputButtons(std::uint16_t xi)
{
    std::uint16_t out = 0;
    if (xi & 0x0001u) out |= CTL_BTN_DPAD_UP;
    if (xi & 0x0002u) out |= CTL_BTN_DPAD_DOWN;
    if (xi & 0x0004u) out |= CTL_BTN_DPAD_LEFT;
    if (xi & 0x0008u) out |= CTL_BTN_DPAD_RIGHT;
    if (xi & 0x0010u) out |= CTL_BTN_START;
    if (xi & 0x0020u) out |= CTL_BTN_BACK;
    if (xi & 0x0040u) out |= CTL_BTN_L3;
    if (xi & 0x0080u) out |= CTL_BTN_R3;
    if (xi & 0x0100u) out |= CTL_BTN_LB;
    if (xi & 0x0200u) out |= CTL_BTN_RB;
    if (xi & 0x1000u) out |= CTL_BTN_A;
    if (xi & 0x2000u) out |= CTL_BTN_B;
    if (xi & 0x4000u) out |= CTL_BTN_X;
    if (xi & 0x8000u) out |= CTL_BTN_Y;
    return out;
}

struct GamepadState {
    bool connected = false;
    const char* source = "-";
    std::uint16_t buttons = 0;
    float lt = 0.f; // 0..1
    float rt = 0.f; // 0..1
    float lx = 0.f; // -1..1
    float ly = 0.f; // -1..1
    float rx = 0.f; // -1..1
    float ry = 0.f; // -1..1
};

float NormAxis(std::int16_t v)
{
    return std::clamp(static_cast<float>(v) / 32767.f, -1.f, 1.f);
}

GamepadState PollGamepad(const AimConfig& cfg)
{
    GamepadState gp{};
    if (!cfg.ControllerEnabled) return gp;

    // 1. Hardware controller bridge (Makcu Controller / Ferrum Controller)
    const auto status = Base::Hardware::Session().GetStatus();
    if (status.state == Base::Hardware::LinkState::Ready && status.input.controllerValid) {
        gp.connected = true;
        gp.source = "device gamepad";
        gp.buttons = status.input.controllerButtons;
        gp.lt = static_cast<float>(status.input.lt) / 255.f;
        gp.rt = static_cast<float>(status.input.rt) / 255.f;
        gp.lx = NormAxis(status.input.lx);
        gp.ly = NormAxis(status.input.ly);
        gp.rx = NormAxis(status.input.rx);
        gp.ry = NormAxis(status.input.ry);
        return gp;
    }

    // 2. Host PC XInput controller (throttle disconnected-slot probing to 1 Hz
    //    because XInputGetState on empty slots stalls ~0.5 ms on some Windows builds)
    if (const auto xinputGet = ResolveXInputGetState()) {
        static std::atomic<int> s_connectedSlot{ -1 };
        static std::atomic<std::uint64_t> s_lastProbeMs{ 0 };
        const int slotCfg = std::clamp(cfg.ControllerSlot, 0, 4);
        const auto trySlot = [&](DWORD slot) -> bool {
            HostXInputState st{};
            if (xinputGet(slot, &st) == ERROR_SUCCESS) {
                gp.connected = true;
                gp.source = "host gamepad";
                gp.buttons = MapXInputButtons(st.Gamepad.wButtons);
                gp.lt = static_cast<float>(st.Gamepad.bLeftTrigger) / 255.f;
                gp.rt = static_cast<float>(st.Gamepad.bRightTrigger) / 255.f;
                gp.lx = NormAxis(st.Gamepad.sThumbLX);
                gp.ly = NormAxis(st.Gamepad.sThumbLY);
                gp.rx = NormAxis(st.Gamepad.sThumbRX);
                gp.ry = NormAxis(st.Gamepad.sThumbRY);
                return true;
            }
            return false;
        };

        const int cached = s_connectedSlot.load(std::memory_order_relaxed);
        const bool cachedMatchesCfg = cached >= 0 && (slotCfg == 0 || cached == slotCfg - 1);
        if (cachedMatchesCfg) {
            if (trySlot(static_cast<DWORD>(cached)))
                return gp;
            s_connectedSlot.store(-1, std::memory_order_relaxed);
        }

        const std::uint64_t nowMs = GetTickCount64();
        std::uint64_t prevProbe = s_lastProbeMs.load(std::memory_order_relaxed);
        if (nowMs - prevProbe >= 1000 &&
            s_lastProbeMs.compare_exchange_strong(prevProbe, nowMs, std::memory_order_relaxed)) {
            const DWORD firstSlot = (slotCfg == 0) ? 0u : static_cast<DWORD>(slotCfg - 1);
            const DWORD lastSlot = (slotCfg == 0) ? 3u : firstSlot;
            for (DWORD slot = firstSlot; slot <= lastSlot; ++slot) {
                if (trySlot(slot)) {
                    s_connectedSlot.store(static_cast<int>(slot), std::memory_order_relaxed);
                    return gp;
                }
            }
        }
    }
    return gp;
}

bool IsGamepadKeyCode(int vk)
{
    return IsCtlVk(vk) || vk == kStickDeflectKey;
}

bool GamepadKeyDown(int vk, const GamepadState& gp, const AimConfig& cfg)
{
    if (!gp.connected) return false;
    const float trigThresh = std::clamp(cfg.ControllerTriggerThreshold, 0.05f, 0.95f);
    const float stickDeadzone = std::clamp(cfg.ControllerStickDeadzone, 0.0f, 0.80f);
    if (vk == CTL_VK_LT) return gp.lt >= trigThresh;
    if (vk == CTL_VK_RT) return gp.rt >= trigThresh;
    if (vk == kStickDeflectKey) {
        const float rMag = std::hypot(gp.rx, gp.ry);
        const float lMag = std::hypot(gp.lx, gp.ly);
        return rMag >= stickDeadzone || lMag >= stickDeadzone;
    }
    const std::uint32_t bit = CtlVkBit(vk);
    return bit != 0 && (gp.buttons & static_cast<std::uint16_t>(bit)) != 0;
}

bool Held(int keyIndex, const char** source, const GamepadState* cachedGp = nullptr)
{
    if (keyIndex < 0 || keyIndex >= kKeyCount) keyIndex = 0;
    const int vk = kKeys[keyIndex];
    if (vk == 0) {
        if (source) *source = "always";
        return true;
    }

    if (IsGamepadKeyCode(vk)) {
        const auto& cfg = ConfigInstance.Aim;
        const GamepadState localGp = cachedGp ? *cachedGp : PollGamepad(cfg);
        if (GamepadKeyDown(vk, localGp, cfg)) {
            if (source) *source = localGp.source;
            return true;
        }
        return false;
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

// ---- Weapon Profile Resolution --------------------------------------------
struct EffectiveSettings {
    int targetBone = 0;
    int fov = 40;
    int smoothness = 6;
    int smoothAlgorithm = 0;
    bool triggerEnabled = false;
    int triggerKey = 2;
    int triggerMinDelayMs = 20;
    int triggerMaxDelayMs = 60;
    float triggerHitboxScale = 1.0f;
    int triggerMaxDistance = 200;
    int weaponClass = -1;
    std::string weaponName;
    bool profileOverrideActive = false;
};

std::string FindLocalWeaponName(const RobRunReader::Snapshot& snap)
{
    for (const auto& ent : snap.entities) {
        if (ent.entityId == snap.localEntityId)
            return ent.weaponNameUtf8;
    }
    return {};
}

EffectiveSettings ResolveEffectiveSettings(const AimConfig& cfg, const RobRunReader::Snapshot* snap)
{
    EffectiveSettings eff{};
    eff.targetBone = std::clamp(cfg.TargetBone, 0, 4);
    eff.fov = std::clamp(cfg.Fov, 5, 180);
    eff.smoothness = std::clamp(cfg.Smoothness, 1, 100);
    eff.smoothAlgorithm = std::clamp(cfg.SmoothAlgorithm, 0, 2);
    eff.triggerEnabled = cfg.TriggerEnabled;
    eff.triggerKey = std::clamp(cfg.TriggerKey, 0, kKeyCount - 1);
    eff.triggerMinDelayMs = std::clamp(cfg.TriggerMinDelayMs, 0, 500);
    eff.triggerMaxDelayMs = std::clamp(cfg.TriggerMaxDelayMs, eff.triggerMinDelayMs, 500);
    eff.triggerHitboxScale = std::clamp(cfg.TriggerHitboxScale, 0.25f, 2.0f);
    eff.triggerMaxDistance = std::clamp(cfg.TriggerMaxDistance, 5, 500);

    if (snap) {
        eff.weaponName = FindLocalWeaponName(*snap);
        eff.weaponClass = ClassifyWeaponName(eff.weaponName);
    }

    if (cfg.WeaponProfilesEnabled && eff.weaponClass >= 0 &&
        eff.weaponClass < AimConfig::kWeaponProfileCount) {
        const WeaponProfile& p = cfg.WeaponProfiles[static_cast<std::size_t>(eff.weaponClass)];
        if (p.Enabled) {
            eff.profileOverrideActive = true;
            if (p.TargetBone >= 0 && p.TargetBone <= 4)
                eff.targetBone = p.TargetBone;
            if (p.FovOverride >= 5 && p.FovOverride <= 180)
                eff.fov = p.FovOverride;
            if (p.SmoothnessOverride >= 1 && p.SmoothnessOverride <= 100)
                eff.smoothness = p.SmoothnessOverride;
            if (p.SmoothAlgorithmOverride >= 0 && p.SmoothAlgorithmOverride <= 2)
                eff.smoothAlgorithm = p.SmoothAlgorithmOverride;
            eff.triggerEnabled = cfg.TriggerEnabled && p.TriggerEnabled;
            if (p.TriggerKeyOverride >= 0 && p.TriggerKeyOverride < kKeyCount)
                eff.triggerKey = p.TriggerKeyOverride;
            if (p.TriggerMinDelayMs >= 0)
                eff.triggerMinDelayMs = std::clamp(p.TriggerMinDelayMs, 0, 500);
            if (p.TriggerMaxDelayMs >= 0)
                eff.triggerMaxDelayMs = std::clamp(p.TriggerMaxDelayMs, eff.triggerMinDelayMs, 500);
            else if (eff.triggerMaxDelayMs < eff.triggerMinDelayMs)
                eff.triggerMaxDelayMs = eff.triggerMinDelayMs;
            if (p.TriggerHitboxScale > 0.f)
                eff.triggerHitboxScale = std::clamp(p.TriggerHitboxScale, 0.25f, 2.0f);
            if (p.TriggerMaxDistance > 0)
                eff.triggerMaxDistance = std::clamp(p.TriggerMaxDistance, 5, 500);
        }
    }
    return eff;
}

// ---- Precision Auto-Fire State Machine ------------------------------------
enum class TriggerPhase { Idle, WaitingDelay, Firing, Cooldown };
TriggerPhase g_triggerPhase = TriggerPhase::Idle;
std::uint64_t g_triggerTimerMs = 0;
bool g_triggerButtonHeld = false;
bool g_triggerUsedHardware = false;
Base::Hardware::DeviceKind g_triggerDeviceKind = Base::Hardware::DeviceKind::Off;
std::atomic<bool> g_triggerDebugActive{ false };
std::atomic<const char*> g_triggerDebugState{ "off" };

void SendHostLeftButton(bool down)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(input));
}

void PressFireButton()
{
    const auto status = Base::Hardware::Session().GetStatus();
    if (status.state == Base::Hardware::LinkState::Ready &&
        status.kind != Base::Hardware::DeviceKind::Off) {
        Base::Hardware::Control c{};
        c.id = 0; // Left Mouse (mouse devices) / RT (controller devices)
        c.keyboard = false;
        c.down = true;
        if (Base::Hardware::Session().Button(c, status.kind)) {
            g_triggerButtonHeld = true;
            g_triggerUsedHardware = true;
            g_triggerDeviceKind = status.kind;
            return;
        }
    }
    SendHostLeftButton(true);
    g_triggerButtonHeld = true;
    g_triggerUsedHardware = false;
    g_triggerDeviceKind = Base::Hardware::DeviceKind::Off;
}

void RenewFireButton()
{
    if (!g_triggerButtonHeld || !g_triggerUsedHardware) return;
    Base::Hardware::Control c{};
    c.id = 0;
    c.keyboard = false;
    c.down = true;
    Base::Hardware::Session().Button(c, g_triggerDeviceKind);
}

void ReleaseFireButton()
{
    if (!g_triggerButtonHeld) return;
    if (g_triggerUsedHardware) {
        Base::Hardware::Control c{};
        c.id = 0;
        c.keyboard = false;
        c.down = false;
        Base::Hardware::Session().Button(c, g_triggerDeviceKind);
    } else {
        SendHostLeftButton(false);
    }
    g_triggerButtonHeld = false;
    g_triggerUsedHardware = false;
    g_triggerDeviceKind = Base::Hardware::DeviceKind::Off;
}

int RollTriggerDelayMs(int minMs, int maxMs)
{
    minMs = std::clamp(minMs, 0, 500);
    maxMs = std::clamp(maxMs, minMs, 500);
    if (minMs == maxMs) return minMs;
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(minMs, maxMs);
    return dist(rng);
}

float PointToSegmentDist(float px, float py, float ax, float ay, float bx, float by, float& outT)
{
    const float vx = bx - ax;
    const float vy = by - ay;
    const float lenSq = vx * vx + vy * vy;
    if (lenSq < 1e-4f) {
        outT = 0.f;
        return std::hypot(px - ax, py - ay);
    }
    outT = std::clamp(((px - ax) * vx + (py - ay) * vy) / lenSq, 0.f, 1.f);
    return std::hypot(px - (ax + vx * outT), py - (ay + vy * outT));
}

bool Finite(const Vector3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Role labels come from the reader's reflection pass. Mirror the ESP's id
// fallbacks so the aim filter and the overlay never disagree about a target.
// The teammate filter is party based: the reader resolves the local party's
// member ids and marks each entity, so same-role strangers stay targetable.
const char* RoleSkipReason(const RobRunReader::EntityData& ent, const AimConfig& cfg)
{
    const std::string role = RobRunReader::RoleLabel(ent.role, ent.roleName);
    if (cfg.ExpIgnorePolice && role == "Police Officer") return "role:police";
    if (cfg.ExpIgnoreCriminal && role == "Criminal") return "role:criminal";
    if (cfg.ExpIgnoreCivilian && role == "Civilian") return "role:civilian";
    if (cfg.ExpIgnoreTeammates && ent.isPartyMember) return "team:party";
    if (cfg.ExpIgnoreTeammates && ent.isClanMate) return "team:clan";
    if (cfg.IsTrusted(ent.publicId)) return "trusted";
    return nullptr;
}

bool ShouldSkipByRole(const RobRunReader::EntityData& ent, const AimConfig& cfg)
{
    return RoleSkipReason(ent, cfg) != nullptr;
}

Vector3 BonePoint(const RobRunReader::EntityData& ent, int bone)
{
    static const float standingHeights[] = { 1.62f, 1.45f, 1.25f, 0.95f };
    static const float crouchHeights[]   = { 1.18f, 1.05f, 0.88f, 0.65f };
    static const float vehicleHeights[]  = { 1.12f, 0.98f, 0.80f, 0.55f };
    // Relative vertical offsets between real skeleton anchors (metres)
    static const float anchorRelY[]      = { 0.62f, 0.48f, 0.28f, 0.00f }; // Head, Neck, Chest, Pelvis vs Pelvis
    const int index = (bone >= 0 && bone < 4) ? bone : 0;

    const Vector3 root(ent.position.x, ent.position.y + 1.f, ent.position.z);
    const float maxBoneDist = ent.inVehicle ? 12.f : 3.f;
    auto saneBone = [&](const Vector3& p) {
        return Finite(p) && Distance(p, root) <= maxBoneDist;
    };

    const auto& points = ent.bonePoints;
    if (points.valid) {
        const Vector3 anchors[4] = { points.head, points.neck, points.chest, points.pelvis };
        const bool hasAnchor[4]  = { points.hasHead, points.hasNeck, points.hasChest, points.hasPelvis };

        // 1. Exact requested bone
        if (hasAnchor[index] && saneBone(anchors[index]))
            return anchors[index];

        // 2. Requested anchor missing (e.g. head unbound in vehicle): derive from
        //    the nearest valid real skeleton anchor rather than dropping to ent.position.
        int bestRef = -1;
        int bestDelta = 99;
        for (int r = 0; r < 4; ++r) {
            if (!hasAnchor[r] || !saneBone(anchors[r])) continue;
            const int d = std::abs(r - index);
            if (d < bestDelta) { bestDelta = d; bestRef = r; }
        }
        if (bestRef >= 0) {
            Vector3 derived = anchors[bestRef];
            derived.y += (anchorRelY[index] - anchorRelY[bestRef]);
            return derived;
        }
    }

    // 3. If named anchors were unavailable but raw skeleton segments exist (ESP draws them),
    //    derive from the highest valid segment endpoint instead of ent.position.
    if (!ent.bones.empty()) {
        bool foundTop = false;
        Vector3 topPt{};
        for (const auto& seg : ent.bones) {
            const Vector3 a(seg.a.x, seg.a.y, seg.a.z);
            const Vector3 b(seg.b.x, seg.b.y, seg.b.z);
            if (saneBone(a) && (!foundTop || a.y > topPt.y)) { topPt = a; foundTop = true; }
            if (saneBone(b) && (!foundTop || b.y > topPt.y)) { topPt = b; foundTop = true; }
        }
        if (foundTop) {
            topPt.y -= (anchorRelY[0] - anchorRelY[index]);
            return topPt;
        }
    }

    // 4. Last-resort fallback when no skeleton exists at all.
    const float* heights = ent.inVehicle
        ? vehicleHeights
        : (ent.isCrouching ? crouchHeights : standingHeights);
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
    // Yaw in PlayerRotationInputModel / LookDirectionModel can accumulate past
    // +-2pi across multiple full turns in a match. Rejecting |yaw| > 7.0 rad
    // silently dropped playerRotInput after ~1.1 turns, leaving the game to
    // overwrite cameraModel every frame (jitter + locked mouse). Allow large
    // finite accumulated yaw and unwrap the written yaw to the nearest 2pi turn.
    const auto yawOk = [](float angle) {
        return std::isfinite(angle) && std::fabs(angle) <= 10000.0f;
    };
    const auto pitchOk = [](float angle) {
        return std::isfinite(angle) && std::fabs(angle) <= 1.75f;
    };
    const auto nearestTurnYaw = [](float currentYaw, float targetYawRad) {
        float d = std::fmod(targetYawRad - currentYaw, 2.f * kPi);
        if (d > kPi) d -= 2.f * kPi;
        if (d < -kPi) d += 2.f * kPi;
        return currentYaw + d;
    };
    if (lookDirModel > 0x10000 && !unitVector(lookModelValue))
        lookDirModel = 0;
    if (lookDirDirect > 0x10000 &&
        (!unitVector(lookDirectValue) || !yawOk(lookDirectYaw) ||
         !pitchOk(lookDirectPitch)))
        lookDirDirect = 0;
    if (playerRotInput > 0x10000 &&
        (!yawOk(rotYaw) || !pitchOk(rotPitch)))
        playerRotInput = 0;

    float writeLookYaw = (lookDirDirect > 0x10000) ? nearestTurnYaw(lookDirectYaw, yaw) : yaw;
    float writeRotYaw  = (playerRotInput > 0x10000) ? nearestTurnYaw(rotYaw, yaw) : yaw;

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
                lookDirDirect + rr::LookDirectionModel::Yaw, &writeLookYaw, sizeof(writeLookYaw));
            TargetProcess.AddScatterWriteRequest(scatter,
                lookDirDirect + rr::LookDirectionModel::Pitch, &pitch, sizeof(pitch));
        }
        if (playerRotInput > 0x10000) {
            TargetProcess.AddScatterWriteRequest(scatter,
                playerRotInput + rr::PlayerRotationInputModel::Yaw, &writeRotYaw, sizeof(writeRotYaw));
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
        TargetProcess.Write(lookDirDirect + rr::LookDirectionModel::Yaw, &writeLookYaw, sizeof(writeLookYaw));
        TargetProcess.Write(lookDirDirect + rr::LookDirectionModel::Pitch, &pitch, sizeof(pitch));
    }
    if (playerRotInput > 0x10000) {
        TargetProcess.Write(playerRotInput + rr::PlayerRotationInputModel::Yaw, &writeRotYaw, sizeof(writeRotYaw));
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
    int strengthPercent, int yScalePercent, Vector3& out, int& leadMs,
    bool inVehicle = false)
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
    // Only reject non-finite values or true teleports/respawns (> 25 m in one sample).
    const float stepDist = std::hypot(position.x - previous.x, position.y - previous.y, position.z - previous.z);
    if (!std::isfinite(speed) || !std::isfinite(stepDist) || stepDist > 25.f) return false;

    float lead = std::min(inVehicle ? 0.120f : 0.085f, responseMs * 0.001f);
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


namespace AdaptiveTuning {
constexpr int   kMaxLeadMs       = 120;
constexpr int   kMinHistory      = 3;
constexpr int   kMaxHistory      = 12;
constexpr float kVelSmoothing    = 0.4f;
constexpr float kAccelSmoothing  = 0.3f;
constexpr bool  kAllowFallback   = true;
constexpr int   kFallbackStrengthPercent = 100;
constexpr int   kFallbackYScalePercent = 100;
constexpr int   kHistoryWindowMs = 260;
constexpr int   kStaleMs         = 500;
// Pure teleport/respawn & curvature guards — no artificial speed or lead-distance cap
// so fast players, cars, and helicopters get full adaptive prediction.
constexpr float kMaxQuadraticAccel  = 80.0f;  // m/s^2, above this fall back to straight-line velocity fit
constexpr float kTeleportJumpMeters = 25.0f;  // only true teleports/respawns reset history
// A sample is recorded when the target actually moved; a standing target is
// refreshed at this interval so a stop is still seen.
constexpr float kMinMoveMeters   = 0.001f;
constexpr std::uint64_t kHoldSampleUs = 90000;
} // namespace AdaptiveTuning

static bool MovedEnough(const Vector3& a, const Vector3& b, float minMeters)
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz > minMeters * minMeters;
}

static int PushMotionSample(int entityId, const Vector3& position,
    std::uint64_t observationUs, std::uint64_t nowMs, bool& changed,
    bool inVehicle = false)
{
    (void)inVehicle;
    MotionHistory& hist = g_motionHistory[entityId];
    changed = false;
    if (observationUs == 0 || observationUs == hist.lastObservationUs)
        return static_cast<int>(hist.samples.size());

    // Clock went backwards (new session) or the target teleported/respawned.
    if (hist.lastObservationUs != 0 && observationUs < hist.lastObservationUs)
        hist = MotionHistory{};
    if (!hist.samples.empty() &&
        MovedEnough(position, hist.samples.back().position, AdaptiveTuning::kTeleportJumpMeters))
        hist = MotionHistory{};

    hist.lastObservationUs = observationUs;
    hist.lastUpdateMs = nowMs;

    // The reader publishes much faster than entity positions change, so most
    // snapshots repeat the previous position. Recording those makes the data a
    // staircase that the fit reads as acceleration. Only record real moves,
    // plus a periodic hold sample so a target that stopped is noticed.
    if (!hist.samples.empty()) {
        const MotionHistoryEntry& back = hist.samples.back();
        if (observationUs <= back.tickUs) {
            hist = MotionHistory{};
            hist.lastObservationUs = observationUs;
            hist.lastUpdateMs = nowMs;
        } else {
            const bool moved = MovedEnough(position, back.position, AdaptiveTuning::kMinMoveMeters);
            const bool holdDue = observationUs - back.tickUs >= AdaptiveTuning::kHoldSampleUs;
            if (!moved && !holdDue)
                return static_cast<int>(hist.samples.size());
        }
    }

    hist.samples.push_back(MotionHistoryEntry{ position, observationUs, true });
    hist.valid = true;
    changed = true;

    // Drop everything older than the window unconditionally: after an
    // occlusion gap the old points belong to a different movement.
    const std::uint64_t windowUs = static_cast<std::uint64_t>(AdaptiveTuning::kHistoryWindowMs) * 1000u;
    while (hist.samples.size() > 1 && observationUs - hist.samples.front().tickUs > windowUs)
        hist.samples.pop_front();
    while (static_cast<int>(hist.samples.size()) > AdaptiveTuning::kMaxHistory)
        hist.samples.pop_front();

    return static_cast<int>(hist.samples.size());
}

static bool EstimateMotion(const MotionHistory& hist,
    float& velX, float& velY, float& velZ,
    float& accX, float& accY, float& accZ,
    bool& quadratic, bool inVehicle = false)
{
    (void)inVehicle;
    AimMotionFit::Derivatives fit{};
    if (!AimMotionFit::FitAdaptive(hist.samples, fit, quadratic)) return false;

    // An implausible curvature is almost always sample noise: fall back to the
    // straight-line velocity fit instead of extrapolating noise or dropping lead.
    if (quadratic && std::hypot(fit.ax, fit.ay, fit.az) > AdaptiveTuning::kMaxQuadraticAccel) {
        double intercept[3]{};
        AimMotionFit::Derivatives lin{};
        if (!AimMotionFit::FitLinearLatest(hist.samples, lin, intercept)) return false;
        fit = lin;
        quadratic = false;
    }

    const double speed = std::hypot(fit.vx, fit.vy, fit.vz);
    const double accel = std::hypot(fit.ax, fit.ay, fit.az);
    if (!std::isfinite(speed) || !std::isfinite(accel))
        return false;

    velX = static_cast<float>(fit.vx);
    velY = static_cast<float>(fit.vy);
    velZ = static_cast<float>(fit.vz);
    accX = static_cast<float>(fit.ax);
    accY = static_cast<float>(fit.ay);
    accZ = static_cast<float>(fit.az);
    return std::isfinite(velX) && std::isfinite(velY) && std::isfinite(velZ) &&
        std::isfinite(accX) && std::isfinite(accY) && std::isfinite(accZ);
}

static void SmoothMotionEstimate(MotionHistory& hist,
    float& velX, float& velY, float& velZ,
    float& accX, float& accY, float& accZ)
{
    // Fixed alphas: boosting them on high acceleration made a single bad
    // sample pass almost unfiltered.
    const float velAlpha = AdaptiveTuning::kVelSmoothing;
    const float accAlpha = AdaptiveTuning::kAccelSmoothing;

    if (!hist.smoothingInitialized) {
        hist.smoothedVelocity = Vector3(velX, velY, velZ);
        hist.smoothedAcceleration = Vector3(accX, accY, accZ);
        hist.smoothingInitialized = true;
        return;
    }

    velX = hist.smoothedVelocity.x = velAlpha * velX + (1.0f - velAlpha) * hist.smoothedVelocity.x;
    velY = hist.smoothedVelocity.y = velAlpha * velY + (1.0f - velAlpha) * hist.smoothedVelocity.y;
    velZ = hist.smoothedVelocity.z = velAlpha * velZ + (1.0f - velAlpha) * hist.smoothedVelocity.z;
    accX = hist.smoothedAcceleration.x = accAlpha * accX + (1.0f - accAlpha) * hist.smoothedAcceleration.x;
    accY = hist.smoothedAcceleration.y = accAlpha * accY + (1.0f - accAlpha) * hist.smoothedAcceleration.y;
    accZ = hist.smoothedAcceleration.z = accAlpha * accZ + (1.0f - accAlpha) * hist.smoothedAcceleration.z;
}

static float ComputeAdaptiveLeadMs(float observationAgeMs, int sampleCount, float responseMs)
{
    return static_cast<float>(AimMotionFit::LeadMilliseconds(
        responseMs, observationAgeMs, sampleCount,
        AdaptiveTuning::kMinHistory, AdaptiveTuning::kMaxLeadMs));
}

static bool ApplyAdaptivePrediction(const Vector3& point, int entityId,
    const Vector3& position, std::uint64_t observationUs, std::uint64_t nowMs,
    float responseMs, float observationAgeMs, Vector3& out, int& leadMs, const char*& mode,
    bool inVehicle = false)
{
    bool newObservation = false;
    const int sampleCount = PushMotionSample(
        entityId, position, observationUs, nowMs, newObservation, inVehicle);
    mode = sampleCount < AdaptiveTuning::kMinHistory ? "warming up" : "fit rejected";

    float velX = 0.f, velY = 0.f, velZ = 0.f;
    float accX = 0.f, accY = 0.f, accZ = 0.f;

    auto it = g_motionHistory.find(entityId);
    const bool haveHist = it != g_motionHistory.end() && it->second.valid;

    bool accelReliable = false;
    if (haveHist) {
        MotionHistory& hist = it->second;
        if (newObservation) {
            hist.estimateValid = EstimateMotion(
                hist, velX, velY, velZ, accX, accY, accZ, hist.quadraticValid, inVehicle);
            if (hist.estimateValid) {
                SmoothMotionEstimate(hist, velX, velY, velZ, accX, accY, accZ);
                if (!hist.quadraticValid) {
                    hist.smoothedAcceleration = Vector3{};
                }
            } else {
                hist.smoothingInitialized = false;
            }
        }
        accelReliable = hist.estimateValid && hist.smoothingInitialized;
        if (accelReliable) {
            velX = hist.smoothedVelocity.x;
            velY = hist.smoothedVelocity.y;
            velZ = hist.smoothedVelocity.z;
            accX = hist.smoothedAcceleration.x;
            accY = hist.smoothedAcceleration.y;
            accZ = hist.smoothedAcceleration.z;
        }
    }

    float leadMsF = 0.f;
    if (accelReliable) {
        leadMsF = ComputeAdaptiveLeadMs(observationAgeMs, sampleCount, responseMs);
        mode = it->second.quadraticValid ? "adaptive quadratic" : "adaptive linear";
        const float t = leadMsF * 0.001f;
        const float halfA = 0.5f;
        const float ox = velX * t + halfA * accX * t * t;
        const float oy = velY * t + halfA * accY * t * t;
        const float oz = velZ * t + halfA * accZ * t * t;
        out = Vector3(point.x + ox, point.y + oy, point.z + oz);
    } else if (AdaptiveTuning::kAllowFallback) {
        if (haveHist && it->second.samples.size() >= 2) {
            const auto& samples = it->second.samples;
            const auto& newest = samples.back();
            const auto& prev = samples[samples.size() - 2];
            const std::uint64_t sampleMs = (newest.tickUs - prev.tickUs + 500) / 1000;
            if (sampleMs >= 4 && sampleMs <= 100) {
                Vector3 lead{};
                int legacyLeadMs = 0;
                if (ApplyLeading(point, newest.position, prev.position,
                        sampleMs, responseMs,
                        AdaptiveTuning::kFallbackStrengthPercent,
                        AdaptiveTuning::kFallbackYScalePercent, lead, legacyLeadMs, inVehicle)) {
                    out = lead;
                    leadMsF = static_cast<float>(legacyLeadMs);
                    mode = "linear fallback";
                }
            }
        }
    }

    if (leadMsF <= 0.f) return false;
    leadMs = static_cast<int>(leadMsF);
    return true;
}

static void PruneMotionHistory(std::uint64_t nowMs)
{
    const std::uint64_t staleMs = static_cast<std::uint64_t>(AdaptiveTuning::kStaleMs);
    for (auto it = g_motionHistory.begin(); it != g_motionHistory.end(); ) {
        if (nowMs - it->second.lastUpdateMs > staleMs) {
            it = g_motionHistory.erase(it);
        } else {
            ++it;
        }
    }
}

static void ResetMotionHistory()
{
    g_motionHistory.clear();
}


const char* const* KeyNames() { return kKeyNames; }
int KeyCount() { return kKeyCount; }
bool IsAlwaysKey(int keyIndex) { return keyIndex == kAlwaysKeyIndex; }
bool IsKeyHeld(int keyIndex)
{
    const char* source = nullptr;
    return Held(keyIndex, &source);
}

const char* const* WeaponClassNames() { return kWeaponClassNames; }
int WeaponClassCount() { return kWeaponClassCount; }

int ClassifyWeaponName(const std::string& weaponNameUtf8)
{
    if (weaponNameUtf8.empty()) return -1;
    std::string lower = weaponNameUtf8;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto has = [&](const char* needle) { return lower.find(needle) != std::string::npos; };

    // 4 = DMR / Sniper
    if (has("drm") || has("sniper") || has("dmr"))
        return 4;
    // 3 = Pistol
    if (has("pistol") || has("revolver") || has("deagle") ||
        has("dp9") || has("dp_9") || has("lbr44") || has("lbr_44") ||
        has("vulture") || has("valture"))
        return 3;
    // 2 = Shotgun
    if (has("shotgun") || has("pump") || has("spaz") ||
        has("tag-12") || has("tag12") || has("harris") || has("ordr"))
        return 2;
    // 1 = SMG
    if (has("mp7") || has("mac10") || has("mac-10") || has("ump") ||
        has("vector") || has("p90") || has("jp7") || has("jp_7") ||
        has("mc10") || has("mc_10") || has("cb45") || has("cb_45") ||
        has("apscalar") || has("ap_scalar") || has("pdw90") || has("pdw_90"))
        return 1;
    // 0 = Rifle / LMG
    if (has("ak") || has("as val") || has("r4c") || has("scar") ||
        has("lm 249") || has("m249") || has("lmg") || has("kl") ||
        has("srreaper") || has("sr_reaper") || has("rc556") || has("rc_556") ||
        has("starh") || has("star_h"))
        return 0;
    return -1;
}
static std::atomic<float> g_displayWidth{ 1920.f };
static std::atomic<float> g_displayHeight{ 1080.f };
struct RejectedDebug {
    int entityId = -1;
    float screenDist = 1.0e9f;
    std::string role;
    const char* reason = "none";
    bool rigValid = false;
    bool hasHead = false;
};
static RejectedDebug g_lastRejected{};

static std::mutex g_debugMutex;
static std::mutex g_workerLifecycleMutex;
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

namespace {

// Small positive reveal duration (5 ms). Never 0.0f: zero is rejected / divides
// by zero in the reveal state machine and causes slots to stay unrevealed.
constexpr float kFastRevealDurationSec = 0.005f;

struct SavedFloatField {
    std::uintptr_t addr = 0;
    float original = 0.f;
};

struct InstantLootState {
    std::uint64_t sessionGen = 0;
    std::uintptr_t battleModel = 0;
    std::uintptr_t revealDescs = 0;
    std::uint64_t lastCheckMs = 0;
    bool patched = false;
    std::vector<SavedFloatField> savedFields;
};

InstantLootState g_instantLoot;

bool IsReasonableDuration(float v)
{
    return std::isfinite(v) && v > kFastRevealDurationSec && v <= 60.f;
}

void RestoreInstantLoot()
{
    if (!g_instantLoot.patched) return;
    if (g_instantLoot.revealDescs > 0x10000 &&
        Il2Cpp::IsClass(g_instantLoot.revealDescs, "InventoryRevealViewDescriptions") &&
        !g_instantLoot.savedFields.empty()) {
        for (const auto& entry : g_instantLoot.savedFields) {
            float cur = 0.f;
            if (RobRunMemory::ReadBytesFresh(entry.addr, &cur, sizeof(cur)) &&
                std::isfinite(cur) && cur >= 0.f && cur <= 0.05f) {
                TargetProcess.WriteRestore(entry.addr, entry.original);
            }
        }
    }
    g_instantLoot.savedFields.clear();
    g_instantLoot.patched = false;
}

void PatchRevealDescriptions(std::uintptr_t revealDescs)
{
    if (revealDescs <= 0x10000 ||
        !Il2Cpp::IsClass(revealDescs, "InventoryRevealViewDescriptions")) {
        return;
    }

    if (g_instantLoot.patched && g_instantLoot.revealDescs == revealDescs)
        return;
    if (g_instantLoot.patched)
        RestoreInstantLoot();

    namespace rv = rr::InventoryRevealViewDescriptions;
    std::vector<SavedFloatField> toPatch;

    const std::uintptr_t scanAddr = revealDescs + rv::TimeToOneCellBorder;
    float scanOrig = 0.f;
    if (RobRunMemory::ReadBytesFresh(scanAddr, &scanOrig, sizeof(scanOrig)) &&
        IsReasonableDuration(scanOrig)) {
        toPatch.push_back({ scanAddr, scanOrig });
    }

    const std::uintptr_t list = RobRunMemory::ReadPtr(revealDescs + rv::List);
    const std::uintptr_t items = RobRunMemory::ReadPtr(list + rr::List::items);
    const std::int32_t count = RobRunMemory::ReadInt32(list + rr::List::count);
    if (items > 0x10000 && count > 0 && count <= 64) {
        for (std::int32_t i = 0; i < count; ++i) {
            const std::uintptr_t pair = items + rr::Array::vector +
                static_cast<std::uintptr_t>(i) * rv::Stride;
            const std::uintptr_t desc = RobRunMemory::ReadPtr(pair + rv::PairDesc);
            if (desc <= 0x10000) continue;

            const std::uintptr_t shineAddr = desc + rv::ShineView + rv::ShineDuration;
            float shineOrig = 0.f;
            if (RobRunMemory::ReadBytesFresh(shineAddr, &shineOrig, sizeof(shineOrig)) &&
                IsReasonableDuration(shineOrig)) {
                toPatch.push_back({ shineAddr, shineOrig });
            }

            const std::uintptr_t iconAddr = desc + rv::IconView + rv::IconDuration;
            float iconOrig = 0.f;
            if (RobRunMemory::ReadBytesFresh(iconAddr, &iconOrig, sizeof(iconOrig)) &&
                IsReasonableDuration(iconOrig)) {
                toPatch.push_back({ iconAddr, iconOrig });
            }
        }
    }

    if (toPatch.empty()) return;

    const float fast = kFastRevealDurationSec;
    for (const auto& entry : toPatch) {
        TargetProcess.Write(entry.addr, fast);
    }

    g_instantLoot.revealDescs = revealDescs;
    g_instantLoot.savedFields = std::move(toPatch);
    g_instantLoot.patched = true;
}

} // namespace

static void TickInstantLoot()
{
    const auto snap = RobRunReader::GetSnapshot();
    if (!snap || snap->battleModel <= 0x10000 || snap->battleEntitiesUpdater <= 0x10000) {
        // Not in a match / menu / process exited: drop cached pointers without
        // writing to potentially unloaded memory.
        g_instantLoot = {};
        return;
    }

    const std::uintptr_t bm = snap->battleModel;
    if (snap->sessionGeneration != g_instantLoot.sessionGen ||
        bm != g_instantLoot.battleModel) {
        // Match or map switched: drop stale references cleanly.
        g_instantLoot = {};
        g_instantLoot.sessionGen = snap->sessionGeneration;
        g_instantLoot.battleModel = bm;
    }

    if (!ConfigInstance.Overlay.MemWriteMaster || !ConfigInstance.World.InstantLoot) {
        RestoreInstantLoot();
        return;
    }

    // Throttle validation/re-check to avoid unnecessary DMA traffic.
    // InventoryRevealViewDescriptions is used exclusively by container/crate/
    // car/body loot reveal (never alarm hacks, doors, driving, or other timed
    // actions). Keeping it patched while in-match ensures frame 0 of opening
    // any container already sees 5 ms so even the first item reveals instantly.
    const std::uint64_t nowMs = GetTickCount64();
    if (g_instantLoot.patched) {
        if (nowMs - g_instantLoot.lastCheckMs < 2000) return;
        g_instantLoot.lastCheckMs = nowMs;
        if (!Il2Cpp::IsClass(g_instantLoot.revealDescs, "InventoryRevealViewDescriptions")) {
            g_instantLoot.revealDescs = 0;
            g_instantLoot.savedFields.clear();
            g_instantLoot.patched = false;
        }
        return;
    }

    if (nowMs - g_instantLoot.lastCheckMs < 250) return;
    g_instantLoot.lastCheckMs = nowMs;

    const std::uintptr_t screenContent = RobRunMemory::ReadPtr(
        snap->battleEntitiesUpdater + rr::BattleEntitiesUpdater::_battleScreenContent);
    if (screenContent <= 0x10000) return;

    const std::uintptr_t revealDescs = RobRunMemory::ReadPtr(
        screenContent + rr::BattleScreenContent::RevealViewDescriptions);
    if (revealDescs > 0x10000)
        PatchRevealDescriptions(revealDescs);
}

static bool CanSafelyWriteSession(const RobRunReader::SnapshotPtr& snap, std::uint64_t now);
static bool ReadPtrFreshSafe(std::uintptr_t addr, std::uintptr_t& out);

static void TickInstantUnlock()
{
    if (!ConfigInstance.Overlay.MemWriteMaster || !ConfigInstance.World.InstantUnlock) return;
    const auto snap = RobRunReader::GetSnapshot();
    const std::uint64_t now = GetTickCount64();
    if (!CanSafelyWriteSession(snap, now)) return;

    std::uintptr_t worldEnts = 0;
    if (!ReadPtrFreshSafe(snap->battleModel + rr::BattleModel::BattleEntitiesModel, worldEnts) ||
        worldEnts != snap->battleEntities)
        return;

    // 1. Safe doors within range
    std::vector<RobRunMemory::DictEntry> safeDoors;
    const std::uintptr_t safeDict = RobRunMemory::ReadPtr(worldEnts + rr::WorldEntitiesModel::SafeDoors);
    if (RobRunMemory::ReadDictInt(safeDict, safeDoors, false)) {
        for (const auto& entry : safeDoors) {
            const std::uintptr_t safeModel = entry.value;
            if (safeModel <= 0x10000) continue;

            const std::uintptr_t desc = RobRunMemory::ReadPtr(safeModel + rr::SafeDoorModel::Description);
            if (desc > 0x10000 && !snap->localPosition.IsZero()) {
                Vector3 pos{};
                if (RobRunMemory::ReadBytes(desc + rr::SafeDoorModel::DescriptionPosition, &pos, sizeof(pos))) {
                    if (Distance(pos, snap->localPosition) > 12.0f)
                        continue;
                }
            }

            const std::uintptr_t isUnlockedData = RobRunMemory::ReadPtr(safeModel + rr::SafeDoorModel::_isUnlocked);
            if (isUnlockedData > 0x10000) {
                const std::uint8_t one = 1;
                TargetProcess.Write(isUnlockedData + rr::ReferenceData::Value, one);
            }

            const std::uintptr_t endTsData = RobRunMemory::ReadPtr(safeModel + rr::SafeDoorModel::_unlockEndTs);
            if (endTsData > 0x10000) {
                const std::int64_t zero = 0;
                TargetProcess.Write(endTsData + rr::ReferenceData::Value, zero);
            }

            const std::uint8_t switchState = 1;
            TargetProcess.Write(safeModel + rr::SafeDoorModel::ShouldSwitchState, switchState);
        }
    }

    // 2. Location doors within range
    std::vector<RobRunMemory::DictEntry> doors;
    const std::uintptr_t doorDict = RobRunMemory::ReadPtr(worldEnts + rr::WorldEntitiesModel::Doors);
    if (RobRunMemory::ReadDictInt(doorDict, doors, false)) {
        for (const auto& entry : doors) {
            const std::uintptr_t doorModel = entry.value;
            if (doorModel <= 0x10000) continue;

            const std::uintptr_t openCloseReq = RobRunMemory::ReadPtr(doorModel + 0x48);
            if (openCloseReq > 0x10000) {
                const std::uintptr_t posModel = RobRunMemory::ReadPtr(doorModel + 0x38);
                if (posModel > 0x10000 && !snap->localPosition.IsZero()) {
                    Vector3 pos{};
                    if (RobRunMemory::ReadBytes(posModel + 0x10, &pos, sizeof(pos))) {
                        if (Distance(pos, snap->localPosition) > 4.5f)
                            continue;
                    }
                }
                const std::uint8_t req = 1;
                TargetProcess.Write(openCloseReq + 0x10, req);
            }
        }
    }
}

struct CustomCameraState {
    std::uintptr_t cameraModel = 0;
    std::uint64_t sessionGen = 0;
    float originalDistance = 0.f;
    bool captured = false;
    bool applied = false;
};
static CustomCameraState g_customCam;

static void TickCustomCamera()
{
    const auto snap = RobRunReader::GetSnapshot();
    const std::uint64_t now = GetTickCount64();

    // Never write into a camera from a previous session — Unity frees it on map switch.
    if (!snap || snap->sessionGeneration != g_customCam.sessionGen) {
        g_customCam = CustomCameraState{};
        if (snap) g_customCam.sessionGen = snap->sessionGeneration;
    }

    if (!ConfigInstance.Overlay.MemWriteMaster || !ConfigInstance.Overlay.CustomCameraDistance) {
        if (g_customCam.applied && g_customCam.captured &&
            CanSafelyWriteSession(snap, now) && snap->cameraModel == g_customCam.cameraModel) {
            std::uintptr_t liveCam = 0;
            if (ReadPtrFreshSafe(snap->battleModel + rr::BattleModel::BattleCameraModel, liveCam) &&
                liveCam == g_customCam.cameraModel) {
                TargetProcess.WriteRestore(g_customCam.cameraModel + rr::BattleCameraModelOffsets::Distance,
                    g_customCam.originalDistance);
            }
        }
        g_customCam.applied = false;
        return;
    }

    if (!CanSafelyWriteSession(snap, now)) return;

    std::uintptr_t liveCam = 0;
    if (!ReadPtrFreshSafe(snap->battleModel + rr::BattleModel::BattleCameraModel, liveCam) ||
        liveCam != snap->cameraModel)
        return;

    if (!g_customCam.captured || g_customCam.cameraModel != liveCam) {
        float curDist = 0.f;
        if (!RobRunMemory::ReadBytesFresh(liveCam + rr::BattleCameraModelOffsets::Distance, &curDist, sizeof(curDist)) ||
            !std::isfinite(curDist) || curDist < 0.2f || curDist > 30.f)
            return;
        g_customCam.cameraModel = liveCam;
        g_customCam.sessionGen = snap->sessionGeneration;
        g_customCam.originalDistance = curDist;
        g_customCam.captured = true;
    }

    const float targetDistance = std::clamp(ConfigInstance.Overlay.CameraDistance, 1.0f, 15.0f);
    TargetProcess.Write(liveCam + rr::BattleCameraModelOffsets::Distance, targetDistance);
    g_customCam.applied = true;
}

static void StartRecoilWorkerLocked();
static void StopRecoilWorkerLocked();

static void StopAimWorkerLocked()
{
    StopRecoilWorkerLocked();
    g_aimWorkerStop.store(true, std::memory_order_relaxed);
    if (g_aimWorkerThread.joinable())
        g_aimWorkerThread.join();
    ResetMotionHistory();
    ReleaseFireButton();
    g_triggerPhase = TriggerPhase::Idle;
    g_triggerDebugActive.store(false, std::memory_order_relaxed);
    g_triggerDebugState.store("off", std::memory_order_relaxed);
    Keyboard::SetAsyncKeyPolling(false);
}

void StartAimWorker()
{
    std::lock_guard<std::mutex> lock(g_workerLifecycleMutex);
    StopAimWorkerLocked();
    StartRecoilWorkerLocked();
    Keyboard::SetAsyncKeyPolling(true);
    g_aimWorkerStop.store(false, std::memory_order_relaxed);
    g_aimWorkerThread = std::thread([] {
        using clock = std::chrono::steady_clock;
        // High-resolution waitable timer: sleep_for(2 ms) rounds up to the
        // 15.6 ms system tick unless something else raised the timer period.
        HANDLE paceTimer = CreateWaitableTimerExW(
            nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        auto lastTick = clock::now() - std::chrono::seconds(1);
        auto lastInstantLoot = clock::now() - std::chrono::seconds(1);
        auto lastLoot = clock::now() - std::chrono::seconds(1);
        while (!g_aimWorkerStop.load(std::memory_order_relaxed)) {
            const auto now = clock::now();
            try {
                // Keep the game-PC key bitmap warm on the worker so the render
                // thread never blocks on kernel DMA reads. Camera polling runs
                // on RobRunReader's dedicated high-priority camera thread.
                Keyboard::PollKeysAsync();
                TickTrigger();
                if (now - lastTick >= kTickInterval) {
                    lastTick = now;
                    Tick();
                    TickRecoilSpread();
                }
                if (now - lastInstantLoot >= std::chrono::milliseconds(16)) {
                    lastInstantLoot = now;
                    TickInstantLoot();
                }
                if (now - lastLoot >= std::chrono::milliseconds(50)) {
                    lastLoot = now;
                    TickInstantUnlock();
                    TickCustomCamera();
                    FovChanger::Tick();
                }
            } catch (...) {}
            bool waited = false;
            if (paceTimer) {
                LARGE_INTEGER due{};
                due.QuadPart = -1000LL * 10; // 1 ms, relative, 100 ns units
                if (SetWaitableTimer(paceTimer, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(paceTimer, 20);
                    waited = true;
                }
            }
            if (!waited)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (paceTimer) CloseHandle(paceTimer);
    });
}

void StopAimWorker()
{
    std::lock_guard<std::mutex> lock(g_workerLifecycleMutex);
    StopAimWorkerLocked();
    RestoreInstantLoot();
    FovChanger::Restore();
}

#ifdef _DEBUG
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
#endif

void TickTrigger()
{
    const auto& cfg = ConfigInstance.Aim;
    const auto snapshot = RobRunReader::GetSnapshot();
    const EffectiveSettings eff = ResolveEffectiveSettings(cfg, snapshot.get());
    const std::uint64_t nowMs = GetTickCount64();

    if (!eff.triggerEnabled || ConfigInstance.Overlay.PanicMode || Overlay::MenuOpen()) {
        ReleaseFireButton();
        g_triggerPhase = TriggerPhase::Idle;
        g_triggerDebugActive.store(false, std::memory_order_relaxed);
        g_triggerDebugState.store(cfg.TriggerEnabled ? "paused" : "off", std::memory_order_relaxed);
        return;
    }

    // Always finish an active fire pulse cleanly before checking new targets.
    if (g_triggerPhase == TriggerPhase::Firing) {
        if (nowMs >= g_triggerTimerMs) {
            ReleaseFireButton();
            const int cooldown = std::clamp(cfg.TriggerCooldownMs, 0, 500);
            if (cooldown > 0) {
                g_triggerPhase = TriggerPhase::Cooldown;
                g_triggerTimerMs = nowMs + static_cast<std::uint64_t>(cooldown);
                g_triggerDebugState.store("cooldown", std::memory_order_relaxed);
            } else {
                g_triggerPhase = TriggerPhase::Idle;
                g_triggerDebugState.store("armed", std::memory_order_relaxed);
            }
            g_triggerDebugActive.store(false, std::memory_order_relaxed);
        } else {
            RenewFireButton();
            g_triggerDebugActive.store(true, std::memory_order_relaxed);
            g_triggerDebugState.store("firing", std::memory_order_relaxed);
        }
        return;
    }

    if (g_triggerPhase == TriggerPhase::Cooldown) {
        if (nowMs < g_triggerTimerMs) {
            g_triggerDebugActive.store(false, std::memory_order_relaxed);
            g_triggerDebugState.store("cooldown", std::memory_order_relaxed);
            return;
        }
        g_triggerPhase = TriggerPhase::Idle;
    }

    const GamepadState gp = PollGamepad(cfg);
    const char* trigSource = "-";
    const bool trigKeyHeld = Held(eff.triggerKey, &trigSource, &gp);
    if (!trigKeyHeld) {
        g_triggerPhase = TriggerPhase::Idle;
        g_triggerDebugActive.store(false, std::memory_order_relaxed);
        g_triggerDebugState.store("waiting key", std::memory_order_relaxed);
        return;
    }

    if (!snapshot || snapshot->entities.empty()) {
        g_triggerPhase = TriggerPhase::Idle;
        g_triggerDebugActive.store(false, std::memory_order_relaxed);
        g_triggerDebugState.store("no entities", std::memory_order_relaxed);
        return;
    }

    const float dispX = g_displayWidth.load(std::memory_order_relaxed);
    const float dispY = g_displayHeight.load(std::memory_order_relaxed);
    const int width = static_cast<int>(dispX);
    const int height = static_cast<int>(dispY);
    if (width <= 0 || height <= 0) {
        g_triggerPhase = TriggerPhase::Idle;
        g_triggerDebugState.store("no display", std::memory_order_relaxed);
        return;
    }

    Matrix4x4 viewProjection{};
    if (!RobRunReader::ResolveViewProjection(viewProjection, width, height)) {
        g_triggerPhase = TriggerPhase::Idle;
        g_triggerDebugState.store("no projection", std::memory_order_relaxed);
        return;
    }

    RobRunCamera::CameraState freshCamera{};
    const bool haveFreshCamera = RobRunReader::GetFreshCamera(freshCamera) && freshCamera.valid;
    const Vector3 camOrigin = haveFreshCamera
        ? freshCamera.position
        : (snapshot->camera.valid ? snapshot->camera.position : snapshot->localPosition);
    if (camOrigin.IsZero()) {
        g_triggerPhase = TriggerPhase::Idle;
        g_triggerDebugState.store("no camera", std::memory_order_relaxed);
        return;
    }

    const float yScale = std::sqrt(
        viewProjection.matrix[0][1] * viewProjection.matrix[0][1] +
        viewProjection.matrix[1][1] * viewProjection.matrix[1][1] +
        viewProjection.matrix[2][1] * viewProjection.matrix[2][1]);
    if (!std::isfinite(yScale) || yScale <= 1e-4f) {
        g_triggerPhase = TriggerPhase::Idle;
        return;
    }

    const float centerX = dispX * 0.5f;
    const float centerY = dispY * 0.5f;
    const float halfH = dispY * 0.5f;
    const bool haveLocalPos = !snapshot->localPosition.IsZero();
    const bool bvhLoaded = RobRunReader::GetBvhStatus().loaded;
    const int trigBone = std::clamp(cfg.TriggerBone, 0, 4);
    // World-space anatomical radii (metres) for Head, Neck, Chest, Pelvis
    static constexpr float kBoneRadii[4] = { 0.13f, 0.11f, 0.22f, 0.20f };

    bool targetOnCrosshair = false;
    for (const auto& ent : snapshot->entities) {
        if (ent.entityId == snapshot->localEntityId) continue;
        if (ent.isDead) continue;
        if (cfg.SkipKnocked && (ent.isKnockedDown || ent.isKnockedOut)) continue;
        if (ShouldSkipByRole(ent, cfg)) continue;
        // When BVH is loaded, rely on the live per-bone IsCameraOccluded check below
        // instead of pre-rejecting on a potentially stale snapshot ent.visible flag.
        if (cfg.TriggerVisibleOnly && !bvhLoaded && !ent.visible) continue;
        if (haveLocalPos &&
            Distance(ent.position, snapshot->localPosition) > static_cast<float>(eff.triggerMaxDistance))
            continue;

        const float entDist = Distance(camOrigin, Vector3(ent.position.x, ent.position.y + 1.2f, ent.position.z));
        if (!std::isfinite(entDist) || entDist < 0.35f) continue;

        auto boneRadiusPx = [&](int b, const Vector3& p) -> float {
            const float d = (std::max)(0.35f, Distance(camOrigin, p));
            const float rM = kBoneRadii[std::clamp(b, 0, 3)] * eff.triggerHitboxScale;
            return std::clamp((rM / d) * halfH * yScale, 2.5f, 180.f);
        };

        if (trigBone < 4) {
            const Vector3 p = BonePoint(ent, trigBone);
            Vector2 sc{};
            if (!RobRunReader::ProjectWorld(viewProjection, p, width, height, sc))
                continue;
            const float rPx = boneRadiusPx(trigBone, p);
            if (std::hypot(sc.x - centerX, sc.y - centerY) <= rPx) {
                if (!cfg.TriggerVisibleOnly || !bvhLoaded || !RobRunReader::IsCameraOccluded(p)) {
                    targetOnCrosshair = true;
                    break;
                }
            }
        } else {
            // Any/Closest bone: test Head, Neck, Chest, Pelvis spheres and spine capsule segments.
            Vector3 pts[4]{};
            Vector2 scs[4]{};
            float rads[4]{};
            bool ok[4]{};
            for (int b = 0; b < 4; ++b) {
                pts[b] = BonePoint(ent, b);
                ok[b] = RobRunReader::ProjectWorld(viewProjection, pts[b], width, height, scs[b]);
                if (ok[b]) rads[b] = boneRadiusPx(b, pts[b]);
            }

            bool hit = false;
            Vector3 hitPoint{};
            for (int b = 0; b < 4; ++b) {
                if (ok[b] && std::hypot(scs[b].x - centerX, scs[b].y - centerY) <= rads[b]) {
                    hit = true;
                    hitPoint = pts[b];
                    break;
                }
            }
            if (!hit) {
                for (int b = 0; b < 3; ++b) {
                    if (!ok[b] || !ok[b + 1]) continue;
                    float segT = 0.f;
                    const float segDist = PointToSegmentDist(
                        centerX, centerY, scs[b].x, scs[b].y, scs[b + 1].x, scs[b + 1].y, segT);
                    const float segRad = rads[b] + (rads[b + 1] - rads[b]) * segT;
                    if (segDist <= segRad) {
                        hit = true;
                        hitPoint = Vector3(
                            pts[b].x + (pts[b + 1].x - pts[b].x) * segT,
                            pts[b].y + (pts[b + 1].y - pts[b].y) * segT,
                            pts[b].z + (pts[b + 1].z - pts[b].z) * segT);
                        break;
                    }
                }
            }
            if (hit) {
                if (!cfg.TriggerVisibleOnly || !bvhLoaded || !RobRunReader::IsCameraOccluded(hitPoint)) {
                    targetOnCrosshair = true;
                    break;
                }
            }
        }
    }

    if (!targetOnCrosshair) {
        g_triggerPhase = TriggerPhase::Idle;
        g_triggerDebugActive.store(false, std::memory_order_relaxed);
        g_triggerDebugState.store("armed", std::memory_order_relaxed);
        return;
    }

    if (g_triggerPhase == TriggerPhase::Idle) {
        const int delayMs = RollTriggerDelayMs(eff.triggerMinDelayMs, eff.triggerMaxDelayMs);
        if (delayMs <= 0) {
            PressFireButton();
            g_triggerPhase = TriggerPhase::Firing;
            g_triggerTimerMs = nowMs + static_cast<std::uint64_t>(std::clamp(cfg.TriggerBurstMs, 10, 250));
            g_triggerDebugActive.store(true, std::memory_order_relaxed);
            g_triggerDebugState.store("firing", std::memory_order_relaxed);
        } else {
            g_triggerPhase = TriggerPhase::WaitingDelay;
            g_triggerTimerMs = nowMs + static_cast<std::uint64_t>(delayMs);
            g_triggerDebugActive.store(false, std::memory_order_relaxed);
            g_triggerDebugState.store("delay", std::memory_order_relaxed);
        }
        return;
    }

    if (g_triggerPhase == TriggerPhase::WaitingDelay) {
        if (nowMs >= g_triggerTimerMs) {
            PressFireButton();
            g_triggerPhase = TriggerPhase::Firing;
            g_triggerTimerMs = nowMs + static_cast<std::uint64_t>(std::clamp(cfg.TriggerBurstMs, 10, 250));
            g_triggerDebugActive.store(true, std::memory_order_relaxed);
            g_triggerDebugState.store("firing", std::memory_order_relaxed);
        } else {
            g_triggerDebugActive.store(false, std::memory_order_relaxed);
            g_triggerDebugState.store("delay", std::memory_order_relaxed);
        }
    }
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

    const GamepadState gp = PollGamepad(cfg);
    const auto snapshot = RobRunReader::GetSnapshot();
    const EffectiveSettings eff = ResolveEffectiveSettings(cfg, snapshot.get());
    g_activeFov.store(eff.fov, std::memory_order_relaxed);

    dbg.enabled = cfg.Enabled;
    dbg.menuOpen = Overlay::MenuOpen();
    dbg.aimType = cfg.AimType;
    dbg.deviceReady = Base::Hardware::Ready();
    dbg.smoothAlgorithm = eff.smoothAlgorithm;
    dbg.activeFov = eff.fov;
    dbg.activeWeaponClass = eff.weaponClass;
    dbg.activeWeaponName = eff.weaponName;
    dbg.profileOverrideActive = eff.profileOverrideActive;
    dbg.controllerConnected = gp.connected;
    dbg.controllerSource = gp.source;
    dbg.controllerLt = gp.lt;
    dbg.controllerRt = gp.rt;
    dbg.controllerRx = gp.rx;
    dbg.controllerRy = gp.ry;
    dbg.triggerEnabled = eff.triggerEnabled;
    dbg.triggerActive = g_triggerDebugActive.load(std::memory_order_relaxed);
    dbg.triggerState = g_triggerDebugState.load(std::memory_order_relaxed);
    dbg.predictionSource = !cfg.Prediction ? "off" : "waiting for target";

    const auto stopAiming = [] {
        if (g_wasAiming) {
            Base::Hardware::Session().Cancel(Base::Hardware::Source::Aim);
            g_wasAiming = false;
        }
    };

    const auto resetSmoothing = [] {
        g_smoothInit = false;
        g_carryYaw = g_carryPitch = 0.f;
        g_bezierT = 0.f;
        g_cursorCurve.Reset();
        g_lastWriteTick = 0;
        g_haveWrittenForward = false;
        g_writeHealthy = false;
        g_lastVerdict = "-";
    };

    auto clearLastRejected = []() {
        std::lock_guard<std::mutex> lk(g_debugMutex);
        g_lastRejected = RejectedDebug{};
    };

    if (!cfg.Enabled) {
        dbg.reason = "aim disabled";
        clearLastRejected();
        stopAiming();
        resetSmoothing();
        g_lastTarget = -1;
        return;
    }
    if (ConfigInstance.Overlay.PanicMode) {
        dbg.reason = "panic active";
        clearLastRejected();
        stopAiming();
        resetSmoothing();
        g_lastTarget = -1;
        return;
    }
    if (Overlay::MenuOpen()) {
        dbg.reason = "menu open";
        clearLastRejected();
        stopAiming();
        resetSmoothing();
        g_lastTarget = -1;
        return;
    }

    auto stickGateOk = [&](int keyIndex) -> bool {
        const int idx = (keyIndex >= 0 && keyIndex < kKeyCount) ? keyIndex : 0;
        if (!IsGamepadKeyCode(kKeys[idx]) || !cfg.ControllerRequireStick)
            return true;
        if (!gp.connected) return false;
        const float deadzone = std::clamp(cfg.ControllerStickDeadzone, 0.f, 0.80f);
        return std::hypot(gp.rx, gp.ry) >= deadzone;
    };

    const char* keySource = "-";
    bool keyHeld = Held(cfg.AimKey, &keySource, &gp) && stickGateOk(cfg.AimKey);
    if (!keyHeld && cfg.ControllerEnabled && cfg.ControllerSecondaryEnable) {
        const char* padSource = "-";
        if (Held(cfg.ControllerAimKey, &padSource, &gp) && stickGateOk(cfg.ControllerAimKey)) {
            keyHeld = true;
            keySource = padSource;
        }
    }
    dbg.held = keyHeld;
    dbg.keySource = keySource;
    if (!dbg.held) {
        dbg.reason = "aim key not held";
        clearLastRejected();
        stopAiming();
        resetSmoothing();
        g_lastTarget = -1;
        ResetMotionHistory();
        return;
    }

    if (!snapshot || snapshot->entities.empty()) {
        dbg.reason = "no entities";
        clearLastRejected();
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
        ResetMotionHistory();
        g_writeBlockedUntil = GetTickCount64() + 1000;
    }

    // One-off debug metadata dump (Debug builds only so Release never stalls
    // the first active tick or writes diagnostic files to C:\).
#ifdef _DEBUG
    DumpAimFieldsOnce();
#endif

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
    const float fovDeg = std::clamp(static_cast<float>(eff.fov), 5.f, 180.f);
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
    const char* bestPredictionSource = "off";
    bool lastValid = false;
    Vector3 lastPoint{};
    Vector2 lastScreen{};
    int lastLeadMs = 0;
    const char* lastPredictionSource = "off";
    // If the local transform could not be resolved the origin would reject
    // every target on distance, so only apply the range filter when we have a
    // real local position.
    const bool haveLocalPosition = !snapshot->localPosition.IsZero();
    const std::uint64_t nowMs = GetTickCount64();
    const float responseMs = cfg.ResponseMs > 0
        ? std::clamp(static_cast<float>(cfg.ResponseMs), 1.f, 250.f)
        : ResponseForSmoothness(eff.smoothness);
    const auto steadyNowUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const float observationAgeMs = snapshot->stampUs != 0 && steadyNowUs >= snapshot->stampUs
        ? std::clamp(static_cast<float>(steadyNowUs - snapshot->stampUs) * 0.001f, 0.f, 80.f)
        : 0.f;
    const bool bvhLoaded = RobRunReader::GetBvhStatus().loaded;

    // Bounded sample table: entity ids churn across matches, so drop stale
    // history rather than letting the map grow for the whole session.
    PruneMotionHistory(nowMs);

    RejectedDebug closestRejected{};
    auto noteRejected = [&](const RobRunReader::EntityData& e, const char* why) {
        Vector2 sc{};
        const Vector3 probe = Vector3(e.position.x, e.position.y + 1.25f, e.position.z);
        if (!RobRunReader::ProjectWorld(viewProjection, probe, width, height, sc)) return;
        const float d = std::hypot(sc.x - centerX, sc.y - centerY);
        if (d < closestRejected.screenDist) {
            closestRejected.entityId = e.entityId;
            closestRejected.screenDist = d;
            closestRejected.role = RobRunReader::RoleLabel(e.role, e.roleName);
            closestRejected.reason = why;
            closestRejected.rigValid = e.bonePoints.valid;
            closestRejected.hasHead = e.bonePoints.hasHead;
        }
    };

    for (const auto& ent : snapshot->entities) {
        if (ent.entityId == snapshot->localEntityId) continue;
        if (ent.isDead) { noteRejected(ent, "dead"); continue; }
        if (cfg.SkipKnocked && (ent.isKnockedDown || ent.isKnockedOut)) {
            noteRejected(ent, ent.isKnockedDown ? "knocked(kd)" : "knocked(ko)");
            continue;
        }
        if (const char* roleWhy = RoleSkipReason(ent, cfg)) { noteRejected(ent, roleWhy); continue; }
        // When BVH is loaded, let the live per-bone IsCameraOccluded check below
        // evaluate Head/Neck/Chest/Pelvis directly instead of pre-rejecting on a
        // potentially stale snapshot ent.visible flag.
        if (cfg.VisibleBonesOnly && !bvhLoaded && !ent.visible) { noteRejected(ent, "vis(no-bvh)"); continue; }
        if (haveLocalPosition &&
            Distance(ent.position, snapshot->localPosition) > cfg.MaxDistance) { noteRejected(ent, "max-dist"); continue; }

        int activeBone = eff.targetBone;
        if (activeBone == 5) {
            // Head & Body (natural random variation per engagement)
            activeBone = g_humanize.PickHeadOrBodyBone(ent.entityId, SteadyUs());
        }
        Vector3 point{};
        if (activeBone == 4) {
            // Closest bone to crosshair among Head, Neck, Chest, Pelvis
            bool bestBoneFound = false;
            float bestBoneDist = 1.0e9f;
            static const int allBones[] = { 0, 1, 2, 3 };
            for (int b : allBones) {
                const Vector3 bp = BonePoint(ent, b);
                if (cfg.VisibleBonesOnly && bvhLoaded && RobRunReader::IsCameraOccluded(bp))
                    continue;
                Vector2 bScreen{};
                if (!RobRunReader::ProjectWorld(viewProjection, bp, width, height, bScreen))
                    continue;
                const float bDist = std::hypot(bScreen.x - centerX, bScreen.y - centerY);
                if (!bestBoneFound || bDist < bestBoneDist) {
                    bestBoneFound = true;
                    bestBoneDist = bDist;
                    point = bp;
                }
            }
            if (!bestBoneFound) {
                if (cfg.VisibleBonesOnly && bvhLoaded) { noteRejected(ent, "vis(all-bones)"); continue; }
                point = BonePoint(ent, 2);
            }
        } else {
            if (cfg.DynamicBoneFallback) {
                // Drop to chest (bone 2) if low health or target bone occluded
                if (ent.healthKnown && ent.health <= 40.f) {
                    activeBone = 2;
                } else if (bvhLoaded && RobRunReader::IsCameraOccluded(BonePoint(ent, activeBone))) {
                    activeBone = 2;
                }
            }

            point = BonePoint(ent, activeBone);
            if (cfg.VisibleBonesOnly && bvhLoaded) {
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
                    if (!altFound) { noteRejected(ent, "vis(all-bones)"); continue; }
                }
            }
        }
        if (cfg.HumanizeOffsetCm > 0.05f) {
            const Vector3 hOff = g_humanize.SampleOffset(ent.entityId, SteadyUs(), cfg.HumanizeOffsetCm);
            point.x += hOff.x;
            point.y += hOff.y;
            point.z += hOff.z;
        }
        int leadMs = 0;
        const char* predictionSource = "off";

        if (cfg.Prediction) {
            Vector3 adaptivePoint{};
            int adaptiveLeadMs = 0;
            if (ApplyAdaptivePrediction(point, ent.entityId, ent.position, snapshot->stampUs, nowMs,
                    responseMs, observationAgeMs, adaptivePoint, adaptiveLeadMs, predictionSource, ent.inVehicle)) {
                point = adaptivePoint;
                leadMs = adaptiveLeadMs;
            }
        }

        Vector2 screen{};
        if (!RobRunReader::ProjectWorld(viewProjection, point, width, height, screen))
            continue;

        if (ent.entityId == g_lastTarget) {
            lastValid = true;
            lastScreen = screen;
            lastPoint = point;
            lastLeadMs = leadMs;
            lastPredictionSource = predictionSource;
        }

        const float dx = screen.x - centerX;
        const float dy = screen.y - centerY;
        const float distance = std::hypot(dx, dy);

        // Hard lock: the sticky target keeps its pick even when it leaves the
        // FOV ring, mirroring the reference's locked-target behaviour.
        const bool isLocked = cfg.HardLock && ent.entityId == g_lastTarget;
        if (!isLocked && distance > radiusPixels) { noteRejected(ent, "outside-fov"); continue; }
        ++dbg.candidates;

        if (isLocked) {
            found = true;
            bestId = ent.entityId;
            bestScreen = screen;
            bestPoint = point;
            bestLeadMs = leadMs;
            bestPredictionSource = predictionSource;
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
            bestPredictionSource = predictionSource;
        }
    }

    // Publish closest rejected target for Aim Debug HUD diagnostics (Issue #11)
    {
        std::lock_guard<std::mutex> lk(g_debugMutex);
        g_lastRejected = closestRejected;
    }

    if (!found) {
        if (closestRejected.entityId >= 0) {
            static std::uint64_t s_lastRejLogMs = 0;
            if (nowMs - s_lastRejLogMs >= 1500) {
                s_lastRejLogMs = nowMs;
                VeoxDiag("[AimSkip] id=%d role='%s' why=%s px=%.0f rig=%d head=%d",
                    closestRejected.entityId,
                    closestRejected.role.empty() ? "?" : closestRejected.role.c_str(),
                    closestRejected.reason, closestRejected.screenDist,
                    closestRejected.rigValid ? 1 : 0, closestRejected.hasHead ? 1 : 0);
            }
        }
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
        bestPredictionSource = lastPredictionSource;
    }
    if (bestId != g_lastTarget)
        g_lastTargetSwitchMs = nowMs;
    g_lastTarget = bestId;
    dbg.target = bestId;
    dbg.leadMs = bestLeadMs;
    dbg.predictionSource = bestPredictionSource;

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
    if (cfg.AimType == 0 && cfg.MemoryWrite && ConfigInstance.Overlay.MemWriteMaster) {
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
            snapshot->stampMs != 0 && now >= snapshot->stampMs && now - snapshot->stampMs <= 400;
        const bool validRoots = cameraModel > 0x10000 &&
            battleModel > 0x10000 && snapshot->battleEntities > 0x10000 &&
            snapshot->localCharacter > 0x10000 && !snapshot->entities.empty();
        const bool sessionSettled =
            snapshot->sessionChangedMs == 0 ||
            (now >= snapshot->sessionChangedMs && now - snapshot->sessionChangedMs >= 2000);
        if (!validRoots) {
            g_writeHealthy = false;
            g_lastVerdict = "waiting for session";
        } else if (!freshSnapshot) {
            g_writeHealthy = false;
            g_lastVerdict = "stale snapshot";
        } else if (now < g_writeBlockedUntil || !sessionSettled) {
            g_writeHealthy = false;
            g_lastVerdict = "level transition";
        } else if (const std::uint64_t nowUs = SteadyUs();
                   g_lastWriteTick == 0 || nowUs - g_lastWriteTick >= kWriteIntervalUs) {
            // g_lastWriteTick is steady_clock microseconds. dt is the real
            // elapsed time (a long stall still gets a bounded catch-up step).
            const float dtMs = g_lastWriteTick == 0
                ? 8.f
                : std::clamp(static_cast<float>(nowUs - g_lastWriteTick) * 0.001f, 0.5f, 25.f);
            g_lastWriteTick = nowUs;

            const float screenErrPx = std::hypot(dx, dy);
            const bool targetChanged = (!g_smoothInit || g_smoothTarget != bestId);
            if (targetChanged) {
                g_carryYaw = g_carryPitch = 0.f;
                g_bezierT = 0.f;
                g_bezierBendSign = ((static_cast<unsigned>(bestId) ^ (static_cast<unsigned>(bestId) >> 2)) & 1u) ? 1.f : -1.f;
                g_smoothInit = true;
                g_smoothTarget = bestId;
                g_targetLockUs = nowUs;
            }

            // Deadzone: once the crosshair is already within DeadzonePx of the
            // target point, stop writing micro-corrections so manual aim feels
            // completely natural with zero micro-stutter.
            if (cfg.DeadzonePx > 0.1f && screenErrPx <= cfg.DeadzonePx) {
                g_carryYaw = g_carryPitch = 0.f;
                dbg.reason = "in deadzone";
                return;
            }

            const float gain = 1.f - std::exp(-dtMs / responseMs);
            const float verticalGain = 1.f - std::exp(-dtMs / (responseMs * 0.8f));
            const bool instantSnap = (eff.smoothness <= 1 && cfg.ResponseMs == 0);

            // Mouse-cooperative live-camera tracking: always step FROM the
            // game's current live camera orientation rather than an internal
            // stale angle, so moving your own mouse never fights the aimbot.
            if (instantSnap) {
                g_curYaw = targetYaw;
                g_curPitch = targetPitch;
            } else {
                const Vector3 liveFwd = haveFreshCamera
                    ? freshCamera.forward
                    : (snapshot->camera.valid ? snapshot->camera.forward : EulerToForward(g_curYaw, g_curPitch));
                const float liveHoriz = std::hypot(liveFwd.x, liveFwd.z);
                if (Finite(liveFwd) && liveHoriz > 1.0e-4f) {
                    g_curYaw = std::atan2(liveFwd.x, liveFwd.z);
                    g_curPitch = std::atan2(-liveFwd.y, liveHoriz);
                }

                float dYaw = targetYaw - g_curYaw;
                while (dYaw > kPi) dYaw -= 2.f * kPi;
                while (dYaw < -kPi) dYaw += 2.f * kPi;
                const float dPitch = targetPitch - g_curPitch;
                const float dtSec = dtMs * 0.001f;
                const float speedMul = std::clamp(cfg.BezierSpeed, 0.25f, 3.f);

                if (dYaw * g_carryYaw < 0.f) g_carryYaw = 0.f;
                if (dPitch * g_carryPitch < 0.f) g_carryPitch = 0.f;

                float stepYaw = 0.f;
                float stepPitch = 0.f;

                if (eff.smoothAlgorithm == 1) {
                    // Linear: constant angular velocity clamped to exact target arrival.
                    const float angDist = std::hypot(dYaw, dPitch);
                    if (angDist > 1e-6f) {
                        const float radPerSec = (65.f / (std::max)(8.f, responseMs)) * speedMul;
                        const float step = (std::min)(angDist, radPerSec * dtSec);
                        stepYaw = (dYaw / angDist) * step + g_carryYaw;
                        stepPitch = (dPitch / angDist) * step + g_carryPitch;
                    }
                } else if (eff.smoothAlgorithm == 2) {
                    // Bezier: cubic S-curve progress with lateral perpendicular arc.
                    const float angDist = std::hypot(dYaw, dPitch);
                    if (angDist > 1e-6f) {
                        const float durationSec = std::clamp((responseMs * 0.0045f) / speedMul, 0.035f, 0.90f);
                        const float prevT = g_bezierT;
                        g_bezierT = (std::min)(1.f, g_bezierT + dtSec / durationSec);
                        auto cubicP = [](float u) -> float {
                            const float inv = 1.f - u;
                            return 3.f * inv * inv * u * 0.15f + 3.f * inv * u * u * 0.85f + u * u * u;
                        };
                        const float pPrev = cubicP(prevT);
                        const float pCur = cubicP(g_bezierT);
                        const float remFrac = (std::max)(1e-3f, 1.f - pPrev);
                        const float stepFrac = (g_bezierT >= 0.999f)
                            ? (std::min)(1.f, 1.f - std::exp(-dtMs / (std::max)(6.f, responseMs * 0.6f)))
                            : std::clamp((pCur - pPrev) / remFrac, 0.f, 1.f);

                        const float bend = std::clamp(cfg.BezierBend, 0.f, 1.f);
                        const float inv = 1.f - g_bezierT;
                        const float arc = 3.f * inv * inv * g_bezierT + 3.f * inv * g_bezierT * g_bezierT * 0.65f;
                        const float nyaw = -dPitch / angDist;
                        const float npitch = dYaw / angDist;
                        const float lat = g_bezierBendSign * bend * 0.35f * angDist * arc * (1.f - g_bezierT);

                        stepYaw = (dYaw + nyaw * lat) * stepFrac + g_carryYaw;
                        stepPitch = (dPitch + npitch * lat) * stepFrac + g_carryPitch;
                    }
                } else {
                    // EMA (0): exponential convergence
                    stepYaw = dYaw * gain + g_carryYaw;
                    stepPitch = dPitch * verticalGain + g_carryPitch;
                }

                // Soft entry ramp (140 ms smoothstep on new target) + FOV edge falloff
                // so targets entering the outer FOV ring don't yank suddenly.
                if (cfg.SoftEntry) {
                    const float lockMs = (nowUs >= g_targetLockUs)
                        ? static_cast<float>(nowUs - g_targetLockUs) * 0.001f
                        : 140.f;
                    const float u = std::clamp(lockMs / 140.f, 0.f, 1.f);
                    const float entryScale = 0.20f + 0.80f * (u * u * (3.f - 2.f * u));
                    const float fovFrac = (radiusPixels > 1.f)
                        ? std::clamp(screenErrPx / radiusPixels, 0.f, 1.f)
                        : 0.f;
                    const float edgeScale = 0.35f + 0.65f * std::pow(1.f - fovFrac, 1.5f);
                    const float assistScale = entryScale * edgeScale;
                    stepYaw *= assistScale;
                    stepPitch *= assistScale;
                }

                g_carryYaw = stepYaw - std::trunc(stepYaw * 1.0e4f) / 1.0e4f;
                g_carryPitch = stepPitch - std::trunc(stepPitch * 1.0e4f) / 1.0e4f;
                g_curYaw += stepYaw;
                g_curPitch += stepPitch;
                g_curPitch = std::clamp(g_curPitch, -1.55f, 1.55f);
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
    settings.smoothX = settings.smoothY = std::clamp(eff.smoothness, 1, 100);
    settings.smoothAlgorithm = eff.smoothAlgorithm;
    settings.bezierBend = std::clamp(cfg.BezierBend, 0.f, 1.f);
    settings.bezierSpeed = std::clamp(cfg.BezierSpeed, 0.25f, 3.f);
    settings.countsPerPixelX = settings.countsPerPixelY = 1.f;
    settings.deadzonePixels = 0.5f;

    if (cfg.AimType == 0) {
        // Cursor fallback: closed-loop relative movement on this PC. Each frame
        // re-reads the target so the gain does not need to match the game's
        // sensitivity exactly. This only reaches a game running on the same PC;
        // a two-PC DMA setup needs Hardware Aim or the angle write above.
        stopAiming();
        settings.responseMs = responseMs;
        const auto step = Base::AimMotion::StepWithCurve(
            g_cursorCurve, static_cast<std::uint64_t>(bestId), { dx, dy }, settings, dt);
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

    settings.responseMs = responseMs;
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
    intent.stickScale = std::clamp(cfg.ControllerStickScale, 10.f, 500.f);
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

static bool CanSafelyWriteSession(const RobRunReader::SnapshotPtr& snap, std::uint64_t now)
{
    if (!snap) return false;
    if (snap->battleModel <= 0x10000 || snap->battleEntities <= 0x10000 ||
        snap->localCharacter <= 0x10000 || snap->cameraModel <= 0x10000 ||
        snap->entities.empty())
        return false;
    if (snap->stampMs == 0 || now < snap->stampMs || (now - snap->stampMs) > 400)
        return false;
    if (snap->sessionChangedMs != 0 &&
        (now < snap->sessionChangedMs || (now - snap->sessionChangedMs) < 2000))
        return false;
    if (now < g_writeBlockedUntil)
        return false;
    return true;
}

static bool ReadPtrFreshSafe(std::uintptr_t addr, std::uintptr_t& out)
{
    out = 0;
    return addr > 0x10000 && RobRunMemory::ReadBytesFresh(addr, &out, sizeof(out)) && out > 0x10000;
}

static void TickRecoil()
{
    if (!ConfigInstance.Overlay.MemWriteMaster || !ConfigInstance.Aim.NoRecoil) {
        g_rcPitchValid = false;
        return;
    }

    const auto snapshot = RobRunReader::GetSnapshot();
    const std::uint64_t now = GetTickCount64();
    if (!CanSafelyWriteSession(snapshot, now)) {
        g_rcClient = 0;
        g_rcPitchValid = false;
        return;
    }

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

    if (g_rcClient <= 0x10000 || now - g_rcLastClientCheck > 250) {
        g_rcLastClientCheck = now;
        std::uintptr_t liveBattleEnts = 0;
        if (!ReadPtrFreshSafe(snapshot->battleModel + rr::BattleModel::BattleEntitiesModel, liveBattleEnts) ||
            liveBattleEnts != snapshot->battleEntities) {
            g_rcClient = 0;
            g_rcPitchValid = false;
            return;
        }
        std::uintptr_t localPlayer = 0;
        if (!ReadPtrFreshSafe(liveBattleEnts + rr::BattleEntitiesModel::LocalPlayerEntityModel, localPlayer)) {
            g_rcClient = 0;
            return;
        }
        std::uintptr_t client = 0;
        if (!ReadPtrFreshSafe(localPlayer + rr::PlayerEntityModel::Client, client)) {
            g_rcClient = 0;
            return;
        }
        g_rcClient = client;
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

static void StopRecoilWorkerLocked()
{
    g_recoilStop.store(true, std::memory_order_relaxed);
    if (g_recoilThread.joinable())
        g_recoilThread.join();
}

static void StartRecoilWorkerLocked()
{
    StopRecoilWorkerLocked();
    g_recoilStop.store(false, std::memory_order_relaxed);
    g_recoilThread = std::thread([] {
        while (!g_recoilStop.load(std::memory_order_relaxed)) {
            try { TickRecoil(); } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}

void StartRecoilWorker()
{
    std::lock_guard<std::mutex> lock(g_workerLifecycleMutex);
    StartRecoilWorkerLocked();
}

void StopRecoilWorker()
{
    std::lock_guard<std::mutex> lock(g_workerLifecycleMutex);
    StopRecoilWorkerLocked();
}

void TickRecoilSpread()
{
    const auto& cfg = ConfigInstance.Aim;
    if (!ConfigInstance.Overlay.MemWriteMaster || (!cfg.NoSpread && !cfg.DynamicRecoilCompensation))
        return;

    const auto snapshot = RobRunReader::GetSnapshot();
    const std::uint64_t now = GetTickCount64();
    if (!CanSafelyWriteSession(snapshot, now)) {
        g_cachedPlayerClient = 0;
        return;
    }

    if (snapshot->sessionGeneration != g_lastRecoilGen) {
        g_cachedPlayerClient = 0;
        g_lastRecoilGen = snapshot->sessionGeneration;
    }

    // Always verify live BattleModel -> BattleEntitiesModel ownership via uncached
    // DMA so a map transition can never write into a freed RecoilModel / FakeSpreadModel.
    std::uintptr_t liveBattleEnts = 0;
    if (!ReadPtrFreshSafe(snapshot->battleModel + rr::BattleModel::BattleEntitiesModel, liveBattleEnts) ||
        liveBattleEnts != snapshot->battleEntities) {
        g_cachedPlayerClient = 0;
        return;
    }

    static std::uint64_t g_lastClientCheck = 0;
    if (g_cachedPlayerClient <= 0x10000 || now - g_lastClientCheck > 250) {
        g_lastClientCheck = now;
        std::uintptr_t localPlayer = 0;
        if (!ReadPtrFreshSafe(liveBattleEnts + rr::BattleEntitiesModel::LocalPlayerEntityModel, localPlayer)) {
            g_cachedPlayerClient = 0;
            return;
        }
        std::uintptr_t client = 0;
        if (!ReadPtrFreshSafe(localPlayer + rr::PlayerEntityModel::Client, client)) {
            g_cachedPlayerClient = 0;
            return;
        }
        g_cachedPlayerClient = client;
    }

    // --- Dynamic Recoil: scale RecoilOffset (Vector2, 8 bytes) ---
    if (cfg.DynamicRecoilCompensation) {
        std::uintptr_t recoilModel = 0;
        if (ReadPtrFreshSafe(g_cachedPlayerClient + rr::PlayerClient::Recoil, recoilModel)) {
            struct { float x, y; } cur{};
            if (RobRunMemory::ReadBytesFresh(recoilModel + rr::RecoilModel::OffsetVec, &cur, sizeof(cur)) &&
                std::isfinite(cur.x) && std::isfinite(cur.y) &&
                std::fabs(cur.x) < 100.f && std::fabs(cur.y) < 100.f &&
                (std::fabs(cur.x) > 1e-4f || std::fabs(cur.y) > 1e-4f)) {
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
        std::uintptr_t spreadModel = 0;
        if (ReadPtrFreshSafe(g_cachedPlayerClient + rr::PlayerClient::FakeSpread, spreadModel)) {
            float curSpread = 0.f, curAir = 0.f, curMove = 0.f;
            if (RobRunMemory::ReadBytesFresh(spreadModel + rr::FakeSpreadModel::Spread, &curSpread, sizeof(curSpread)) &&
                RobRunMemory::ReadBytesFresh(spreadModel + rr::FakeSpreadModel::Air, &curAir, sizeof(curAir)) &&
                RobRunMemory::ReadBytesFresh(spreadModel + rr::FakeSpreadModel::Move, &curMove, sizeof(curMove)) &&
                std::isfinite(curSpread) && std::isfinite(curAir) && std::isfinite(curMove) &&
                (std::fabs(curSpread) > 1e-4f || std::fabs(curAir) > 1e-4f || std::fabs(curMove) > 1e-4f)) {
                Memory::ScopedScatter scatter(TargetProcess);
                if (scatter.handle) {
                    float zero = 0.f;
                    TargetProcess.AddScatterWriteRequest(scatter,
                        spreadModel + rr::FakeSpreadModel::Spread, &zero, sizeof(zero));
                    TargetProcess.AddScatterWriteRequest(scatter,
                        spreadModel + rr::FakeSpreadModel::Air, &zero, sizeof(zero));
                    TargetProcess.AddScatterWriteRequest(scatter,
                        spreadModel + rr::FakeSpreadModel::Move, &zero, sizeof(zero));
                    TargetProcess.ExecuteScatterWrite(scatter);
                }
            }
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

    const float activeFov = static_cast<float>(std::clamp(g_activeFov.load(std::memory_order_relaxed), 5, 180));
    Matrix4x4 viewProjection{};
    float radius;
    if (RobRunReader::ResolveViewProjection(viewProjection, (int)display.x, (int)display.y)) {
        radius = ConeRadiusPixels(viewProjection, activeFov,
            verticalFov, display.y);
    } else {
        radius = FovRadiusPixels(activeFov, verticalFov, display.y);
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
    static const char* const kCurveNames[] = { "EMA", "Linear", "Bezier" };
    const char* curveName = (d.smoothAlgorithm >= 0 && d.smoothAlgorithm < 3)
        ? kCurveNames[d.smoothAlgorithm] : "EMA";
    const char* profName = (d.activeWeaponClass >= 0 && d.activeWeaponClass < kWeaponClassCount)
        ? kWeaponClassNames[d.activeWeaponClass] : "Default";

    char lines[11][160];
    std::snprintf(lines[0], sizeof(lines[0]), "Aim: %s  Curve: %s  FOV: %d deg",
        d.enabled ? "on" : "off", curveName, d.activeFov);
    std::snprintf(lines[1], sizeof(lines[1]), "Menu: %s  Key: %s (%s)",
        d.menuOpen ? "open" : "closed", d.held ? "held" : "up", d.keySource);
    std::snprintf(lines[2], sizeof(lines[2]), "Entities: %d  Candidates: %d  Target: %d",
        d.entities, d.candidates, d.target);
    std::snprintf(lines[3], sizeof(lines[3]), "Delta: %.1f, %.1f  Step: %d, %d  Lead: %dms  Pred: %s",
        d.dx, d.dy, d.stepX, d.stepY, d.leadMs, d.predictionSource);
    std::snprintf(lines[4], sizeof(lines[4]), "Type: %s  Device: %s",
        d.aimType == 0 ? "memory" : "hardware", d.deviceReady ? "ready" : "not ready");
    std::snprintf(lines[5], sizeof(lines[5]), "Write: %s  %s  yaw %.1f pitch %.1f dist %.0fm",
        d.writeMode ? (d.writeOk ? "ok" : "fail") : "off",
        d.writeState, d.tgtYawDeg, d.tgtPitchDeg, d.distance);
    std::snprintf(lines[6], sizeof(lines[6]), "Weapon: %s [%s%s]  Trigger: %s",
        d.activeWeaponName.empty() ? "-" : d.activeWeaponName.c_str(),
        profName, d.profileOverrideActive ? "*" : "", d.triggerState);
    std::snprintf(lines[7], sizeof(lines[7]), "Pad: %s (%s)  LT %.2f RT %.2f  RS (%.2f, %.2f)",
        d.controllerConnected ? "on" : "off", d.controllerSource,
        d.controllerLt, d.controllerRt, d.controllerRx, d.controllerRy);
    std::snprintf(lines[8], sizeof(lines[8]), "Reason: %s", d.reason);
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
            std::snprintf(lines[9], sizeof(lines[9]),
                "ESP cam: native d=%.2fm a=%.1f  %s", cam.positionError, angle,
                pose);
        else
            std::snprintf(lines[9], sizeof(lines[9]),
                "ESP cam: %s (native %s)  %s",
                RobRunReader::GetViewProjectionSource(),
                cam.nativeEnabled ? "not found" : "off", pose);
    }

    {
        RejectedDebug rej{};
        {
            std::lock_guard<std::mutex> lk(g_debugMutex);
            rej = g_lastRejected;
        }
        if (rej.entityId >= 0)
            std::snprintf(lines[10], sizeof(lines[10]),
                "Skip: id=%d [%s] why=%s px=%.0f rig=%d head=%d",
                rej.entityId, rej.role.empty() ? "?" : rej.role.c_str(),
                rej.reason, rej.screenDist, rej.rigValid ? 1 : 0, rej.hasHead ? 1 : 0);
        else
            std::snprintf(lines[10], sizeof(lines[10]), "Skip: none");
    }

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 origin(12.f, 12.f);
    for (int i = 0; i < 11; ++i) {
        dl->AddText(ImVec2(origin.x + 1.f, origin.y + i * 15.f + 1.f),
            IM_COL32(36, 38, 44, 220), lines[i]);
        dl->AddText(ImVec2(origin.x, origin.y + i * 15.f),
            IM_COL32(120, 200, 255, 235), lines[i]);
    }
}

} // namespace Aim
