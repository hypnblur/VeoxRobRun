#include "pch.h"
#include "FovChanger.h"
#include "RobRunSDK.h"
#include "RobRunCamera.h"
#include "RobRunReader.h"
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "Memory.h"
#include "InputManager.h"
#include "../Config/ConfigInstance.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace FovChanger {

constexpr int kMaxFovTargets = 16;

struct FovTarget {
    std::uintptr_t addr = 0;
    float original = 0.f;
    bool used = false;
};

static std::mutex g_fovMutex;
static FovTarget g_fovTargets[kMaxFovTargets];
static std::atomic<bool> g_fovHolding{ false };
static std::uint64_t g_fovModUntil = 0;

static inline bool IsValidPtr(std::uintptr_t ptr)
{
    return ptr > 0x10000 && ptr < 0x7FFFFFFFFFFF;
}

// Class-name match: "Type" or "Namespace.Type", exact suffix with '.' boundary
static bool ClassNamed(std::uintptr_t obj, const char* want)
{
    if (!IsValidPtr(obj) || !want) return false;
    const std::string nm = Il2Cpp::ClassNameOf(obj);
    if (nm.empty()) return false;
    if (_stricmp(nm.c_str(), want) == 0) return true;

    const std::size_t wantLen = std::strlen(want);
    const std::size_t nmLen = nm.length();
    if (nmLen >= wantLen) {
        if (_stricmp(nm.c_str() + (nmLen - wantLen), want) == 0) {
            if (nmLen == wantLen || nm[nmLen - wantLen - 1] == '.') return true;
        }
    }

    // Check with namespace if available
    const std::uintptr_t klass = Il2Cpp::ClassOf(obj);
    if (IsValidPtr(klass)) {
        const std::string ns = Il2Cpp::NamespaceOfClass(klass);
        if (!ns.empty()) {
            const std::string fullName = ns + "." + nm;
            if (_stricmp(fullName.c_str(), want) == 0) return true;
        }
    }
    return false;
}

// Read-modify-write: an unchanged value costs one fresh read instead of a DMA write
static void WriteFloatMaybe(std::uintptr_t addr, float value)
{
    float current = 0.f;
    if (!TargetProcess.ReadFresh(addr, &current, sizeof(current))) return;
    if (current == value) return;
    TargetProcess.Write(addr, value);
}

// Collect CinemachineVirtualCamera leaves that actually carry m_Lens.
// MixingCamera nodes have no lens; recurse into _cameras.
static void CollectVcams(std::uintptr_t obj, int depth, std::uintptr_t* out, int* n)
{
    if (!IsValidPtr(obj) || depth > 3 || *n >= kMaxFovTargets) return;
    for (int i = 0; i < *n; ++i) {
        if (out[i] == obj) return;
    }
    if (ClassNamed(obj, "CinemachineVirtualCamera")) {
        out[(*n)++] = obj;
        return;
    }
    if (ClassNamed(obj, "MixingCamera")) {
        const std::uintptr_t list = TargetProcess.Read<std::uintptr_t>(obj + rr::Fov::MixCameras);
        if (!IsValidPtr(list)) return;

        std::vector<std::uintptr_t> children;
        if (RobRunMemory::ReadList(list, children, false)) {
            for (std::uintptr_t child : children) {
                if (IsValidPtr(child)) {
                    CollectVcams(child, depth + 1, out, n);
                }
            }
        } else {
            // Raw IL2CPP array fallback
            const std::uintptr_t items = TargetProcess.Read<std::uintptr_t>(list + rr::List::items);
            const std::int32_t count = TargetProcess.Read<std::int32_t>(list + rr::List::count);
            if (IsValidPtr(items) && count > 0 && count <= 32) {
                const int limit = std::min(count, 16);
                std::uintptr_t childArray[16]{};
                if (TargetProcess.Read(items + rr::Array::vector, childArray, limit * sizeof(std::uintptr_t))) {
                    for (int i = 0; i < limit; ++i) {
                        if (IsValidPtr(childArray[i])) {
                            CollectVcams(childArray[i], depth + 1, out, n);
                        }
                    }
                }
            }
        }
    }
}

// CinemachineBrain: check RobRunCamera cached brain first; if unpopulated,
// walk the views updater's dictionaries.
static std::uintptr_t FindBrain(std::uintptr_t views)
{
    // 1. CinemachineCore brain list path
    std::uintptr_t brain = RobRunCamera::GetCachedBrain();
    if (IsValidPtr(brain) && ClassNamed(brain, "CinemachineBrain")) {
        return brain;
    }

    // 2. BattleEntitiesUpdater updater dictionaries
    if (IsValidPtr(views) && ClassNamed(views, "BattleEntitiesUpdater")) {
        const std::uintptr_t dictOffsets[2] = {
            rr::Fov::BeuUpdaters,
            rr::Fov::BeuViewUpdaters
        };
        for (int d = 0; d < 2; ++d) {
            const std::uintptr_t dict = TargetProcess.Read<std::uintptr_t>(views + dictOffsets[d]);
            if (!IsValidPtr(dict)) continue;
            std::vector<RobRunMemory::DictEntry> entries;
            if (RobRunMemory::ReadDictInt(dict, entries, false)) {
                for (const auto& entry : entries) {
                    const std::uintptr_t updater = entry.value;
                    if (!IsValidPtr(updater)) continue;
                    if (!ClassNamed(updater, "BattleAimTargetUpdater")) continue;
                    brain = TargetProcess.Read<std::uintptr_t>(updater + rr::Fov::AimBrain);
                    if (IsValidPtr(brain) && ClassNamed(brain, "CinemachineBrain")) {
                        return brain;
                    }
                }
            }
        }
    }
    return 0;
}

// Sprint FOV: the game animates the camera lens while Shift is held and for a
// short tail after release. Forcing our value against those per-frame writes
// pumps the view frame-to-frame and desyncs overlay projection. While sprint
// is active the game owns the lens; the hold re-applies after the tail.
static bool IsFovModActive()
{
    const bool shiftDown =
        (Keyboard::IsReady() && Keyboard::IsKeyDown(VK_SHIFT)) ||
        ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
    const std::uint64_t now = GetTickCount64();
    if (shiftDown) {
        g_fovModUntil = now + 600;
        return true;
    }
    return now < g_fovModUntil;
}

static void RestoreLocked()
{
    if (!g_fovHolding.load(std::memory_order_relaxed)) return;
    for (int i = 0; i < kMaxFovTargets; ++i) {
        FovTarget* t = &g_fovTargets[i];
        if (!t->used) continue;
        if (IsValidPtr(t->addr) && t->original >= 1.f && t->original <= 179.f) {
            TargetProcess.Write(t->addr + rr::Fov::VcamLensFov, t->original);
        }
        t->used = false;
        t->addr = 0;
        t->original = 0.f;
    }
    g_fovHolding.store(false, std::memory_order_release);
}

void Restore()
{
    std::lock_guard<std::mutex> lock(g_fovMutex);
    RestoreLocked();
}

static std::uint64_t g_fovSessionGen = 0;

static void ClearTargetsNoWrite()
{
    std::memset(g_fovTargets, 0, sizeof(g_fovTargets));
    g_fovHolding.store(false, std::memory_order_release);
}

void Reset()
{
    std::lock_guard<std::mutex> lock(g_fovMutex);
    // Reset is called on re-attach / shutdown — drop cached vcam pointers
    // without blindly writing into potentially freed process/scene memory.
    ClearTargetsNoWrite();
    g_fovSessionGen = 0;
    g_fovModUntil = 0;
}

bool IsHolding()
{
    return g_fovHolding.load(std::memory_order_acquire);
}

void Tick()
{
    std::lock_guard<std::mutex> lock(g_fovMutex);
    const bool enabled = ConfigInstance.Overlay.MemWriteMaster &&
        ConfigInstance.Overlay.FovChanger && !ConfigInstance.Overlay.PanicMode;
    const float fovVal = std::clamp(ConfigInstance.Overlay.CameraFov, 20.0f, 160.0f);

    const auto snap = RobRunReader::GetSnapshot();
    const std::uint64_t now = GetTickCount64();

    // On map/session change, Unity frees the old Cinemachine vcams: drop cached
    // pointers immediately without writing to them.
    if (!snap || snap->sessionGeneration != g_fovSessionGen) {
        ClearTargetsNoWrite();
        if (snap) g_fovSessionGen = snap->sessionGeneration;
        return;
    }

    // Block all FOV writes during stale snapshots, missing roots, or the 2s
    // post-transition settle window.
    if (snap->battleModel <= 0x10000 || snap->battleEntities <= 0x10000 ||
        snap->cameraModel <= 0x10000 || snap->entities.empty() ||
        snap->stampMs == 0 || now < snap->stampMs || (now - snap->stampMs) > 400 ||
        (snap->sessionChangedMs != 0 && (now < snap->sessionChangedMs || (now - snap->sessionChangedMs) < 2000)))
        return;

    const std::uintptr_t views = snap->battleEntitiesUpdater;

    std::uintptr_t targets[kMaxFovTargets]{};
    int n = 0;

    if (enabled || g_fovHolding.load(std::memory_order_relaxed)) {
        std::uintptr_t brain = FindBrain(views);
        if (IsValidPtr(brain)) {
            const std::uintptr_t activeVcam = TargetProcess.Read<std::uintptr_t>(brain + rr::Fov::BrainActiveCamera);
            CollectVcams(activeVcam, 0, targets, &n);
        }

        // Fallback entry: the mixing camera from the views updater
        if (n == 0 && IsValidPtr(views) && ClassNamed(views, "BattleEntitiesUpdater")) {
            const std::uintptr_t mixup = TargetProcess.Read<std::uintptr_t>(views + rr::Fov::BeuMixup);
            if (IsValidPtr(mixup) && ClassNamed(mixup, "MixingCameraBlendUpdater")) {
                const std::uintptr_t mixer = TargetProcess.Read<std::uintptr_t>(mixup + rr::Fov::MixupMixer);
                CollectVcams(mixer, 0, targets, &n);
            }
        }
    }

    if (!enabled) {
        if (g_fovHolding.load(std::memory_order_relaxed)) {
            for (int i = 0; i < kMaxFovTargets; ++i) {
                FovTarget* t = &g_fovTargets[i];
                if (!t->used) continue;
                if (n > 0) {
                    for (int j = 0; j < n; ++j) {
                        if (targets[j] == t->addr) {
                            TargetProcess.Write(t->addr + rr::Fov::VcamLensFov, t->original);
                            break;
                        }
                    }
                }
                // Never blindly write to t->addr when n == 0 (old vcam may already be freed).
                t->used = false;
                t->addr = 0;
                t->original = 0.f;
            }
            g_fovHolding.store(false, std::memory_order_release);
        }
        return;
    }

    // Sprint pause: stop writing while the game animates the lens. The
    // captured originals and holding state stay armed for the resume.
    if (IsFovModActive()) return;
    if (n <= 0) return;
    g_fovHolding.store(true, std::memory_order_release);

    for (int j = 0; j < n; ++j) {
        FovTarget* t = nullptr;
        for (int i = 0; i < kMaxFovTargets; ++i) {
            if (g_fovTargets[i].used && g_fovTargets[i].addr == targets[j]) {
                t = &g_fovTargets[i];
                break;
            }
            if (!t && !g_fovTargets[i].used) {
                t = &g_fovTargets[i];
            }
        }
        if (!t) break;
        if (!t->used) {
            t->addr = targets[j];
            float orig = 0.f;
            if (TargetProcess.Read(targets[j] + rr::Fov::VcamLensFov, &orig, sizeof(orig)) &&
                orig >= 1.f && orig <= 179.f) {
                t->original = orig;
            } else {
                t->original = 60.0f;
            }
            t->used = true;
        }
        WriteFloatMaybe(targets[j] + rr::Fov::VcamLensFov, fovVal);
    }

    // Drop entries for vcams that left the tree; the game reset them.
    for (int i = 0; i < kMaxFovTargets; ++i) {
        if (!g_fovTargets[i].used) continue;
        bool live = false;
        for (int j = 0; j < n; ++j) {
            if (targets[j] == g_fovTargets[i].addr) {
                live = true;
                break;
            }
        }
        if (!live) {
            g_fovTargets[i].used = false;
            g_fovTargets[i].addr = 0;
            g_fovTargets[i].original = 0.f;
        }
    }
}

} // namespace FovChanger
